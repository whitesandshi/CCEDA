#include "meshing/GmshMsh41Reader.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <iostream>
#include <fstream>

namespace {

class TestWorkspace
{
public:
    TestWorkspace()
        : path_(QDir::current().filePath(
            QString("cceda-worker-test-%1").arg(QCoreApplication::applicationPid())))
        , valid_(QDir().mkpath(path_))
    {
    }

    ~TestWorkspace()
    {
        if (valid_) {
            QDir(path_).removeRecursively();
        }
    }

    [[nodiscard]] bool isValid() const { return valid_; }
    [[nodiscard]] const QString& path() const { return path_; }

private:
    QString path_;
    bool valid_ = false;
};

bool expect(bool condition, const QString& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message.toStdString() << '\n';
    }
    return condition;
}

QString sha256(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString{};
}

QJsonObject parseMessage(const QByteArray& line)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(line, &error);
    return error.error == QJsonParseError::NoError && document.isObject()
        ? document.object()
        : QJsonObject{};
}

bool writeRequest(const QString& workspace, const QString& sourceStep)
{
    QDir root(workspace);
    if (!QDir().mkpath(root.filePath("geometry"))
        || !QDir().mkpath(root.filePath("output"))
        || !QDir().mkpath(root.filePath("logs"))) {
        std::cerr << "Cannot create workspace directories under "
                  << workspace.toStdString() << '\n';
        return false;
    }
    const QString input = root.filePath("geometry/input.step");
    if (!QFile::copy(sourceStep, input)) {
        std::cerr << "Cannot copy " << sourceStep.toStdString()
                  << " to " << input.toStdString() << '\n';
        return false;
    }
    const QJsonObject request{
        {"protocol", QJsonObject{{"major", 0}, {"minor", 1}}},
        {"jobId", "worker-test-job"},
        {"sourceGeometryRevision", 17},
        {"geometry", QJsonObject{{"path", "geometry/input.step"}, {"sha256", sha256(input)}}},
        {"output", QJsonObject{{"meshPath", "output/gmsh-intermediate.msh"}, {"resultPath", "output/result.json"}}},
        {"settings", QJsonObject{{"minimumSizeMm", 0.0}, {"maximumSizeMm", 0.0}}},
    };
    QFile file(root.filePath("request.json"));
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(request).toJson(QJsonDocument::Indented)) <= 0) {
        std::cerr << "Cannot write request.json: "
                  << file.errorString().toStdString() << '\n';
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (!expect(argc == 4, "expected Worker, Gmsh, and STEP paths")) {
        return 1;
    }

    const QString workerPath = QDir::cleanPath(QString::fromLocal8Bit(argv[1]));
    const QString gmshPath = QDir::cleanPath(QString::fromLocal8Bit(argv[2]));
    const QString stepPath = QDir::cleanPath(QString::fromLocal8Bit(argv[3]));
    TestWorkspace workspace;
    if (!expect(workspace.isValid(), "temporary workspace is unavailable")
        || !expect(writeRequest(workspace.path(), stepPath), "cannot create Worker request")) {
        return 1;
    }

    const QDir root(workspace.path());
    {
        const QJsonObject incompatibleAck{
            {"type", "helloAck"},
            {"protocol", QJsonObject{{"major", 99}, {"minor", 0}}},
            {"messageId", "main-incompatible"},
            {"jobId", "worker-test-job"},
            {"timestampUtc", "2026-07-23T00:00:00.000Z"},
            {"payload", QJsonObject{
                {"workspace", QDir::cleanPath(workspace.path())},
                {"mainBuildId", "worker-test"},
            }},
        };
        const QString inputPath = root.filePath("incompatible-ack.ndjson");
        const QString outputPath = root.filePath("incompatible-output.ndjson");
        const QString errorPath = root.filePath("incompatible-error.log");
        QFile input(inputPath);
        const QByteArray inputLine =
            QJsonDocument(incompatibleAck).toJson(QJsonDocument::Compact) + '\n';
        if (!expect(input.open(QIODevice::WriteOnly)
                && input.write(inputLine) == inputLine.size(),
                    "cannot write incompatible helloAck")) {
            return 1;
        }
        input.close();

        QProcess incompatibleWorker;
        incompatibleWorker.setStandardInputFile(inputPath);
        incompatibleWorker.setStandardOutputFile(outputPath);
        incompatibleWorker.setStandardErrorFile(errorPath);
        incompatibleWorker.start(
            workerPath, {"--workspace", workspace.path(), "--gmsh", gmshPath});
        if (!expect(incompatibleWorker.waitForStarted(5000),
                    "incompatible Worker did not start")
            || !expect(incompatibleWorker.waitForFinished(10000),
                       "incompatible Worker did not finish")
            || !expect(incompatibleWorker.exitCode() != 0,
                       "incompatible protocol was accepted")) {
            return 1;
        }

        QFile messages(outputPath);
        if (!expect(messages.open(QIODevice::ReadOnly),
                    "incompatible Worker control output is missing")) {
            return 1;
        }
        const QJsonObject hello = parseMessage(messages.readLine());
        const QJsonObject error = parseMessage(messages.readLine());
        if (!expect(hello.value("type").toString() == "hello",
                    "incompatible Worker must still send hello first")
            || !expect(error.value("type").toString() == "error"
                    && error.value("payload").toObject().value("code").toString()
                        == "MESH_PROTOCOL_INCOMPATIBLE",
                       "incompatible protocol must return the stable error code")) {
            return 1;
        }
    }

    const QJsonObject ack{
        {"type", "helloAck"},
        {"protocol", QJsonObject{{"major", 0}, {"minor", 1}}},
        {"messageId", "main-1"},
        {"jobId", "worker-test-job"},
        {"timestampUtc", "2026-07-23T00:00:00.000Z"},
        {"payload", QJsonObject{{"workspace", QDir::cleanPath(workspace.path())}, {"mainBuildId", "worker-test"}}},
    };
    const QByteArray ackLine = QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n';
    const QString controlInputPath = root.filePath("hello-ack.ndjson");
    const QString controlOutputPath = root.filePath("control-output.ndjson");
    const QString controlErrorPath = root.filePath("control-error.log");
    QFile controlInput(controlInputPath);
    if (!expect(controlInput.open(QIODevice::WriteOnly)
            && controlInput.write(ackLine) == ackLine.size(),
                "cannot write helloAck control input")) {
        return 1;
    }
    controlInput.close();

    QProcess worker;
    worker.setStandardInputFile(controlInputPath);
    worker.setStandardOutputFile(controlOutputPath);
    worker.setStandardErrorFile(controlErrorPath);
    worker.start(workerPath, {"--workspace", workspace.path(), "--gmsh", gmshPath});
    if (!expect(worker.waitForStarted(5000),
                QString("Worker did not start: %1 (%2)")
                    .arg(worker.errorString(), workerPath))
        || !expect(worker.waitForFinished(30000), "Worker did not finish")) {
        worker.kill();
        return 1;
    }
    if (worker.exitStatus() != QProcess::NormalExit || worker.exitCode() != 0) {
        QFile errorFile(controlErrorPath);
        QFile outputFile(controlOutputPath);
        errorFile.open(QIODevice::ReadOnly);
        outputFile.open(QIODevice::ReadOnly);
        expect(false, QString("Worker failed: %1 %2")
            .arg(QString::fromLocal8Bit(errorFile.readAll()),
                 QString::fromUtf8(outputFile.readAll())));
        return 1;
    }

    QFile controlOutput(controlOutputPath);
    if (!expect(controlOutput.open(QIODevice::ReadOnly), "Worker control output is missing")) {
        return 1;
    }
    const QJsonObject hello = parseMessage(controlOutput.readLine());
    if (!expect(hello.value("type").toString() == "hello", "first message must be hello")
        || !expect(hello.value("payload").toObject().value("backendId").toString() == "gmsh",
                   "hello must identify Gmsh")) {
        return 1;
    }

    bool completed = false;
    bool succeeded = false;
    while (controlOutput.canReadLine()) {
        const QJsonObject message = parseMessage(controlOutput.readLine());
        completed = completed
            || (message.value("type").toString() == "progress"
                && message.value("payload").toObject().value("stage").toString() == "completed");
        succeeded = succeeded || message.value("type").toString() == "result";
    }
    if (!expect(completed && succeeded, "Worker must report completed and result")) {
        return 1;
    }

    QFile resultFile(QDir(workspace.path()).filePath("output/result.json"));
    if (!expect(resultFile.open(QIODevice::ReadOnly), "result.json is missing")) {
        return 1;
    }
    const QJsonObject result = QJsonDocument::fromJson(resultFile.readAll()).object();
    const QJsonObject counts = result.value("counts").toObject();
    const QString meshPath = root.filePath("output/gmsh-intermediate.msh");
    if (!expect(result.value("experimental").toBool(), "intermediate result must be marked experimental")
        || !expect(result.value("format").toString() == "gmsh-msh-4.1-intermediate",
                   "intermediate format is not explicit")
        || !expect(result.value("sourceGeometryRevision").toInteger() == 17,
                   "geometry revision must round-trip")
        || !expect(QFileInfo::exists(meshPath), "validated intermediate mesh is missing")
        || !expect(result.value("meshSha256").toString() == sha256(meshPath),
                   "intermediate mesh hash does not match result")
        || !expect(!QFileInfo::exists(root.filePath("output/gmsh-intermediate.tmp.msh")),
                   "temporary mesh was not atomically published")
        || !expect(counts.value("nodes").toInteger() > 0, "mesh has no nodes")
        || !expect(counts.value("triangles").toInteger() > 0, "mesh has no Triangle3")
        || !expect(counts.value("tetrahedra").toInteger() > 0, "mesh has no Tetrahedron4")) {
        return 1;
    }

    cceda::meshing::GmshMeshMapping meshMapping;
    meshMapping.regions = {
        {10, {"screw/solid-1"}},
    };
    for (int surfaceTag = 1; surfaceTag <= 10; ++surfaceTag) {
        meshMapping.entities.push_back({
            2,
            surfaceTag,
            10,
            QString("screw/solid-1/face-%1").arg(surfaceTag).toStdString(),
        });
    }
    meshMapping.entities.push_back({3, 1, 10, "screw/solid-1"});

    const QByteArray nativeMeshPath = QFile::encodeName(meshPath);
    std::ifstream meshInput(nativeMeshPath.constData(), std::ios::binary);
    const cceda::meshing::GmshMsh41ReadResult converted =
        cceda::meshing::readGmshMsh41(
            meshInput,
            {
                17,
                "2026-07-24T00:00:00.000Z",
                result.value("backendVersion").toString().toStdString(),
                cceda::domain::MeshConformity::NonConformal,
            },
            meshMapping,
            {
                4 * 1024 * 1024,
                64 * 1024,
                1'000'000,
                2'000'000,
                100'000,
            });
    if (!expect(meshInput.is_open(), "published MSH cannot be opened by the neutral reader")
        || !expect(converted.succeeded(),
            QString("published MSH cannot be converted at line %1: %2")
                .arg(converted.line)
                .arg(QString::fromStdString(converted.detail)))
        || !expect(converted.mesh->nodes.size()
                == static_cast<std::size_t>(counts.value("nodes").toInteger()),
            "neutral node count differs from Worker result")
        || !expect(converted.mesh->elements.size()
                == static_cast<std::size_t>(
                    counts.value("triangles").toInteger()
                    + counts.value("tetrahedra").toInteger()),
            "neutral retained-element count differs from Worker result")
        || !expect(cceda::domain::validateMeshModel(
                *converted.mesh,
                cceda::domain::MeshValidationMode::WorkerArtifact).valid(),
            "real Gmsh output does not satisfy the neutral Worker contract")) {
        return 1;
    }

    std::cout << "Worker process test passed: "
              << counts.value("nodes").toInteger() << " nodes, "
              << counts.value("triangles").toInteger() << " triangles, "
              << counts.value("tetrahedra").toInteger() << " tetrahedra\n";
    return 0;
}
