#include "store/tsdb.hpp"

#include "tsdb_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace photon::tsdb {
namespace {

namespace fs = std::filesystem;

// Appends pages to a level's column files, rotating at kColumnFileCap, starting
// from a caller-chosen file number. Compaction and pyramid sealing both need
// this and neither may reuse an existing file number: an offset recorded against
// a file that later gets rewritten from zero points into stale bytes (§10).
class PageAppender {
public:
    PageAppender(fs::path sessionDir, int level, std::uint32_t firstFileNo)
        : dir_(std::move(sessionDir)), level_(level), fileNo_(firstFileNo) {}

    bool write(std::span<const std::uint8_t> bytes, IndexRecord& rec, std::string* error) {
        if (path_.empty() || offset_ + bytes.size() > kColumnFileCap) {
            if (!path_.empty()) {
                ++fileNo_;
            }
            offset_ = 0;
            path_ = detail::columnPath(dir_, level_, fileNo_);
            std::error_code ec;
            fs::create_directories(path_.parent_path(), ec);
        }
        const auto mode = offset_ == 0 ? std::ios::trunc : std::ios::app;
        std::ofstream out(path_, std::ios::binary | mode);
        if (!out) {
            if (error) {
                *error = "cannot append " + path_.string();
            }
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            if (error) {
                *error = "short write " + path_.string();
            }
            return false;
        }
        rec.fileNo = fileNo_;
        rec.offset = offset_;
        rec.pageBytes = static_cast<std::uint32_t>(bytes.size());
        offset_ += static_cast<std::uint32_t>(bytes.size());
        return true;
    }

private:
    fs::path dir_;
    int level_ = 0;
    std::uint32_t fileNo_ = 0;
    std::uint32_t offset_ = 0;
    fs::path path_;
};

bool parseFileNo(const fs::path& file, std::uint32_t& out) {
    const auto stem = file.stem().string();
    if (stem.empty()) {
        return false;
    }
    for (const char c : stem) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    out = static_cast<std::uint32_t>(std::strtoul(stem.c_str(), nullptr, 10));
    return true;
}

std::vector<fs::path> columnFiles(const fs::path& sessionDir, int level) {
    std::vector<fs::path> out;
    std::error_code ec;
    const auto sub = sessionDir / ("l" + std::to_string(level));
    for (const auto& entry : fs::directory_iterator(sub, ec)) {
        if (ec) {
            break;
        }
        if (entry.path().extension() == ".pcol") {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::uint32_t nextFreeFileNo(const fs::path& sessionDir, int level, std::span<const IndexRecord> recs) {
    std::uint32_t highest = 0;
    bool any = false;
    for (const auto& r : recs) {
        highest = std::max(highest, r.fileNo);
        any = true;
    }
    for (const auto& f : columnFiles(sessionDir, level)) {
        std::uint32_t no = 0;
        if (parseFileNo(f, no)) {
            highest = std::max(highest, no);
            any = true;
        }
    }
    return any ? highest + 1 : 0;
}

bool readPageAt(const fs::path& sessionDir, int level, const IndexRecord& rec, std::vector<std::uint8_t>& out) {
    const auto path = detail::columnPath(sessionDir, level, rec.fileNo);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(static_cast<std::streamoff>(rec.offset), std::ios::beg);
    out.resize(rec.pageBytes);
    if (rec.pageBytes == 0) {
        return true;
    }
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(rec.pageBytes));
    return static_cast<bool>(in);
}

// Lay a run of present buckets onto the aligned slot grid the page format wants
// (§5.1): index is (t - baseMs) / bucketMs, absent slots carry no data.
std::vector<RollupBucket> gridOf(std::span<const RollupBucket> present, std::uint32_t bucketMs) {
    std::vector<RollupBucket> grid;
    if (present.empty() || bucketMs == 0) {
        return grid;
    }
    const auto first = present.front().startMs;
    const auto last = present.back().startMs;
    const auto n = static_cast<std::size_t>((last - first) / bucketMs + 1);
    grid.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        grid[i].startMs = first + static_cast<std::uint64_t>(bucketMs) * i;
        grid[i].present = false;
        grid[i].count = 0;
    }
    for (const auto& b : present) {
        const auto idx = static_cast<std::size_t>((b.startMs - first) / bucketMs);
        if (idx < grid.size()) {
            grid[idx] = b;
            grid[idx].present = true;
        }
    }
    return grid;
}

IndexRecord rollupRecordOf(std::uint32_t signalId, std::span<const RollupBucket> grid, std::uint32_t bucketMs) {
    IndexRecord rec;
    rec.signalId = signalId;
    rec.startMs = grid.front().startMs;
    rec.endMs = grid.back().startMs + bucketMs;
    rec.sampleCount = static_cast<std::uint32_t>(grid.size());
    bool have = false;
    for (const auto& b : grid) {
        if (!b.present || b.count == 0) {
            continue;
        }
        const auto lo = static_cast<float>(b.min);
        const auto hi = static_cast<float>(b.max);
        if (!have) {
            rec.vMin = lo;
            rec.vMax = hi;
            have = true;
        } else {
            rec.vMin = std::min(rec.vMin, lo);
            rec.vMax = std::max(rec.vMax, hi);
        }
    }
    return rec;
}

IndexRecord rawRecordOf(std::uint32_t signalId, std::span<const std::uint64_t> times,
                        std::span<const std::int64_t> values) {
    IndexRecord rec;
    rec.signalId = signalId;
    rec.startMs = times.front();
    rec.endMs = times.back();
    rec.sampleCount = static_cast<std::uint32_t>(times.size());
    rec.vMin = static_cast<float>(values.front());
    rec.vMax = rec.vMin;
    for (const auto v : values) {
        rec.vMin = std::min(rec.vMin, static_cast<float>(v));
        rec.vMax = std::max(rec.vMax, static_cast<float>(v));
    }
    return rec;
}

void accumulate(RollupBucket& b, std::int64_t raw, bool isSigned) {
    if (b.count == 0) {
        b.min = raw;
        b.max = raw;
        b.first = raw;
        b.last = raw;
        b.sum = 0;
        b.sumHi = 0;
        b.wide = false;
        b.count = 1;
        b.present = true;
        if (isSigned) {
            b.sum = raw;
            b.wide = false;
        } else {
            b.sum = raw;
        }
        return;
    }
    if (isSigned) {
        b.min = std::min(b.min, raw);
        b.max = std::max(b.max, raw);
        const auto cur = b.wide ? ((static_cast<__int128>(static_cast<std::int64_t>(b.sumHi)) << 64) |
                                   static_cast<__int128>(static_cast<unsigned __int128>(
                                       static_cast<std::uint64_t>(b.sum))))
                                : static_cast<__int128>(b.sum);
        const auto sum = cur + static_cast<__int128>(raw);
        b.sum = static_cast<std::int64_t>(static_cast<std::uint64_t>(sum));
        b.sumHi = static_cast<std::uint64_t>(static_cast<unsigned __int128>(sum) >> 64);
        b.wide = sum < static_cast<__int128>(INT64_MIN) || sum > static_cast<__int128>(INT64_MAX);
    } else {
        if (static_cast<std::uint64_t>(raw) < static_cast<std::uint64_t>(b.min)) {
            b.min = raw;
        }
        if (static_cast<std::uint64_t>(raw) > static_cast<std::uint64_t>(b.max)) {
            b.max = raw;
        }
        const auto cur = b.wide ? ((static_cast<unsigned __int128>(b.sumHi) << 64) |
                                   static_cast<std::uint64_t>(b.sum))
                                : static_cast<unsigned __int128>(static_cast<std::uint64_t>(b.sum));
        const auto sum = cur + static_cast<unsigned __int128>(static_cast<std::uint64_t>(raw));
        b.sum = static_cast<std::int64_t>(static_cast<std::uint64_t>(sum));
        b.sumHi = static_cast<std::uint64_t>(sum >> 64);
        b.wide = b.sumHi != 0;
    }
    b.last = raw;
    ++b.count;
}

}  // namespace

// ---------------------------------------------------------------------------
// Live tail writer
// ---------------------------------------------------------------------------

bool LiveTailWriter::begin(const fs::path& root, const std::string& sessionId, const dbc::Database& db,
                           LiveOptions options, std::string* error) {
    db_ = &db;
    dir_ = root / sessionId;
    samples_ = 0;
    frames_ = 0;
    feedMs_ = 0;
    failed_ = false;
    lastError_.clear();
    SessionManifest man;
    man.id = sessionId;
    man.dbcLabel = db.label.empty() ? db.path : db.label;
    if (!store_.createLive(dir_, man, options, error)) {
        failed_ = true;
        if (error != nullptr) {
            lastError_ = *error;
        }
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
            store_.setSignal(info);
        }
    }
    open_ = true;
    return true;
}

