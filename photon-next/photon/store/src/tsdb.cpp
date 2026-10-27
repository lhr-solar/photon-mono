#include "photon/tsdb.hpp"

#include "photon/endian.hpp"
#include "photon/json.hpp"
#include "photon/sha1.hpp"
#include "photon/vsd1.hpp"
#include "tsdb_internal.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

namespace photon::tsdb {
namespace {

constexpr std::uint32_t kBuckets[kLevelCount] = {0, 100, 1000, 10000, 60000, 600000};

std::string hexSha1(std::string_view text) {
    const auto digest = crypto::sha1(text);
    std::string out(40, '0');
    for (std::size_t i = 0; i < digest.size(); ++i) {
        char tmp[3];
        std::snprintf(tmp, sizeof(tmp), "%02x", digest[i]);
        out[i * 2] = tmp[0];
        out[i * 2 + 1] = tmp[1];
    }
    return out;
}

using detail::dirBytes;
using detail::fileSizeOf;
using detail::pad4;
using detail::readFile;
using detail::signalFromJson;
using detail::signalJson;
using detail::writeFile;
using detail::writeFileAtomic;
using photon::readU16Be;
using photon::readU32Be;
using photon::readU64Be;

// visit returns false to stop the walk. Without that, a failure partway through
// still reads every remaining byte of a multi-gigabyte corpus.
bool walkVsd1(const std::filesystem::path& logs, std::string* error,
              const std::function<bool(const canp::Batch&, std::uint64_t)>& visit) {
    auto files = vsd1::listLogFiles(logs);
    if (files.empty()) {
        if (error) {
            *error = "no VSD1 .log files at " + logs.string();
        }
        return false;
    }
    for (const auto& file : files) {
        std::vector<std::uint8_t> bytes;
        if (!readFile(file, bytes)) {
            if (error) {
                *error = "cannot read " + file.string();
            }
            return false;
        }
        const auto fileBytes = static_cast<std::uint64_t>(bytes.size());
        std::size_t off = 0;
        while (off + vsd1::kHeaderSize <= bytes.size()) {
            vsd1::FrameHeader fh{};
            if (!vsd1::parseHeader(std::span<const std::uint8_t>(bytes.data() + off, vsd1::kHeaderSize), fh)) {
                break;
            }
            off += vsd1::kHeaderSize;
            if (off + fh.payloadBytes > bytes.size()) {
                break;
            }
            const auto payload = std::span<const std::uint8_t>(bytes.data() + off, fh.payloadBytes);
            off += fh.payloadBytes;
            for (const auto& batch : vsd1::extractCanp(payload)) {
                if (!visit(batch, fileBytes)) {
                    return true;
                }
            }
        }
    }
    return true;
}

std::int64_t wrapAdd(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}

std::int64_t wrapSub(std::int64_t a, std::int64_t b) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
}

std::int64_t uminRaw(std::int64_t a, std::int64_t b) {
    return static_cast<std::uint64_t>(a) < static_cast<std::uint64_t>(b) ? a : b;
}

std::int64_t umaxRaw(std::int64_t a, std::int64_t b) {
    return static_cast<std::uint64_t>(a) > static_cast<std::uint64_t>(b) ? a : b;
}

unsigned __int128 wideSumOf(const RollupBucket& b) {
    if (!b.wide) {
        return static_cast<unsigned __int128>(static_cast<std::uint64_t>(b.sum));
    }
    return (static_cast<unsigned __int128>(b.sumHi) << 64) | static_cast<std::uint64_t>(b.sum);
}

void setWideSum(RollupBucket& b, unsigned __int128 w) {
    b.sum = static_cast<std::int64_t>(static_cast<std::uint64_t>(w));
    b.sumHi = static_cast<std::uint64_t>(w >> 64);
    b.wide = b.sumHi != 0;
}

__int128 signedWideOf(const RollupBucket& b) {
    if (!b.wide) {
        return static_cast<__int128>(b.sum);
    }
    return (static_cast<__int128>(static_cast<std::int64_t>(b.sumHi)) << 64) |
           static_cast<__int128>(static_cast<unsigned __int128>(static_cast<std::uint64_t>(b.sum)));
}

void setSignedWide(RollupBucket& b, __int128 w) {
    b.sum = static_cast<std::int64_t>(static_cast<std::uint64_t>(w));
    b.sumHi = static_cast<std::uint64_t>(static_cast<unsigned __int128>(w) >> 64);
    b.wide = w < static_cast<__int128>(INT64_MIN) || w > static_cast<__int128>(INT64_MAX);
}

bool signedNeedsWide(const RollupBucket& b) {
    if (b.wide) {
        return true;
    }
    const auto w = signedWideOf(b);
    return w < static_cast<__int128>(INT64_MIN) || w > static_cast<__int128>(INT64_MAX);
}

std::size_t varintSizeOf(std::uint64_t v) {
    std::size_t n = 1;
    while (v >= 0x80u) {
        ++n;
        v >>= 7;
    }
    return n;
}

std::size_t zigzagSizeOf(std::int64_t v) {
    const auto u = (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
    return varintSizeOf(u);
}

void foldPairSigned(RollupBucket& a, const RollupBucket& b) {
    if (!b.present || b.count == 0) {
        return;
    }
    if (!a.present || a.count == 0) {
        const auto start = a.startMs;
        a = b;
        a.startMs = start;
        return;
    }
    a.min = std::min(a.min, b.min);
    a.max = std::max(a.max, b.max);
    a.last = b.last;
    setSignedWide(a, signedWideOf(a) + signedWideOf(b));
    a.count += b.count;
}

void foldPairUnsigned(RollupBucket& a, const RollupBucket& b) {
    if (!b.present || b.count == 0) {
        return;
    }
    if (!a.present || a.count == 0) {
        const auto start = a.startMs;
        a = b;
        a.startMs = start;
        return;
    }
    a.min = uminRaw(a.min, b.min);
    a.max = umaxRaw(a.max, b.max);
    a.last = b.last;
    setWideSum(a, wideSumOf(a) + wideSumOf(b));
    a.count += b.count;
}

void foldToBudgetSigned(std::vector<RollupBucket>& buckets, std::uint32_t& bucketMs, std::uint32_t maxPoints) {
    while (buckets.size() > static_cast<std::size_t>(maxPoints) && buckets.size() >= 2) {
        std::vector<RollupBucket> next;
        next.reserve((buckets.size() + 1) / 2);
        for (std::size_t i = 0; i < buckets.size(); i += 2) {
            RollupBucket acc = buckets[i];
            if (i + 1 < buckets.size()) {
                foldPairSigned(acc, buckets[i + 1]);
            }
            next.push_back(acc);
        }
        buckets.swap(next);
        if (bucketMs > 0xffffffffu / 2) {
            break;
        }
        bucketMs *= 2;
    }
}

void foldToBudgetUnsigned(std::vector<RollupBucket>& buckets, std::uint32_t& bucketMs, std::uint32_t maxPoints) {
    while (buckets.size() > static_cast<std::size_t>(maxPoints) && buckets.size() >= 2) {
        std::vector<RollupBucket> next;
        next.reserve((buckets.size() + 1) / 2);
        for (std::size_t i = 0; i < buckets.size(); i += 2) {
            RollupBucket acc = buckets[i];
            if (i + 1 < buckets.size()) {
                foldPairUnsigned(acc, buckets[i + 1]);
            }
            next.push_back(acc);
        }
        buckets.swap(next);
        if (bucketMs > 0xffffffffu / 2) {
            break;
        }
        bucketMs *= 2;
    }
}

bool bucketIntersects(std::uint64_t startMs, std::uint32_t bucketMs, std::uint64_t fromMs, std::uint64_t toMs) {
    const auto end = startMs + static_cast<std::uint64_t>(bucketMs);
    return startMs <= toMs && end > fromMs;
}

}  // namespace

namespace detail {

std::string pad4(std::uint32_t n) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04u", static_cast<unsigned>(n));
    return buf;
}

std::uint64_t fileSizeOf(const std::filesystem::path& path) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(n);
}

std::uint64_t dirBytes(const std::filesystem::path& dir) {
    std::uint64_t total = 0;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return 0;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

bool writeFile(const std::filesystem::path& path, std::string_view text, std::string* error) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) {
            *error = "cannot write " + path.string();
        }
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

bool readFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const auto n = in.tellg();
    if (n < 0) {
        return false;
    }
    bytes.resize(static_cast<std::size_t>(n));
    in.seekg(0, std::ios::beg);
    if (n == 0) {
        return true;
    }
    in.read(reinterpret_cast<char*>(bytes.data()), n);
    return static_cast<bool>(in);
}

bool writeFileAtomic(const std::filesystem::path& path, std::span<const std::uint8_t> bytes, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) {
                *error = "cannot write " + tmp.string();
            }
            return false;
        }
        if (!bytes.empty()) {
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        out.flush();
        if (!out) {
            if (error) {
                *error = "short write " + tmp.string();
            }
            return false;
        }
    }
    // Same transient-handle problem as the directory publish in Store::finish:
    // a scanner holding the old file makes the replace fail for a moment.
    std::error_code renameEc;
    for (int attempt = 0; attempt < 8; ++attempt) {
        renameEc.clear();
        std::filesystem::rename(tmp, path, renameEc);
        if (!renameEc) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5 * (attempt + 1)));
    }
    if (error) {
        *error = "cannot replace " + path.string() + ": " + renameEc.message();
    }
    std::filesystem::remove(tmp, ec);
    return false;
}

json::Value signalJson(const SignalInfo& s) {
    json::Object o;
    o.emplace("id", json::Value(static_cast<std::uint64_t>(s.id)));
    o.emplace("message", json::Value(s.message));
    o.emplace("name", json::Value(s.name));
    o.emplace("unit", json::Value(s.unit));
    o.emplace("factor", json::Value(s.factor));
    o.emplace("offset", json::Value(s.offset));
    o.emplace("bits", json::Value(s.bits));
    o.emplace("signed", json::Value(s.isSigned));
    o.emplace("min", json::Value(s.min));
    o.emplace("max", json::Value(s.max));
    o.emplace("native_hz", json::Value(s.nativeHz));
    o.emplace("value_type", json::Value(static_cast<int>(s.valueType)));
    json::Array levels;
    for (int L = 0; L < kLevelCount; ++L) {
        if ((s.levels & static_cast<std::uint8_t>(1u << L)) != 0) {
            levels.emplace_back(json::Value(L));
        }
    }
    o.emplace("levels", json::Value(std::move(levels)));
    return json::Value(std::move(o));
}

SignalInfo signalFromJson(const json::Value& v) {
    SignalInfo s;
    s.id = static_cast<std::uint32_t>(v["id"].asInt());
    s.message = v["message"].asString();
    s.name = v["name"].asString();
    s.unit = v["unit"].asString();
    s.factor = v["factor"].asNumber(1.0);
    s.offset = v["offset"].asNumber();
    s.bits = static_cast<int>(v["bits"].asInt());
    s.isSigned = v["signed"].asBool();
    s.min = v["min"].asNumber();
    s.max = v["max"].asNumber();
    s.nativeHz = v["native_hz"].asNumber();
    s.valueType = static_cast<dbc::ValueType>(static_cast<int>(v["value_type"].asInt()));
    s.levels = 0;
    if (v["levels"].isArray()) {
        for (const auto& item : v["levels"].asArray()) {
            const int L = static_cast<int>(item.asInt());
            if (L >= 0 && L < kLevelCount) {
                s.levels = static_cast<std::uint8_t>(s.levels | (1u << L));
            }
        }
    }
    return s;
}

bool loadSessionJson(const std::filesystem::path& sessionDir, SessionManifest& manifest, std::string* error) {
    std::vector<std::uint8_t> bytes;
    if (!readFile(sessionDir / "session.json", bytes)) {
        if (error) {
            *error = "missing session.json in " + sessionDir.string();
        }
        return false;
    }
    const auto text = std::string(bytes.begin(), bytes.end());
    const auto root = json::parse(text);
    if (!root || !root->isObject()) {
        if (error) {
            *error = "bad session.json in " + sessionDir.string();
        }
        return false;
    }
    manifest.id = (*root)["id"].asString();
    manifest.minMs = static_cast<std::uint64_t>((*root)["min_ms"].asInt());
    manifest.maxMs = static_cast<std::uint64_t>((*root)["max_ms"].asInt());
    manifest.dbcSha = (*root)["dbc_sha"].asString();
    manifest.dbcLabel = (*root)["dbc_label"].asString();
    manifest.builtLevels = static_cast<std::uint8_t>((*root)["built_levels"].asInt());
    manifest.samples = static_cast<std::uint64_t>((*root)["samples"].asInt());
    manifest.frames = static_cast<std::uint64_t>((*root)["frames"].asInt());
    manifest.generation = static_cast<std::uint64_t>((*root)["generation"].asInt());
    manifest.live = (*root)["live"].asBool();
    manifest.l0Dropped = (*root)["l0_dropped"].asBool();
    manifest.videoClockOffsetMs = (*root)["video_clock_offset_ms"].asInt();
    manifest.videoClockProv = (*root)["video_clock_prov"].asString();
    if (manifest.builtLevels == 0) {
        manifest.builtLevels = 0x01;
    }
    return true;
}

