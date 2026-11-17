#include "persistence/ProjectResourceVerification.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QMap>
#include <QFile>
#include <QTemporaryDir>
#include <algorithm>
#include <cstring>
#include <iostream>

namespace
{
using namespace cceda::persistence;
int failures = 0;
void expect(bool condition, const char* message)
{
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

struct Fixture
{
  QMap<QString, QByteArray> resources{
    {"project.json", "project metadata"},
    {"geometry/healing-history.json", "[]"},
    {"mesh/settings.json", "settings"},
  };
  ProjectManifest manifest;
  int opens = 0;

  Fixture()
  {
    manifest.projectId = "01234567-89ab-cdef-0123-456789abcdef";
    manifest.createdBy = "CCEDA/0.2.0+test";
    manifest.createdAt = "2026-11-18T00:00:00Z";
    manifest.modifiedAt = manifest.createdAt;
    manifest.occtVersion = "8.0.0";
    refresh();
  }

  void refresh()
  {
    manifest.entries.clear();
    for (auto it = resources.cbegin(); it != resources.cend(); ++it)
    {
      manifest.entries.push_back({it.key(), it.key().startsWith("logs/") ? "log" : "resource",
        static_cast<std::uint64_t>(it.value().size()),
        QString::fromLatin1(QCryptographicHash::hash(it.value(), QCryptographicHash::Sha256).toHex())});
    }
  }

  ProjectResourceOpenResult open(const QString& path)
  {
    ++opens;
    if (!resources.contains(path)) { return {{}, ProjectResourceOpenError::Missing}; }
    auto buffer = std::make_unique<QBuffer>();
    buffer->setData(resources.value(path));
    buffer->open(QIODevice::ReadOnly);
    return {std::move(buffer), ProjectResourceOpenError::None};
  }

  ProjectResourceVerificationResult verify(const ProjectResourceVerificationOptions& options = {})
  {
    return verifyProjectResources(manifest, [this](const QString& path) { return open(path); }, options);
  }
};

// Sequential source with deliberately short reads, optional failure/stall,
// and an observed bound on the verifier's read requests.
class ChunkStream final : public QIODevice
{
public:
  ChunkStream(QByteArray bytes, bool fail, bool stall, qint64& largestRead)
    : bytes_(std::move(bytes)), fail_(fail), stall_(stall), largestRead_(largestRead)
  { open(QIODevice::ReadOnly | QIODevice::Unbuffered); }
  bool isSequential() const override { return true; }
  bool atEnd() const override { return offset_ == bytes_.size(); }
protected:
  qint64 readData(char* data, qint64 maximum) override
  {
    largestRead_ = std::max(largestRead_, maximum);
    if (fail_ && offset_ > 0) { return -1; }
    if (stall_) { return 0; }
    const auto count = std::min({maximum, qint64(997), qint64(bytes_.size()) - offset_});
    std::memcpy(data, bytes_.constData() + offset_, static_cast<std::size_t>(count));
    offset_ += count;
    return count;
  }
  qint64 writeData(const char*, qint64) override { return -1; }
private:
  QByteArray bytes_;
  bool fail_;
  bool stall_;
  qint64& largestRead_;
  qint64 offset_ = 0;
};
}

int main()
{
  using Error = ProjectResourceVerificationError;
  Fixture good;
  expect(good.verify().succeeded(), "valid declared resources verify");
  expect(good.opens == 3, "each declared resource opens once; manifest is not hashed");

  Fixture incompatible;
  incompatible.manifest.requiredFeatures = {"future-feature/1"};
  const auto unsupported = incompatible.verify();
  expect(unsupported.error == Error::UnsupportedFeature && unsupported.subject == "future-feature/1"
    && incompatible.opens == 0, "unsupported features fail before I/O");
  ProjectResourceVerificationOptions supported;
  supported.supportedFeatures = {"future-feature/1"};
  expect(incompatible.verify(supported).succeeded(), "explicitly supported features pass");

  for (const auto* required : {"project.json", "geometry/healing-history.json", "mesh/settings.json"})
  {
    Fixture missing;
    missing.resources.remove(required);
    missing.refresh();
    expect(missing.verify().error == Error::MissingDeclaration && missing.opens == 0,
      "missing required declaration fails before I/O");
  }
  for (const auto* member : {"source/original.step", "geometry/model.xbf", "mesh/current.meshbin", "mesh/quality.json"})
  {
    Fixture incomplete;
    incomplete.resources.insert(member, "data");
    incomplete.refresh();
    expect(incomplete.verify().error == Error::IncompleteResourcePair && incomplete.opens == 0,
      "geometry and mesh resource pairs must be complete");
  }
  Fixture pairs;
  for (const auto* member : {"source/original.step", "geometry/model.xbf", "mesh/current.meshbin", "mesh/quality.json"})
  { pairs.resources.insert(member, "data"); }
  pairs.refresh();
  expect(pairs.verify().succeeded(), "complete imported geometry and mesh pairs pass integrity checks");
  pairs.resources.remove("source/original.step");
  pairs.resources.remove("geometry/model.xbf");
  pairs.refresh();
  pairs.opens = 0;
  expect(pairs.verify().error == Error::MissingDeclaration && pairs.opens == 0,
    "stored mesh requires its imported geometry resources");

  Fixture budget;
  ProjectResourceVerificationOptions limit;
  limit.maximumTotalBytes = 0;
  for (const auto& entry : budget.manifest.entries) { limit.maximumTotalBytes += entry.uncompressedBytes; }
  expect(budget.verify(limit).succeeded(), "exact total budget passes");
  budget.opens = 0;
  --limit.maximumTotalBytes;
  expect(budget.verify(limit).error == Error::BudgetExceeded && budget.opens == 0,
    "total declared size is checked before opening resources");

  Fixture invalid;
  invalid.manifest.entries[0].path = "../escape";
  expect(invalid.verify().error == Error::InvalidManifest && invalid.opens == 0,
    "in-memory manifests are validated before I/O");

  Fixture absent;
  absent.resources.remove("project.json");
  const auto missing = absent.verify();
  expect(missing.error == Error::MissingResource && missing.subject == "project.json",
    "missing declared required resource identifies its path");
  for (auto& entry : absent.manifest.entries) { entry.role = "log"; }
  expect(absent.verify().error == Error::MissingResource, "log role cannot waive required resource presence");
  Fixture tampered;
  tampered.resources["project.json"][0] = 'X';
  expect(tampered.verify().error == Error::HashMismatch, "same-size tampering fails digest verification");
  Fixture truncated;
  truncated.resources["project.json"].chop(1);
  expect(truncated.verify().error == Error::SizeMismatch, "truncated content is rejected");
  Fixture oversized;
  oversized.resources["project.json"].append('X');
  expect(oversized.verify().error == Error::SizeMismatch, "undeclared extra bytes are rejected");

  Fixture logs;
  logs.resources.insert("logs/geometry.log", "log data");
  logs.refresh();
  logs.resources.remove("logs/geometry.log");
  const auto optional = logs.verify();
  expect(optional.succeeded() && optional.missingOptionalLogs == QStringList{"logs/geometry.log"},
    "missing optional logs return a success warning");
  logs.resources.insert("logs/geometry.log", "bad data");
  expect(logs.verify().error == Error::HashMismatch, "present corrupted logs fail integrity verification");
  logs.resources.remove("logs/geometry.log");
  logs.resources["project.json"][0] = 'X';
  expect(logs.verify().missingOptionalLogs.isEmpty(), "failure does not return a partial success report");

  Fixture streamed;
  streamed.resources.insert("source/original.step", "STEP");
  streamed.resources.insert("geometry/model.xbf", "XCAF");
  streamed.resources.insert("mesh/current.meshbin", QByteArray(3 * 65536 + 17, 'm'));
  streamed.resources.insert("mesh/quality.json", "quality");
  streamed.resources.insert("logs/empty.log", QByteArray{});
  streamed.refresh();
  qint64 largestRead = 0;
  const auto chunked = [&](bool fail, bool stall) {
    return verifyProjectResources(streamed.manifest, [&](const QString& path) -> ProjectResourceOpenResult {
      return {std::make_unique<ChunkStream>(streamed.resources.value(path), fail, stall, largestRead),
        ProjectResourceOpenError::None};
    });
  };
  expect(chunked(false, false).succeeded(), "short sequential reads and empty resources verify");
  expect(largestRead <= 65536, "read buffer remains bounded independently of resource size");
  expect(chunked(true, false).error == Error::ReadFailed, "mid-stream I/O errors are rejected");
  expect(chunked(false, true).error == Error::ReadFailed, "zero read before EOF fails without spinning");
  expect(verifyProjectResources(good.manifest, {}).error == Error::ReadFailed, "missing provider fails closed");
  expect(verifyProjectResources(good.manifest, [](const QString&) {
    return ProjectResourceOpenResult{};
  }).error == Error::ReadFailed, "null successful source is rejected");
  for (int mode = 0; mode < 3; ++mode)
  {
    const auto result = verifyProjectResources(good.manifest, [&](const QString& path) {
      auto buffer = std::make_unique<QBuffer>();
      buffer->setData(good.resources.value(path));
      if (mode == 1) { buffer->open(QIODevice::ReadOnly | QIODevice::Text); }
      if (mode == 2) { buffer->open(QIODevice::ReadOnly); buffer->seek(1); }
      return ProjectResourceOpenResult{std::move(buffer), ProjectResourceOpenError::None};
    });
    expect(result.error == Error::ReadFailed, "closed, text, and consumed devices are rejected");
  }
  Fixture unreadableLog;
  unreadableLog.resources.insert("logs/geometry.log", "log data");
  unreadableLog.refresh();
  expect(verifyProjectResources(unreadableLog.manifest, [&](const QString& path) {
    if (path.startsWith("logs/"))
    { return ProjectResourceOpenResult{{}, ProjectResourceOpenError::ReadFailed}; }
    return unreadableLog.open(path);
  }).error == Error::ReadFailed, "optional log I/O failure is not mistaken for absence");

  QTemporaryDir directory;
  expect(directory.isValid(), "temporary directory is available");
  QMap<QString, QString> files;
  for (auto it = streamed.resources.cbegin(); it != streamed.resources.cend(); ++it)
  {
    const auto filename = directory.filePath(QString::number(files.size()));
    QFile file(filename);
    expect(file.open(QIODevice::WriteOnly) && file.write(it.value()) == it.value().size(),
      "real resource file is written");
    files.insert(it.key(), filename);
  }
  const auto disk = verifyProjectResources(streamed.manifest, [&](const QString& path) {
    auto file = std::make_unique<QFile>(files.value(path));
    const bool opened = file->open(QIODevice::ReadOnly);
    return ProjectResourceOpenResult{std::move(file),
      opened ? ProjectResourceOpenError::None : ProjectResourceOpenError::ReadFailed};
  });
  expect(disk.succeeded(), "real binary file streams verify across multiple buffer boundaries");
  return failures == 0 ? 0 : 1;
}