void LiveTailWriter::onSample(std::uint32_t signalId, std::uint64_t timestampMs, std::int64_t raw) {
    if (!open_ || failed_) {
        return;
    }
    store_.append(signalId, timestampMs, raw);
    ++samples_;
    if (timestampMs > feedMs_) {
        feedMs_ = timestampMs;
    }
    std::string err;
    if (!store_.tick(feedMs_, &err)) {
        failed_ = true;
        lastError_ = err;
    }
}

void LiveTailWriter::onFrame(std::uint32_t canId, std::uint8_t dlc, const std::uint8_t data[8],
                             std::uint64_t timestampMs) {
    if (!open_ || failed_ || db_ == nullptr) {
        return;
    }
    ++frames_;
    // decodeFrameRaw, not decodeFrame: the store holds the pre-scaling CAN bits
    // (§4), and reversing factor/offset from a double is not exact.
    for (const auto& s : dbc::decodeFrameRaw(*db_, canId, data, dlc)) {
        store_.append(s.signalId, timestampMs, s.raw);
        ++samples_;
    }
    if (timestampMs > feedMs_) {
        feedMs_ = timestampMs;
    }
    std::string err;
    if (!store_.tick(feedMs_, &err)) {
        failed_ = true;
        lastError_ = err;
    }
}