bool storeSessionJson(const std::filesystem::path& sessionDir, const SessionManifest& manifest, std::string* error) {
    json::Object sess;
    sess.emplace("id", json::Value(manifest.id));
    sess.emplace("min_ms", json::Value(manifest.minMs));
    sess.emplace("max_ms", json::Value(manifest.maxMs));
    sess.emplace("dbc_sha", json::Value(manifest.dbcSha));
    sess.emplace("dbc_label", json::Value(manifest.dbcLabel));
    sess.emplace("built_levels", json::Value(static_cast<int>(manifest.builtLevels)));
    sess.emplace("samples", json::Value(manifest.samples));
    sess.emplace("frames", json::Value(manifest.frames));
    sess.emplace("generation", json::Value(manifest.generation));
    sess.emplace("live", json::Value(manifest.live));
    sess.emplace("l0_dropped", json::Value(manifest.l0Dropped));
    sess.emplace("video_clock_offset_ms", json::Value(manifest.videoClockOffsetMs));
    sess.emplace("video_clock_prov", json::Value(manifest.videoClockProv));
    const auto text = json::stringify(json::Value(std::move(sess)));
    return writeFileAtomic(sessionDir / "session.json",
                           std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                                         text.size()),
                           error);
}

bool loadSignalsJson(const std::filesystem::path& sessionDir, std::vector<SignalInfo>& out) {
    out.clear();
    std::vector<std::uint8_t> bytes;
    if (!readFile(sessionDir / "signals.json", bytes)) {
        return false;
    }
    const auto text = std::string(bytes.begin(), bytes.end());
    const auto root = json::parse(text);
    if (!root || !root->isArray()) {
        return false;
    }
    for (const auto& item : root->asArray()) {
        auto s = signalFromJson(item);
        if (s.id != 0) {
            out.push_back(std::move(s));
        }
    }
    return true;
}

bool storeSignalsJson(const std::filesystem::path& sessionDir, std::vector<SignalInfo> signals, std::string* error) {
    std::sort(signals.begin(), signals.end(), [](const SignalInfo& a, const SignalInfo& b) { return a.id < b.id; });
    json::Array arr;
    for (const auto& s : signals) {
        arr.emplace_back(signalJson(s));
    }
    const auto text = json::stringify(json::Value(std::move(arr)));
    return writeFileAtomic(sessionDir / "signals.json",
                           std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                                         text.size()),
                           error);
}

std::filesystem::path columnPath(const std::filesystem::path& sessionDir, int level, std::uint32_t fileNo) {
    return sessionDir / ("l" + std::to_string(level)) / (pad4(fileNo) + ".pcol");
}

std::filesystem::path indexPath(const std::filesystem::path& sessionDir, int level) {
    return sessionDir / ("l" + std::to_string(level) + ".pidx");
}

bool loadLevelIndex(const std::filesystem::path& sessionDir, int level, IndexHeader& header,
                    std::vector<IndexRecord>& records) {
    records.clear();
    std::vector<std::uint8_t> bytes;
    if (!readFile(indexPath(sessionDir, level), bytes)) {
        return false;
    }
    return decodeIndex(bytes, header, records);
}

bool storeLevelIndex(const std::filesystem::path& sessionDir, int level, std::span<const IndexRecord> records,
                     std::string* error) {
    IndexHeader hdr;
    hdr.level = static_cast<std::uint8_t>(level);
    hdr.recordCount = static_cast<std::uint32_t>(records.size());
    hdr.bucketMs = bucketMsOf(level);
    std::uint32_t sigs = 0;
    std::uint32_t last = 0;
    bool have = false;
    for (const auto& r : records) {
        if (!have || r.signalId != last) {
            ++sigs;
            last = r.signalId;
            have = true;
        }
        if (hdr.minMs == 0 || r.startMs < hdr.minMs) {
            hdr.minMs = r.startMs;
        }
        if (r.endMs > hdr.maxMs) {
            hdr.maxMs = r.endMs;
        }
    }
    hdr.signalCount = sigs;
    const auto bytes = encodeIndex(hdr, records);
    return writeFileAtomic(indexPath(sessionDir, level), bytes, error);
}

}  // namespace detail

std::uint32_t bucketMsOf(int level) {
    if (level < 0 || level >= kLevelCount) {
        return 0;
    }
    return kBuckets[level];
}

std::uint64_t tileSpanMs(int level) {
    if (level <= 0) {
        return kL0TileSpanMs;
    }
    return static_cast<std::uint64_t>(bucketMsOf(level)) * kRollupTileBuckets;
}

std::int64_t tileIndexOf(std::uint64_t tMs, int level) {
    const auto span = tileSpanMs(level);
    return span == 0 ? 0 : static_cast<std::int64_t>(tMs / span);
}

bool shouldBuildLevel(double nativeHz, int level) {
    if (level <= 0) {
        return true;
    }
    const auto bms = bucketMsOf(level);
    if (bms == 0) {
        return false;
    }
    const double bucketS = static_cast<double>(bms) / 1000.0;
    return nativeHz >= 4.0 / bucketS;
}

int pickLevel(std::uint64_t fromMs, std::uint64_t toMs, std::uint32_t maxPoints, std::uint64_t rawCountInRange,
              std::uint8_t builtLevels) {
    const std::uint64_t budget = maxPoints == 0 ? 1ull : static_cast<std::uint64_t>(maxPoints) * 2ull;
    int want = 0;
    if (rawCountInRange <= budget) {
        want = 0;
    } else {
        const std::uint64_t span = toMs > fromMs ? toMs - fromMs : 0;
        want = 5;
        for (int L = 1; L <= 5; ++L) {
            const auto bms = bucketMsOf(L);
            if (bms != 0 && span / bms <= budget) {
                want = L;
                break;
            }
        }
    }
    while (want > 0 && (builtLevels & static_cast<std::uint8_t>(1u << want)) == 0) {
        --want;
    }
    if ((builtLevels & static_cast<std::uint8_t>(1u << want)) == 0) {
        for (int L = 0; L <= 5; ++L) {
            if ((builtLevels & static_cast<std::uint8_t>(1u << L)) != 0) {
                return L;
            }
        }
    }
    return want;
}

void writeVarint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80u) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80u));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

bool readVarint(std::span<const std::uint8_t> in, std::size_t& off, std::uint64_t& v) {
    v = 0;
    int shift = 0;
    while (off < in.size()) {
        const std::uint8_t b = in[off++];
        v |= static_cast<std::uint64_t>(b & 0x7fu) << shift;
        if ((b & 0x80u) == 0) {
            return true;
        }
        shift += 7;
        if (shift > 63) {
            return false;
        }
    }
    return false;
}

void writeZigzag(std::vector<std::uint8_t>& out, std::int64_t v) {
    const auto u = (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
    writeVarint(out, u);
}

bool readZigzag(std::span<const std::uint8_t> in, std::size_t& off, std::int64_t& v) {
    std::uint64_t u = 0;
    if (!readVarint(in, off, u)) {
        return false;
    }
    v = static_cast<std::int64_t>((u >> 1) ^ static_cast<std::uint64_t>(-static_cast<std::int64_t>(u & 1u)));
    return true;
}

void encodePageHeader(std::vector<std::uint8_t>& out, const PageHeader& header) {
    writeU32Le(out, header.magic);
    writeU32Le(out, header.signalId);
    writeU64Le(out, header.baseMs);
    writeU32Le(out, header.sampleCount);
    writeU8(out, header.tEnc);
    writeU8(out, header.vEnc);
    writeU8(out, header.level);
    writeU8(out, header.flags);
    writeU32Le(out, header.spanMs);
    writeU32Le(out, header.payloadBytes);
}

bool decodePageHeader(std::span<const std::uint8_t> bytes, PageHeader& header) {
    if (bytes.size() < kPageHeaderBytes) {
        return false;
    }
    std::size_t off = 0;
    if (!readU32Le(bytes, off, header.magic) ||
        (header.magic != kPageMagic && header.magic != kPageMagicV2)) {
        return false;
    }
    return readU32Le(bytes, off, header.signalId) && readU64Le(bytes, off, header.baseMs) &&
           readU32Le(bytes, off, header.sampleCount) && readU8(bytes, off, header.tEnc) &&
           readU8(bytes, off, header.vEnc) && readU8(bytes, off, header.level) && readU8(bytes, off, header.flags) &&
           readU32Le(bytes, off, header.spanMs) && readU32Le(bytes, off, header.payloadBytes);
}

std::vector<std::uint8_t> encodeRawPage(std::uint32_t signalId, std::span<const std::uint64_t> times,
                                        std::span<const std::int64_t> values, bool lastRun) {
    const std::size_t n = std::min(times.size(), values.size());
    PageHeader hdr{};
    hdr.signalId = signalId;
    hdr.sampleCount = static_cast<std::uint32_t>(n);
    hdr.level = 0;
    hdr.flags = kFlagDense;
    if (lastRun) {
        hdr.flags = static_cast<std::uint8_t>(hdr.flags | kFlagLastRun);
    }
    if (n > 0) {
        hdr.baseMs = times[0];
        const auto span = times[n - 1] - times[0];
        hdr.spanMs = span > 0xffffffffull ? 0xffffffffu : static_cast<std::uint32_t>(span);
    }

    bool constant = n > 0;
    for (std::size_t i = 1; i < n; ++i) {
        if (values[i] != values[0]) {
            constant = false;
            break;
        }
    }
    bool stride = n >= 2 && hdr.spanMs != 0xffffffffu;
    std::int64_t step = n >= 2 ? static_cast<std::int64_t>(times[1] - times[0]) : 0;
    for (std::size_t i = 2; i < n; ++i) {
        if (static_cast<std::int64_t>(times[i] - times[i - 1]) != step) {
            stride = false;
            break;
        }
    }

    hdr.tEnc = stride ? kTEncStride : kTEncDelta;
    hdr.vEnc = constant ? kVEncConstant : kVEncZigzag;

    std::vector<std::uint8_t> payload;
    if (hdr.tEnc == kTEncDelta && n >= 2) {
        std::int64_t prevDelta = static_cast<std::int64_t>(times[1] - times[0]);
        writeZigzag(payload, prevDelta);
        for (std::size_t i = 2; i < n; ++i) {
            const auto delta = static_cast<std::int64_t>(times[i] - times[i - 1]);
            writeZigzag(payload, wrapSub(delta, prevDelta));
            prevDelta = delta;
        }
    }
    if (n > 0) {
        if (hdr.vEnc == kVEncConstant) {
            writeZigzag(payload, values[0]);
        } else {
            writeZigzag(payload, values[0]);
            for (std::size_t i = 1; i < n; ++i) {
                writeZigzag(payload, wrapSub(values[i], values[i - 1]));
            }
        }
    }
    hdr.payloadBytes = static_cast<std::uint32_t>(payload.size());

    std::vector<std::uint8_t> out;
    out.reserve(kPageHeaderBytes + payload.size());
    encodePageHeader(out, hdr);
    writeBytes(out, payload);
    return out;
}

bool decodeRawPage(std::span<const std::uint8_t> bytes, PageHeader& header, std::vector<std::uint64_t>& times,
                   std::vector<std::int64_t>& values) {
    times.clear();
    values.clear();
    if (!decodePageHeader(bytes, header) || header.level != 0) {
        return false;
    }
    if (bytes.size() < kPageHeaderBytes + header.payloadBytes) {
        return false;
    }
    const auto payload = bytes.subspan(kPageHeaderBytes, header.payloadBytes);
    std::size_t off = 0;
    const std::size_t n = header.sampleCount;
    times.resize(n);
    values.resize(n);
    if (n == 0) {
        return off == payload.size();
    }
    times[0] = header.baseMs;
    if (header.tEnc == kTEncStride) {
        const std::int64_t step = n > 1 ? static_cast<std::int64_t>(header.spanMs) / static_cast<std::int64_t>(n - 1)
                                        : 0;
        for (std::size_t i = 1; i < n; ++i) {
            times[i] = header.baseMs + static_cast<std::uint64_t>(step) * i;
        }
    } else if (header.tEnc == kTEncDelta) {
        if (n >= 2) {
            std::int64_t prevDelta = 0;
            if (!readZigzag(payload, off, prevDelta)) {
                return false;
            }
            times[1] = times[0] + static_cast<std::uint64_t>(prevDelta);
            for (std::size_t i = 2; i < n; ++i) {
                std::int64_t dod = 0;
                if (!readZigzag(payload, off, dod)) {
                    return false;
                }
                prevDelta = wrapAdd(prevDelta, dod);
                times[i] = times[i - 1] + static_cast<std::uint64_t>(prevDelta);
            }
        }
    } else {
        return false;
    }

    if (header.vEnc == kVEncConstant) {
        std::int64_t v = 0;
        if (!readZigzag(payload, off, v)) {
            return false;
        }
        std::fill(values.begin(), values.end(), v);
    } else if (header.vEnc == kVEncZigzag) {
        if (!readZigzag(payload, off, values[0])) {
            return false;
        }
        for (std::size_t i = 1; i < n; ++i) {
            std::int64_t d = 0;
            if (!readZigzag(payload, off, d)) {
                return false;
            }
            values[i] = wrapAdd(values[i - 1], d);
        }
    } else {
        return false;
    }
    return off == payload.size();
}

std::vector<std::uint8_t> encodeRollupPage(std::uint32_t signalId, std::uint8_t level, std::uint64_t baseMs,
                                           std::uint32_t bucketMs, std::span<const RollupBucket> slots,
                                           bool lastRun, bool signedSum) {
    PageHeader hdr{};
    hdr.magic = kPageMagicV2;
    hdr.signalId = signalId;
    hdr.baseMs = baseMs;
    hdr.sampleCount = static_cast<std::uint32_t>(slots.size());
    hdr.tEnc = kTEncImplicit;
    hdr.vEnc = kVEncZigzag;
    hdr.level = level;
    hdr.flags = 0;
    if (lastRun) {
        hdr.flags = static_cast<std::uint8_t>(hdr.flags | kFlagLastRun);
    }
    if (!slots.empty()) {
        const auto last = slots.back().startMs;
        const auto span = last >= baseMs ? last - baseMs : 0;
        hdr.spanMs = span > 0xffffffffull ? 0xffffffffu : static_cast<std::uint32_t>(span);
    }

    bool dense = true;
    for (const auto& s : slots) {
        if (!s.present || s.count == 0) {
            dense = false;
            break;
        }
    }
    if (dense) {
        hdr.flags = static_cast<std::uint8_t>(hdr.flags | kFlagDense);
    }

    std::vector<std::uint8_t> payload;
    if (!dense) {
        const std::size_t bytes = (slots.size() + 7) / 8;
        payload.resize(bytes, 0);
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].present && slots[i].count > 0) {
                payload[i / 8] = static_cast<std::uint8_t>(payload[i / 8] | (1u << (i % 8)));
            }
        }
    }

    auto writeDeltaStream = [&](auto getter) {
        bool have = false;
        std::int64_t prev = 0;
        for (const auto& s : slots) {
            if (!dense && (!s.present || s.count == 0)) {
                continue;
            }
            const std::int64_t cur = getter(s);
            if (!have) {
                writeZigzag(payload, cur);
                prev = cur;
                have = true;
            } else {
                writeZigzag(payload, wrapSub(cur, prev));
                prev = cur;
            }
        }
    };
    bool wideSum = false;
    for (const auto& s : slots) {
        if (!s.present || s.count == 0) {
            continue;
        }
        if (signedSum) {
            if (signedNeedsWide(s)) {
                wideSum = true;
                break;
            }
        } else if (s.wide || s.sumHi != 0) {
            wideSum = true;
            break;
        }
    }
    if (wideSum) {
        hdr.flags = static_cast<std::uint8_t>(hdr.flags | kFlagWideSum);
    }

    writeDeltaStream([](const RollupBucket& s) { return s.min; });
    writeDeltaStream([](const RollupBucket& s) { return s.max; });
    writeDeltaStream([](const RollupBucket& s) { return s.first; });
    writeDeltaStream([](const RollupBucket& s) { return s.last; });
    writeDeltaStream([](const RollupBucket& s) { return s.sum; });
    if (wideSum) {
        writeDeltaStream([](const RollupBucket& s) { return static_cast<std::int64_t>(s.sumHi); });
    }
    for (const auto& s : slots) {
        if (!dense && (!s.present || s.count == 0)) {
            continue;
        }
        writeVarint(payload, s.count);
    }

    hdr.payloadBytes = static_cast<std::uint32_t>(payload.size());
    (void)bucketMs;
    std::vector<std::uint8_t> out;
    out.reserve(kPageHeaderBytes + payload.size());
    encodePageHeader(out, hdr);
    writeBytes(out, payload);
    return out;
}

