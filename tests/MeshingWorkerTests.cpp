#include "geometry/OccGeometryRepository.h"
#include "geometry/OccGeometryService.h"
#include "geometry/OccGeometrySnapshotService.h"
#include "meshing/GmshExecutableJobWorkspace.h"
#include "meshing/MeshBinaryCodec.h"
#include "meshing/MeshQualityJson.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <STEPControl_Writer.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <iostream>
#include <filesystem>
#include <memory>
#include <vector>

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

bool writeTwoSolidStep(const QString& path)
{
    BRep_Builder builder;
    TopoDS_Compound compound;
    builder.MakeCompound(compound);
    builder.Add(compound, BRepPrimAPI_MakeBox(
        gp_Pnt(0.0, 0.0, 0.0), 1.0, 1.0, 1.0).Solid());
    builder.Add(compound, BRepPrimAPI_MakeBox(
        gp_Pnt(3.0, 0.0, 0.0), 1.0, 1.0, 1.0).Solid());
    STEPControl_Writer writer;
    const QByteArray nativePath = QFile::encodeName(path);
    return writer.Transfer(compound, STEPControl_AsIs) == IFSelect_RetDone
        && writer.Write(nativePath.constData()) == IFSelect_RetDone;
}

bool writeRequest(
    const QString& workspace,
    const QString& sourceStep,
    cceda::domain::MeshingResourceLimits resourceLimits = {
        120'000, 2ULL * 1024ULL * 1024ULL * 1024ULL})
{
    const auto repository =
        std::make_shared<cceda::geometry::OccGeometryRepository>();
    cceda::geometry::OccGeometryService geometryService(repository);
    cceda::geometry::OccGeometrySnapshotService snapshotService(repository);
    const auto imported = geometryService.importStep(
        std::filesystem::path(sourceStep.toStdWString()));
    if (!imported.success) {
        std::cerr << "Cannot import Worker STEP fixture\n";
        return false;
    }

    QTemporaryDir snapshotRoot(QDir::current().filePath(
        "cceda-worker-source-snapshot-XXXXXX"));
    if (!snapshotRoot.isValid()) {
        std::cerr << "Cannot create Worker source snapshot directory\n";
        return false;
    }
    const auto snapshot = snapshotService.createSnapshot({
        imported.document.revision,
        std::filesystem::path(snapshotRoot.path().toStdWString()) / "geometry",
        32ULL * 1024ULL * 1024ULL,
    });
    if (!snapshot.succeeded()) {
        std::cerr << "Cannot create Worker geometry snapshot: "
                  << snapshot.detail << '\n';
        return false;
    }
    const auto& value = *snapshot.snapshot;
    const auto prepared = cceda::meshing::prepareGmshSnapshotJobWorkspace({
        workspace,
        QString::fromStdWString(value.geometryPath.wstring()),
        QString::fromStdWString(value.geometryMapPath.wstring()),
        QString::fromStdString(value.geometrySha256),
        QString::fromStdString(value.geometryMapSha256),
        "worker-test-job",
        value.sourceGeometryRevision,
        {0.0, 0.0, 24, true, 2,
          cceda::domain::MeshGenerationAlgorithm::Hxt},
        resourceLimits,
        32ULL * 1024ULL * 1024ULL,
    });
    if (!prepared.succeeded()) {
        std::cerr << "Cannot seal Worker geometry snapshot: "
                  << prepared.detail.toStdString() << '\n';
    }
    return prepared.succeeded();
}

bool rejectsTinyWorkingSetLimit(
    const QString& workerPath,
    const QString& gmshPath,
    const QString& sourceStep)
{
    QTemporaryDir workspace(QDir::current().filePath(
        "cceda-worker-resource-limit-XXXXXX"));
    if (!workspace.isValid()
        || !writeRequest(workspace.path(), sourceStep, {0, 1})) {
        return false;
    }

    const QJsonObject ack{
        {"type", "helloAck"},
        {"protocol", QJsonObject{{"major", 0}, {"minor", 1}}},
        {"messageId", "resource-limit-main"},
        {"jobId", "worker-test-job"},
        {"timestampUtc", "2026-07-26T00:00:00.000Z"},
        {"payload", QJsonObject{
            {"workspace", QDir::cleanPath(workspace.path())},
            {"mainBuildId", "worker-test"},
        }},
    };
    const QDir root(workspace.path());
    const QString inputPath = root.filePath("resource-limit-input.ndjson");
    const QString outputPath = root.filePath("resource-limit-output.ndjson");
    QFile input(inputPath);
    const QByteArray ackLine =
        QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n';
    if (!input.open(QIODevice::WriteOnly)
        || input.write(ackLine) != ackLine.size()) {
        return false;
    }
    input.close();

    QProcess worker;
    worker.setStandardInputFile(inputPath);
    worker.setStandardOutputFile(outputPath);
    worker.start(workerPath,
        {"--workspace", workspace.path(), "--gmsh", gmshPath});
    if (!worker.waitForStarted(5000) || !worker.waitForFinished(10000)) {
        worker.kill();
        worker.waitForFinished(5000);
        return false;
    }

    QFile output(outputPath);
    if (!output.open(QIODevice::ReadOnly)) {
        return false;
    }
    bool resourceLimitReported = false;
    while (!output.atEnd()) {
        const QJsonObject message = parseMessage(output.readLine());
        resourceLimitReported = resourceLimitReported
            || (message.value("type").toString() == "error"
                && message.value("payload").toObject().value("code").toString()
                    == "MESH_RESOURCE_LIMIT");
    }
    const bool passed = worker.exitCode() != 0 && resourceLimitReported
        && !QFileInfo::exists(root.filePath("output/result.json"))
        && !QFileInfo::exists(root.filePath("output/result.meshbin"))
        && !QFileInfo::exists(root.filePath("output/gmsh-intermediate.msh"));
    if (!passed) {
        output.seek(0);
        std::cerr << "Resource-limit Worker exit=" << worker.exitCode()
                  << " reported=" << resourceLimitReported
                  << " result="
                  << QFileInfo::exists(root.filePath("output/result.json"))
                  << " meshbin="
                  << QFileInfo::exists(root.filePath("output/result.meshbin"))
                  << " msh="
                  << QFileInfo::exists(root.filePath("output/gmsh-intermediate.msh"))
                  << " messages=" << output.readAll().toStdString() << '\n';
    }
    return passed;
}

bool multiRegionWorkerPublishesEverySolid(
    const QString& workerPath,
    const QString& gmshPath)
{
    QTemporaryDir source(QDir::current().filePath(
        "cceda-worker-multi-source-XXXXXX"));
    QTemporaryDir workspace(QDir::current().filePath(
        "cceda-worker-multi-job-XXXXXX"));
    const QString stepPath = QDir(source.path()).filePath("two-solids.step");
    if (!source.isValid() || !workspace.isValid()
        || !writeTwoSolidStep(stepPath)
        || !writeRequest(workspace.path(), stepPath)) {
        return false;
    }

    const QJsonObject ack{
        {"type", "helloAck"},
        {"protocol", QJsonObject{{"major", 0}, {"minor", 1}}},
        {"messageId", "multi-region-main"},
        {"jobId", "worker-test-job"},
        {"timestampUtc", "2026-07-29T00:00:00.000Z"},
        {"payload", QJsonObject{
            {"workspace", QDir::cleanPath(workspace.path())},
            {"mainBuildId", "worker-test"},
        }},
    };
    const QDir root(workspace.path());
    const QString inputPath = root.filePath("multi-region-input.ndjson");
    const QString outputPath = root.filePath("multi-region-output.ndjson");
    const QString errorPath = root.filePath("multi-region-error.log");
    QFile input(inputPath);
    const QByteArray ackLine =
        QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n';
    if (!input.open(QIODevice::WriteOnly)
        || input.write(ackLine) != ackLine.size()) {
        return false;
    }
    input.close();

    QProcess worker;
    worker.setStandardInputFile(inputPath);
    worker.setStandardOutputFile(outputPath);
    worker.setStandardErrorFile(errorPath);
    worker.start(workerPath,
        {"--workspace", workspace.path(), "--gmsh", gmshPath});
    if (!worker.waitForStarted(5000) || !worker.waitForFinished(30000)
        || worker.exitStatus() != QProcess::NormalExit
        || worker.exitCode() != 0) {
        worker.kill();
        worker.waitForFinished(5000);
        QFile output(outputPath);
        output.open(QIODevice::ReadOnly);
        std::cerr << "Multi-region Worker failed: "
                  << output.readAll().toStdString() << '\n';
        return false;
    }

    QFile mapResult(root.filePath("output/geometry-map-result.json"));
    if (!mapResult.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonArray mappedRegions =
        QJsonDocument::fromJson(mapResult.readAll()).object()
            .value("regions").toArray();
    QHash<quint64, QString> expectedSolids;
    for (const QJsonValue& value : mappedRegions) {
        const QJsonObject region = value.toObject();
        const auto regionId = static_cast<quint64>(
            region.value("regionId").toInteger());
        const QString solidId =
            region.value("sourceSolidGeometryId").toString();
        if (regionId == 0 || solidId.isEmpty()
            || region.value("gmshVolumeEntityTags").toArray().isEmpty()
            || expectedSolids.contains(regionId)) {
            return false;
        }
        expectedSolids.insert(regionId, solidId);
    }

    QFile meshBinary(root.filePath("output/result.meshbin"));
    if (mappedRegions.size() != 2
        || !meshBinary.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray bytes = meshBinary.readAll();
    const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.constData());
    const auto decoded = cceda::meshing::readMeshBinary(
        std::vector<std::uint8_t>(begin, begin + bytes.size()),
        cceda::domain::MeshValidationMode::WorkerArtifact);
    return decoded.succeeded()
        && decoded.mesh->regions.size() == 2
        && std::all_of(decoded.mesh->regions.begin(), decoded.mesh->regions.end(),
            [&](const cceda::domain::MeshRegion& region) {
                return region.sourceGeometryIds.size() == 1
                    && expectedSolids.value(region.id).toStdString()
                        == region.sourceGeometryIds.front();
            })
        && std::all_of(decoded.mesh->elements.begin(), decoded.mesh->elements.end(),
            [&](const cceda::domain::MeshElement& element) {
                return expectedSolids.value(element.regionId).toStdString()
                    == element.geometryId;
            });
}

bool rejectsLegacyStepRequest(
    const QString& workerPath,
    const QString& gmshPath,
    const QString& sourceStep)
{
    QTemporaryDir legacyRoot(QDir::current().filePath(
        "cceda-legacy-worker-request-XXXXXX"));
    if (!legacyRoot.isValid()) {
        return false;
    }
    const QDir root(legacyRoot.path());
    if (!QDir().mkpath(root.filePath("geometry"))
        || !QDir().mkpath(root.filePath("output"))
        || !QDir().mkpath(root.filePath("logs"))) {
        return false;
    }
    const QString legacyInput = root.filePath("geometry/input.step");
    if (!QFile::copy(sourceStep, legacyInput)) {
        return false;
    }
    const QJsonObject request{
        {"protocol", QJsonObject{{"major", 0}, {"minor", 1}}},
        {"jobId", "legacy-worker-job"},
        {"sourceGeometryRevision", 1},
        {"geometry", QJsonObject{
            {"path", "geometry/input.step"},
            {"sha256", sha256(legacyInput)},
        }},
        {"output", QJsonObject{
            {"meshPath", "output/gmsh-intermediate.msh"},
            {"resultPath", "output/result.json"},
        }},
        {"settings", QJsonObject{
            {"minimumSizeMm", 0.0},
            {"maximumSizeMm", 0.0},
        }},
    };
    QFile requestFile(root.filePath("request.json"));
    if (!requestFile.open(QIODevice::WriteOnly)
        || requestFile.write(QJsonDocument(request).toJson(
            QJsonDocument::Indented)) <= 0) {
        return false;
    }
    requestFile.close();

    const QJsonObject ack{
        {"type", "helloAck"},
        {"protocol", QJsonObject{{"major", 0}, {"minor", 1}}},
        {"messageId", "legacy-main-1"},
        {"jobId", "legacy-worker-job"},
        {"timestampUtc", "2026-07-25T00:00:00.000Z"},
        {"payload", QJsonObject{
            {"workspace", QDir::cleanPath(legacyRoot.path())},
            {"mainBuildId", "worker-test"},
        }},
    };
    const QString controlInput = root.filePath("legacy-ack.ndjson");
    const QString controlOutput = root.filePath("legacy-output.ndjson");
    QFile ackFile(controlInput);
    const QByteArray ackLine =
        QJsonDocument(ack).toJson(QJsonDocument::Compact) + '\n';
    if (!ackFile.open(QIODevice::WriteOnly)
        || ackFile.write(ackLine) != ackLine.size()) {
        return false;
    }
    ackFile.close();

    QProcess worker;
    worker.setStandardInputFile(controlInput);
    worker.setStandardOutputFile(controlOutput);
    worker.start(workerPath,
        {"--workspace", legacyRoot.path(), "--gmsh", gmshPath});
    if (!worker.waitForStarted(5000) || !worker.waitForFinished(30000)
        || worker.exitCode() == 0) {
        worker.kill();
        return false;
    }
    QFile messages(controlOutput);
    if (!messages.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonObject hello = parseMessage(messages.readLine());
    bool inputRejected = false;
    while (messages.canReadLine()) {
        const QJsonObject message = parseMessage(messages.readLine());
        inputRejected = inputRejected
            || (message.value("type").toString() == "error"
                && message.value("payload").toObject().value("code").toString()
                    == "MESH_INPUT_INVALID");
    }
    return hello.value("type").toString() == "hello" && inputRejected;
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
    if (!expect(rejectsLegacyStepRequest(workerPath, gmshPath, stepPath),
                "Worker did not reject the removed STEP request shape")) {
        return 1;
    }
    if (!expect(rejectsTinyWorkingSetLimit(workerPath, gmshPath, stepPath),
                "Worker did not enforce the configured working-set limit")) {
        return 1;
    }
    if (!expect(multiRegionWorkerPublishesEverySolid(workerPath, gmshPath),
                "Worker did not publish every independent-Solid region")) {
        return 1;
    }
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
                   "hello must identify Gmsh")
        || !expect(hello.value("payload").toObject().value("capabilities").toObject()
                       .value("optimization").toBool(),
                   "hello must advertise the implemented optimization capability")) {
        return 1;
    }

    bool completed = false;
    bool succeeded = false;
    bool progressTelemetryValid = true;
    qint64 lastElapsedMilliseconds = -1;
    qint64 maximumWorkingSetBytes = 0;
    QJsonObject successPayload;
    while (controlOutput.canReadLine()) {
        const QJsonObject message = parseMessage(controlOutput.readLine());
        const QJsonObject payload = message.value("payload").toObject();
        if (message.value("type").toString() == "progress") {
            const qint64 elapsed = payload.value("elapsedMilliseconds").toInteger(-1);
            const qint64 workingSet = payload.value("workingSetBytes").toInteger(-1);
            progressTelemetryValid = progressTelemetryValid
                && elapsed >= lastElapsedMilliseconds && workingSet > 0;
            lastElapsedMilliseconds = elapsed;
            maximumWorkingSetBytes = std::max(maximumWorkingSetBytes, workingSet);
        }
        completed = completed
            || (message.value("type").toString() == "progress"
                && payload.value("stage").toString() == "completed");
        if (message.value("type").toString() == "result") {
            succeeded = true;
            successPayload = payload;
        }
    }
    if (!expect(completed && succeeded, "Worker must report completed and result")
        || !expect(progressTelemetryValid && lastElapsedMilliseconds >= 0
                && maximumWorkingSetBytes > 0,
                   "Worker progress must expose monotonic elapsed and working-set telemetry")) {
        return 1;
    }

    QFile resultFile(QDir(workspace.path()).filePath("output/result.json"));
    if (!expect(resultFile.open(QIODevice::ReadOnly), "result.json is missing")) {
        return 1;
    }
    const QJsonObject result = QJsonDocument::fromJson(resultFile.readAll()).object();
    const QJsonObject counts = result.value("counts").toObject();
    const QJsonObject effectiveSettings =
        result.value("effectiveSettings").toObject();
    const QJsonObject effectiveResourceLimits =
        result.value("effectiveResourceLimits").toObject();
    const QJsonObject statistics = result.value("statistics").toObject();
    const QJsonObject controlStatistics =
        successPayload.value("statistics").toObject();
    const QString meshPath = root.filePath("output/gmsh-intermediate.msh");
    const QString meshBinaryPath = root.filePath("output/result.meshbin");
    if (!expect(result.value("experimental").toBool(), "intermediate result must be marked experimental")
        || !expect(result.value("format").toString() == "gmsh-msh-4.1-intermediate",
                   "intermediate format is not explicit")
        || !expect(result.value("sourceGeometryRevision").toInteger() == 1,
                   "geometry revision must round-trip")
        || !expect(effectiveSettings.value("curvatureElementsPerCircle").toInt() == 24
                && effectiveSettings.value("optimizeTetrahedra").toBool()
                && effectiveSettings.value("maximumThreads").toInt() == 2
                && effectiveSettings.value("algorithm").toString() == "hxt",
                   "result must record the effective typed advanced settings")
        || !expect(effectiveResourceLimits.value("maximumElapsedMilliseconds")
                        .toInteger() == 120'000
                && effectiveResourceLimits.value("maximumWorkingSetBytes")
                        .toInteger() == 2LL * 1024LL * 1024LL * 1024LL
                && successPayload.value("effectiveResourceLimits").toObject()
                    == effectiveResourceLimits,
                   "result must record the effective resource limits")
        || !expect(statistics.value("elapsedMilliseconds").toInteger(-1) >= 0
                && statistics.value("peakWorkingSetBytes").toInteger(-1)
                    >= maximumWorkingSetBytes
                && controlStatistics == statistics,
                   "manifest and control result must agree on terminal resource telemetry")
        || !expect(QFileInfo::exists(meshPath), "validated intermediate mesh is missing")
        || !expect(result.value("meshSha256").toString() == sha256(meshPath),
                   "intermediate mesh hash does not match result")
        || !expect(QFileInfo::exists(meshBinaryPath), "neutral mesh result is missing")
        || !expect(result.value("meshBinaryPath").toString() == "output/result.meshbin",
                   "neutral mesh path does not match the workspace contract")
        || !expect(result.value("meshBinarySha256").toString() == sha256(meshBinaryPath),
                   "neutral mesh hash does not match result")
        || !expect(successPayload.value("meshBinarySha256").toString()
                       == result.value("meshBinarySha256").toString(),
                   "control result and manifest neutral mesh hashes differ")
        || !expect(QFileInfo::exists(root.filePath("output/quality.json")),
                   "quality result is missing")
        || !expect(result.value("qualityPath").toString()
                       == "output/quality.json",
                   "quality path does not match the workspace contract")
        || !expect(result.value("qualitySha256").toString()
                       == sha256(root.filePath("output/quality.json")),
                   "quality hash does not match result")
        || !expect(successPayload.value("qualitySha256").toString()
                       == result.value("qualitySha256").toString(),
                   "control result and manifest quality hashes differ")
        || !expect(!QFileInfo::exists(root.filePath("output/gmsh-intermediate.tmp.msh")),
                   "temporary mesh was not atomically published")
        || !expect(counts.value("nodes").toInteger() > 0, "mesh has no nodes")
        || !expect(counts.value("triangles").toInteger() > 0, "mesh has no Triangle3")
        || !expect(counts.value("tetrahedra").toInteger() > 0, "mesh has no Tetrahedron4")) {
        return 1;
    }

    QFile qualityFile(root.filePath("output/quality.json"));
    if (!expect(qualityFile.open(QIODevice::ReadOnly),
                "quality result cannot be opened")) {
        return 1;
    }
    const auto quality = cceda::meshing::readMeshQualityJson(qualityFile);
    if (!expect(quality.succeeded(), "quality result is not valid protocol JSON")
        || !expect(quality.artifact->jobId == "worker-test-job",
                   "quality result job identity does not match")) {
        return 1;
    }

    const QString mapResultPath =
        root.filePath("output/geometry-map-result.json");
    QFile mapResultFile(mapResultPath);
    if (!expect(mapResultFile.open(QIODevice::ReadOnly),
                "geometry-map-result.json is missing")) {
        return 1;
    }
    const QJsonObject mapResult =
        QJsonDocument::fromJson(mapResultFile.readAll()).object();
    const QJsonArray mappedRegions = mapResult.value("regions").toArray();
    if (!expect(result.value("geometryMapResultSha256").toString()
                    == sha256(mapResultPath),
                "geometry-map result hash does not match result.json")
        || !expect(successPayload.value("geometryMapResultSha256").toString()
                        == result.value("geometryMapResultSha256").toString(),
                   "control result and manifest map hashes differ")
        || !expect(mappedRegions.size() == 1,
                   "single-region snapshot must return one mapped region")
        || !expect(!mappedRegions.first().toObject()
                        .value("sourceSolidGeometryId").toString().isEmpty(),
                   "mapped region must retain its source Solid identity")
        || !expect(!mappedRegions.first().toObject()
                        .value("gmshVolumeEntityTags").toArray().isEmpty(),
                   "mapped region must expose actual Gmsh volume entity tags")) {
        return 1;
    }

    const QJsonObject mappedRegion = mappedRegions.first().toObject();
    const auto mappedRegionId = static_cast<cceda::domain::MeshRegionId>(
        mappedRegion.value("regionId").toInteger());
    const std::string mappedSolidId =
        mappedRegion.value("sourceSolidGeometryId").toString().toStdString();

    QFile meshBinaryFile(meshBinaryPath);
    if (!expect(meshBinaryFile.open(QIODevice::ReadOnly),
                "neutral mesh result cannot be opened")) {
        return 1;
    }
    const QByteArray meshBinaryBytes = meshBinaryFile.readAll();
    if (!expect(!meshBinaryBytes.isEmpty(), "neutral mesh result is empty")) {
        return 1;
    }
    const auto* meshBinaryBegin = reinterpret_cast<const std::uint8_t*>(
        meshBinaryBytes.constData());
    const cceda::meshing::MeshBinaryReadResult converted =
        cceda::meshing::readMeshBinary(
            std::vector<std::uint8_t>(
                meshBinaryBegin,
                meshBinaryBegin + meshBinaryBytes.size()),
            cceda::domain::MeshValidationMode::WorkerArtifact);
    if (!expect(converted.succeeded(),
            QString("neutral mesh result cannot be decoded: %1")
                .arg(QString::fromStdString(converted.detail)))
        || !expect(converted.mesh->meshRevision == 0,
            "Worker neutral mesh revision must be zero")
        || !expect(converted.mesh->sourceGeometryRevision == 1,
            "neutral mesh geometry revision must round-trip")
        || !expect(converted.mesh->nodes.size()
                == static_cast<std::size_t>(counts.value("nodes").toInteger()),
            "neutral node count differs from Worker result")
        || !expect(converted.mesh->elements.size()
                == static_cast<std::size_t>(
                    counts.value("triangles").toInteger()
                    + counts.value("tetrahedra").toInteger()),
            "neutral retained-element count differs from Worker result")
        || !expect(converted.mesh->regions.size() == 1
                && converted.mesh->regions.front().id == mappedRegionId
                && converted.mesh->regions.front().sourceGeometryIds
                    == std::vector<std::string>{mappedSolidId},
            "neutral region does not preserve its source Solid mapping")
        || !expect(std::all_of(
                converted.mesh->elements.begin(),
                converted.mesh->elements.end(),
                [&](const cceda::domain::MeshElement& element) {
                    return element.regionId == mappedRegionId
                        && element.geometryId == mappedSolidId;
                }),
            "neutral elements are not associated with the mapped source Solid")) {
        return 1;
    }

    std::cout << "Worker process test passed: "
              << counts.value("nodes").toInteger() << " nodes, "
              << counts.value("triangles").toInteger() << " triangles, "
              << counts.value("tetrahedra").toInteger() << " tetrahedra\n";
    return 0;
}