bool LiveTailWriter::end(std::string* error) {
    if (!open_) {
        return !failed_;
    }
    open_ = false;
    store_.setFrameCount(frames_);
    std::string err;
    if (!store_.finish(&err)) {
        failed_ = true;
        lastError_ = err;
        if (error) {
            *error = err;
        }
        return false;
    }
    store_.close();
    return true;
}

// ---------------------------------------------------------------------------
// Crash recovery
// ---------------------------------------------------------------------------

bool recoverSession(const fs::path& sessionDir, RecoveryReport& report, std::string* error) {
    report = {};
    SessionManifest man{};
    if (!detail::loadSessionJson(sessionDir, man, error)) {
        return false;
    }
    report.generation = man.generation;
    const auto lock = sessionDir / "WRITER.lock";
    std::error_code ec;
    const bool locked = fs::exists(lock, ec);
    if (!man.live && !locked) {
        return true;
    }
    report.ranRecovery = true;

    for (int L = 0; L < kLevelCount; ++L) {
        IndexHeader hdr{};
        std::vector<IndexRecord> recs;
        detail::loadLevelIndex(sessionDir, L, hdr, recs);

        // Sizes first: a record pointing past the end of its file is a page the
        // index claims and the disk does not have. Write order is page then
        // index, so this needs the OS to have lost a buffered write, but a
        // reader that trusts such a record decodes garbage.
        std::unordered_map<std::uint32_t, std::uint64_t> sizes;
        for (const auto& f : columnFiles(sessionDir, L)) {
            std::uint32_t no = 0;
            if (parseFileNo(f, no)) {
                sizes[no] = detail::fileSizeOf(f);
            }
        }
        const auto before = recs.size();
        recs.erase(std::remove_if(recs.begin(), recs.end(),
                                  [&](const IndexRecord& r) {
                                      const auto it = sizes.find(r.fileNo);
                                      if (it == sizes.end()) {
                                          return true;
                                      }
                                      return static_cast<std::uint64_t>(r.offset) + r.pageBytes > it->second;
                                  }),
                   recs.end());
        if (recs.size() != before) {
            report.recordsDropped += static_cast<std::uint32_t>(before - recs.size());
            if (!detail::storeLevelIndex(sessionDir, L, recs, error)) {
                return false;
            }
        }

        std::unordered_map<std::uint32_t, std::uint64_t> highWater;
        for (const auto& r : recs) {
            auto& hw = highWater[r.fileNo];
            hw = std::max<std::uint64_t>(hw, static_cast<std::uint64_t>(r.offset) + r.pageBytes);
        }
        for (const auto& f : columnFiles(sessionDir, L)) {
            std::uint32_t no = 0;
            if (!parseFileNo(f, no)) {
                continue;
            }
            const auto size = detail::fileSizeOf(f);
            const auto it = highWater.find(no);
            if (it == highWater.end()) {
                // No record references this file at all: either the writer died
                // before its first checkpoint, or a compaction died after
                // writing pages and before swapping the index in.
                report.bytesDiscarded += size;
                ++report.filesRemoved;
                fs::remove(f, ec);
                continue;
            }
            if (size > it->second) {
                report.bytesDiscarded += size - it->second;
                ++report.filesTruncated;
                fs::resize_file(f, static_cast<std::uintmax_t>(it->second), ec);
                if (ec) {
                    if (error) {
                        *error = "cannot truncate " + f.string() + ": " + ec.message();
                    }
                    return false;
                }
            }
        }
        // Leftover atomic-write temporaries.
        const auto tmp = detail::indexPath(sessionDir, L);
        auto tmpPath = tmp;
        tmpPath += ".tmp";
        fs::remove(tmpPath, ec);
    }

    man.live = false;
    ++man.generation;
    if (!detail::storeSessionJson(sessionDir, man, error)) {
        return false;
    }
    fs::remove(lock, ec);
    report.generation = man.generation;
    return true;
}

