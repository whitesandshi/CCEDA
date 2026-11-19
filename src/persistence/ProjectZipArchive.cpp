#include "persistence/ProjectZipArchive.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QUuid>
#include <QtEndian>
#include <miniz.h>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <fcntl.h>
#include <io.h>

namespace cceda::persistence
{
namespace detail
{
// Bounds miniz's central directory and inflater allocations before it parses
// any untrusted entry counts. Header alignment also suits compressor structs.
struct alignas(std::max_align_t) Allocation { std::size_t size; };
struct ZipMemory
{
  static constexpr std::size_t Limit = 32 * 1024 * 1024;
  std::size_t used = 0;
  bool exceeded = false;
  static void* resize(void* opaque, void* address, size_t items, size_t size)
  {
    auto& memory = *static_cast<ZipMemory*>(opaque);
    auto* old = address ? static_cast<Allocation*>(address) - 1 : nullptr;
    const auto previous = old ? old->size : 0;
    const auto available = Limit - (memory.used - previous);
    if (available < sizeof(Allocation) || (size && items > (available - sizeof(Allocation)) / size))
    { memory.exceeded = true; return nullptr; }
    const auto bytes = items * size;
    auto* block = static_cast<Allocation*>(std::realloc(old, sizeof(Allocation) + bytes));
    if (!block) { return nullptr; }
    block->size = sizeof(Allocation) + bytes;
    memory.used = memory.used - previous + block->size;
    return block + 1;
  }
  static void* allocate(void* opaque, size_t items, size_t size)
  { return resize(opaque, nullptr, items, size); }
  static void release(void* opaque, void* address)
  {
    if (!address) { return; }
    auto* block = static_cast<Allocation*>(address) - 1;
    static_cast<ZipMemory*>(opaque)->used -= block->size;
    std::free(block);
  }
};

struct ProjectZipState
{
  ZipMemory memory;
  mz_zip_archive zip{};
  QFile file;
  QHash<QString, mz_uint> indexes;
  ProjectManifest manifest;
  QStringList missingLogs;
  std::uint64_t writeLimit = 0;

  ProjectZipState()
  {
    zip.m_pAlloc = ZipMemory::allocate;
    zip.m_pRealloc = ZipMemory::resize;
    zip.m_pFree = ZipMemory::release;
    zip.m_pAlloc_opaque = &memory;
    zip.m_pIO_opaque = this;
  }
  ~ProjectZipState() { if (zip.m_pState) { mz_zip_end(&zip); } }
  static size_t read(void* opaque, mz_uint64 offset, void* data, size_t bytes)
  {
    auto& file = static_cast<ProjectZipState*>(opaque)->file;
    if (offset > static_cast<mz_uint64>(file.size())
      || bytes > static_cast<mz_uint64>(file.size()) - offset
      || !file.seek(static_cast<qint64>(offset))) { return 0; }
    const auto count = file.read(static_cast<char*>(data), static_cast<qint64>(bytes));
    return count < 0 ? 0 : static_cast<size_t>(count);
  }
  static size_t write(void* opaque, mz_uint64 offset, const void* data, size_t bytes)
  {
    auto& state = *static_cast<ProjectZipState*>(opaque);
    if (offset > state.writeLimit || bytes > state.writeLimit - offset
      || !state.file.seek(static_cast<qint64>(offset))) { return 0; }
    const auto count = state.file.write(static_cast<const char*>(data), static_cast<qint64>(bytes));
    return count < 0 ? 0 : static_cast<size_t>(count);
  }
};
}

namespace
{
using State = detail::ProjectZipState;
using Error = ProjectZipError;

bool validOptions(const ProjectZipOptions& options)
{
  return options.maximumArchiveBytes > 0
    && options.maximumArchiveBytes <= static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())
    && options.maximumCompressionRatio > 0;
}

bool packagePath(const QString& path)
{ return !path.contains(QChar(0)) && QFileInfo(path).suffix().compare("cceda", Qt::CaseInsensitive) == 0; }

// Deny writes/deletes for the entire lifetime of the shared reader and streams.
bool openLocked(QFile& file, const QString& path, bool guardedWriter)
{
  // Only internal reopen validation may share with the still-held staging
  // writer, whose handle continues to deny writes/deletes by other processes.
  const DWORD sharing = FILE_SHARE_READ | (guardedWriter ? FILE_SHARE_WRITE | FILE_SHARE_DELETE : 0);
  const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
    sharing, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) { return false; }
  const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
  if (descriptor < 0) { CloseHandle(handle); return false; }
  if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
  { _close(descriptor); return false; }
  return true;
}

