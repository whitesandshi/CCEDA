#include "persistence/ProjectZipArchive.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QTemporaryDir>
#include <miniz.h>
#include <QtEndian>
#include <iostream>
#include <Windows.h>
#include <winioctl.h>
#include <io.h>

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
    {"logs/empty.log", {}},
    {QString::fromUtf8("logs/记录.log"), QByteArray(300000, 'x')},
  };
  ProjectManifest manifest;
  Fixture()
  {
    manifest.projectId = "01234567-89ab-cdef-0123-456789abcdef";
    manifest.createdBy = "CCEDA/0.2.0+test";
    manifest.createdAt = "2026-11-18T00:00:00Z";
    manifest.modifiedAt = manifest.createdAt;
    manifest.occtVersion = "8.0.0";
    for (auto it = resources.cbegin(); it != resources.cend(); ++it)
    {
      manifest.entries.push_back({it.key(), it.key().startsWith("logs/") ? "log" : "resource",
        static_cast<std::uint64_t>(it.value().size()),
        QString::fromLatin1(QCryptographicHash::hash(it.value(), QCryptographicHash::Sha256).toHex())});
    }
  }
  ProjectResourceOpenResult open(const QString& path) const
  {
    if (!resources.contains(path)) { return {{}, ProjectResourceOpenError::Missing}; }
    auto buffer = std::make_unique<QBuffer>();
    buffer->setData(resources.value(path));
    buffer->open(QIODevice::ReadOnly);
    return {std::move(buffer), ProjectResourceOpenError::None};
  }
  ProjectZipResult write(const QString& path, const ProjectZipOptions& options = {}) const
  { return writeNewProjectZip(path, manifest, [this](const QString& name) { return open(name); }, options); }
};

using Entries = QVector<QPair<QString, QByteArray>>;
Entries entries(const Fixture& fixture)
{
  Entries result;
  for (auto it = fixture.resources.cbegin(); it != fixture.resources.cend(); ++it)
  { result.push_back({it.key(), it.value()}); }
  result.push_back({"manifest.json", writeProjectManifestJson(fixture.manifest).bytes});
  return result;
}
QByteArray rawZip(const Entries& data, bool zip64 = false)
{
  mz_zip_archive zip{};
  expect(mz_zip_writer_init_heap_v2(&zip, 0, 0, zip64 ? MZ_ZIP_FLAG_WRITE_ZIP64 : 0), "raw ZIP initializes");
  for (const auto& entry : data)
  {
    const auto name = entry.first.toUtf8();
    expect(mz_zip_writer_add_mem(&zip, name.constData(), entry.second.constData(), entry.second.size(), 0),
      "raw stored entry is added");
  }
  void* memory = nullptr;
  size_t size = 0;
  expect(mz_zip_writer_finalize_heap_archive(&zip, &memory, &size), "raw ZIP finalizes");
  const QByteArray bytes(static_cast<const char*>(memory), static_cast<qsizetype>(size));
  mz_zip_writer_end(&zip);
  mz_free(memory);
  return bytes;
}
bool put(const QString& path, const QByteArray& bytes)
{
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray get(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) { return {}; }
  return file.readAll();
}
size_t writeAt(void* opaque, mz_uint64 offset, const void* bytes, size_t size)
{
  auto& file = *static_cast<QFile*>(opaque);
  if (!file.seek(offset)) { return 0; }
  const auto written = file.write(static_cast<const char*>(bytes), size);
  return written < 0 ? 0 : static_cast<size_t>(written);
}
}

