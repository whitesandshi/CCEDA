#include "geometry/OccGeometryRepository.h"
#include "geometry/OccGeometryService.h"
#include "geometry/OccGeometrySnapshotService.h"
#include "meshing/GmshExecutableJobWorkspace.h"
#include "meshing/GmshWorkerClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QTimer>
#include <QTemporaryDir>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>

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

bool writeRequest(
  const QString& workspace,
  const QString& sourceStep,
  cceda::domain::GeometryRevision requestRevision)
{
  const auto repository =
    std::make_shared<cceda::geometry::OccGeometryRepository>();
  cceda::geometry::OccGeometryService geometryService(repository);
  cceda::geometry::OccGeometrySnapshotService snapshotService(repository);
  const auto imported = geometryService.importStep(
    std::filesystem::path(sourceStep.toStdWString()));
  QTemporaryDir snapshotRoot(QDir::current().filePath(
    "cceda-worker-client-source-XXXXXX"));
  if (!imported.success || !snapshotRoot.isValid()
    || imported.document.revision != requestRevision)
  {
    std::cerr << "Cannot create matching Worker client source fixture\n";
    return false;
  }
  const auto snapshot = snapshotService.createSnapshot({
    imported.document.revision,
    std::filesystem::path(snapshotRoot.path().toStdWString()) / "geometry",
    4 * 1024 * 1024,
  });
  if (!snapshot.succeeded())
  {
    std::cerr << "Cannot create Worker client snapshot: " << snapshot.detail << '\n';
    return false;
  }
  const auto& value = *snapshot.snapshot;
  const cceda::meshing::GmshJobWorkspaceResult result =
    cceda::meshing::prepareGmshSnapshotJobWorkspace({
      workspace,
      QString::fromStdWString(value.geometryPath.wstring()),
      QString::fromStdWString(value.geometryMapPath.wstring()),
      QString::fromStdString(value.geometrySha256),
      QString::fromStdString(value.geometryMapSha256),
      "worker-client-test",
      requestRevision,
      0.0,
      0.0,
      4 * 1024 * 1024,
    });
  if (!result.succeeded())
  {
    std::cerr << "Cannot prepare Worker request: "
              << result.detail.toStdString() << '\n';
  }
  return result.succeeded();
}