bool createStaging(QFile& file, const QString& path)
{
  const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE | DELETE,
    FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) { return false; }
  const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_WRONLY | _O_BINARY);
  if (descriptor < 0)
  {
    FILE_DISPOSITION_INFO disposition{TRUE};
    SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
    CloseHandle(handle);
    return false;
  }
  if (!file.open(descriptor, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle))
  {
    FILE_DISPOSITION_INFO disposition{TRUE};
    SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
    _close(descriptor);
    return false;
  }
  return true;
}

bool publishStaging(HANDLE handle, const QString& finalPath)
{
  const QString nativePath = QDir::toNativeSeparators(finalPath);
  const auto native = reinterpret_cast<LPCWSTR>(nativePath.utf16());
  const DWORD attributes = GetFileAttributesW(native);
  if (attributes == INVALID_FILE_ATTRIBUTES)
  {
    if (GetLastError() != ERROR_FILE_NOT_FOUND) { return false; }
  }
  else if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
  {
    return false;
  }
  const auto nameBytes = static_cast<DWORD>(nativePath.size() * sizeof(wchar_t));
  const auto infoBytes = static_cast<DWORD>(sizeof(FILE_RENAME_INFO)) + nameBytes;
  auto buffer = std::make_unique<std::byte[]>(infoBytes);
  auto* info = new (buffer.get()) FILE_RENAME_INFO{};
  info->ReplaceIfExists = TRUE;
  info->FileNameLength = nameBytes;
  // The Win32 structure has a variable-length tail. Copy through the buffer.
  std::memcpy(buffer.get() + offsetof(FILE_RENAME_INFO, FileName), native, nameBytes);
  return SetFileInformationByHandle(handle, FileRenameInfo, info, infoBytes) != 0;
}

class ZipStream final : public QIODevice
{
public:
  ZipStream(std::shared_ptr<State> state, mz_uint index) : state_(std::move(state))
  {
    iterator_ = mz_zip_reader_extract_iter_new(&state_->zip, index, 0);
    if (iterator_) { open(QIODevice::ReadOnly | QIODevice::Unbuffered); }
  }
  ~ZipStream() override { if (iterator_) { mz_zip_reader_extract_iter_free(iterator_); } }
  bool isSequential() const override { return true; }
  bool atEnd() const override { return finished_ && !failed_; }
protected:
  qint64 readData(char* data, qint64 maximum) override
  {
    if (failed_) { return -1; }
    if (finished_ || maximum == 0) { return 0; }
    const auto count = mz_zip_reader_extract_iter_read(iterator_, data, static_cast<size_t>(maximum));
    if (!count)
    {
      // free performs final size and CRC validation, including stored entries.
      failed_ = !mz_zip_reader_extract_iter_free(iterator_);
      iterator_ = nullptr;
      finished_ = true;
      return failed_ ? -1 : 0;
    }
    return static_cast<qint64>(count);
  }
  qint64 writeData(const char*, qint64) override { return -1; }
private:
  std::shared_ptr<State> state_;
  mz_zip_reader_extract_iter_state* iterator_ = nullptr;
  bool finished_ = false;
  bool failed_ = false;
};

ProjectResourceOpenResult resource(const std::shared_ptr<State>& state, const QString& path)
{
  if (!state->indexes.contains(path) || path == "manifest.json")
  { return {{}, ProjectResourceOpenError::Missing}; }
  auto stream = std::make_unique<ZipStream>(state, state->indexes.value(path));
  if (!stream->isOpen()) { return {{}, ProjectResourceOpenError::ReadFailed}; }
  return {std::move(stream), ProjectResourceOpenError::None};
}

