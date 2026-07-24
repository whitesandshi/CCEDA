#include "meshing/GmshWorkerClient.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace cceda::meshing
{
namespace
{
constexpr int ProtocolMajor = 0;
constexpr int ProtocolMinor = 1;
constexpr qsizetype MaximumMessageBytes = 64 * 1024;

GmshWorkerFailure makeFailure(
  GmshWorkerFailureKind kind,
  QString code,
  QString stage,
  QString detail,
  int exitCode = 0)
{
  return {
    kind,
    std::move(code),
    std::move(stage),
    std::move(detail),
    false,
    exitCode,
  };
}

bool isRelativeWorkspacePath(const QString& path)
{
  if (path.isEmpty() || QDir::isAbsolutePath(path))
  {
    return false;
  }
  const QString clean = QDir::cleanPath(path);
  return clean.startsWith("output/")
    && clean != "output/.."
    && !clean.startsWith("../")
    && !clean.startsWith("..\\");
}

bool isSha256(const QString& value)
{
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
  return pattern.match(value).hasMatch();
}

bool contains(const QJsonArray& values, const QJsonValue& expected)
{
  return std::any_of(values.cbegin(), values.cend(),
    [&](const QJsonValue& value) { return value == expected; });
}
}

class GmshWorkerClient::Implementation
{
public:
  explicit Implementation(GmshWorkerClient* owner)
    : owner_(owner)
  {
    cancellationDeadline_.setSingleShot(true);
    // Qt documents QProcess as a sequential I/O device and provides these
    // signals for asynchronous output, errors, and completion.
    // https://doc.qt.io/qt-6.8/qprocess.html#communicating-via-channels
    QObject::connect(&process_, &QProcess::readyReadStandardOutput,
      owner_, [this] { consumeOutput(); });
    QObject::connect(&process_, &QProcess::readyReadStandardError,
      owner_, [this] { consumeErrorOutput(); });
    QObject::connect(&process_, &QProcess::errorOccurred,
      owner_, [this](QProcess::ProcessError error) {
        if (error != QProcess::UnknownError && !forcedCancellation_)
        {
          fail(makeFailure(
            GmshWorkerFailureKind::WorkerLost,
            "MESH_WORKER_LOST",
            lastStage_,
            process_.errorString(),
            process_.exitCode()));
        }
      });
    QObject::connect(&process_,
      qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
      owner_,
      [this](int exitCode, QProcess::ExitStatus exitStatus) {
        cancellationDeadline_.stop();
        consumeOutput();
        consumeErrorOutput();
        if (terminalEmitted_)
        {
          return;
        }
        if (!outputBuffer_.isEmpty())
        {
          fail(protocolFailure("Worker output ended without a message delimiter"));
          return;
        }
        if (forcedCancellation_ && cancellationRequested_)
        {
          terminalEmitted_ = true;
          emit owner_->cancelled({
            request_.jobId,
            cancellationReason_,
            lastStage_,
            true,
          });
          return;
        }
        if (exitStatus != QProcess::NormalExit || exitCode != 0)
        {
          QString detail = QString("Worker exited with code %1").arg(exitCode);
          if (!errorOutput_.isEmpty())
          {
            detail += QString(": %1").arg(QString::fromLocal8Bit(errorOutput_));
          }
          fail(makeFailure(
            GmshWorkerFailureKind::WorkerLost,
            "MESH_WORKER_LOST",
            lastStage_,
            detail,
            exitCode));
          return;
        }
        if (pendingCancellation_)
        {
          terminalEmitted_ = true;
          emit owner_->cancelled(*pendingCancellation_);
          return;
        }
        if (cancellationRequested_)
        {
          fail(protocolFailure(
            "Worker exited successfully without a cancelled message"));
          return;
        }
        if (!pendingResult_)
        {
          fail(protocolFailure("Worker exited successfully without a result message"));
          return;
        }
        terminalEmitted_ = true;
        emit owner_->succeeded(*pendingResult_);
      });
    QObject::connect(&cancellationDeadline_, &QTimer::timeout,
      owner_, [this] {
        if (!terminalEmitted_ && cancellationRequested_
          && process_.state() != QProcess::NotRunning)
        {
          // Qt documents kill() as the reliable immediate termination path
          // for Windows console processes.
          // https://doc.qt.io/qt-6.8/qprocess.html#kill
          forcedCancellation_ = true;
          process_.kill();
        }
      });
  }

  ~Implementation()
  {
    if (process_.state() != QProcess::NotRunning)
    {
      process_.kill();
      process_.waitForFinished(5000);
    }
  }

  std::optional<GmshWorkerFailure> start(const GmshWorkerLaunchRequest& request)
  {
    if (process_.state() != QProcess::NotRunning)
    {
      return makeFailure(
        GmshWorkerFailureKind::InvalidRequest,
        "MESH_INPUT_INVALID",
        "initializing",
        "A Worker job is already running");
    }
    if (request.jobId.isEmpty() || request.mainBuildId.isEmpty()
      || request.sourceGeometryRevision == 0
      || !QDir::isAbsolutePath(request.workspace)
      || !QFileInfo(request.workspace).isDir()
      || !QFileInfo(QDir(request.workspace).filePath("request.json")).isFile()
      || !QDir::isAbsolutePath(request.workerExecutable)
      || !QFileInfo(request.workerExecutable).isFile()
      || !QDir::isAbsolutePath(request.gmshExecutable)
      || !QFileInfo(request.gmshExecutable).isFile())
    {
      return makeFailure(
        GmshWorkerFailureKind::InvalidRequest,
        "MESH_INPUT_INVALID",
        "initializing",
        "Worker launch paths, identity, revision, or request file are invalid");
    }

    request_ = request;
    outputBuffer_.clear();
    errorOutput_.clear();
    pendingResult_.reset();
    pendingCancellation_.reset();
    lastStage_ = "initializing";
    helloReceived_ = false;
    terminalEmitted_ = false;
    cancellationRequested_ = false;
    cancellationAcknowledged_ = false;
    forcedCancellation_ = false;
    cancellationReason_.clear();
    cancellationDeadlineUtc_.clear();
    cancellationDeadline_.stop();

    process_.setWorkingDirectory(request.workspace);
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    process_.start(request.workerExecutable,
      {"--workspace", request.workspace, "--gmsh", request.gmshExecutable},
      QIODevice::ReadWrite);
    return std::nullopt;
  }

  std::optional<GmshWorkerFailure> cancel(
    const QString& reason,
    int deadlineMilliseconds)
  {
    const QString cleanReason = reason.trimmed();
    if (process_.state() == QProcess::NotRunning
      || terminalEmitted_ || pendingResult_ || cancellationRequested_
      || lastStage_ == "completed"
      || cleanReason.isEmpty() || cleanReason.size() > 256
      || deadlineMilliseconds <= 0 || deadlineMilliseconds > 60'000)
    {
      return makeFailure(
        GmshWorkerFailureKind::InvalidRequest,
        "MESH_INPUT_INVALID",
        lastStage_,
        "Cancellation requires one active job, a bounded reason, and a valid deadline");
    }

    cancellationRequested_ = true;
    cancellationReason_ = cleanReason;
    cancellationDeadlineUtc_ = QDateTime::currentDateTimeUtc()
      .addMSecs(deadlineMilliseconds)
      .toString(Qt::ISODateWithMs);
    cancellationDeadline_.start(deadlineMilliseconds);
    if (helloReceived_)
    {
      sendCancel();
    }
    return std::nullopt;
  }

  [[nodiscard]] bool isRunning() const noexcept
  {
    return process_.state() != QProcess::NotRunning;
  }

  [[nodiscard]] bool isCancelling() const noexcept
  {
    return cancellationRequested_ && !terminalEmitted_;
  }

private:
  GmshWorkerFailure protocolFailure(QString detail) const
  {
    return makeFailure(
      GmshWorkerFailureKind::ProtocolError,
      "MESH_PROTOCOL_INCOMPATIBLE",
      lastStage_,
      std::move(detail),
      process_.exitCode());
  }

  void consumeOutput()
  {
    if (terminalEmitted_)
    {
      process_.readAllStandardOutput();
      return;
    }

    outputBuffer_ += process_.readAllStandardOutput();
    while (true)
    {
      const qsizetype delimiter = outputBuffer_.indexOf('\n');
      if (delimiter < 0)
      {
        if (outputBuffer_.size() > MaximumMessageBytes)
        {
          fail(protocolFailure("Worker control message exceeds the byte limit"));
        }
        return;
      }

      const QByteArray line = outputBuffer_.left(delimiter);
      outputBuffer_.remove(0, delimiter + 1);
      if (line.size() > MaximumMessageBytes)
      {
        fail(protocolFailure("Worker control message exceeds the byte limit"));
        return;
      }
      if (!consumeMessage(line))
      {
        return;
      }
    }
  }

  void consumeErrorOutput()
  {
    constexpr qsizetype MaximumDiagnosticBytes = 4096;
    errorOutput_ += process_.readAllStandardError();
    if (errorOutput_.size() > MaximumDiagnosticBytes)
    {
      errorOutput_ = errorOutput_.right(MaximumDiagnosticBytes);
    }
  }

  bool consumeMessage(const QByteArray& line)
  {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
      fail(protocolFailure("Worker emitted invalid JSON"));
      return false;
    }

    const QJsonObject message = document.object();
    const QJsonObject protocol = message.value("protocol").toObject();
    const QString type = message.value("type").toString();
    if (type.isEmpty()
      || protocol.value("major").toInt(-1) != ProtocolMajor
      || protocol.value("minor").toInt(-1) < ProtocolMinor
      || message.value("messageId").toString().isEmpty()
      || message.value("timestampUtc").toString().isEmpty()
      || !message.value("payload").isObject())
    {
      fail(protocolFailure("Worker message envelope is invalid"));
      return false;
    }

    if (!helloReceived_)
    {
      const QJsonObject payload = message.value("payload").toObject();
      const QJsonObject capabilities = payload.value("capabilities").toObject();
      if (type != "hello" || !message.value("jobId").toString().isEmpty()
        || payload.value("workerVersion").toString().isEmpty()
        || payload.value("buildId").toString().isEmpty()
        || payload.value("backendId").toString() != "gmsh"
        || payload.value("backendVersion").toString().isEmpty()
        || !contains(capabilities.value("dimensions").toArray(), 3)
        || !contains(capabilities.value("elementTypes").toArray(), "Triangle3")
        || !contains(capabilities.value("elementTypes").toArray(), "Tetrahedron4")
        || !contains(capabilities.value("modes").toArray(), "independent-solids"))
      {
        fail(protocolFailure("The first Worker message must be a compatible Gmsh hello"));
        return false;
      }
      helloReceived_ = true;
      sendHelloAck();
      if (!terminalEmitted_ && cancellationRequested_)
      {
        sendCancel();
      }
      return !terminalEmitted_;
    }

    if (message.value("jobId").toString() != request_.jobId)
    {
      fail(protocolFailure("Worker message job ID does not match the active job"));
      return false;
    }
    if (pendingResult_ || pendingCancellation_)
    {
      fail(protocolFailure("Worker emitted a message after its terminal result"));
      return false;
    }

    const QJsonObject payload = message.value("payload").toObject();
    if (type == "cancelAck")
    {
      if (!cancellationRequested_ || cancellationAcknowledged_
        || payload.value("reason").toString() != cancellationReason_
        || payload.value("deadlineUtc").toString() != cancellationDeadlineUtc_)
      {
        fail(protocolFailure("Worker cancellation acknowledgement is invalid"));
        return false;
      }
      cancellationAcknowledged_ = true;
      return true;
    }

    if (type == "cancelled")
    {
      const QString stage = payload.value("stage").toString();
      if (!cancellationRequested_ || !cancellationAcknowledged_
        || payload.value("reason").toString() != cancellationReason_
        || stage.isEmpty() || payload.value("forced").toBool(true))
      {
        fail(protocolFailure("Worker cancelled payload is invalid"));
        return false;
      }
      lastStage_ = stage;
      pendingCancellation_ = GmshWorkerCancellation{
        request_.jobId,
        cancellationReason_,
        stage,
        false,
      };
      return true;
    }

    if (type == "progress")
    {
      const QString stage = payload.value("stage").toString();
      const QJsonValue percentValue = payload.value("percent");
      if (stage.isEmpty()
        || (percentValue.isDouble()
          && (percentValue.toInt(-1) < 0 || percentValue.toInt(-1) > 100)))
      {
        fail(protocolFailure("Worker progress payload is invalid"));
        return false;
      }
      lastStage_ = stage;
      GmshWorkerProgress progress;
      progress.stage = stage;
      if (percentValue.isDouble())
      {
        progress.percent = percentValue.toInt();
      }
      emit owner_->progressChanged(progress);
      return true;
    }

    if (type == "error")
    {
      GmshWorkerFailure failure;
      failure.kind = GmshWorkerFailureKind::WorkerReportedError;
      failure.code = payload.value("code").toString();
      failure.stage = payload.value("stage").toString();
      failure.detail = payload.value("detail").toString();
      failure.retryable = payload.value("retryable").toBool(false);
      if (failure.code.isEmpty() || failure.stage.isEmpty())
      {
        fail(protocolFailure("Worker error payload is invalid"));
      }
      else
      {
        fail(std::move(failure));
      }
      return false;
    }

    if (type == "result")
    {
      if (cancellationRequested_)
      {
        fail(protocolFailure("Worker published a result after cancellation"));
        return false;
      }
      const qint64 revision = payload.value("sourceGeometryRevision").toInteger(-1);
      const qint64 nodes = payload.value("nodeCount").toInteger(-1);
      const qint64 triangles = payload.value("triangleCount").toInteger(-1);
      const qint64 tetrahedra = payload.value("tetrahedronCount").toInteger(-1);
      const QString resultPath = payload.value("resultPath").toString();
      const QString meshSha256 = payload.value("meshSha256").toString();
      if (revision <= 0
        || static_cast<domain::GeometryRevision>(revision)
          != request_.sourceGeometryRevision
        || nodes <= 0 || triangles <= 0 || tetrahedra <= 0
        || !isRelativeWorkspacePath(resultPath)
        || !isSha256(meshSha256))
      {
        fail(protocolFailure("Worker result payload or geometry revision is invalid"));
        return false;
      }
      pendingResult_ = GmshWorkerResult{
        static_cast<domain::GeometryRevision>(revision),
        resultPath,
        meshSha256,
        static_cast<std::uint64_t>(nodes),
        static_cast<std::uint64_t>(triangles),
        static_cast<std::uint64_t>(tetrahedra),
      };
      return true;
    }

    fail(protocolFailure(QString("Unknown Worker message type: %1").arg(type)));
    return false;
  }

  void sendHelloAck()
  {
    const QJsonObject message{
      {"type", "helloAck"},
      {"protocol", QJsonObject{{"major", ProtocolMajor}, {"minor", ProtocolMinor}}},
      {"messageId", "main-1"},
      {"jobId", request_.jobId},
      {"timestampUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {"payload", QJsonObject{
        {"workspace", QDir::cleanPath(request_.workspace)},
        {"mainBuildId", request_.mainBuildId},
      }},
    };
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (process_.write(line) != line.size())
    {
      fail(makeFailure(
        GmshWorkerFailureKind::WorkerLost,
        "MESH_WORKER_LOST",
        "initializing",
        "Cannot write helloAck to the Worker"));
    }
  }

  void sendCancel()
  {
    const QJsonObject message{
      {"type", "cancel"},
      {"protocol", QJsonObject{{"major", ProtocolMajor}, {"minor", ProtocolMinor}}},
      {"messageId", "main-cancel-1"},
      {"jobId", request_.jobId},
      {"timestampUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {"payload", QJsonObject{
        {"reason", cancellationReason_},
        {"deadlineUtc", cancellationDeadlineUtc_},
      }},
    };
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (process_.write(line) != line.size())
    {
      fail(makeFailure(
        GmshWorkerFailureKind::WorkerLost,
        "MESH_WORKER_LOST",
        lastStage_,
        "Cannot write cancel to the Worker"));
    }
  }

  void fail(GmshWorkerFailure failure)
  {
    if (terminalEmitted_)
    {
      return;
    }
    terminalEmitted_ = true;
    cancellationDeadline_.stop();
    if (process_.state() != QProcess::NotRunning)
    {
      process_.kill();
    }
    emit owner_->failed(failure);
  }

  GmshWorkerClient* owner_ = nullptr;
  QProcess process_;
  QTimer cancellationDeadline_;
  GmshWorkerLaunchRequest request_;
  QByteArray outputBuffer_;
  QByteArray errorOutput_;
  std::optional<GmshWorkerResult> pendingResult_;
  std::optional<GmshWorkerCancellation> pendingCancellation_;
  QString lastStage_;
  bool helloReceived_ = false;
  bool terminalEmitted_ = false;
  bool cancellationRequested_ = false;
  bool cancellationAcknowledged_ = false;
  bool forcedCancellation_ = false;
  QString cancellationReason_;
  QString cancellationDeadlineUtc_;
};

GmshWorkerClient::GmshWorkerClient(QObject* parent)
  : QObject(parent)
  , implementation_(std::make_unique<Implementation>(this))
{
}

GmshWorkerClient::~GmshWorkerClient() = default;

std::optional<GmshWorkerFailure> GmshWorkerClient::start(
  const GmshWorkerLaunchRequest& request)
{
  return implementation_->start(request);
}

std::optional<GmshWorkerFailure> GmshWorkerClient::cancel(
  const QString& reason,
  int deadlineMilliseconds)
{
  return implementation_->cancel(reason, deadlineMilliseconds);
}

bool GmshWorkerClient::isRunning() const noexcept
{
  return implementation_->isRunning();
}

bool GmshWorkerClient::isCancelling() const noexcept
{
  return implementation_->isCancelling();
}
}
