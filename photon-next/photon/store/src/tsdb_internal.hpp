#pragma once

// Helpers shared between tsdb.cpp (codec, store, indexer) and tsdb_live.cpp
// (live writer driver, recovery, retention, compaction). Not part of the public
// TSDB surface; the definitions live in tsdb.cpp.

#include "photon/json.hpp"
#include "photon/tsdb.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace photon::tsdb::detail {

std::string pad4(std::uint32_t n);
std::uint64_t fileSizeOf(const std::filesystem::path& path);
std::uint64_t dirBytes(const std::filesystem::path& dir);
bool readFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes);
bool writeFile(const std::filesystem::path& path, std::string_view text, std::string* error);

// Write to a sibling .tmp then rename over the target, retrying briefly. A
// reader polling a live session must see either the whole previous file or the
// whole new one; a half-written page index would decode as garbage or, worse,
// as a shorter valid index.
bool writeFileAtomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string* error);

json::Value signalJson(const SignalInfo& s);
SignalInfo signalFromJson(const json::Value& v);

bool loadSessionJson(const std::filesystem::path& sessionDir, SessionManifest& manifest, std::string* error);
bool storeSessionJson(const std::filesystem::path& sessionDir, const SessionManifest& manifest, std::string* error);
bool loadSignalsJson(const std::filesystem::path& sessionDir, std::vector<SignalInfo>& out);
bool storeSignalsJson(const std::filesystem::path& sessionDir, std::vector<SignalInfo> signals, std::string* error);

// Column file path for a level and file number.
std::filesystem::path columnPath(const std::filesystem::path& sessionDir, int level, std::uint32_t fileNo);
std::filesystem::path indexPath(const std::filesystem::path& sessionDir, int level);

bool loadLevelIndex(const std::filesystem::path& sessionDir, int level, IndexHeader& header,
                    std::vector<IndexRecord>& records);
bool storeLevelIndex(const std::filesystem::path& sessionDir, int level, std::span<const IndexRecord> records,
                     std::string* error);

}  // namespace photon::tsdb::detail