bool localSizesMatch(State& state, const mz_zip_archive_file_stat& stat,
  const unsigned char* header, std::uint64_t& end)
{
  const auto nameLength = qFromLittleEndian<quint16>(header + 26);
  const auto extraLength = qFromLittleEndian<quint16>(header + 28);
  const auto dataStart = stat.m_local_header_ofs + 30 + nameLength + extraLength;
  const auto directory = state.zip.m_central_directory_file_ofs;
  if (dataStart > directory || stat.m_comp_size > directory - dataStart) { return false; }
  end = dataStart + stat.m_comp_size;
  if (stat.m_bit_flag & 8)
  {
    // ZIP64 containers may contain ZIP32-sized entry descriptors. Upstream's
    // validate_file chooses width by container, so validate both legal widths
    // against the exact central-directory CRC and sizes here.
    unsigned char descriptor[24]{};
    const auto available = std::min<std::uint64_t>(sizeof(descriptor), directory - end);
    if (State::read(&state, end, descriptor, available) != available) { return false; }
    for (const unsigned prefix : {0U, 4U})
    {
      if (prefix && qFromLittleEndian<quint32>(descriptor) != 0x08074b50) { continue; }
      if (available < prefix + 12 || qFromLittleEndian<quint32>(descriptor + prefix) != stat.m_crc32) { continue; }
      const auto* sizes = descriptor + prefix + 4;
      if (available >= prefix + 20 && qFromLittleEndian<quint64>(sizes) == stat.m_comp_size
        && qFromLittleEndian<quint64>(sizes + 8) == stat.m_uncomp_size)
      { end += prefix + 20; return true; }
      if (qFromLittleEndian<quint32>(sizes) == stat.m_comp_size
        && qFromLittleEndian<quint32>(sizes + 4) == stat.m_uncomp_size)
      { end += prefix + 12; return true; }
    }
    return false;
  }
  std::uint64_t compressed = qFromLittleEndian<quint32>(header + 18);
  std::uint64_t uncompressed = qFromLittleEndian<quint32>(header + 22);
  if (compressed == 0xffffffff || uncompressed == 0xffffffff)
  {
    QByteArray extra(extraLength, '\0');
    if (State::read(&state, stat.m_local_header_ofs + 30 + nameLength, extra.data(), extra.size())
      != static_cast<size_t>(extra.size())) { return false; }
    bool found = false;
    for (qsizetype offset = 0; offset + 4 <= extra.size();)
    {
      const auto type = qFromLittleEndian<quint16>(extra.constData() + offset);
      const auto size = qFromLittleEndian<quint16>(extra.constData() + offset + 2);
      offset += 4;
      if (size > extra.size() - offset) { return false; }
      if (type == 1)
      {
        const auto required = (uncompressed == 0xffffffff ? 8 : 0) + (compressed == 0xffffffff ? 8 : 0);
        if (size < required) { return false; }
        const auto* value = extra.constData() + offset;
        if (uncompressed == 0xffffffff) { uncompressed = qFromLittleEndian<quint64>(value); value += 8; }
        if (compressed == 0xffffffff) { compressed = qFromLittleEndian<quint64>(value); }
        found = true;
        break;
      }
      offset += size;
    }
    if (!found) { return false; }
  }
  return qFromLittleEndian<quint32>(header + 14) == stat.m_crc32
    && compressed == stat.m_comp_size && uncompressed == stat.m_uncomp_size;
}