bool decodeRollupPage(std::span<const std::uint8_t> bytes, PageHeader& header, std::uint32_t bucketMs,
                      std::vector<RollupBucket>& slots) {
    slots.clear();
    if (!decodePageHeader(bytes, header) || header.level == 0) {
        return false;
    }
    if (bytes.size() < kPageHeaderBytes + header.payloadBytes) {
        return false;
    }
    const auto payload = bytes.subspan(kPageHeaderBytes, header.payloadBytes);
    std::size_t off = 0;
    const std::size_t n = header.sampleCount;
    const bool dense = (header.flags & kFlagDense) != 0;
    std::vector<std::uint8_t> bitmap;
    if (!dense) {
        const std::size_t bytesNeeded = (n + 7) / 8;
        if (off + bytesNeeded > payload.size()) {
            return false;
        }
        bitmap.assign(payload.begin() + static_cast<std::ptrdiff_t>(off),
                      payload.begin() + static_cast<std::ptrdiff_t>(off + bytesNeeded));
        off += bytesNeeded;
    }
    auto presentAt = [&](std::size_t i) {
        if (dense) {
            return true;
        }
        return i < n && (bitmap[i / 8] & static_cast<std::uint8_t>(1u << (i % 8))) != 0;
    };

    slots.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        slots[i].startMs = header.baseMs + static_cast<std::uint64_t>(bucketMs) * i;
        slots[i].present = presentAt(i);
    }

    auto readDeltaStream = [&](auto setter) -> bool {
        bool have = false;
        std::int64_t prev = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (!slots[i].present) {
                continue;
            }
            std::int64_t cur = 0;
            if (!have) {
                if (!readZigzag(payload, off, cur)) {
                    return false;
                }
                have = true;
                prev = cur;
            } else {
                std::int64_t d = 0;
                if (!readZigzag(payload, off, d)) {
                    return false;
                }
                cur = wrapAdd(prev, d);
                prev = cur;
            }
            setter(slots[i], cur);
        }
        return true;
    };
    if (!readDeltaStream([](RollupBucket& s, std::int64_t v) { s.min = v; })) {
        return false;
    }
    if (!readDeltaStream([](RollupBucket& s, std::int64_t v) { s.max = v; })) {
        return false;
    }
    if (!readDeltaStream([](RollupBucket& s, std::int64_t v) { s.first = v; })) {
        return false;
    }
    if (!readDeltaStream([](RollupBucket& s, std::int64_t v) { s.last = v; })) {
        return false;
    }
    if (!readDeltaStream([](RollupBucket& s, std::int64_t v) { s.sum = v; })) {
        return false;
    }
    if ((header.flags & kFlagWideSum) != 0) {
        if (!readDeltaStream([](RollupBucket& s, std::int64_t v) { s.sumHi = static_cast<std::uint64_t>(v); })) {
            return false;
        }
        for (auto& s : slots) {
            if (s.present) {
                s.wide = true;
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!slots[i].present) {
            continue;
        }
        std::uint64_t c = 0;
        if (!readVarint(payload, off, c)) {
            return false;
        }
        slots[i].count = static_cast<std::uint32_t>(c);
    }
    return off == payload.size();
}

std::vector<std::uint8_t> encodeIndex(const IndexHeader& header, std::span<const IndexRecord> records) {
    std::vector<std::uint8_t> out;
    out.reserve(kIndexHeaderBytes + records.size() * kIndexRecordBytes);
    writeU32Le(out, header.magic);
    writeU16Le(out, header.version);
    writeU8(out, header.level);
    writeU8(out, header.reserved0);
    writeU16Le(out, header.recordBytes);
    writeU16Le(out, header.reserved1);
    writeU32Le(out, static_cast<std::uint32_t>(records.size()));
    writeU64Le(out, header.minMs);
    writeU64Le(out, header.maxMs);
    writeU32Le(out, header.bucketMs);
    writeU32Le(out, header.signalCount);
    while (out.size() < kIndexHeaderBytes) {
        writeU8(out, 0);
    }
    for (const auto& r : records) {
        writeU32Le(out, r.signalId);
        writeU64Le(out, r.startMs);
        writeU64Le(out, r.endMs);
        writeU32Le(out, r.fileNo);
        writeU32Le(out, r.offset);
        writeU32Le(out, r.pageBytes);
        writeU32Le(out, r.sampleCount);
        writeF32Le(out, r.vMin);
        writeF32Le(out, r.vMax);
        writeU32Le(out, r.reserved);
    }
    return out;
}

bool decodeIndex(std::span<const std::uint8_t> bytes, IndexHeader& header, std::vector<IndexRecord>& records) {
    records.clear();
    if (bytes.size() < kIndexHeaderBytes) {
        return false;
    }
    std::size_t off = 0;
    if (!readU32Le(bytes, off, header.magic) || header.magic != kIndexMagic) {
        return false;
    }
    if (!readU16Le(bytes, off, header.version) || header.version != kIndexVersion) {
        return false;
    }
    if (!readU8(bytes, off, header.level) || !readU8(bytes, off, header.reserved0) ||
        !readU16Le(bytes, off, header.recordBytes) || !readU16Le(bytes, off, header.reserved1) ||
        !readU32Le(bytes, off, header.recordCount) || !readU64Le(bytes, off, header.minMs) ||
        !readU64Le(bytes, off, header.maxMs) || !readU32Le(bytes, off, header.bucketMs) ||
        !readU32Le(bytes, off, header.signalCount)) {
        return false;
    }
    off = kIndexHeaderBytes;
    if (header.recordBytes != kIndexRecordBytes) {
        return false;
    }
    if (bytes.size() < kIndexHeaderBytes + static_cast<std::size_t>(header.recordCount) * kIndexRecordBytes) {
        return false;
    }
    records.resize(header.recordCount);
    for (auto& r : records) {
        if (!readU32Le(bytes, off, r.signalId) || !readU64Le(bytes, off, r.startMs) ||
            !readU64Le(bytes, off, r.endMs) || !readU32Le(bytes, off, r.fileNo) || !readU32Le(bytes, off, r.offset) ||
            !readU32Le(bytes, off, r.pageBytes) || !readU32Le(bytes, off, r.sampleCount) ||
            !readF32Le(bytes, off, r.vMin) || !readF32Le(bytes, off, r.vMax) || !readU32Le(bytes, off, r.reserved)) {
            return false;
        }
    }
    return true;
}

std::size_t indexLowerBound(std::span<const IndexRecord> records, std::uint32_t signalId, std::uint64_t startMs) {
    const auto it = std::lower_bound(records.begin(), records.end(), startMs,
                                     [signalId](const IndexRecord& r, std::uint64_t t) {
                                         if (r.signalId != signalId) {
                                             return r.signalId < signalId;
                                         }
                                         return r.startMs < t;
                                     });
    return static_cast<std::size_t>(it - records.begin());
}

