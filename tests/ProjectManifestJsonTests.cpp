#include "persistence/ProjectManifestJson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>

namespace
{
int failures = 0;
void expect(bool condition, const char* message)
{ if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; } }

cceda::persistence::ProjectManifest fixture()
{
  using namespace cceda::persistence;
  ProjectManifest manifest;
  manifest.projectId = "01234567-89ab-cdef-0123-456789abcdef";
  manifest.createdBy = "CCEDA/0.2.0+test";
  manifest.createdAt = "2026-11-11T00:00:00.000Z";
  manifest.modifiedAt = manifest.createdAt;
  manifest.occtVersion = "8.0.0";
  manifest.entries = {
    {"geometry/model.xbf", "geometry", 274877906944ULL,
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
    {"project.json", "project", 512,
      "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"},
  };
  manifest.requiredFeatures = {"mesh-quality/0.1", "mesh-settings/0.1"};
  return manifest;
}
}

int main()
{
  using namespace cceda::persistence;
  const auto expected = fixture();
  const auto written = writeProjectManifestJson(expected);
  expect(written.succeeded(), "valid manifest serializes");
  if (!written.succeeded()) { return 1; }
  const auto read = readProjectManifestJson(written.bytes);
  expect(read.succeeded(), "valid manifest reads");
  if (read.manifest)
  {
    expect(read.manifest->entries.size() == 2 && read.manifest->entries.front().path == "geometry/model.xbf",
      "entries are retained in canonical path order");
    expect(read.manifest->entries.front().uncompressedBytes == 274877906944ULL,
      "large entry sizes are retained");
    expect(writeProjectManifestJson(*read.manifest).bytes == written.bytes,
      "manifest roundtrip is deterministic");
  }
  auto unordered = expected;
  std::swap(unordered.entries[0], unordered.entries[1]);
  std::swap(unordered.requiredFeatures[0], unordered.requiredFeatures[1]);
  expect(writeProjectManifestJson(unordered).bytes == written.bytes, "writer normalizes ordering");
  const auto root = QJsonDocument::fromJson(written.bytes).object();
  const auto rejects = [&](const char* field, QJsonValue value) {
    auto changed = root;
    changed.insert(field, value);
    expect(!readProjectManifestJson(QJsonDocument(changed).toJson()).succeeded(), "invalid root value is rejected");
  };
  rejects("format", "wrong");
  rejects("formatVersion", "1.0");
  rejects("projectId", "project");
  rejects("projectId", QString(36, '-'));
  rejects("createdAt", "yesterday");
  rejects("createdAt", "2026-02-30T00:00:00Z");
  rejects("modifiedAt", "2026-11-11T00:00:00+08:00");
  rejects("canonicalLengthUnit", "m");
  rejects("entries", QJsonArray{});
  rejects("requiredFeatures", QJsonObject{});
  auto missing = root;
  missing.remove("createdAt");
  expect(!readProjectManifestJson(QJsonDocument(missing).toJson()).succeeded(), "missing field is rejected");
  auto entry = root.value("entries").toArray().at(0).toObject();
  for (const auto& field : {"path", "role", "uncompressedBytes", "sha256"})
  {
    auto changedEntry = entry;
    changedEntry.remove(field);
    auto changed = root;
    auto entries = root.value("entries").toArray();
    entries[0] = changedEntry;
    changed.insert("entries", entries);
    expect(!readProjectManifestJson(QJsonDocument(changed).toJson()).succeeded(), "missing entry field is rejected");
  }
  auto changed = root;
  auto entries = root.value("entries").toArray();
  auto bad = entries[0].toObject();
  bad.insert("path", "/absolute"); entries[0] = bad; changed.insert("entries", entries);
  expect(!readProjectManifestJson(QJsonDocument(changed).toJson()).succeeded(), "absolute paths are rejected");
  for (const auto& path : {"../escape", "geometry/../model", "geometry\\model", "C:/model", "geometry//model", "manifest.json", "logs/\nlog"})
  {
    auto p = root.value("entries").toArray();
    auto e = p[0].toObject(); e.insert("path", path); p[0] = e;
    auto c = root; c.insert("entries", p);
    expect(!readProjectManifestJson(QJsonDocument(c).toJson()).succeeded(), "unsafe path is rejected");
  }
  for (const auto& hash : {"short", "GG00000000000000000000000000000000000000000000000000000000000000"})
  {
    auto p = root.value("entries").toArray(); auto e = p[0].toObject(); e.insert("sha256", hash); p[0] = e;
    auto c = root; c.insert("entries", p);
    expect(!readProjectManifestJson(QJsonDocument(c).toJson()).succeeded(), "invalid hash is rejected");
  }
  auto duplicate = root.value("entries").toArray(); duplicate.append(duplicate.at(0));
  changed = root; changed.insert("entries", duplicate);
  expect(!readProjectManifestJson(QJsonDocument(changed).toJson()).succeeded(), "duplicate paths are rejected");
  auto unsorted = root.value("entries").toArray();
  const QJsonValue firstEntry = unsorted.at(0);
  unsorted[0] = unsorted.at(1);
  unsorted[1] = firstEntry;
  changed = root; changed.insert("entries", unsorted);
  expect(readProjectManifestJson(QJsonDocument(changed).toJson()).succeeded(),
    "reader accepts indentation and arbitrary entry order");
  expect(readProjectManifestJson(written.bytes + " \r\n").succeeded(), "JSON whitespace is accepted");
  expect(!readProjectManifestJson(written.bytes + "{}").succeeded(), "trailing documents are rejected");
  auto duplicateKey = written.bytes;
  duplicateKey.insert(1, "\"format\":\"cceda-project\",");
  expect(!readProjectManifestJson(duplicateKey).succeeded(), "duplicate object keys are rejected");
  duplicateKey = written.bytes;
  duplicateKey.replace("\"path\":", "\"p\\u0061th\":\"project.json\",\"path\":");
  expect(!readProjectManifestJson(duplicateKey).succeeded(), "escaped duplicate keys are rejected");
  expect(!readProjectManifestJson(QByteArray("\xEF\xBB\xBF") + written.bytes).succeeded(), "BOM is rejected");
  auto invalidUtf8 = written.bytes;
  invalidUtf8.replace("CCEDA", QByteArray::fromHex("ff"));
  expect(!readProjectManifestJson(invalidUtf8).succeeded(), "invalid UTF-8 is rejected");
  expect(!readProjectManifestJson("{}").succeeded(), "empty manifest is rejected");
  expect(readProjectManifestJson(QByteArray(MaximumProjectManifestBytes + 1, ' ')).error
    == ProjectManifestJsonError::InputTooLarge, "manifest byte budget is enforced");
  auto tooLarge = expected; tooLarge.entries[0].uncompressedBytes = MaximumProjectManifestEntryBytes + 1;
  expect(!writeProjectManifestJson(tooLarge).succeeded(), "entry size budget is enforced");
  auto badRequired = expected; badRequired.requiredFeatures.push_back("mesh-settings/0.1");
  expect(!writeProjectManifestJson(badRequired).succeeded(), "duplicate features are rejected");
  auto caseCollision = expected;
  caseCollision.entries.push_back(caseCollision.entries[0]);
  caseCollision.entries.back().path = caseCollision.entries[0].path.toUpper();
  expect(writeProjectManifestJson(caseCollision).error == ProjectManifestJsonError::DuplicateEntry,
    "case-folded path aliases are rejected");
  auto nullPath = expected;
  nullPath.entries[0].path.append(QChar(0));
  expect(!writeProjectManifestJson(nullPath).succeeded(), "NUL path is rejected");
  auto newlineHash = expected;
  newlineHash.entries[0].sha256.append('\n');
  expect(!writeProjectManifestJson(newlineHash).succeeded(), "hash must be exactly 64 hexadecimal characters");
  auto oversizedString = expected;
  oversizedString.createdBy = QString(1025, 'x');
  expect(!writeProjectManifestJson(oversizedString).succeeded(), "string budget is enforced");
  auto tooMany = expected;
  tooMany.entries.resize(MaximumProjectManifestEntries + 1);
  expect(!writeProjectManifestJson(tooMany).succeeded(), "entry count budget is enforced");
  auto tooDeep = written.bytes;
  tooDeep.insert(1, "\"extra\":[[[[]]]],");
  expect(!readProjectManifestJson(tooDeep).succeeded(), "nesting depth budget is enforced");
  return failures == 0 ? 0 : 1;
}