ProjectZipResult inspectDirectory(State& state, const ProjectZipOptions& options)
{
  if (state.zip.m_total_files > MaximumProjectManifestEntries + 1)
  { return {Error::LimitsExceeded}; }
  QSet<QString> names;
  QVector<QPair<std::uint64_t, std::uint64_t>> extents;
  std::uint64_t total = 0;
  for (mz_uint index = 0; index < state.zip.m_total_files; ++index)
  {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&state.zip, index, &stat)) { return {Error::InvalidArchive}; }
    const auto nameBytes = mz_zip_reader_get_filename(&state.zip, index, nullptr, 0);
    // Bound filename allocation independently of the manifest string limit.
    if (nameBytes < 2 || nameBytes > MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE)
    { return {Error::UnsafeEntry}; }
    QByteArray encoded(static_cast<qsizetype>(nameBytes), '\0');
    mz_zip_reader_get_filename(&state.zip, index, encoded.data(), nameBytes);
    encoded.chop(1);
    const auto name = QString::fromUtf8(encoded);
    if (name.toUtf8() != encoded || encoded.contains('\0')
      || (name != "manifest.json" && !isSafeProjectResourcePath(name))
      || names.contains(name.toCaseFolded())) { return {Error::UnsafeEntry, name}; }
    const auto unixType = (stat.m_external_attr >> 16) & 0170000;
    if (stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported
      || (unixType != 0 && unixType != 0100000) || (stat.m_external_attr & (0x10 | 0x400)))
    { return {Error::UnsafeEntry, name}; }
    const auto entryLimit = name == "manifest.json" ? static_cast<std::uint64_t>(MaximumProjectManifestBytes)
      : MaximumProjectManifestEntryBytes;
    if (stat.m_uncomp_size > entryLimit
      || (stat.m_uncomp_size && (!stat.m_comp_size
        || (stat.m_uncomp_size - 1) / stat.m_comp_size >= options.maximumCompressionRatio)))
    { return {Error::LimitsExceeded, name}; }
    if (name != "manifest.json")
    {
      if (stat.m_uncomp_size > options.resources.maximumTotalBytes - total)
      { return {Error::LimitsExceeded, name}; }
      total += stat.m_uncomp_size;
    }
    // Check local header for every entry (upstream skips empty files).
    unsigned char header[30];
    if (State::read(&state, stat.m_local_header_ofs, header, sizeof(header)) != sizeof(header)
      || qFromLittleEndian<quint32>(header) != 0x04034b50
      || qFromLittleEndian<quint16>(header + 6) != stat.m_bit_flag
      || qFromLittleEndian<quint16>(header + 8) != stat.m_method
      || qFromLittleEndian<quint16>(header + 26) != encoded.size())
    { return {Error::InvalidArchive, name}; }
    QByteArray localName(encoded.size(), '\0');
    std::uint64_t end = 0;
    if (State::read(&state, stat.m_local_header_ofs + 30, localName.data(), localName.size())
        != static_cast<size_t>(localName.size()) || localName != encoded
      || !localSizesMatch(state, stat, header, end))
    { return {Error::InvalidArchive, name}; }
    extents.push_back({stat.m_local_header_ofs, end});
    names.insert(name.toCaseFolded());
    state.indexes.insert(name, index);
  }
  std::sort(extents.begin(), extents.end());
  for (qsizetype i = 1; i < extents.size(); ++i)
  { if (extents[i].first < extents[i - 1].second) { return {Error::InvalidArchive}; } }
  for (const auto& name : names)
  {
    auto parent = name;
    for (auto slash = parent.lastIndexOf('/'); slash >= 0; slash = parent.lastIndexOf('/'))
    {
      parent.truncate(slash);
      if (names.contains(parent)) { return {Error::UnsafeEntry, name}; }
    }
  }
  return {};
}
}

ProjectZipReader::ProjectZipReader(std::shared_ptr<State> state) : state_(std::move(state)) {}
const ProjectManifest& ProjectZipReader::manifest() const noexcept { return state_->manifest; }
const QStringList& ProjectZipReader::missingOptionalLogs() const noexcept { return state_->missingLogs; }
ProjectResourceOpenResult ProjectZipReader::openResource(const QString& path) const
{ return resource(state_, path); }

ProjectZipOpenResult ProjectZipReader::open(const QString& path, const ProjectZipOptions& options)
{ return openInternal(path, options, false); }