std::uint32_t recoverCrashedSessions(const fs::path& root, std::string* error) {
    std::uint32_t n = 0;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return 0;
    }
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        SessionManifest man{};
        if (!detail::loadSessionJson(entry.path(), man, nullptr) || !man.live) {
            continue;
        }
        RecoveryReport report{};
        std::string err;
        if (!recoverSession(entry.path(), report, &err)) {
            if (error) {
                *error = err;
            }
            continue;
        }
        if (report.ranRecovery) {
            ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Retention
// ---------------------------------------------------------------------------

std::uint64_t level0Bytes(const fs::path& sessionDir) {
    return detail::dirBytes(sessionDir / "l0") + detail::fileSizeOf(detail::indexPath(sessionDir, 0));
}

namespace {

// Build L5 from L0 for one signal. Called before an L0 drop for signals that
// have no rollup at all: §5.3's build rule says a level below four samples per
// bucket costs more than the raw data it summarizes, which is true while the raw
// data is there. Once L0 is going away the comparison changes — the choice is a
// coarse bucket or nothing — so a floor level is built unconditionally. L5 is the
// right floor: the signals that reach here are below 0.0067 Hz (anything faster
// already qualifies for L4 or better), so a 600 s bucket holds at most a handful
// of samples and the pages stay tiny.
bool sealSignalToL5(const fs::path& sessionDir, std::uint32_t signalId, bool isSigned,
                    std::span<const IndexRecord> l0, PageAppender& appender, std::vector<IndexRecord>& outRecs,
                    std::string* error) {
    const auto bucketMs = bucketMsOf(5);
    std::vector<RollupBucket> present;
    RollupBucket cur{};
    bool open = false;

    auto emit = [&](bool force) -> bool {
        if (present.empty()) {
            return true;
        }
        const auto span = (present.back().startMs - present.front().startMs) / bucketMs + 1;
        if (!force && span < kPageSampleCap) {
            return true;
        }
        auto grid = gridOf(present, bucketMs);
        const auto bytes = encodeRollupPage(signalId, 5, grid.front().startMs, bucketMs, grid, force, isSigned);
        auto rec = rollupRecordOf(signalId, grid, bucketMs);
        if (!appender.write(bytes, rec, error)) {
            return false;
        }
        outRecs.push_back(rec);
        present.clear();
        return true;
    };

    std::size_t i = indexLowerBound(l0, signalId, 0);
    for (; i < l0.size() && l0[i].signalId == signalId; ++i) {
        std::vector<std::uint8_t> bytes;
        if (!readPageAt(sessionDir, 0, l0[i], bytes)) {
            if (error) {
                *error = "cannot read L0 page while sealing signal " + std::to_string(signalId);
            }
            return false;
        }
        PageHeader hdr{};
        std::vector<std::uint64_t> times;
        std::vector<std::int64_t> values;
        if (!decodeRawPage(bytes, hdr, times, values)) {
            if (error) {
                *error = "bad L0 page while sealing signal " + std::to_string(signalId);
            }
            return false;
        }
        for (std::size_t k = 0; k < times.size(); ++k) {
            const auto start = (times[k] / bucketMs) * bucketMs;
            if (!open || cur.startMs != start) {
                if (open) {
                    present.push_back(cur);
                    if (!emit(false)) {
                        return false;
                    }
                }
                cur = RollupBucket{};
                cur.startMs = start;
                open = true;
            }
            accumulate(cur, values[k], isSigned);
        }
    }
    if (open) {
        present.push_back(cur);
    }
    return emit(true);
}

}  // namespace

bool dropLevel0(const fs::path& sessionDir, bool sealPyramid, std::uint32_t* sealedSignals,
                std::uint64_t* freedBytes, std::string* error) {
    if (sealedSignals != nullptr) {
        *sealedSignals = 0;
    }
    if (freedBytes != nullptr) {
        *freedBytes = 0;
    }
    SessionManifest man{};
    if (!detail::loadSessionJson(sessionDir, man, error)) {
        return false;
    }
    std::error_code ec;
    if (man.live || fs::exists(sessionDir / "WRITER.lock", ec)) {
        if (error) {
            *error = "refusing to drop L0 from a session with a live writer: " + sessionDir.string();
        }
        return false;
    }

    IndexHeader l0Hdr{};
    std::vector<IndexRecord> l0;
    detail::loadLevelIndex(sessionDir, 0, l0Hdr, l0);
    if (l0.empty()) {
        // Already dropped. Idempotent so a retention pass can be re-run.
        return true;
    }

    std::vector<SignalInfo> signals;
    detail::loadSignalsJson(sessionDir, signals);
    std::unordered_map<std::uint32_t, std::size_t> byId;
    for (std::size_t i = 0; i < signals.size(); ++i) {
        byId[signals[i].id] = i;
    }

    if (sealPyramid) {
        // Which levels actually exist comes from the indexes, not from
        // signals.json: a stale levels array is exactly what would let a signal
        // lose its last copy here.
        std::vector<IndexRecord> higher[kLevelCount];
        for (int L = 1; L < kLevelCount; ++L) {
            IndexHeader h{};
            detail::loadLevelIndex(sessionDir, L, h, higher[L]);
        }
        std::vector<std::uint32_t> dark;
        std::uint32_t prev = 0;
        bool first = true;
        for (const auto& r : l0) {
            if (!first && r.signalId == prev) {
                continue;
            }
            first = false;
            prev = r.signalId;
            bool covered = false;
            for (int L = 1; L < kLevelCount && !covered; ++L) {
                const auto i = indexLowerBound(higher[L], r.signalId, 0);
                covered = i < higher[L].size() && higher[L][i].signalId == r.signalId;
            }
            if (!covered) {
                dark.push_back(r.signalId);
            }
        }
        if (!dark.empty()) {
            auto& l5 = higher[5];
            PageAppender appender(sessionDir, 5, nextFreeFileNo(sessionDir, 5, l5));
            for (const auto id : dark) {
                bool isSigned = true;
                if (const auto it = byId.find(id); it != byId.end()) {
                    isSigned = signals[it->second].isSigned;
                }
                if (!sealSignalToL5(sessionDir, id, isSigned, l0, appender, l5, error)) {
                    return false;
                }
                if (const auto it = byId.find(id); it != byId.end()) {
                    signals[it->second].levels = static_cast<std::uint8_t>(signals[it->second].levels | (1u << 5));
                }
            }
            std::sort(l5.begin(), l5.end(), [](const IndexRecord& a, const IndexRecord& b) {
                if (a.signalId != b.signalId) {
                    return a.signalId < b.signalId;
                }
                return a.startMs < b.startMs;
            });
            if (!detail::storeLevelIndex(sessionDir, 5, l5, error)) {
                return false;
            }
            man.builtLevels = static_cast<std::uint8_t>(man.builtLevels | (1u << 5));
            if (sealedSignals != nullptr) {
                *sealedSignals = static_cast<std::uint32_t>(dark.size());
            }
        }
    }

    const auto before = level0Bytes(sessionDir);
    // Empty but well-formed index rather than a missing file: "L0 exists as a
    // level and deliberately has no pages" is a different fact from "this store
    // was never built", and the reader distinguishes them.
    if (!detail::storeLevelIndex(sessionDir, 0, std::span<const IndexRecord>{}, error)) {
        return false;
    }
    fs::remove_all(sessionDir / "l0", ec);
    fs::create_directories(sessionDir / "l0", ec);

    for (auto& s : signals) {
        s.levels = static_cast<std::uint8_t>(s.levels & ~0x01u);
    }
    if (!detail::storeSignalsJson(sessionDir, signals, error)) {
        return false;
    }
    man.builtLevels = static_cast<std::uint8_t>(man.builtLevels & ~0x01u);
    man.l0Dropped = true;
    ++man.generation;
    if (!detail::storeSessionJson(sessionDir, man, error)) {
        return false;
    }
    if (freedBytes != nullptr) {
        *freedBytes = before > level0Bytes(sessionDir) ? before - level0Bytes(sessionDir) : 0;
    }
    return true;
}

RetentionReport applyRetention(const fs::path& root, const RetentionPolicy& policy, std::uint64_t nowMs,
                               std::string* error) {
    RetentionReport report;
    struct Candidate {
        fs::path dir;
        std::string id;
        std::uint64_t maxMs = 0;
        std::uint64_t l0Bytes = 0;
    };
    std::vector<Candidate> candidates;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory() || !fs::exists(entry.path() / "session.json")) {
            continue;
        }
        ++report.sessionsScanned;
        SessionManifest man{};
        if (!detail::loadSessionJson(entry.path(), man, nullptr)) {
            continue;
        }
        const auto bytes = level0Bytes(entry.path());
        report.l0BytesBefore += bytes;
        if (man.live || fs::exists(entry.path() / "WRITER.lock", ec)) {
            ++report.sessionsSkippedLive;
            continue;
        }
        if (man.l0Dropped) {
            continue;
        }
        candidates.push_back(Candidate{entry.path(), man.id, man.maxMs, bytes});
    }
    // Oldest first: both rules want to give up the least useful raw data.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.maxMs < b.maxMs; });

    std::vector<std::size_t> chosen;
    std::vector<std::string> reasons;
    std::uint64_t remaining = report.l0BytesBefore;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto age = nowMs > candidates[i].maxMs ? nowMs - candidates[i].maxMs : 0;
        if (policy.l0MaxAgeMs > 0 && age >= policy.l0MaxAgeMs) {
            chosen.push_back(i);
            reasons.emplace_back("age");
            remaining -= std::min(remaining, candidates[i].l0Bytes);
        }
    }
    if (policy.l0BudgetBytes > 0) {
        for (std::size_t i = 0; i < candidates.size() && remaining > policy.l0BudgetBytes; ++i) {
            if (std::find(chosen.begin(), chosen.end(), i) != chosen.end()) {
                continue;
            }
            chosen.push_back(i);
            reasons.emplace_back("budget");
            remaining -= std::min(remaining, candidates[i].l0Bytes);
        }
    }

    for (std::size_t k = 0; k < chosen.size(); ++k) {
        const auto& c = candidates[chosen[k]];
        RetentionAction action;
        action.sessionId = c.id;
        action.maxMs = c.maxMs;
        action.ageMs = nowMs > c.maxMs ? nowMs - c.maxMs : 0;
        action.l0Bytes = c.l0Bytes;
        action.reason = reasons[k];
        if (!policy.dryRun) {
            std::uint32_t sealed = 0;
            std::uint64_t freed = 0;
            if (!dropLevel0(c.dir, policy.sealPyramid, &sealed, &freed, error)) {
                report.dropped.push_back(std::move(action));
                return report;
            }
            action.sealedSignals = sealed;
            report.l0BytesFreed += freed;
        } else {
            report.l0BytesFreed += c.l0Bytes;
        }
        report.dropped.push_back(std::move(action));
    }
    report.ok = true;
    return report;
}