bool upsertCatalog(const std::filesystem::path& root, std::string sessionId, std::uint64_t minMs, std::uint64_t maxMs,
                   std::string dbcSha, std::string* error) {
    if (sessionId.empty()) {
        if (error) {
            *error = "empty catalog session id";
        }
        return false;
    }
    struct CatalogRow {
        std::string id;
        std::string path;
        std::string dbcSha;
        std::uint64_t minMs = 0;
        std::uint64_t maxMs = 0;
    };
    std::vector<CatalogRow> rows;
    std::vector<std::uint8_t> catBytes;
    const auto catalogPath = root / "catalog.json";
    if (detail::readFile(catalogPath, catBytes)) {
        // A concurrent writer used to truncate this file in place. A reader that
        // saw the empty or half-written text treated it as "no sessions" and
        // then saved that, which dropped every other log.
        if (catBytes.empty()) {
            if (error) {
                *error = "catalog.json is empty; left it unchanged";
            }
            return false;
        }
        const auto text = std::string(catBytes.begin(), catBytes.end());
        const auto parsed = json::parse(text);
        if (!parsed || !parsed->isObject() || !(*parsed)["sessions"].isArray()) {
            if (error) {
                *error = "catalog.json is unreadable; left it unchanged";
            }
            return false;
        }
        for (const auto& item : (*parsed)["sessions"].asArray()) {
            CatalogRow r;
            r.id = item["id"].asString();
            if (r.id.empty()) {
                continue;
            }
            r.path = item["path"].asString();
            r.dbcSha = item["dbc_sha"].asString();
            r.minMs = static_cast<std::uint64_t>(item["min_ms"].asInt());
            r.maxMs = static_cast<std::uint64_t>(item["max_ms"].asInt());
            rows.push_back(std::move(r));
        }
    }
    CatalogRow r;
    r.id = std::move(sessionId);
    r.path = (root / r.id).string();
    r.dbcSha = std::move(dbcSha);
    r.minMs = minMs;
    r.maxMs = maxMs;
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const CatalogRow& x) { return x.id == r.id; });
    if (it != rows.end()) {
        *it = std::move(r);
    } else {
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [](const CatalogRow& a, const CatalogRow& b) {
        if (a.minMs != b.minMs) {
            return a.minMs < b.minMs;
        }
        return a.id < b.id;
    });
    json::Object catalog;
    catalog.emplace("version", json::Value(1));
    json::Array sessions;
    for (const auto& row : rows) {
        json::Object obj;
        obj.emplace("id", json::Value(row.id));
        obj.emplace("path", json::Value(row.path));
        obj.emplace("min_ms", json::Value(row.minMs));
        obj.emplace("max_ms", json::Value(row.maxMs));
        obj.emplace("dbc_sha", json::Value(row.dbcSha));
        sessions.emplace_back(std::move(obj));
    }
    catalog.emplace("sessions", json::Value(std::move(sessions)));
    const auto text = json::stringify(json::Value(std::move(catalog)));
    return detail::writeFileAtomic(catalogPath,
                                   std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                                                 text.size()),
                                   error);
}

std::vector<CatalogListing> listCatalog(const std::filesystem::path& root) {
    struct CatalogRow {
        std::string id;
        std::string path;
        std::string dbcSha;
        std::uint64_t minMs = 0;
        std::uint64_t maxMs = 0;
    };
    std::vector<CatalogRow> rows;
    std::vector<std::uint8_t> catBytes;
    if (detail::readFile(root / "catalog.json", catBytes)) {
        const auto text = std::string(catBytes.begin(), catBytes.end());
        if (const auto parsed = json::parse(text); parsed && parsed->isObject()) {
            const auto& arr = (*parsed)["sessions"];
            if (arr.isArray()) {
                for (const auto& item : arr.asArray()) {
                    CatalogRow r;
                    r.id = item["id"].asString();
                    if (r.id.empty()) {
                        continue;
                    }
                    r.path = item["path"].asString();
                    r.dbcSha = item["dbc_sha"].asString();
                    if (r.dbcSha.empty()) {
                        r.dbcSha = item["dbc_hash"].asString();
                    }
                    r.minMs = static_cast<std::uint64_t>(item["min_ms"].asInt());
                    r.maxMs = static_cast<std::uint64_t>(item["max_ms"].asInt());
                    rows.push_back(std::move(r));
                }
            }
        }
    }
    std::vector<CatalogListing> out;
    out.reserve(rows.size());
    for (const auto& row : rows) {
        CatalogListing listing;
        listing.id = row.id;
        listing.path = row.path.empty() ? (root / row.id).string() : row.path;
        listing.dbcSha = row.dbcSha;
        listing.minMs = row.minMs;
        listing.maxMs = row.maxMs;
        const auto sessionDir = std::filesystem::path(listing.path);
        SessionManifest man;
        if (detail::loadSessionJson(sessionDir, man, nullptr)) {
            listing.sampleCount = man.samples;
            if (listing.dbcSha.empty()) {
                listing.dbcSha = man.dbcSha;
            }
            if (listing.minMs == 0 && listing.maxMs == 0) {
                listing.minMs = man.minMs;
                listing.maxMs = man.maxMs;
            }
        }
        listing.hasVideo = videoVptOverlapsRange(sessionDir / "video.vpt", listing.minMs, listing.maxMs);
        out.push_back(std::move(listing));
    }
    return out;
}

namespace {

constexpr std::uint32_t kVptMagic = 0x56505431u;
constexpr std::uint16_t kVptVersion = 1;
constexpr std::uint16_t kVptHeaderSize = 64;
constexpr std::uint16_t kVptRowSize = 40;

bool readVptFile(const std::filesystem::path& vpt, std::vector<std::uint8_t>& bytes, std::uint16_t& headerBytes,
                 std::uint16_t& rowBytes, std::uint32_t& rowCount) {
    if (!readFile(vpt, bytes) || bytes.size() < kVptHeaderSize) {
        return false;
    }
    std::size_t off = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    if (!readU32Be(bytes, off, magic) || magic != kVptMagic) {
        return false;
    }
    if (!readU16Be(bytes, off, version) || version != kVptVersion) {
        return false;
    }
    if (!readU16Be(bytes, off, headerBytes) || !readU16Be(bytes, off, rowBytes)) {
        return false;
    }
    off = 12;
    if (!readU32Be(bytes, off, rowCount)) {
        return false;
    }
    return headerBytes >= kVptHeaderSize && rowBytes == kVptRowSize;
}

}  // namespace

bool videoVptOverlapsRange(const std::filesystem::path& vpt, std::uint64_t minMs, std::uint64_t maxMs) {
    if (minMs != 0 && maxMs < minMs) {
        return false;
    }
    std::vector<std::uint8_t> bytes;
    std::uint16_t headerBytes = 0;
    std::uint16_t rowBytes = 0;
    std::uint32_t rowCount = 0;
    if (!readVptFile(vpt, bytes, headerBytes, rowBytes, rowCount) || rowCount == 0) {
        return false;
    }
    const auto rowAt = [&](std::uint32_t i, std::uint64_t& startMs, std::uint64_t& endMs) -> bool {
        std::size_t off = static_cast<std::size_t>(headerBytes) + static_cast<std::size_t>(i) * rowBytes;
        return readU64Be(bytes, off, startMs) && readU64Be(bytes, off, endMs);
    };
    std::uint32_t lo = 0;
    std::uint32_t hi = rowCount;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        std::uint64_t startMs = 0;
        std::uint64_t endMs = 0;
        if (!rowAt(mid, startMs, endMs)) {
            return false;
        }
        if (endMs < minMs) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo >= rowCount) {
        return false;
    }
    std::uint64_t startMs = 0;
    std::uint64_t endMs = 0;
    if (!rowAt(lo, startMs, endMs)) {
        return false;
    }
    const std::uint64_t hiMs = maxMs == 0 ? startMs : maxMs;
    return startMs <= hiMs;
}

bool copySessionVideoVpt(const std::filesystem::path& src, const std::filesystem::path& dest, std::uint64_t minMs,
                         std::uint64_t maxMs) {
    std::vector<std::uint8_t> bytes;
    std::uint16_t headerBytes = 0;
    std::uint16_t rowBytes = 0;
    std::uint32_t rowCount = 0;
    if (!readVptFile(src, bytes, headerBytes, rowBytes, rowCount)) {
        return false;
    }
    std::vector<std::uint8_t> kept;
    kept.reserve(static_cast<std::size_t>(rowCount) * rowBytes);
    std::uint64_t spanMin = 0;
    std::uint64_t spanMax = 0;
    std::uint32_t n = 0;
    std::size_t off = headerBytes;
    for (std::uint32_t i = 0; i < rowCount; ++i) {
        const std::size_t rowOff = off;
        std::uint64_t startMs = 0;
        std::uint64_t endMs = 0;
        if (!readU64Be(bytes, off, startMs) || !readU64Be(bytes, off, endMs)) {
            return false;
        }
        off = rowOff + rowBytes;
        const bool overlap = endMs >= minMs && (maxMs == 0 || startMs <= maxMs);
        if (!overlap) {
            continue;
        }
        kept.insert(kept.end(), bytes.begin() + static_cast<std::ptrdiff_t>(rowOff),
                    bytes.begin() + static_cast<std::ptrdiff_t>(rowOff + rowBytes));
        if (n == 0 || startMs < spanMin) {
            spanMin = startMs;
        }
        if (n == 0 || endMs > spanMax) {
            spanMax = endMs;
        }
        ++n;
    }
    if (n == 0) {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
        return false;
    }
    std::int64_t clockOff = 0;
    std::uint8_t prov = 0;
    std::size_t hdrOff = 32;
    std::uint64_t clockBits = 0;
    if (readU64Be(bytes, hdrOff, clockBits)) {
        clockOff = static_cast<std::int64_t>(clockBits);
    }
    if (bytes.size() > 40) {
        prov = bytes[40];
    }
    std::vector<std::uint8_t> out;
    out.resize(kVptHeaderSize, 0);
    std::size_t w = 0;
    auto poke32 = [&](std::size_t at, std::uint32_t v) {
        out[at] = static_cast<std::uint8_t>(v >> 24);
        out[at + 1] = static_cast<std::uint8_t>(v >> 16);
        out[at + 2] = static_cast<std::uint8_t>(v >> 8);
        out[at + 3] = static_cast<std::uint8_t>(v);
    };
    auto poke16 = [&](std::size_t at, std::uint16_t v) {
        out[at] = static_cast<std::uint8_t>(v >> 8);
        out[at + 1] = static_cast<std::uint8_t>(v);
    };
    auto poke64 = [&](std::size_t at, std::uint64_t v) {
        poke32(at, static_cast<std::uint32_t>(v >> 32));
        poke32(at + 4, static_cast<std::uint32_t>(v));
    };
    poke32(0, kVptMagic);
    poke16(4, kVptVersion);
    poke16(6, kVptHeaderSize);
    poke16(8, kVptRowSize);
    poke32(12, n);
    poke64(16, spanMin);
    poke64(24, spanMax);
    poke64(32, static_cast<std::uint64_t>(clockOff));
    out[40] = prov;
    (void)w;
    out.insert(out.end(), kept.begin(), kept.end());
    return writeFileAtomic(dest, out, nullptr);
}

std::string sessionIdFromMs(std::uint64_t timestampMs, int seq) {
    const auto sec = static_cast<std::time_t>(timestampMs / 1000);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &sec);
#else
    gmtime_r(&sec, &utc);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d-%02d", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec, seq);
    return buf;
}

double physical(const SignalInfo& sig, std::int64_t raw) {
    dbc::Signal tmp;
    tmp.factor = sig.factor;
    tmp.offset = sig.offset;
    tmp.valueType = sig.valueType;
    tmp.isSigned = sig.isSigned;
    tmp.length = sig.bits;
    return dbc::physicalFromRaw(tmp, raw);
}

const SignalInfo* Store::signal(std::uint32_t id) const {
    const auto it = signals_.find(id);
    return it == signals_.end() ? nullptr : &it->second;
}

const std::vector<IndexRecord>& Store::index(int level) const {
    static const std::vector<IndexRecord> empty;
    if (level < 0 || level >= kLevelCount) {
        return empty;
    }
    return indexes_[level];
}

Store::Builder& Store::builder(std::uint32_t signalId) {
    auto it = builders_.find(signalId);
    if (it != builders_.end()) {
        return it->second;
    }
    Builder b;
    b.signalId = signalId;
    for (int L = 1; L < kLevelCount; ++L) {
        b.rollups[L].level = static_cast<std::uint8_t>(L);
        b.rollups[L].bucketMs = bucketMsOf(L);
    }
    auto [ins, _] = builders_.emplace(signalId, std::move(b));
    return ins->second;
}

void Store::resetRawEnc(Builder& b) {
    b.rawConstant = true;
    b.rawStride = true;
    b.rawSpanSaturated = false;
    b.rawFirstT = 0;
    b.rawPrevT = 0;
    b.rawFirstV = 0;
    b.rawPrevV = 0;
    b.rawFirstDelta = 0;
    b.rawPrevDelta = 0;
    b.rawTBytes = 0;
    b.rawVConstBytes = 0;
    b.rawVZigzagBytes = 0;
}

void Store::noteRawSample(Builder& b, std::uint64_t timestampMs, std::int64_t raw) {
    if (b.times.size() == 1) {
        b.rawFirstT = timestampMs;
        b.rawPrevT = timestampMs;
        b.rawFirstV = raw;
        b.rawPrevV = raw;
        b.rawConstant = true;
        b.rawStride = true;
        b.rawSpanSaturated = false;
        b.rawTBytes = 0;
        b.rawVConstBytes = zigzagSizeOf(raw);
        b.rawVZigzagBytes = zigzagSizeOf(raw);
        return;
    }
    if (timestampMs - b.rawFirstT > 0xffffffffull) {
        b.rawSpanSaturated = true;
    }
    const auto delta = static_cast<std::int64_t>(timestampMs - b.rawPrevT);
    if (b.times.size() == 2) {
        b.rawFirstDelta = delta;
        b.rawPrevDelta = delta;
        b.rawTBytes = zigzagSizeOf(delta);
    } else {
        b.rawTBytes += zigzagSizeOf(wrapSub(delta, b.rawPrevDelta));
        if (delta != b.rawFirstDelta) {
            b.rawStride = false;
        }
        b.rawPrevDelta = delta;
    }
    if (raw != b.rawFirstV) {
        b.rawConstant = false;
    }
    b.rawVZigzagBytes += zigzagSizeOf(wrapSub(raw, b.rawPrevV));
    b.rawPrevT = timestampMs;
    b.rawPrevV = raw;
}