ProjectZipOpenResult ProjectZipReader::openInternal(
  const QString& path, const ProjectZipOptions& options, bool guardedWriter)
{
  const auto fail = [](Error error, const QString& subject = QString{}) {
    return ProjectZipOpenResult{{}, {error, subject}};
  };
  if (!packagePath(path)) { return fail(Error::InvalidPath); }
  if (!validOptions(options)) { return fail(Error::LimitsExceeded); }
  auto state = std::make_shared<State>();
  if (!openLocked(state->file, path, guardedWriter)) { return fail(Error::OpenFailed); }
  if (static_cast<std::uint64_t>(state->file.size()) > options.maximumArchiveBytes)
  { return fail(Error::LimitsExceeded); }
  state->zip.m_pRead = State::read;
  if (!mz_zip_reader_init(&state->zip, state->file.size(), 0))
  { return fail(state->memory.exceeded ? Error::LimitsExceeded : Error::InvalidArchive); }
  const auto directory = inspectDirectory(*state, options);
  if (!directory.succeeded()) { return {{}, directory}; }
  if (!state->indexes.contains("manifest.json")) { return fail(Error::InvalidManifest); }
  ZipStream manifestStream(state, state->indexes.value("manifest.json"));
  QByteArray bytes;
  char buffer[65536];
  while (manifestStream.isOpen())
  {
    const auto count = manifestStream.read(buffer, sizeof(buffer));
    if (count < 0 || (count == 0 && !manifestStream.atEnd())) { return fail(Error::InvalidManifest); }
    if (!count) { break; }
    if (count > MaximumProjectManifestBytes - bytes.size()) { return fail(Error::LimitsExceeded); }
    bytes.append(buffer, count);
  }
  const auto decoded = readProjectManifestJson(bytes);
  if (!decoded.succeeded()) { return fail(Error::InvalidManifest); }
  state->manifest = *decoded.manifest;
  QSet<QString> declared{"manifest.json"};
  for (const auto& entry : state->manifest.entries)
  {
    declared.insert(entry.path);
    if (state->indexes.contains(entry.path))
    {
      mz_zip_archive_file_stat stat{};
      mz_zip_reader_file_stat(&state->zip, state->indexes.value(entry.path), &stat);
      if (stat.m_uncomp_size != entry.uncompressedBytes)
      { return fail(Error::InvalidManifest, entry.path); }
    }
  }
  for (auto it = state->indexes.cbegin(); it != state->indexes.cend(); ++it)
  { if (!declared.contains(it.key())) { return fail(Error::InvalidManifest, it.key()); } }
  const auto verified = verifyProjectResources(state->manifest,
    [state](const QString& name) { return resource(state, name); }, options.resources);
  if (!verified.succeeded())
  { return {{}, {Error::ResourceVerificationFailed, verified.subject, verified.error}}; }
  state->missingLogs = verified.missingOptionalLogs;
  return {std::shared_ptr<ProjectZipReader>(new ProjectZipReader(state)), {}};
}

namespace
{
struct Source
{
  QIODevice& device;
  std::uint64_t expected;
  QCryptographicHash hash{QCryptographicHash::Sha256};
  std::uint64_t consumed = 0;
  bool failed = false;
  static size_t read(void* opaque, mz_uint64 offset, void* data, size_t bytes)
  {
    auto& source = *static_cast<Source*>(opaque);
    if (offset != source.consumed) { source.failed = true; return 0; }
    bytes = static_cast<size_t>(std::min<std::uint64_t>(bytes, source.expected - source.consumed));
    size_t received = 0;
    while (received < bytes)
    {
      const auto count = source.device.read(static_cast<char*>(data) + received, bytes - received);
      if (count <= 0) { source.failed = true; break; }
      source.hash.addData(QByteArrayView(static_cast<char*>(data) + received, count));
      received += static_cast<size_t>(count);
      source.consumed += static_cast<std::uint64_t>(count);
    }
    return received;
  }
};
}