// ---------------------------------------------------------------------------
// Compaction
// ---------------------------------------------------------------------------

namespace {

struct Group {
    std::size_t begin = 0;
    std::size_t end = 0;
};

std::vector<Group> groupsFor(std::span<const IndexRecord> recs, int level, const CompactionOptions& opts) {
    std::vector<Group> groups;
    const auto bucketMs = bucketMsOf(level);
    std::size_t i = 0;
    while (i < recs.size()) {
        std::size_t j = i + 1;
        std::uint64_t samples = recs[i].sampleCount;
        std::uint64_t bytes = recs[i].pageBytes;
        while (j < recs.size() && recs[j].signalId == recs[i].signalId) {
            if (bytes + recs[j].pageBytes > opts.targetPayloadBytes) {
                break;
            }
            if (level == 0) {
                if (samples + recs[j].sampleCount > opts.targetSamples) {
                    break;
                }
                samples += recs[j].sampleCount;
            } else {
                // Rollup pages are a dense slot array, so the merge cost is the
                // span, not the number of present buckets: two pages an hour
                // apart at 100 ms buckets would be one 36 000-slot page.
                if (bucketMs == 0 || recs[j].endMs <= recs[i].startMs + bucketMs) {
                    break;
                }
                const auto lastStart = recs[j].endMs - bucketMs;
                const auto slots = (lastStart - recs[i].startMs) / bucketMs + 1;
                if (slots > opts.targetSamples) {
                    break;
                }
            }
            bytes += recs[j].pageBytes;
            ++j;
        }
        groups.push_back(Group{i, j});
        i = j;
    }
    return groups;
}

bool compactLevel(const fs::path& sessionDir, int level, const CompactionOptions& opts,
                  const std::unordered_map<std::uint32_t, bool>& signedById, CompactionReport& report,
                  std::string* error) {
    IndexHeader hdr{};
    std::vector<IndexRecord> recs;
    if (!detail::loadLevelIndex(sessionDir, level, hdr, recs)) {
        return true;
    }
    report.pagesBefore[level] = static_cast<std::uint32_t>(recs.size());
    report.pagesAfter[level] = static_cast<std::uint32_t>(recs.size());
    if (recs.size() < 2) {
        return true;
    }
    const auto groups = groupsFor(recs, level, opts);
    if (groups.size() == recs.size()) {
        // Nothing merges. Rewriting the level would move every byte for no gain.
        return true;
    }

    const auto bucketMs = bucketMsOf(level);
    PageAppender appender(sessionDir, level, nextFreeFileNo(sessionDir, level, recs));
    std::vector<IndexRecord> out;
    out.reserve(groups.size());

    for (std::size_t g = 0; g < groups.size(); ++g) {
        const auto& grp = groups[g];
        const bool lastOfSignal = grp.end == recs.size() || recs[grp.end].signalId != recs[grp.begin].signalId;
        const auto signalId = recs[grp.begin].signalId;
        bool isSigned = true;
        if (const auto it = signedById.find(signalId); it != signedById.end()) {
            isSigned = it->second;
        }
        if (level == 0) {
            std::vector<std::uint64_t> times;
            std::vector<std::int64_t> values;
            for (std::size_t i = grp.begin; i < grp.end; ++i) {
                std::vector<std::uint8_t> bytes;
                if (!readPageAt(sessionDir, level, recs[i], bytes)) {
                    if (error) {
                        *error = "compaction cannot read page for signal " + std::to_string(signalId);
                    }
                    return false;
                }
                PageHeader ph{};
                std::vector<std::uint64_t> t;
                std::vector<std::int64_t> v;
                if (!decodeRawPage(bytes, ph, t, v)) {
                    if (error) {
                        *error = "compaction hit a bad L0 page for signal " + std::to_string(signalId);
                    }
                    return false;
                }
                times.insert(times.end(), t.begin(), t.end());
                values.insert(values.end(), v.begin(), v.end());
            }
            if (times.empty()) {
                continue;
            }
            const auto page = encodeRawPage(signalId, times, values, lastOfSignal);
            auto rec = rawRecordOf(signalId, times, values);
            if (!appender.write(page, rec, error)) {
                return false;
            }
            out.push_back(rec);
            report.samplesRewritten += times.size();
        } else {
            std::vector<RollupBucket> present;
            for (std::size_t i = grp.begin; i < grp.end; ++i) {
                std::vector<std::uint8_t> bytes;
                if (!readPageAt(sessionDir, level, recs[i], bytes)) {
                    if (error) {
                        *error = "compaction cannot read rollup page for signal " + std::to_string(signalId);
                    }
                    return false;
                }
                PageHeader ph{};
                std::vector<RollupBucket> slots;
                if (!decodeRollupPage(bytes, ph, bucketMs, slots)) {
                    if (error) {
                        *error = "compaction hit a bad rollup page for signal " + std::to_string(signalId);
                    }
                    return false;
                }
                for (const auto& s : slots) {
                    if (s.present && s.count > 0) {
                        present.push_back(s);
                    }
                }
            }
            if (present.empty()) {
                continue;
            }
            std::sort(present.begin(), present.end(),
                      [](const RollupBucket& a, const RollupBucket& b) { return a.startMs < b.startMs; });
            auto grid = gridOf(present, bucketMs);
            const auto page =
                encodeRollupPage(signalId, static_cast<std::uint8_t>(level), grid.front().startMs, bucketMs, grid,
                                 lastOfSignal, isSigned);
            auto rec = rollupRecordOf(signalId, grid, bucketMs);
            if (!appender.write(page, rec, error)) {
                return false;
            }
            out.push_back(rec);
            report.samplesRewritten += present.size();
        }
    }

    std::sort(out.begin(), out.end(), [](const IndexRecord& a, const IndexRecord& b) {
        if (a.signalId != b.signalId) {
            return a.signalId < b.signalId;
        }
        return a.startMs < b.startMs;
    });
    // The index swap is the commit point. Until it lands the old index is still
    // valid and the new pages are unreferenced bytes that recovery collects.
    if (!detail::storeLevelIndex(sessionDir, level, out, error)) {
        return false;
    }
    std::unordered_set<std::uint32_t> live;
    for (const auto& r : out) {
        live.insert(r.fileNo);
    }
    std::error_code ec;
    for (const auto& f : columnFiles(sessionDir, level)) {
        std::uint32_t no = 0;
        if (parseFileNo(f, no) && live.count(no) == 0) {
            fs::remove(f, ec);
        }
    }
    report.pagesAfter[level] = static_cast<std::uint32_t>(out.size());
    return true;
}

}  // namespace