std::size_t Store::rawEncodedPayload(const Builder& b) const {
    const auto n = b.times.size();
    if (n == 0) {
        return 0;
    }
    const bool stride = n >= 2 && b.rawStride && !b.rawSpanSaturated;
    const auto tBytes = (n >= 2 && !stride) ? b.rawTBytes : 0;
    const auto vBytes = b.rawConstant ? b.rawVConstBytes : b.rawVZigzagBytes;
    return tBytes + vBytes;
}

bool Store::create(const std::filesystem::path& sessionDir, SessionManifest manifest, std::string* error) {
    close();
    finalDir_ = sessionDir;
    // Build beside the target and rename on success. Writing in place would
    // interleave new pages with an existing session's .pcol files while the
    // fresh index claims offsets from zero, so every record would point into
    // stale bytes.
    dir_ = sessionDir;
    dir_ += ".building";
    manifest_ = std::move(manifest);
    writable_ = true;
    finished_ = false;
    writeFailed_ = false;
    writeError_.clear();
    std::error_code ec;
    lockPath_ = dir_ / "WRITER.lock";
    if (std::filesystem::exists(lockPath_, ec)) {
        if (error) {
            *error = "writer lock exists (another build, or a crashed one to remove): " + lockPath_.string();
        }
        lockPath_.clear();
        return false;
    }
    std::filesystem::remove_all(dir_, ec);
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        if (error) {
            *error = "cannot create " + dir_.string();
        }
        return false;
    }
    std::ofstream lock(lockPath_, std::ios::binary | std::ios::trunc);
    if (!lock) {
        if (error) {
            *error = "cannot create writer lock";
        }
        return false;
    }
    lock << "photon-tsdb\n";
    for (int L = 0; L < kLevelCount; ++L) {
        files_[L] = {};
        indexes_[L].clear();
        sortedCount_[L] = 0;
        const auto sub = dir_ / ("l" + std::to_string(L));
        std::filesystem::create_directories(sub, ec);
    }
    return true;
}

bool Store::createLive(const std::filesystem::path& sessionDir, SessionManifest manifest, LiveOptions options,
                       std::string* error) {
    // A live session is published from the first checkpoint rather than by an
    // atomic rename at the end: it has no end, and a reader that cannot see it
    // until the car stops is not a live tail. The .building-plus-rename model
    // still guards batch builds, which do have an end.
    std::error_code probeEc;
    if (std::filesystem::exists(sessionDir / "session.json", probeEc)) {
        if (error) {
            *error = "refusing to open a live writer over an existing session: " + sessionDir.string();
        }
        return false;
    }
    close();
    finalDir_ = sessionDir;
    dir_ = sessionDir;
    manifest_ = std::move(manifest);
    manifest_.live = true;
    writable_ = true;
    finished_ = false;
    writeFailed_ = false;
    writeError_.clear();
    live_ = true;
    liveOpts_ = options;
    lastCheckpointMs_ = 0;
    checkpoints_ = 0;
    std::error_code ec;
    lockPath_ = dir_ / "WRITER.lock";
    if (std::filesystem::exists(lockPath_, ec)) {
        if (error) {
            *error = "writer lock exists (another writer, or a crashed one to recover): " + lockPath_.string();
        }
        lockPath_.clear();
        live_ = false;
        writable_ = false;
        return false;
    }
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        if (error) {
            *error = "cannot create " + dir_.string();
        }
        live_ = false;
        writable_ = false;
        return false;
    }
    {
        std::ofstream lock(lockPath_, std::ios::binary | std::ios::trunc);
        if (!lock) {
            if (error) {
                *error = "cannot create writer lock";
            }
            live_ = false;
            writable_ = false;
            return false;
        }
        lock << "photon-tsdb live\n";
    }
    for (int L = 0; L < kLevelCount; ++L) {
        files_[L] = {};
        indexes_[L].clear();
        sortedCount_[L] = 0;
        std::filesystem::create_directories(dir_ / ("l" + std::to_string(L)), ec);
    }
    // Write the manifests straight away so Store::open succeeds on an empty
    // live session instead of failing until the first sample lands.
    return writeIndexes(error) && writeManifests(error);
}

void Store::sortIndexTails() {
    for (int L = 0; L < kLevelCount; ++L) {
        auto& recs = indexes_[L];
        auto sorted = sortedCount_[L];
        if (sorted > recs.size()) {
            sorted = 0;
        }
        if (sorted == recs.size()) {
            continue;
        }
        const auto less = [](const IndexRecord& a, const IndexRecord& b) {
            if (a.signalId != b.signalId) {
                return a.signalId < b.signalId;
            }
            return a.startMs < b.startMs;
        };
        std::sort(recs.begin() + static_cast<std::ptrdiff_t>(sorted), recs.end(), less);
        if (sorted > 0) {
            std::inplace_merge(recs.begin(), recs.begin() + static_cast<std::ptrdiff_t>(sorted), recs.end(), less);
        }
        sortedCount_[L] = recs.size();
    }
}

bool Store::tick(std::uint64_t nowMs, std::string* error) {
    if (!live_ || !writable_ || finished_) {
        return true;
    }
    if (writeFailed_) {
        if (error) {
            *error = writeError_;
        }
        return false;
    }
    if (lastCheckpointMs_ == 0) {
        lastCheckpointMs_ = nowMs;
        return true;
    }
    if (nowMs < lastCheckpointMs_) {
        return true;
    }
    // A checkpoint rewrites every level index in full, so its cost tracks the
    // session's page count. Stretch the interval to hold that rewrite under the
    // configured byte rate; otherwise a multi-hour session spends most of its
    // time re-serialising an index nobody has read.
    std::uint64_t indexBytes = 0;
    for (int L = 0; L < kLevelCount; ++L) {
        indexBytes += kIndexHeaderBytes + indexes_[L].size() * kIndexRecordBytes;
    }
    std::uint64_t interval = liveOpts_.checkpointMs;
    if (liveOpts_.indexRewriteBudgetBps > 0) {
        // Not auto: uint64_t * 1000ull is unsigned long long, a different type from
        // uint64_t (unsigned long) on Linux, and std::max needs the two to match.
        const std::uint64_t needed = indexBytes * 1000ull / liveOpts_.indexRewriteBudgetBps;
        interval = std::max(interval, needed);
    }
    if (nowMs - lastCheckpointMs_ < interval) {
        return true;
    }
    return checkpoint(nowMs, error);
}

bool Store::checkpoint(std::uint64_t nowMs, std::string* error) {
    if (!live_ || !writable_ || finished_) {
        return true;
    }
    for (auto& [id, b] : builders_) {
        (void)id;
        if (!b.times.empty()) {
            const auto age = nowMs > b.times.front() ? nowMs - b.times.front() : 0;
            const bool enough = b.times.size() >= liveOpts_.pageMinSamples;
            const bool idle = age >= liveOpts_.pageMaxIdleMs;
            if ((enough && age >= liveOpts_.pageMaxAgeMs) || idle) {
                flushRaw(b, false);
            }
        }
        // Which rollup levels are worth writing is a rate question (§5.3) and a
        // live writer does not know the rate up front. Latch each level as soon
        // as the running estimate qualifies, on enough evidence not to be fooled
        // by two samples that happened to arrive close together.
        const double durS = b.lastMs > b.firstMs ? static_cast<double>(b.lastMs - b.firstMs) / 1000.0 : 0.0;
        if (b.count >= liveOpts_.rateMinSamples && b.lastMs - b.firstMs >= liveOpts_.rateMinSpanMs && durS > 0.0) {
            const double hz = static_cast<double>(b.count - 1) / durS;
            for (int L = 1; L < kLevelCount; ++L) {
                if (shouldBuildLevel(hz, L)) {
                    b.liveLevels = static_cast<std::uint8_t>(b.liveLevels | (1u << L));
                }
            }
        }
        for (int L = 1; L < kLevelCount; ++L) {
            if ((b.liveLevels & static_cast<std::uint8_t>(1u << L)) == 0) {
                continue;
            }
            auto& acc = b.rollups[L];
            if (acc.bucketMs == 0) {
                continue;
            }
            // Feed time has moved past the end of the open bucket, so nothing
            // more can land in it and it is safe to close. Emitting it earlier
            // would let a later sample open a second bucket on the same slot.
            if (acc.open && nowMs >= acc.cur.startMs + acc.bucketMs) {
                acc.pending.push_back(acc.cur);
                acc.open = false;
            }
            if (acc.pending.empty()) {
                continue;
            }
            const auto age = nowMs > acc.pending.front().startMs ? nowMs - acc.pending.front().startMs : 0;
            if (age >= liveOpts_.pageMaxAgeMs) {
                flushRollup(b, L, false, false);
            }
        }
    }
    if (writeFailed_) {
        if (error) {
            *error = writeError_.empty() ? "page write failed" : writeError_;
        }
        return false;
    }
    sortIndexTails();
    std::uint8_t levels = 0x01;
    for (auto& [id, b] : builders_) {
        const double durS = b.lastMs > b.firstMs ? static_cast<double>(b.lastMs - b.firstMs) / 1000.0 : 0.0;
        auto& info = signals_[id];
        info.id = id;
        // nativeHz keeps drifting as a live session runs, but nothing reads it
        // mid-session and republishing signals.json for a third decimal place
        // costs a file rename per checkpoint. Only a change to which levels
        // exist forces the rewrite; finish() publishes the final rate.
        info.nativeHz = (b.count > 1 && durS > 0.0) ? static_cast<double>(b.count - 1) / durS : 0.0;
        if (info.levels != b.liveLevels) {
            signalsDirty_ = true;
        }
        info.levels = b.liveLevels;
        levels = static_cast<std::uint8_t>(levels | b.liveLevels);
    }
    manifest_.builtLevels = static_cast<std::uint8_t>(manifest_.builtLevels | levels);
    ++manifest_.generation;
    // Indexes before the manifest. The other order lets a crash leave a
    // manifest claiming samples the index cannot locate, which reads as a hole;
    // this order can only leave indexed pages the manifest has not caught up to,
    // which reads as extra data at the tail.
    if (!writeIndexes(error, true) || !writeManifests(error, true)) {
        return false;
    }
    lastCheckpointMs_ = nowMs;
    ++checkpoints_;
    return true;
}

bool Store::open(const std::filesystem::path& sessionDir, std::string* error) {
    close();
    dir_ = sessionDir;
    finalDir_ = sessionDir;
    writable_ = false;
    finished_ = true;
    if (!detail::loadSessionJson(dir_, manifest_, error)) {
        return false;
    }
    std::vector<SignalInfo> sigs;
    if (detail::loadSignalsJson(dir_, sigs)) {
        for (auto& s : sigs) {
            signals_[s.id] = std::move(s);
        }
    }
    return loadIndexes(error);
}

bool Store::refresh(std::string* error) {
    if (writable_ || dir_.empty()) {
        return false;
    }
    SessionManifest fresh{};
    if (!detail::loadSessionJson(dir_, fresh, error)) {
        return false;
    }
    if (fresh.generation == manifest_.generation && fresh.samples == manifest_.samples) {
        return false;
    }
    manifest_ = fresh;
    signals_.clear();
    std::vector<SignalInfo> sigs;
    if (detail::loadSignalsJson(dir_, sigs)) {
        for (auto& s : sigs) {
            signals_[s.id] = std::move(s);
        }
    }
    return loadIndexes(error);
}

