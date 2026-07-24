#include "meshing/GmshWorkerClient.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <cstdio>
#include <iostream>

namespace
{
class TestWorkspace
{
public:
  explicit TestWorkspace(QString suffix)
    : path_(QDir::current().filePath(
        QString("cceda-worker-client-test-%1-%2")
          .arg(QCoreApplication::applicationPid())
          .arg(suffix)))
    , valid_(QDir().mkpath(path_))
  {
  }

  ~TestWorkspace()
  {
    if (valid_)
    {
      QDir(path_).removeRecursively();
    }
  }

  [[nodiscard]] const QString& path() const noexcept { return path_; }
  [[nodiscard]] bool isValid() const noexcept { return valid_; }

private:
  QString path_;
  bool valid_ = false;
};

QString sha256(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
  {
    return {};
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString{};
}

bool writeRequest(
  const QString& workspace,
  const QString& sourceStep,
  cceda::domain::GeometryRevision requestRevision)
{
  QDir root(workspace);
  if (!QDir().mkpath(root.filePath("geometry"))
    || !QDir().mkpath(root.filePath("output"))
    || !QDir().mkpath(root.filePath("logs")))
  {
    return false;
  }

  const QString input = root.filePath("geometry/input.step");
  if (!QFile::copy(sourceStep, input))
  {
    return false;
  }

  const QJsonObject request{
    {"protocol", QJsonObject{{"major", 0}, {"minor", 1}}},
    {"jobId", "worker-client-test"},
    {"sourceGeometryRevision", static_cast<qint64>(requestRevision)},
    {"geometry", QJsonObject{{"path", "geometry/input.step"}, {"sha256", sha256(input)}}},
    {"output", QJsonObject{
      {"meshPath", "output/gmsh-intermediate.msh"},
      {"resultPath", "output/result.json"},
    }},
    {"settings", QJsonObject{{"minimumSizeMm", 0.0}, {"maximumSizeMm", 0.0}}},
  };
  QFile file(root.filePath("request.json"));
  return file.open(QIODevice::WriteOnly)
    && file.write(QJsonDocument(request).toJson(QJsonDocument::Indented)) > 0;
}

struct RunOutcome
{
  bool timedOut = false;
  std::optional<cceda::meshing::GmshWorkerResult> result;
  std::optional<cceda::meshing::GmshWorkerFailure> failure;
  QStringList stages;
};

RunOutcome runClient(const cceda::meshing::GmshWorkerLaunchRequest& request)
{
  cceda::meshing::GmshWorkerClient client;
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  RunOutcome outcome;

  QObject::connect(&client, &cceda::meshing::GmshWorkerClient::progressChanged,
    &loop, [&](const cceda::meshing::GmshWorkerProgress& progress) {
      outcome.stages.push_back(progress.stage);
    });
  QObject::connect(&client, &cceda::meshing::GmshWorkerClient::succeeded,
    &loop, [&](const cceda::meshing::GmshWorkerResult& result) {
      outcome.result = result;
      loop.quit();
    });
  QObject::connect(&client, &cceda::meshing::GmshWorkerClient::failed,
    &loop, [&](const cceda::meshing::GmshWorkerFailure& failure) {
      outcome.failure = failure;
      loop.quit();
    });
  QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
    outcome.timedOut = true;
    loop.quit();
  });