struct RunOutcome
{
  bool timedOut = false;
  std::optional<cceda::meshing::GmshWorkerResult> result;
  std::optional<cceda::meshing::GmshWorkerFailure> failure;
  std::optional<cceda::meshing::GmshWorkerCancellation> cancellation;
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
  QObject::connect(&client, &cceda::meshing::GmshWorkerClient::cancelled,
    &loop, [&](const cceda::meshing::GmshWorkerCancellation& cancellation) {
      outcome.cancellation = cancellation;
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

int runCancellationIgnoringWorker()
{
  const QJsonObject hello{
    {"type", "hello"},
    {"protocol", QJsonObject{{"major", 0}, {"minor", 1}}},
    {"messageId", "fake-1"},
    {"jobId", ""},
    {"timestampUtc", "2026-07-24T00:00:00.000Z"},
    {"payload", QJsonObject{
      {"workerVersion", "fake"},
      {"buildId", "fake"},
      {"backendId", "gmsh"},
      {"backendVersion", "fake"},
      {"capabilities", QJsonObject{
        {"dimensions", QJsonArray{3}},
        {"elementTypes", QJsonArray{"Triangle3", "Tetrahedron4"}},
        {"modes", QJsonArray{"independent-solids"}},
      }},
    }},
  };
  const QByteArray line = QJsonDocument(hello).toJson(QJsonDocument::Compact) + '\n';
  std::fwrite(line.constData(), 1, static_cast<std::size_t>(line.size()), stdout);
  std::fflush(stdout);
  QThread::msleep(10'000);
  return 0;
}

RunOutcome runForcedCancellation(
  const cceda::meshing::GmshWorkerLaunchRequest& request)
{
  cceda::meshing::GmshWorkerClient client;
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  RunOutcome outcome;

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
  QObject::connect(&client, &cceda::meshing::GmshWorkerClient::cancelled,
    &loop, [&](const cceda::meshing::GmshWorkerCancellation& cancellation) {
      outcome.cancellation = cancellation;
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
  QTimer::singleShot(50, &loop, [&] {
    const auto cancelFailure = client.cancel("test cancellation", 100);
    if (cancelFailure)
    {
      outcome.failure = cancelFailure;
      loop.quit();
    }
  });
  timeout.start(5'000);
  loop.exec();
  return outcome;
}

RunOutcome runCooperativeCancellation(
  const cceda::meshing::GmshWorkerLaunchRequest& request)
{
  cceda::meshing::GmshWorkerClient client;
  QEventLoop loop;
  QTimer timeout;
  timeout.setSingleShot(true);
  RunOutcome outcome;
  bool cancellationRequested = false;

  QObject::connect(&client, &cceda::meshing::GmshWorkerClient::progressChanged,
    &loop, [&](const cceda::meshing::GmshWorkerProgress& progress) {
      outcome.stages.push_back(progress.stage);
      if (!cancellationRequested && progress.stage == "generating-volume")
      {
        cancellationRequested = true;
        if (const auto cancelFailure = client.cancel("cooperative test", 2'000))
        {
          outcome.failure = cancelFailure;
          loop.quit();
        }
      }
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
  QObject::connect(&client, &cceda::meshing::GmshWorkerClient::cancelled,
    &loop, [&](const cceda::meshing::GmshWorkerCancellation& cancellation) {
      outcome.cancellation = cancellation;
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
  timeout.start(5'000);
  loop.exec();
  return outcome;
}
}

int main(int argc, char* argv[])
{
  QCoreApplication application(argc, argv);
  if (argc == 2
    && (QString::fromLocal8Bit(argv[1]) == "-version"
      || QString::fromLocal8Bit(argv[1]) == "--version"))
  {
    std::fputs("4.13.1\n", stdout);
    return 0;
  }
  if (application.arguments().contains("-3"))
  {
    QThread::msleep(10'000);
    return 1;
  }
  if (argc == 5 && QString::fromLocal8Bit(argv[1]) == "--workspace")
  {
    if (QString::fromLocal8Bit(argv[2]).contains("force-cancel"))
    {
      return runCancellationIgnoringWorker();
    }
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
    || !expect(writeRequest(incompatibleWorkspace.path(), stepPath, 1),
      "cannot write the incompatible request"))
  {
    return 1;
  }
  const RunOutcome incompatible = runClient({
    QCoreApplication::applicationFilePath(),
    gmshPath,
    incompatibleWorkspace.path(),
    "worker-client-test",
    1,
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
    || !expect(writeRequest(successWorkspace.path(), stepPath, 1),
      "cannot write the success request"))
  {
    return 1;
  }

  const RunOutcome success = runClient({
    workerPath, gmshPath, successWorkspace.path(), "worker-client-test", 1, "client-tests",
  });
  if (!expect(!success.timedOut, "valid Worker run timed out")
    || !expect(success.result.has_value(), "valid Worker run did not publish a result")
    || !expect(!success.failure.has_value(), "valid Worker run reported a failure")
    || !expect(success.result->sourceGeometryRevision == 1,
      "source geometry revision did not round-trip")
    || !expect(success.result->geometryMapResultSha256.size() == 64,
      "result does not expose the geometry-map digest")
    || !expect(success.result->nodeCount > 0, "result has no nodes")
    || !expect(success.stages.contains("completed"), "completed progress was not observed"))
  {
    return 1;
  }

  TestWorkspace staleWorkspace("stale");
  if (!expect(staleWorkspace.isValid(), "stale workspace is unavailable")
    || !expect(writeRequest(staleWorkspace.path(), stepPath, 1),
      "cannot write the stale request"))
  {
    return 1;
  }
  const RunOutcome stale = runClient({
    workerPath, gmshPath, staleWorkspace.path(), "worker-client-test", 2, "client-tests",
  });
  if (!expect(!stale.timedOut, "revision-mismatch Worker run timed out")
    || !expect(!stale.result.has_value(), "revision mismatch published a result")
    || !expect(stale.failure.has_value(), "revision mismatch did not fail")
    || !expect(stale.failure->kind == cceda::meshing::GmshWorkerFailureKind::ProtocolError,
      "revision mismatch must be a protocol failure"))
  {
    return 1;
  }

  TestWorkspace forceCancelWorkspace("force-cancel");
  if (!expect(forceCancelWorkspace.isValid(), "force-cancel workspace is unavailable")
    || !expect(writeRequest(forceCancelWorkspace.path(), stepPath, 1),
      "cannot write the force-cancel request"))
  {
    return 1;
  }
  const RunOutcome forceCancelled = runForcedCancellation({
    QCoreApplication::applicationFilePath(),
    gmshPath,
    forceCancelWorkspace.path(),
    "worker-client-test",
    1,
    "client-tests",
  });
  if (!expect(!forceCancelled.timedOut, "forced cancellation timed out")
    || !expect(!forceCancelled.result.has_value(), "forced cancellation published a result")
    || !expect(!forceCancelled.failure.has_value(), "forced cancellation reported failure")
    || !expect(forceCancelled.cancellation.has_value(),
      "forced cancellation did not publish its terminal state")
    || !expect(forceCancelled.cancellation->forced,
      "deadline cancellation should be marked forced")
    || !expect(forceCancelled.cancellation->reason == "test cancellation",
      "cancellation reason did not round-trip"))
  {
    return 1;
  }

  TestWorkspace cooperativeWorkspace("cooperative-cancel");
  if (!expect(cooperativeWorkspace.isValid(),
      "cooperative-cancel workspace is unavailable")
    || !expect(writeRequest(cooperativeWorkspace.path(), stepPath, 1),
      "cannot write the cooperative-cancel request"))
  {
    return 1;
  }
  const RunOutcome cooperativeCancelled = runCooperativeCancellation({
    workerPath,
    QCoreApplication::applicationFilePath(),
    cooperativeWorkspace.path(),
    "worker-client-test",
    1,
    "client-tests",
  });
  if (cooperativeCancelled.failure)
  {
    std::cerr << "Cooperative cancellation failure: "
              << cooperativeCancelled.failure->code.toStdString() << " at "
              << cooperativeCancelled.failure->stage.toStdString() << ": "
              << cooperativeCancelled.failure->detail.toStdString() << '\n';
  }
  if (!expect(!cooperativeCancelled.timedOut, "cooperative cancellation timed out")
    || !expect(!cooperativeCancelled.result.has_value(),
      "cooperative cancellation published a result")
    || !expect(!cooperativeCancelled.failure.has_value(),
      "cooperative cancellation reported failure")
    || !expect(cooperativeCancelled.cancellation.has_value(),
      "cooperative cancellation did not publish its terminal state")
    || !expect(!cooperativeCancelled.cancellation->forced,
      "responsive Worker cancellation should not be forced")
    || !expect(!QFileInfo(QDir(cooperativeWorkspace.path()).filePath(
      "output/gmsh-intermediate.msh")).exists(),
      "cancelled job retained a formal mesh output")
    || !expect(!QFileInfo(QDir(cooperativeWorkspace.path()).filePath(
      "output/result.json")).exists(),
      "cancelled job retained a result manifest"))
  {
    return 1;
  }

  std::cout << "Gmsh Worker client tests passed.\n";
  return 0;
}