void Store::discard() {
    if (writable_ && !dir_.empty() && dir_ != finalDir_) {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    close();
}

void Store::close() {
    if (writable_ && !lockPath_.empty()) {
        std::error_code ec;
        std::filesystem::remove(lockPath_, ec);
    }
    builders_.clear();
    signals_.clear();
    for (int L = 0; L < kLevelCount; ++L) {
        indexes_[L].clear();
        files_[L] = {};
        sortedCount_[L] = 0;
    }
    writable_ = false;
    finished_ = false;
    live_ = false;
    liveOpts_ = {};
    lastCheckpointMs_ = 0;
    checkpoints_ = 0;
    signalsDirty_ = true;
    for (int L = 0; L < kLevelCount; ++L) {
        indexDirty_[L] = false;
    }
    writeFailed_ = false;
    writeError_.clear();
    dir_.clear();
    finalDir_.clear();
    lockPath_.clear();
    manifest_ = {};
}

void Store::setSignal(const SignalInfo& info) {
    signals_[info.id] = info;
    signalsDirty_ = true;
}

void Store::append(std::uint32_t signalId, std::uint64_t timestampMs, std::int64_t raw) {
    if (!writable_ || finished_ || writeFailed_) {
        return;
    }
    auto& b = builder(signalId);
    if (b.count == 0) {
        b.firstMs = timestampMs;
        b.vMin = raw;
        b.vMax = raw;
    }
    b.lastMs = timestampMs;
    ++b.count;
    b.vMin = std::min(b.vMin, raw);
    b.vMax = std::max(b.vMax, raw);
    if (manifest_.minMs == 0 || timestampMs < manifest_.minMs) {
        manifest_.minMs = timestampMs;
    }
    if (timestampMs > manifest_.maxMs) {
        manifest_.maxMs = timestampMs;
    }
    ++manifest_.samples;
    b.times.push_back(timestampMs);
    b.values.push_back(raw);
    noteRawSample(b, timestampMs, raw);
    for (int L = 1; L < kLevelCount; ++L) {
        addRollup(b, L, timestampMs, raw);
    }
    if (b.times.size() >= kPageSampleCap ||
        (b.times.size() >= 16 && kPageHeaderBytes + rawEncodedPayload(b) >= kPagePayloadCap)) {
        flushRaw(b, false);
    }
}

void Store::addRollup(Builder& b, int level, std::uint64_t t, std::int64_t raw) {
    auto& acc = b.rollups[level];
    if (acc.bucketMs == 0) {
        return;
    }
    const auto start = (t / acc.bucketMs) * acc.bucketMs;
    if (!acc.open || acc.cur.startMs != start) {
        if (acc.open) {
            acc.pending.push_back(acc.cur);
            if (acc.pending.size() >= kPageSampleCap) {
                flushRollup(b, level, false);
            }
        }
        acc.cur = RollupBucket{start, raw, raw, raw, raw, raw, 0, 1, true, false};
        const auto* info = signal(b.signalId);
        if (info == nullptr || info->isSigned) {
            setSignedWide(acc.cur, raw);
        }
        acc.open = true;
        return;
    }
    const auto* info = signal(b.signalId);
    const bool isUnsigned = info != nullptr && !info->isSigned;
    if (isUnsigned) {
        acc.cur.min = uminRaw(acc.cur.min, raw);
        acc.cur.max = umaxRaw(acc.cur.max, raw);
        setWideSum(acc.cur, wideSumOf(acc.cur) + static_cast<std::uint64_t>(raw));
    } else {
        acc.cur.min = std::min(acc.cur.min, raw);
        acc.cur.max = std::max(acc.cur.max, raw);
        setSignedWide(acc.cur, signedWideOf(acc.cur) + raw);
    }
    acc.cur.last = raw;
    ++acc.cur.count;
}

void Store::flushRaw(Builder& b, bool lastRun) {
    if (b.times.empty()) {
        return;
    }
    const auto bytes = encodeRawPage(b.signalId, b.times, b.values, lastRun);
    IndexRecord rec;
    rec.signalId = b.signalId;
    rec.startMs = b.times.front();
    rec.endMs = b.times.back();
    rec.pageBytes = static_cast<std::uint32_t>(bytes.size());
    rec.sampleCount = static_cast<std::uint32_t>(b.times.size());
    rec.vMin = static_cast<float>(b.values.front());
    rec.vMax = rec.vMin;
    for (const auto v : b.values) {
        rec.vMin = std::min(rec.vMin, static_cast<float>(v));
        rec.vMax = std::max(rec.vMax, static_cast<float>(v));
    }
    if (!writePage(0, bytes, rec, &writeError_) && !writeFailed_) {
        writeFailed_ = true;
    }
    b.times.clear();
    b.values.clear();
    resetRawEnc(b);
}

void Store::flushRollup(Builder& b, int level, bool lastRun, bool closeOpen) {
    auto& acc = b.rollups[level];
    if (closeOpen && acc.open) {
        acc.pending.push_back(acc.cur);
        acc.open = false;
    }
    if (acc.pending.empty()) {
        return;
    }
    const auto first = acc.pending.front().startMs;
    const auto last = acc.pending.back().startMs;
    const auto slotsN = static_cast<std::size_t>((last - first) / acc.bucketMs + 1);
    std::vector<RollupBucket> slots(slotsN);
    for (std::size_t i = 0; i < slotsN; ++i) {
        slots[i].startMs = first + static_cast<std::uint64_t>(acc.bucketMs) * i;
        slots[i].present = false;
    }
    for (const auto& bk : acc.pending) {
        const auto idx = static_cast<std::size_t>((bk.startMs - first) / acc.bucketMs);
        if (idx < slots.size()) {
            slots[idx] = bk;
            slots[idx].present = true;
        }
    }
    const auto* info = signal(b.signalId);
    const bool signedSum = info == nullptr || info->isSigned;
    const auto bytes = encodeRollupPage(b.signalId, static_cast<std::uint8_t>(level), first, acc.bucketMs, slots,
                                        lastRun, signedSum);
    IndexRecord rec;
    rec.signalId = b.signalId;
    rec.startMs = first;
    rec.endMs = last + acc.bucketMs;
    rec.pageBytes = static_cast<std::uint32_t>(bytes.size());
    rec.sampleCount = static_cast<std::uint32_t>(slots.size());
    rec.vMin = static_cast<float>(acc.pending.front().min);
    rec.vMax = rec.vMin;
    for (const auto& bk : acc.pending) {
        rec.vMin = std::min(rec.vMin, static_cast<float>(bk.min));
        rec.vMax = std::max(rec.vMax, static_cast<float>(bk.max));
    }
    if (!writePage(level, bytes, rec, &writeError_) && !writeFailed_) {
        writeFailed_ = true;
    }
    acc.pending.clear();
}

bool Store::writePage(int level, std::span<const std::uint8_t> bytes, IndexRecord rec, std::string* error) {
    if (level < 0 || level >= kLevelCount) {
        return false;
    }
    auto& lf = files_[level];
    const auto sub = dir_ / ("l" + std::to_string(level));
    if (lf.path.empty() || lf.offset + bytes.size() > kColumnFileCap) {
        if (!lf.path.empty()) {
            ++lf.fileNo;
        }
        lf.offset = 0;
        lf.path = sub / (pad4(lf.fileNo) + ".pcol");
        std::filesystem::create_directories(sub);
    }
    // Truncate when starting a file so a recorded offset of zero always means
    // byte zero, whatever was there before.
    const auto mode = lf.offset == 0 ? std::ios::trunc : std::ios::app;
    std::ofstream out(lf.path, std::ios::binary | mode);
    if (!out) {
        if (error) {
            *error = "cannot append " + lf.path.string();
        }
        return false;
    }
    rec.fileNo = lf.fileNo;
    rec.offset = lf.offset;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        if (error) {
            *error = "short write " + lf.path.string();
        }
        return false;
    }
    lf.offset += static_cast<std::uint32_t>(bytes.size());
    indexes_[level].push_back(rec);
    indexDirty_[level] = true;
    return true;
}

bool Store::finish(std::string* error) {
    if (!writable_ || finished_) {
        return true;
    }
    for (auto& [id, b] : builders_) {
        (void)id;
        flushRaw(b, true);
        const double durS = b.lastMs > b.firstMs ? static_cast<double>(b.lastMs - b.firstMs) / 1000.0 : 0.0;
        const double hz = (b.count > 1 && durS > 0.0) ? static_cast<double>(b.count - 1) / durS : 0.0;
        auto& info = signals_[b.signalId];
        info.id = b.signalId;
        info.nativeHz = hz;
        // A live session may already have pages on disk for a level its final
        // rate estimate would not qualify. Those pages are indexed and correct,
        // so keep claiming the level rather than orphaning them.
        std::uint8_t built = live_ ? b.liveLevels : static_cast<std::uint8_t>(0x01);
        for (int L = 1; L < kLevelCount; ++L) {
            if (shouldBuildLevel(hz, L) || (built & static_cast<std::uint8_t>(1u << L)) != 0) {
                flushRollup(b, L, true);
                built = static_cast<std::uint8_t>(built | (1u << L));
            } else {
                b.rollups[L].pending.clear();
                b.rollups[L].open = false;
            }
        }
        info.levels = built;
        manifest_.builtLevels = static_cast<std::uint8_t>(manifest_.builtLevels | built);
    }
    builders_.clear();
    for (int L = 0; L < kLevelCount; ++L) {
        std::sort(indexes_[L].begin(), indexes_[L].end(), [](const IndexRecord& a, const IndexRecord& b) {
            if (a.signalId != b.signalId) {
                return a.signalId < b.signalId;
            }
            return a.startMs < b.startMs;
        });
        sortedCount_[L] = indexes_[L].size();
    }
    manifest_.live = false;
    ++manifest_.generation;
    // A page that failed to write must never get an index record pointing at
    // it. Refuse here rather than publishing a store that reads as valid.
    if (writeFailed_) {
        if (error) {
            *error = writeError_.empty() ? "page write failed" : writeError_;
        }
        return false;
    }
    if (!writeIndexes(error) || !writeManifests(error)) {
        return false;
    }
    std::error_code ec;
    if (!lockPath_.empty()) {
        std::filesystem::remove(lockPath_, ec);
        lockPath_.clear();
    }
    if (!finalDir_.empty() && finalDir_ != dir_) {
        // Directory rename on Windows fails transiently when a scanner still
        // holds a handle on a file we just wrote, so retry briefly before
        // treating it as fatal.
        std::error_code renameEc;
        for (int attempt = 0; attempt < 10; ++attempt) {
            renameEc.clear();
            std::filesystem::remove_all(finalDir_, ec);
            std::filesystem::rename(dir_, finalDir_, renameEc);
            if (!renameEc) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
        }
        if (renameEc) {
            if (error) {
                *error = "cannot publish " + dir_.string() + " -> " + finalDir_.string() + ": " + renameEc.message() +
                         " (code " + std::to_string(renameEc.value()) + ")";
            }
            return false;
        }
        dir_ = finalDir_;
    }
    finished_ = true;
    writable_ = false;
    return true;
}

bool Store::writeIndexes(std::string* error, bool dirtyOnly) {
    for (int L = 0; L < kLevelCount; ++L) {
        if (indexes_[L].empty() && L > 0) {
            continue;
        }
        if (dirtyOnly && !indexDirty_[L]) {
            continue;
        }
        // Atomic replace: a live reader may be loading this file right now, and
        // a truncate-then-write leaves it decoding a shorter but well-formed
        // index, which looks like data loss rather than an error.
        if (!detail::storeLevelIndex(dir_, L, indexes_[L], error)) {
            return false;
        }
        indexDirty_[L] = false;
    }
    return true;
}

bool Store::loadIndexes(std::string* error) {
    for (int L = 0; L < kLevelCount; ++L) {
        indexes_[L].clear();
        const auto path = dir_ / ("l" + std::to_string(L) + ".pidx");
        std::vector<std::uint8_t> bytes;
        if (!readFile(path, bytes)) {
            continue;
        }
        IndexHeader hdr{};
        if (!decodeIndex(bytes, hdr, indexes_[L])) {
            if (error) {
                *error = "bad index " + path.string();
            }
            return false;
        }
        if (!indexes_[L].empty()) {
            manifest_.builtLevels = static_cast<std::uint8_t>(manifest_.builtLevels | (1u << L));
        }
        sortedCount_[L] = indexes_[L].size();
    }
    for (auto& [id, s] : signals_) {
        if (s.levels != 0) {
            continue;
        }
        for (int L = 0; L < kLevelCount; ++L) {
            const auto& recs = indexes_[L];
            const auto i = indexLowerBound(recs, id, 0);
            if (i < recs.size() && recs[i].signalId == id) {
                s.levels = static_cast<std::uint8_t>(s.levels | (1u << L));
            }
        }
        if (s.levels == 0) {
            s.levels = 0x01;
        }
    }
    // An empty L0 index means retention dropped it (§11). Level availability is
    // read from signals.json, so a stale array that still claims L0 would send
    // every query to a level with no pages and return nothing. Availability is
    // derived from the index, not trusted from the manifest.
    if (indexes_[0].empty()) {
        manifest_.builtLevels = static_cast<std::uint8_t>(manifest_.builtLevels & ~0x01u);
        for (auto& [id, s] : signals_) {
            (void)id;
            s.levels = static_cast<std::uint8_t>(s.levels & ~0x01u);
        }
    }
    return true;
}