bool compactSession(const fs::path& sessionDir, const CompactionOptions& options, CompactionReport& report,
                    std::string* error) {
    report = {};
    const auto t0 = std::chrono::steady_clock::now();
    SessionManifest man{};
    if (!detail::loadSessionJson(sessionDir, man, error)) {
        return false;
    }
    std::error_code ec;
    const auto lock = sessionDir / "WRITER.lock";
    if (man.live || fs::exists(lock, ec)) {
        if (error) {
            *error = "refusing to compact a session with a live writer: " + sessionDir.string();
        }
        return false;
    }
    {
        std::ofstream out(lock, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) {
                *error = "cannot take writer lock for compaction";
            }
            return false;
        }
        out << "photon-tsdb compact\n";
    }
    report.bytesBefore = detail::dirBytes(sessionDir);

    std::vector<SignalInfo> signals;
    detail::loadSignalsJson(sessionDir, signals);
    std::unordered_map<std::uint32_t, bool> signedById;
    for (const auto& s : signals) {
        signedById[s.id] = s.isSigned;
    }

    bool ok = true;
    for (int L = 0; L < kLevelCount && ok; ++L) {
        if (!options.levels[L]) {
            IndexHeader hdr{};
            std::vector<IndexRecord> recs;
            detail::loadLevelIndex(sessionDir, L, hdr, recs);
            report.pagesBefore[L] = static_cast<std::uint32_t>(recs.size());
            report.pagesAfter[L] = report.pagesBefore[L];
            continue;
        }
        ok = compactLevel(sessionDir, L, options, signedById, report, error);
    }
    if (ok) {
        ++man.generation;
        ok = detail::storeSessionJson(sessionDir, man, error);
    }
    fs::remove(lock, ec);
    report.bytesAfter = detail::dirBytes(sessionDir);
    report.generation = man.generation;
    report.elapsedS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return ok;
}

}  // namespace photon::tsdb