ProjectZipResult ProjectZipReader::writeInternal(const QString& stagingPath, const QString& finalPath,
  const ProjectManifest& manifest, const OpenProjectResource& openResource, const ProjectZipOptions& options)
{
  if (!packagePath(stagingPath)) { return {Error::InvalidPath}; }
  if (!validOptions(options)) { return {Error::LimitsExceeded}; }
  const auto encoded = writeProjectManifestJson(manifest);
  if (!encoded.succeeded()) { return {Error::InvalidManifest}; }
  const auto verified = verifyProjectResources(manifest, openResource, options.resources);
  if (!verified.succeeded())
  { return {Error::ResourceVerificationFailed, verified.subject, verified.error}; }
  // Saving requires all declared resources, including optional logs.
  if (!verified.missingOptionalLogs.isEmpty())
  { return {Error::WriteFailed, verified.missingOptionalLogs.front()}; }
  for (const auto& entry : manifest.entries)
  { if (entry.path.toUtf8().size() >= MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE) { return {Error::UnsafeEntry, entry.path}; } }
  State state;
  if (!createStaging(state.file, stagingPath)) { return {Error::OpenFailed}; }
  // RAII cleanup ends the ZIP writer before removing only our exclusive file.
  struct Cleanup
  {
    State& state;
    bool keep = false;
    ~Cleanup()
    {
      if (state.zip.m_pState) { mz_zip_end(&state.zip); }
      if (!keep)
      {
        // Delete by owned handle, never resolve the path again for cleanup.
        FILE_DISPOSITION_INFO disposition{TRUE};
        const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(state.file.handle()));
        SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
      }
      state.file.close();
    }
  } cleanup{state};
  state.writeLimit = options.maximumArchiveBytes;
  state.zip.m_pWrite = State::write;
  if (!mz_zip_writer_init_v2(&state.zip, 0, MZ_ZIP_FLAG_WRITE_ZIP64)) { return {Error::WriteFailed}; }
  for (const auto& entry : manifest.entries)
  {
    auto source = openResource(entry.path);
    if (source.error != ProjectResourceOpenError::None || !source.device || !source.device->isReadable()
      || source.device->isTextModeEnabled() || (!source.device->isSequential() && source.device->pos() != 0))
    { return {Error::WriteFailed, entry.path}; }
    Source input{*source.device, entry.uncompressedBytes};
    const auto name = entry.path.toUtf8();
    if (!mz_zip_writer_add_read_buf_callback(&state.zip, name.constData(), Source::read, &input,
      entry.uncompressedBytes, nullptr, nullptr, 0, MZ_BEST_SPEED | MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE,
      nullptr, 0, nullptr, 0))
    { return {Error::WriteFailed, entry.path}; }
    char extra;
    if (input.failed || input.consumed != entry.uncompressedBytes
      || source.device->read(&extra, 1) != 0 || !source.device->atEnd()
      || input.hash.result().toHex() != entry.sha256.toLatin1())
    { return {Error::WriteFailed, entry.path}; }
  }
  if (!mz_zip_writer_add_mem(&state.zip, "manifest.json", encoded.bytes.constData(), encoded.bytes.size(), 0)
    || !mz_zip_writer_finalize_archive(&state.zip) || !mz_zip_writer_end(&state.zip) || !state.file.flush())
  { return {Error::WriteFailed}; }
  {
    const auto reopened = openInternal(stagingPath, options, true);
    if (!reopened.succeeded()) { return reopened.status; }
  }
  if (!finalPath.isEmpty())
  {
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(state.file.handle()));
    if (!FlushFileBuffers(handle) || !publishStaging(handle, finalPath))
    { return {Error::PublishFailed, finalPath}; }
  }
  cleanup.keep = true;
  return {};
}

ProjectZipResult writeNewProjectZip(const QString& stagingPath, const ProjectManifest& manifest,
  const OpenProjectResource& openResource, const ProjectZipOptions& options)
{
  return ProjectZipReader::writeInternal(stagingPath, {}, manifest, openResource, options);
}

ProjectZipResult replaceProjectZip(const QString& finalPath, const ProjectManifest& manifest,
  const OpenProjectResource& openResource, const ProjectZipOptions& options)
{
  if (!packagePath(finalPath)) { return {Error::InvalidPath, finalPath}; }
  const QString absolute = QDir::cleanPath(QFileInfo(finalPath).absoluteFilePath());
  const QFileInfo target(absolute);
  if (absolute.size() >= 32767 || absolute.startsWith("//?/") || absolute.startsWith("//./")
    || target.fileName().contains(':'))
  { return {Error::InvalidPath, finalPath}; }
  const QString staging = target.dir().filePath(".cceda-save-"
    + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".cceda");
  return ProjectZipReader::writeInternal(staging, absolute, manifest, openResource, options);
}
}