bool Store::writeManifests(std::string* error, bool signalsOnlyIfDirty) {
    if (!signalsOnlyIfDirty || signalsDirty_) {
        std::vector<SignalInfo> ordered;
        ordered.reserve(signals_.size());
        for (const auto& [id, s] : signals_) {
            (void)id;
            ordered.push_back(s);
        }
        // signals.json first: it is what levelsOf reads, so a reader that picks
        // up a new generation must never find a manifest promising a level whose
        // per-signal availability has not been published yet.
        if (!detail::storeSignalsJson(dir_, std::move(ordered), error)) {
            return false;
        }
        signalsDirty_ = false;
    }
    return detail::storeSessionJson(dir_, manifest_, error);
}

bool Store::readPageBytes(int level, const IndexRecord& rec, std::vector<std::uint8_t>& out) const {
    const auto path = dir_ / ("l" + std::to_string(level)) / (pad4(rec.fileNo) + ".pcol");
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(static_cast<std::streamoff>(rec.offset), std::ios::beg);
    out.resize(rec.pageBytes);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(rec.pageBytes));
    return static_cast<bool>(in);
}

std::uint8_t Store::levelsOf(std::uint32_t signalId) const {
    if (const auto* s = signal(signalId); s != nullptr && s->levels != 0) {
        return s->levels;
    }
    std::uint8_t bits = 0;
    for (int L = 0; L < kLevelCount; ++L) {
        const auto& recs = indexes_[L];
        const auto i = indexLowerBound(recs, signalId, 0);
        if (i < recs.size() && recs[i].signalId == signalId) {
            bits = static_cast<std::uint8_t>(bits | (1u << L));
        }
    }
    return bits == 0 ? static_cast<std::uint8_t>(0x01) : bits;
}

std::uint64_t Store::rawCount(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs) const {
    const auto& recs = indexes_[0];
    std::size_t i = indexLowerBound(recs, signalId, fromMs);
    if (i > 0 && recs[i - 1].signalId == signalId && recs[i - 1].endMs >= fromMs) {
        --i;
    }
    std::uint64_t n = 0;
    for (; i < recs.size() && recs[i].signalId == signalId && recs[i].startMs <= toMs; ++i) {
        const auto& rec = recs[i];
        if (rec.startMs >= fromMs && rec.endMs <= toMs) {
            n += rec.sampleCount;
            continue;
        }
        std::vector<std::uint8_t> bytes;
        if (!readPageBytes(0, rec, bytes)) {
            continue;
        }
        PageHeader hdr{};
        std::vector<std::uint64_t> t;
        std::vector<std::int64_t> v;
        if (!decodeRawPage(bytes, hdr, t, v)) {
            continue;
        }
        for (const auto ts : t) {
            if (ts >= fromMs && ts <= toMs) {
                ++n;
            }
        }
    }
    return n;
}

bool Store::hasLevel0(std::uint32_t signalId) const {
    const auto& recs = indexes_[0];
    const auto i = indexLowerBound(recs, signalId, 0);
    return i < recs.size() && recs[i].signalId == signalId;
}

// Boundary samples after retention has dropped L0. §6.1 wants raw samples with
// exact timestamps, and once L0 is gone there are none — but returning nothing
// is worse than returning something honest: every sparse signal would blank on
// seek, which is exactly the failure §6.1 exists to prevent. So reconstruct from
// the finest surviving level, from a bucket that lies wholly on the correct side
// of the boundary so no value from the wrong side of it can leak in, and mark the
// result approximate.
std::optional<RawSample> Store::priorFromRollup(std::uint32_t signalId, std::uint64_t atMs) const {
    const auto bits = levelsOf(signalId);
    for (int L = 1; L < kLevelCount; ++L) {
        if ((bits & static_cast<std::uint8_t>(1u << L)) == 0) {
            continue;
        }
        const auto bms = bucketMsOf(L);
        if (bms == 0) {
            continue;
        }
        const auto& recs = indexes_[L];
        std::size_t i = indexLowerBound(recs, signalId, atMs);
        // Walk back through this signal's pages until one yields a closed bucket
        // at or before atMs. A page may be entirely present-but-later.
        std::optional<RawSample> best;
        while (i > 0) {
            --i;
            if (recs[i].signalId != signalId) {
                break;
            }
            std::vector<std::uint8_t> bytes;
            if (!readPageBytes(L, recs[i], bytes)) {
                continue;
            }
            PageHeader hdr{};
            std::vector<RollupBucket> slots;
            if (!decodeRollupPage(bytes, hdr, bms, slots)) {
                continue;
            }
            for (const auto& slot : slots) {
                if (!slot.present || slot.count == 0) {
                    continue;
                }
                if (slot.startMs + bms > atMs) {
                    continue;
                }
                best = RawSample{slot.startMs + bms - 1, slot.last, true};
            }
            if (best) {
                return best;
            }
        }
    }
    return std::nullopt;
}

std::optional<RawSample> Store::nextFromRollup(std::uint32_t signalId, std::uint64_t atMs) const {
    const auto bits = levelsOf(signalId);
    for (int L = 1; L < kLevelCount; ++L) {
        if ((bits & static_cast<std::uint8_t>(1u << L)) == 0) {
            continue;
        }
        const auto bms = bucketMsOf(L);
        if (bms == 0) {
            continue;
        }
        const auto& recs = indexes_[L];
        std::size_t i = indexLowerBound(recs, signalId, atMs);
        if (i > 0 && recs[i - 1].signalId == signalId && recs[i - 1].endMs > atMs) {
            --i;
        }
        for (; i < recs.size() && recs[i].signalId == signalId; ++i) {
            std::vector<std::uint8_t> bytes;
            if (!readPageBytes(L, recs[i], bytes)) {
                continue;
            }
            PageHeader hdr{};
            std::vector<RollupBucket> slots;
            if (!decodeRollupPage(bytes, hdr, bms, slots)) {
                continue;
            }
            for (const auto& slot : slots) {
                if (!slot.present || slot.count == 0) {
                    continue;
                }
                if (slot.startMs < atMs) {
                    continue;
                }
                return RawSample{slot.startMs, slot.first, true};
            }
        }
    }
    return std::nullopt;
}

std::optional<RawSample> Store::prior(std::uint32_t signalId, std::uint64_t atMs) const {
    const auto& recs = indexes_[0];
    if (!hasLevel0(signalId)) {
        return priorFromRollup(signalId, atMs);
    }
    std::size_t i = indexLowerBound(recs, signalId, atMs);
    auto tryPage = [&](const IndexRecord& rec) -> std::optional<RawSample> {
        if (rec.startMs > atMs) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> bytes;
        if (!readPageBytes(0, rec, bytes)) {
            return std::nullopt;
        }
        PageHeader hdr{};
        std::vector<std::uint64_t> t;
        std::vector<std::int64_t> v;
        if (!decodeRawPage(bytes, hdr, t, v)) {
            return std::nullopt;
        }
        std::optional<RawSample> found;
        for (std::size_t k = 0; k < t.size(); ++k) {
            if (t[k] <= atMs) {
                found = RawSample{t[k], v[k]};
            }
        }
        return found;
    };
    if (i < recs.size() && recs[i].signalId == signalId && recs[i].startMs <= atMs) {
        if (auto hit = tryPage(recs[i])) {
            return hit;
        }
    }
    if (i > 0 && recs[i - 1].signalId == signalId) {
        return tryPage(recs[i - 1]);
    }
    return std::nullopt;
}

std::optional<RawSample> Store::next(std::uint32_t signalId, std::uint64_t atMs) const {
    const auto& recs = indexes_[0];
    if (!hasLevel0(signalId)) {
        return nextFromRollup(signalId, atMs);
    }
    std::size_t i = indexLowerBound(recs, signalId, atMs);
    auto tryPage = [&](const IndexRecord& rec) -> std::optional<RawSample> {
        if (rec.endMs < atMs && rec.startMs < atMs) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> bytes;
        if (!readPageBytes(0, rec, bytes)) {
            return std::nullopt;
        }
        PageHeader hdr{};
        std::vector<std::uint64_t> t;
        std::vector<std::int64_t> v;
        if (!decodeRawPage(bytes, hdr, t, v)) {
            return std::nullopt;
        }
        for (std::size_t k = 0; k < t.size(); ++k) {
            if (t[k] >= atMs) {
                return RawSample{t[k], v[k]};
            }
        }
        return std::nullopt;
    };
    if (i > 0 && recs[i - 1].signalId == signalId && recs[i - 1].endMs >= atMs) {
        if (auto hit = tryPage(recs[i - 1])) {
            return hit;
        }
    }
    for (; i < recs.size() && recs[i].signalId == signalId; ++i) {
        if (auto hit = tryPage(recs[i])) {
            return hit;
        }
    }
    return std::nullopt;
}

SeriesResult Store::querySignal(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                                std::uint32_t maxPoints, std::atomic<bool>* cancel) const {
    SeriesResult out;
    out.signalId = signalId;
    out.prior = prior(signalId, fromMs);
    out.next = next(signalId, toMs);
    const auto rawN = rawCount(signalId, fromMs, toMs);
    const int level = pickLevel(fromMs, toMs, maxPoints, rawN, levelsOf(signalId));
    out.level = level;
    out.bucketMs = bucketMsOf(level);
    const auto& recs = indexes_[level];
    std::size_t i = indexLowerBound(recs, signalId, fromMs);
    if (i > 0 && recs[i - 1].signalId == signalId && recs[i - 1].endMs >= fromMs) {
        --i;
    }
    if (level == 0) {
        for (; i < recs.size() && recs[i].signalId == signalId && recs[i].startMs <= toMs; ++i) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
                out.timestamps.clear();
                out.raw.clear();
                return out;
            }
            std::vector<std::uint8_t> bytes;
            if (!readPageBytes(0, recs[i], bytes)) {
                continue;
            }
            PageHeader hdr{};
            std::vector<std::uint64_t> t;
            std::vector<std::int64_t> v;
            if (!decodeRawPage(bytes, hdr, t, v)) {
                continue;
            }
            for (std::size_t k = 0; k < t.size(); ++k) {
                if (t[k] >= fromMs && t[k] <= toMs) {
                    out.timestamps.push_back(t[k]);
                    out.raw.push_back(v[k]);
                }
            }
        }
        return out;
    }
    const auto srcBms = out.bucketMs;
    if (srcBms == 0 || fromMs > toMs) {
        return out;
    }
    std::uint64_t firstStart = (fromMs / srcBms) * srcBms;
    if (firstStart + srcBms <= fromMs) {
        firstStart += srcBms;
    }
    if (firstStart > toMs) {
        return out;
    }
    const std::uint64_t lastStart = (toMs / srcBms) * srcBms;
    const auto nSlots = static_cast<std::size_t>((lastStart - firstStart) / srcBms + 1);
    std::vector<RollupBucket> grid(nSlots);
    for (std::size_t k = 0; k < nSlots; ++k) {
        grid[k].startMs = firstStart + static_cast<std::uint64_t>(srcBms) * k;
        grid[k].present = false;
    }
    for (; i < recs.size() && recs[i].signalId == signalId && recs[i].startMs <= toMs; ++i) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            out.buckets.clear();
            return out;
        }
        std::vector<std::uint8_t> bytes;
        if (!readPageBytes(level, recs[i], bytes)) {
            continue;
        }
        PageHeader hdr{};
        std::vector<RollupBucket> slots;
        if (!decodeRollupPage(bytes, hdr, srcBms, slots)) {
            continue;
        }
        for (auto& slot : slots) {
            if (!slot.present || slot.count == 0) {
                continue;
            }
            if (!bucketIntersects(slot.startMs, srcBms, fromMs, toMs)) {
                continue;
            }
            if (slot.startMs < firstStart) {
                continue;
            }
            const auto idx = static_cast<std::size_t>((slot.startMs - firstStart) / srcBms);
            if (idx < grid.size()) {
                grid[idx] = slot;
                grid[idx].present = true;
            }
        }
    }
    const auto* info = signal(signalId);
    if (info != nullptr && !info->isSigned) {
        foldToBudgetUnsigned(grid, out.bucketMs, maxPoints);
    } else {
        foldToBudgetSigned(grid, out.bucketMs, maxPoints);
    }
    out.buckets = std::move(grid);
    return out;
}