int main(int argc, char** argv)
{
  Fixture fixture;
  if (argc == 3 && QByteArray(argv[1]) == "--write")
  { return fixture.write(QString::fromLocal8Bit(argv[2])).succeeded() ? 0 : 1; }
  if (argc == 3 && QByteArray(argv[1]) == "--read")
  { return ProjectZipReader::open(QString::fromLocal8Bit(argv[2])).succeeded() ? 0 : 1; }
  QTemporaryDir temporary;
  expect(temporary.isValid(), "temporary directory is available");
  const auto path = temporary.filePath(QString::fromUtf8("项目.cceda"));
  const auto written = fixture.write(path);
  expect(written.succeeded(), "new ZIP64 package writes and reopens");
  if (!written.succeeded())
  { std::cerr << "write error " << int(written.error) << ": " << written.subject.toStdString() << '\n'; }
  auto opened = ProjectZipReader::open(path);
  expect(opened.succeeded(), "written package verifies");
  if (!opened.succeeded()) { std::cerr << "open error " << int(opened.status.error) << '\n'; return 1; }
  expect(writeProjectManifestJson(opened.archive->manifest()).bytes == writeProjectManifestJson(fixture.manifest).bytes,
    "manifest roundtrips exactly");
  for (auto it = fixture.resources.cbegin(); it != fixture.resources.cend(); ++it)
  {
    auto source = opened.archive->openResource(it.key());
    expect(source.device && source.device->readAll() == it.value(), "binary and Unicode resource bytes roundtrip");
  }
  expect(opened.archive->openResource("../escape").error == ProjectResourceOpenError::Missing,
    "lookup never resolves arbitrary filesystem paths");
  QFile denied(path);
  expect(!denied.open(QIODevice::WriteOnly), "reader denies archive writes while streams are active");
  auto retained = opened.archive->openResource("project.json");
  opened.archive.reset();
  expect(!denied.open(QIODevice::WriteOnly), "retained stream keeps the archive locked");
  expect(retained.device->readAll() == fixture.resources.value("project.json"), "stream outlives reader");
  retained.device.reset();

  const auto original = get(path);
  expect(original.contains(QByteArray::fromHex("504b0606")), "writer emits a ZIP64 end record");
  expect(!fixture.write(path).succeeded() && get(path) == original, "existing target is preserved byte-for-byte");
  expect(fixture.write(temporary.filePath("wrong.zip")).error == ProjectZipError::InvalidPath,
    "project extension is required");

  int serial = 0;
  const auto openBytes = [&](const QByteArray& bytes, const ProjectZipOptions& options = {}) {
    const auto filename = temporary.filePath(QString::number(serial++) + ".cceda");
    expect(put(filename, bytes), "test archive is written");
    return ProjectZipReader::open(filename, options);
  };
  const auto stored = rawZip(entries(fixture));
  expect(openBytes(stored).succeeded(), "ordinary ZIP32 stored files are accepted");
  expect(openBytes(rawZip(entries(fixture), true)).succeeded(), "ZIP64 stored files are accepted");
  expect(!openBytes("not a zip").succeeded(), "invalid container is rejected");
  expect(!openBytes(original.left(original.size() - 20)).succeeded(), "truncated archive is rejected");
  auto altered = stored;
  altered.replace("project metadata", "Project metadata");
  expect(!openBytes(altered).succeeded(), "CRC corruption is rejected");
  auto tampered = entries(fixture);
  for (auto& entry : tampered) { if (entry.first == "project.json") { entry.second[0] = 'X'; } }
  expect(!openBytes(rawZip(tampered)).succeeded(), "valid ZIP CRC cannot hide a SHA-256 mismatch");
  auto duplicate = entries(fixture);
  duplicate.push_back(duplicate.front());
  expect(openBytes(rawZip(duplicate)).status.error == ProjectZipError::UnsafeEntry, "duplicate archive paths fail");
  duplicate.back().first = duplicate.front().first.toUpper();
  expect(openBytes(rawZip(duplicate)).status.error == ProjectZipError::UnsafeEntry, "case aliases fail");
  for (const auto* name : {"../escape", "logs/../escape", "C:/escape", "logs\\escape"})
  {
    auto unsafe = entries(fixture);
    unsafe.push_back({name, "data"});
    expect(!openBytes(rawZip(unsafe)).succeeded(), "unsafe archive path fails");
  }
  auto extra = entries(fixture);
  extra.push_back({"undeclared.json", "data"});
  expect(openBytes(rawZip(extra)).status.error == ProjectZipError::InvalidManifest, "undeclared resources fail");
  auto missing = entries(fixture);
  for (qsizetype i = missing.size() - 1; i >= 0; --i)
  { if (missing[i].first == "project.json") { missing.removeAt(i); } }
  expect(!openBytes(rawZip(missing)).succeeded(), "missing required archive resource fails");
  missing = entries(fixture);
  for (qsizetype i = missing.size() - 1; i >= 0; --i)
  { if (missing[i].first.startsWith("logs/")) { missing.removeAt(i); } }
  const auto logs = openBytes(rawZip(missing));
  expect(logs.succeeded() && logs.archive->missingOptionalLogs().size() == 2, "missing optional logs remain warnings");

  auto linked = stored;
  const auto central = linked.indexOf(QByteArray::fromHex("504b0102"));
  qToLittleEndian<quint32>(0120777U << 16, linked.data() + central + 38);
  expect(openBytes(linked).status.error == ProjectZipError::UnsafeEntry, "symlink attributes fail");
  auto localNameMismatch = stored;
  localNameMismatch[30] = 'X';
  expect(!openBytes(localNameMismatch).succeeded(), "local and central names must agree");
  ProjectZipOptions tiny;
  tiny.maximumArchiveBytes = 10;
  expect(openBytes(original, tiny).status.error == ProjectZipError::LimitsExceeded, "archive byte cap is enforced");
  tiny = {};
  tiny.resources.maximumTotalBytes = 10;
  expect(openBytes(original, tiny).status.error == ProjectZipError::LimitsExceeded, "expanded byte cap is enforced");
  tiny = {};
  tiny.maximumCompressionRatio = 1;
  expect(openBytes(original, tiny).status.error == ProjectZipError::LimitsExceeded, "compression ratio cap is enforced");
  const auto failedOutput = temporary.filePath("failed.cceda");
  expect(!fixture.write(failedOutput, tiny).succeeded() && !QFileInfo::exists(failedOutput),
    "failed reopen verification removes the owned partial package");
  tiny = {};
  tiny.maximumArchiveBytes = 40;
  expect(!fixture.write(failedOutput, tiny).succeeded() && !QFileInfo::exists(failedOutput),
    "failed write removes the owned partial package");
  int opens = 0;
  const auto changedSource = writeNewProjectZip(failedOutput, fixture.manifest, [&](const QString& name) {
    auto source = fixture.open(name);
    if (++opens > fixture.resources.size()) { return ProjectResourceOpenResult{{}, ProjectResourceOpenError::ReadFailed}; }
    return source;
  });
  expect(!changedSource.succeeded() && !QFileInfo::exists(failedOutput), "source failure during writing cleans up");
  opens = 0;
  bool attempted = false;
  const auto guardedWrite = writeNewProjectZip(failedOutput, fixture.manifest, [&](const QString& name) {
    if (++opens > fixture.resources.size() && !attempted)
    {
      attempted = true;
      const auto other = temporary.filePath("renamed.cceda");
      expect(!MoveFileExW(reinterpret_cast<LPCWSTR>(failedOutput.utf16()),
        reinterpret_cast<LPCWSTR>(other.utf16()), 0), "active staging file cannot be renamed");
      QFile competing(failedOutput);
      expect(!competing.open(QIODevice::WriteOnly), "active staging file cannot be overwritten");
    }
    return fixture.open(name);
  });
  expect(attempted && guardedWrite.succeeded(), "guarded writer still reopens and verifies its own file");
  opens = 0;
  const auto changedPath = temporary.filePath("changed.cceda");
  const auto changedBytes = writeNewProjectZip(changedPath, fixture.manifest, [&](const QString& name) {
    auto source = fixture.open(name);
    if (++opens > fixture.resources.size() && name == "geometry/healing-history.json")
    { static_cast<QBuffer*>(source.device.get())->buffer()[1] = '}'; }
    return source;
  });
  expect(!changedBytes.succeeded() && !QFileInfo::exists(changedPath),
    "same-size source changes between validation and writing are rejected");
  auto unsupported = fixture;
  unsupported.manifest.requiredFeatures = {"future-capability"};
  const auto incompatible = openBytes(rawZip(entries(unsupported)));
  expect(incompatible.status.resourceError == ProjectResourceVerificationError::UnsupportedFeature,
    "container open preserves feature compatibility errors");

  Entries many;
  for (int i = 0; i < 10002; ++i) { many.push_back({"entry", {}}); }
  expect(openBytes(rawZip(many)).status.error == ProjectZipError::LimitsExceeded, "archive entry count is bounded");
  QByteArray hugeDirectory(33 * 1024 * 1024, '\0');
  QByteArray endRecord(22, '\0');
  qToLittleEndian<quint32>(0x06054b50, endRecord.data());
  qToLittleEndian<quint16>(1, endRecord.data() + 8);
  qToLittleEndian<quint16>(1, endRecord.data() + 10);
  qToLittleEndian<quint32>(hugeDirectory.size(), endRecord.data() + 12);
  hugeDirectory.append(endRecord);
  expect(openBytes(hugeDirectory).status.error == ProjectZipError::LimitsExceeded,
    "miniz memory budget applies before parsing a huge central directory");

  // A sparse file exercises genuine >4 GiB central-directory/local offsets
  // without allocating or writing four GiB of fixture data.
  const auto sparsePath = temporary.filePath("sparse.cceda");
  QFile sparse(sparsePath);
  expect(sparse.open(QIODevice::WriteOnly), "sparse fixture opens");
  DWORD returned = 0;
  const HANDLE native = reinterpret_cast<HANDLE>(_get_osfhandle(sparse.handle()));
  expect(DeviceIoControl(native, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr), "sparse mode is available");
  mz_zip_archive large{};
  large.m_pWrite = writeAt;
  large.m_pIO_opaque = &sparse;
  expect(mz_zip_writer_init_v2(&large, (1ULL << 32) + 32, MZ_ZIP_FLAG_WRITE_ZIP64), "large-offset writer initializes");
  for (const auto& entry : entries(fixture))
  {
    const auto name = entry.first.toUtf8();
    expect(mz_zip_writer_add_mem(&large, name.constData(), entry.second.constData(), entry.second.size(), 0),
      "large-offset entry writes");
  }
  expect(mz_zip_writer_finalize_archive(&large), "large-offset archive finalizes");
  mz_zip_writer_end(&large);
  sparse.close();
  expect(ProjectZipReader::open(sparsePath).succeeded(), "true 64-bit archive offsets are read correctly");
  return failures == 0 ? 0 : 1;
}