  if (const auto startFailure = client.start(request))
  {
    outcome.failure = startFailure;
    return outcome;
  }
  timeout.start(30'000);
  loop.exec();
  return outcome;
}

bool expect(bool condition, const char* message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

int runIncompatibleWorker()
{
  const QJsonObject hello{
    {"type", "hello"},
    {"protocol", QJsonObject{{"major", 99}, {"minor", 0}}},
    {"messageId", "fake-1"},
    {"jobId", ""},
    {"timestampUtc", "2026-07-24T00:00:00.000Z"},
    {"payload", QJsonObject{
      {"workerVersion", "fake"},
      {"buildId", "fake"},
      {"backendId", "gmsh"},
      {"backendVersion", "fake"},
      {"capabilities", QJsonObject{}},
    }},
  };
  const QByteArray line = QJsonDocument(hello).toJson(QJsonDocument::Compact) + '\n';
  std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
  std::fflush(stdout);
  return 0;
}
}

int main(int argc, char* argv[])
{
  QCoreApplication application(argc, argv);
  if (argc == 5 && QString::fromLocal8Bit(argv[1]) == "--workspace")
  {
    return runIncompatibleWorker();
  }
  if (!expect(argc == 4, "expected Worker, Gmsh, and STEP paths"))
  {
    return 1;
  }

  const QString workerPath = QDir::cleanPath(QString::fromLocal8Bit(argv[1]));
  const QString gmshPath = QDir::cleanPath(QString::fromLocal8Bit(argv[2]));
  const QString stepPath = QDir::cleanPath(QString::fromLocal8Bit(argv[3]));

  cceda::meshing::GmshWorkerClient invalidClient;
  const auto invalidStart = invalidClient.start({});
  if (!expect(invalidStart.has_value(), "an empty launch request was accepted")
    || !expect(invalidStart->kind
        == cceda::meshing::GmshWorkerFailureKind::InvalidRequest,
      "an invalid launch request returned the wrong failure kind")
    || !expect(!invalidClient.isRunning(), "an invalid launch request started a process"))
  {
    return 1;
  }

  TestWorkspace incompatibleWorkspace("incompatible");
  if (!expect(incompatibleWorkspace.isValid(), "incompatible workspace is unavailable")
    || !expect(writeRequest(incompatibleWorkspace.path(), stepPath, 17),
      "cannot write the incompatible request"))
  {
    return 1;
  }
  const RunOutcome incompatible = runClient({
    QCoreApplication::applicationFilePath(),
    gmshPath,
    incompatibleWorkspace.path(),
    "worker-client-test",
    17,
    "client-tests",
  });
  if (!expect(!incompatible.timedOut, "incompatible Worker run timed out")
    || !expect(!incompatible.result.has_value(), "incompatible Worker published a result")
    || !expect(incompatible.failure.has_value(), "incompatible Worker did not fail")
    || !expect(incompatible.failure->kind
        == cceda::meshing::GmshWorkerFailureKind::ProtocolError,
      "incompatible Worker returned the wrong failure kind"))
  {
    return 1;
  }

  TestWorkspace successWorkspace("success");
  if (!expect(successWorkspace.isValid(), "success workspace is unavailable")
    || !expect(writeRequest(successWorkspace.path(), stepPath, 17),
      "cannot write the success request"))
  {
    return 1;
  }

  const RunOutcome success = runClient({
    workerPath, gmshPath, successWorkspace.path(), "worker-client-test", 17, "client-tests",
  });
  if (!expect(!success.timedOut, "valid Worker run timed out")
    || !expect(success.result.has_value(), "valid Worker run did not publish a result")
    || !expect(!success.failure.has_value(), "valid Worker run reported a failure")
    || !expect(success.result->sourceGeometryRevision == 17,
      "source geometry revision did not round-trip")
    || !expect(success.result->nodeCount > 0, "result has no nodes")
    || !expect(success.stages.contains("completed"), "completed progress was not observed"))
  {
    return 1;
  }

  TestWorkspace staleWorkspace("stale");
  if (!expect(staleWorkspace.isValid(), "stale workspace is unavailable")
    || !expect(writeRequest(staleWorkspace.path(), stepPath, 17),
      "cannot write the stale request"))
  {
    return 1;
  }
  const RunOutcome stale = runClient({
    workerPath, gmshPath, staleWorkspace.path(), "worker-client-test", 18, "client-tests",
  });
  if (!expect(!stale.timedOut, "revision-mismatch Worker run timed out")
    || !expect(!stale.result.has_value(), "revision mismatch published a result")
    || !expect(stale.failure.has_value(), "revision mismatch did not fail")
    || !expect(stale.failure->kind == cceda::meshing::GmshWorkerFailureKind::ProtocolError,
      "revision mismatch must be a protocol failure"))
  {
    return 1;
  }

  std::cout << "Gmsh Worker client tests passed.\n";
  return 0;
}