QueryResult Store::query(const Query& q) const {
    QueryResult out;
    out.fromMs = q.fromMs;
    out.toMs = q.toMs;
    out.series.reserve(q.signalIds.size());
    int maxLevel = 0;
    std::uint32_t bucket = 0;
    for (const auto id : q.signalIds) {
        if (q.cancel != nullptr && q.cancel->load(std::memory_order_relaxed)) {
            out.series.clear();
            out.truncated = true;
            return out;
        }
        auto series = querySignal(id, q.fromMs, q.toMs, q.maxPoints, q.cancel);
        maxLevel = std::max(maxLevel, series.level);
        if (bucket == 0) {
            bucket = series.bucketMs;
        }
        out.series.push_back(std::move(series));
    }
    out.level = maxLevel;
    out.bucketMs = bucket;
    return out;
}

IndexStats indexLogs(const std::filesystem::path& logs, const std::filesystem::path& outRoot, const dbc::Database& db,
                     std::string sessionId, std::string* error, std::uint64_t sessionGapMs) {
    const auto t0 = std::chrono::steady_clock::now();
    IndexStats stats;
    std::string dbcText;
    if (!db.path.empty()) {
        std::ifstream in(db.path, std::ios::binary);
        if (in) {
            dbcText.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    const auto dbcSha = hexSha1(dbcText.empty() ? db.label : dbcText);
    const auto dbcLabel = db.label.empty() ? db.path : db.label;
    const bool forcedSession = !sessionId.empty();

    Store store;
    bool opened = false;
    bool failed = false;
    std::uint64_t frames = 0;
    std::uint64_t lastMs = 0;
    std::set<std::string> usedIds;
    auto openSession = [&](std::uint64_t firstMs) -> bool {
        SessionManifest man;
        if (forcedSession) {
            man.id = sessionId;
        } else {
            // The corpus contains clock resets, so two distinct sessions can
            // begin in the same UTC second. Walk the sequence suffix until the
            // id is unused in this run; deterministic, so a rebuild reproduces
            // the same ids and still overwrites in place.
            for (int seq = 0; seq < 100; ++seq) {
                const auto candidate = sessionIdFromMs(firstMs, seq);
                if (usedIds.insert(candidate).second) {
                    man.id = candidate;
                    break;
                }
            }
            if (man.id.empty()) {
                if (error) {
                    *error = "cannot allocate a session id near " + std::to_string(firstMs);
                }
                return false;
            }
        }
        man.dbcSha = dbcSha;
        man.dbcLabel = dbcLabel;
        man.builtLevels = 0x01;
        if (!store.create(outRoot / man.id, man, error)) {
            return false;
        }
        for (const auto& msg : db.messages) {
            for (const auto& sig : msg.signals) {
                SignalInfo info;
                info.id = sig.signalId;
                info.message = sig.message;
                info.name = sig.name;
                info.unit = sig.unit;
                info.factor = sig.factor;
                info.offset = sig.offset;
                info.bits = sig.length;
                info.isSigned = sig.isSigned;
                info.min = sig.min;
                info.max = sig.max;
                info.valueType = sig.valueType;
                store.setSignal(info);
            }
        }
        frames = 0;
        opened = true;
        return true;
    };
    auto closeSession = [&]() -> bool {
        if (!opened) {
            return true;
        }
        // Some stretches of the corpus carry CAN ids this DBC does not define,
        // so frames arrive and nothing decodes. Publishing that as a session
        // leaves an empty store with a degenerate [0,0] range, which then looks
        // like it overlaps every other session.
        if (store.manifest().samples == 0) {
            stats.undecodedFrames += frames;
            ++stats.emptySessions;
            store.discard();
            opened = false;
            return true;
        }
        store.setFrameCount(frames);
        if (!store.finish(error)) {
            return false;
        }
        SessionStats ss;
        ss.id = store.manifest().id;
        ss.minMs = store.manifest().minMs;
        ss.maxMs = store.manifest().maxMs;
        ss.samples = store.manifest().samples;
        ss.frames = frames;
        const auto published = store.dir();
        store.close();
        ss.storeBytes = dirBytes(published);
        if (stats.sessionId.empty()) {
            stats.sessionId = ss.id;
        }
        stats.samples += ss.samples;
        stats.frames += ss.frames;
        stats.storeBytes += ss.storeBytes;
        stats.sessions.push_back(std::move(ss));
        opened = false;
        return true;
    };
    if (!walkVsd1(logs, error, [&](const canp::Batch& batch, std::uint64_t) -> bool {
            // A hole in wall clock, or time running backwards, means the logger
            // stopped and started. That is a session boundary, not a gap inside
            // one session.
            if (opened && !forcedSession) {
                if (isSessionBoundary(batch.timestamp, lastMs, sessionGapMs) && !closeSession()) {
                    failed = true;
                    return false;
                }
            }
            if (!opened && !openSession(batch.timestamp)) {
                failed = true;
                return false;
            }
            ++frames;
            lastMs = batch.timestamp;
            for (std::uint16_t p = 0; p < batch.count; ++p) {
                const auto samples = dbc::decodeFrameRaw(db, batch.packets[p].canId, batch.packets[p].data,
                                                         batch.packets[p].dlc);
                for (const auto& s : samples) {
                    store.append(s.signalId, batch.timestamp, s.raw);
                }
            }
            return true;
        })) {
        store.close();
        return stats;
    }
    if (failed) {
        store.close();
        return stats;
    }
    if (!opened && stats.sessions.empty()) {
        if (error && error->empty()) {
            *error = "no CANP batches in logs";
        }
        return stats;
    }
    if (!closeSession()) {
        store.close();
        return stats;
    }
    stats.sourceBytes = 0;
    for (const auto& file : vsd1::listLogFiles(logs)) {
        stats.sourceBytes += fileSizeOf(file);
    }

    for (const auto& ss : stats.sessions) {
        upsertCatalog(outRoot, ss.id, ss.minMs, ss.maxMs, dbcSha, nullptr);
    }

    const auto t1 = std::chrono::steady_clock::now();
    stats.elapsedS = std::chrono::duration<double>(t1 - t0).count();
    stats.complete = true;
    return stats;
}

bool collectDecodeTriples(const std::filesystem::path& logs, const dbc::Database& db, std::vector<std::uint32_t>& ids,
                          std::vector<std::uint64_t>& times, std::vector<std::int64_t>& raws, std::string* error) {
    ids.clear();
    times.clear();
    raws.clear();
    return walkVsd1(logs, error, [&](const canp::Batch& batch, std::uint64_t) -> bool {
        for (std::uint16_t p = 0; p < batch.count; ++p) {
            const auto samples =
                dbc::decodeFrameRaw(db, batch.packets[p].canId, batch.packets[p].data, batch.packets[p].dlc);
            for (const auto& s : samples) {
                ids.push_back(s.signalId);
                times.push_back(batch.timestamp);
                raws.push_back(s.raw);
            }
        }
        return true;
    });
}

namespace {

// Validation streams a bounded window of wall clock per session: decode it,
// compare it against the store, discard it. Materializing every triple costs
// ~24 B/sample, which is tens of GB at corpus scale and will not run.
constexpr std::uint64_t kValidateWindowMs = 30000;

struct ValidateState {
    const Store* store = nullptr;
    std::uint64_t minMs = 0;
    std::uint64_t maxMs = 0;
    std::uint64_t windowStart = 0;
    std::uint64_t checked = 0;
    std::vector<std::uint32_t> ids;
    std::vector<std::uint64_t> times;
    std::vector<std::int64_t> raws;
};

bool compareValidateWindow(ValidateState& st, std::uint64_t fromMs, std::uint64_t toMs, std::string& err) {
    if (st.ids.empty()) {
        return true;
    }
    std::vector<std::uint32_t> unique;
    {
        std::unordered_map<std::uint32_t, std::size_t> seen;
        for (const auto id : st.ids) {
            seen[id] = 0;
        }
        unique.reserve(seen.size());
        for (const auto& [id, unused] : seen) {
            (void)unused;
            unique.push_back(id);
        }
    }
    Query q;
    q.fromMs = fromMs;
    q.toMs = toMs;
    q.signalIds = unique;
    q.maxPoints = 0xffffffffu;
    const auto result = st.store->query(q);
    std::unordered_map<std::uint32_t, const SeriesResult*> byId;
    std::size_t stored = 0;
    for (const auto& series : result.series) {
        if (series.level != 0) {
            err = "validation expected L0 for raw window query";
            return false;
        }
        if (series.timestamps.size() != series.raw.size()) {
            err = "series length mismatch";
            return false;
        }
        byId[series.signalId] = &series;
        stored += series.timestamps.size();
    }
    if (stored != st.ids.size()) {
        err = "sample count in [" + std::to_string(fromMs) + "," + std::to_string(toMs) +
              "] store=" + std::to_string(stored) + " decode=" + std::to_string(st.ids.size());
        return false;
    }
    std::unordered_map<std::uint32_t, std::size_t> cursor;
    for (std::size_t i = 0; i < st.ids.size(); ++i) {
        const auto it = byId.find(st.ids[i]);
        if (it == byId.end()) {
            err = "missing signal " + std::to_string(st.ids[i]) + " in store";
            return false;
        }
        auto& pos = cursor[st.ids[i]];
        const auto* series = it->second;
        if (pos >= series->timestamps.size() || series->timestamps[pos] != st.times[i] ||
            series->raw[pos] != st.raws[i]) {
            err = "triple mismatch for signal " + std::to_string(st.ids[i]) + " at t=" + std::to_string(st.times[i]);
            return false;
        }
        ++pos;
    }
    st.checked += st.ids.size();
    st.ids.clear();
    st.times.clear();
    st.raws.clear();
    return true;
}

}  // namespace

bool validateSessions(std::span<const Store* const> stores, const std::filesystem::path& logs,
                      const dbc::Database& db, std::string* error) {
    // One pass over the logs for every session. Validating sessions one at a
    // time would re-read the whole corpus per session, which is the difference
    // between minutes and hours once a day splits into a dozen stints.
    std::vector<ValidateState> states;
    states.reserve(stores.size());
    for (const auto* s : stores) {
        if (s == nullptr) {
            continue;
        }
        ValidateState st;
        st.store = s;
        st.minMs = s->manifest().minMs;
        st.maxMs = s->manifest().maxMs;
        st.windowStart = st.minMs;
        states.push_back(std::move(st));
    }
    std::sort(states.begin(), states.end(),
              [](const ValidateState& a, const ValidateState& b) { return a.minMs < b.minMs; });
    for (std::size_t i = 1; i < states.size(); ++i) {
        if (states[i].minMs <= states[i - 1].maxMs) {
            if (error) {
                *error = "sessions overlap in time: " + states[i - 1].store->manifest().id + " and " +
                         states[i].store->manifest().id;
            }
            return false;
        }
    }

    bool ok = true;
    std::string localError;
    if (!walkVsd1(logs, error, [&](const canp::Batch& batch, std::uint64_t) -> bool {
            // Ranges are disjoint, so at most one session owns this batch.
            const auto it = std::upper_bound(states.begin(), states.end(), batch.timestamp,
                                             [](std::uint64_t t, const ValidateState& s) { return t < s.minMs; });
            if (it == states.begin()) {
                return true;
            }
            auto& st = *(it - 1);
            if (batch.timestamp < st.minMs || batch.timestamp > st.maxMs) {
                return true;
            }
            // Close the window before this batch so no sample straddles two
            // windows. Snapping rather than stepping keeps a long idle gap from
            // spinning through empty windows.
            if (batch.timestamp >= st.windowStart + kValidateWindowMs && batch.timestamp > 0) {
                if (!compareValidateWindow(st, st.windowStart, batch.timestamp - 1, localError)) {
                    ok = false;
                    return false;
                }
                st.windowStart = batch.timestamp;
            }
            for (std::uint16_t p = 0; p < batch.count; ++p) {
                const auto samples =
                    dbc::decodeFrameRaw(db, batch.packets[p].canId, batch.packets[p].data, batch.packets[p].dlc);
                for (const auto& s : samples) {
                    st.ids.push_back(s.signalId);
                    st.times.push_back(batch.timestamp);
                    st.raws.push_back(s.raw);
                }
            }
            return true;
        })) {
        return false;
    }
    for (auto& st : states) {
        if (ok && !compareValidateWindow(st, st.windowStart, st.maxMs, localError)) {
            ok = false;
        }
        if (ok && st.checked != st.store->manifest().samples) {
            localError = st.store->manifest().id + ": validated " + std::to_string(st.checked) +
                         " samples, manifest claims " + std::to_string(st.store->manifest().samples);
            ok = false;
        }
    }
    if (!ok) {
        if (error) {
            *error = localError;
        }
        return false;
    }
    return true;
}

bool validateAgainstDecode(const Store& store, const std::filesystem::path& logs, const dbc::Database& db,
                           std::string* error) {
    const Store* one = &store;
    return validateSessions(std::span<const Store* const>(&one, 1), logs, db, error);
}

}  // namespace photon::tsdb
