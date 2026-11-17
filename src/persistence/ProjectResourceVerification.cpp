#include "persistence/ProjectResourceVerification.h"

#include <QCryptographicHash>
#include <QSet>
#include <algorithm>
#include <array>

namespace cceda::persistence
{
namespace
{
using Error = ProjectResourceVerificationError;

Error verifyStream(QIODevice& device, const ProjectManifestEntry& entry)
{
  if (!device.isOpen() || !device.isReadable() || device.isTextModeEnabled()
    || (!device.isSequential() && device.pos() != 0))
  { return Error::ReadFailed; }

  QCryptographicHash hash(QCryptographicHash::Sha256);
  std::array<char, 64 * 1024> buffer;
  std::uint64_t remaining = entry.uncompressedBytes;
  for (;;)
  {
    // One byte beyond the declaration detects oversize content without ever
    // draining a hostile source. Manifest validation bounds remaining + 1.
    const auto requested = static_cast<qint64>(
      std::min<std::uint64_t>(buffer.size(), remaining + 1));
    const auto count = device.read(buffer.data(), requested);
    if (count < 0) { return Error::ReadFailed; }
    if (count == 0)
    {
      if (!device.atEnd()) { return Error::ReadFailed; }
      if (remaining != 0) { return Error::SizeMismatch; }
      return hash.result().toHex() == entry.sha256.toLatin1() ? Error::None : Error::HashMismatch;
    }
    if (static_cast<std::uint64_t>(count) > remaining) { return Error::SizeMismatch; }
    hash.addData(QByteArrayView(buffer.data(), count));
    remaining -= static_cast<std::uint64_t>(count);
  }
}
}

ProjectResourceVerificationResult verifyProjectResources(
  const ProjectManifest& manifest, const OpenProjectResource& openResource,
  const ProjectResourceVerificationOptions& options)
{
  // Check the encoded budget too: callers may supply an in-memory manifest.
  if (!writeProjectManifestJson(manifest).succeeded())
  { return {Error::InvalidManifest, {}, {}}; }
  for (const auto& feature : manifest.requiredFeatures)
  {
    if (!options.supportedFeatures.contains(feature))
    { return {Error::UnsupportedFeature, feature, {}}; }
  }

  QSet<QString> paths;
  std::uint64_t total = 0;
  for (const auto& entry : manifest.entries)
  {
    // Subtraction keeps the sum bounded even for caller-supplied UINT64_MAX.
    if (entry.uncompressedBytes > options.maximumTotalBytes - total)
    { return {Error::BudgetExceeded, entry.path, {}}; }
    total += entry.uncompressedBytes;
    paths.insert(entry.path);
  }
  for (const auto* required : {"project.json", "geometry/healing-history.json", "mesh/settings.json"})
  {
    if (!paths.contains(QLatin1String(required)))
    { return {Error::MissingDeclaration, QLatin1String(required), {}}; }
  }
  const std::array<std::pair<QString, QString>, 2> pairs{{
    {"source/original.step", "geometry/model.xbf"},
    {"mesh/current.meshbin", "mesh/quality.json"},
  }};
  for (const auto& pair : pairs)
  {
    if (paths.contains(pair.first) != paths.contains(pair.second))
    { return {Error::IncompleteResourcePair, paths.contains(pair.first) ? pair.second : pair.first, {}}; }
  }
  if (paths.contains("mesh/current.meshbin") && !paths.contains("geometry/model.xbf"))
  { return {Error::MissingDeclaration, "geometry/model.xbf", {}}; }
  if (!openResource) { return {Error::ReadFailed, {}, {}}; }

  QStringList missingLogs;
  for (const auto& entry : manifest.entries)
  {
    auto source = openResource(entry.path);
    if (source.error == ProjectResourceOpenError::Missing && !source.device)
    {
      if (entry.path.startsWith("logs/") && entry.role == "log")
      {
        missingLogs.push_back(entry.path);
        continue;
      }
      return {Error::MissingResource, entry.path, {}};
    }
    if (source.error != ProjectResourceOpenError::None || !source.device)
    { return {Error::ReadFailed, entry.path, {}}; }
    const auto error = verifyStream(*source.device, entry);
    if (error != Error::None) { return {error, entry.path, {}}; }
  }
  return {Error::None, {}, std::move(missingLogs)};
}
}
