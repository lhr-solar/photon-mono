#pragma once

#include "photon/dbc.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace photon::tsdb {

inline constexpr std::uint32_t kPageMagic = 0x31475050u;    // "PPG1" (v1, still readable)
inline constexpr std::uint32_t kPageMagicV2 = 0x32475050u;  // "PPG2"
inline constexpr std::uint32_t kIndexMagic = 0x31584950u;  // "PIX1"
inline constexpr std::uint16_t kIndexVersion = 1;
inline constexpr std::uint16_t kIndexRecordBytes = 48;
inline constexpr std::size_t kPageHeaderBytes = 32;
inline constexpr std::size_t kIndexHeaderBytes = 64;
inline constexpr std::uint32_t kPageSampleCap = 4096;
inline constexpr std::uint32_t kPagePayloadCap = 64 * 1024;
inline constexpr std::uint32_t kColumnFileCap = 64 * 1024 * 1024;
inline constexpr std::uint32_t kL0TileSpanMs = 60 * 1000;
inline constexpr std::uint32_t kRollupTileBuckets = 1024;

inline constexpr std::uint8_t kTEncDelta = 0;
inline constexpr std::uint8_t kTEncStride = 1;
inline constexpr std::uint8_t kTEncImplicit = 2;

inline constexpr std::uint8_t kVEncZigzag = 0;
inline constexpr std::uint8_t kVEncF32 = 1;
inline constexpr std::uint8_t kVEncF64 = 2;
inline constexpr std::uint8_t kVEncConstant = 3;

inline constexpr std::uint8_t kFlagLz4 = 1 << 0;
inline constexpr std::uint8_t kFlagDense = 1 << 1;
inline constexpr std::uint8_t kFlagLastRun = 1 << 2;
inline constexpr std::uint8_t kFlagWideSum = 1 << 3;

inline constexpr int kLevelCount = 6;

struct PageHeader {
    std::uint32_t magic = kPageMagic;
    std::uint32_t signalId = 0;
    std::uint64_t baseMs = 0;
    std::uint32_t sampleCount = 0;
    std::uint8_t tEnc = kTEncDelta;
    std::uint8_t vEnc = kVEncZigzag;
    std::uint8_t level = 0;
    std::uint8_t flags = 0;
    std::uint32_t spanMs = 0;
    std::uint32_t payloadBytes = 0;
};

struct IndexHeader {
    std::uint32_t magic = kIndexMagic;
    std::uint16_t version = kIndexVersion;
    std::uint8_t level = 0;
    std::uint8_t reserved0 = 0;
    std::uint16_t recordBytes = kIndexRecordBytes;
    std::uint16_t reserved1 = 0;
    std::uint32_t recordCount = 0;
    std::uint64_t minMs = 0;
    std::uint64_t maxMs = 0;
    std::uint32_t bucketMs = 0;
    std::uint32_t signalCount = 0;
};

struct IndexRecord {
    std::uint32_t signalId = 0;
    std::uint64_t startMs = 0;
    std::uint64_t endMs = 0;
    std::uint32_t fileNo = 0;
    std::uint32_t offset = 0;
    std::uint32_t pageBytes = 0;
    std::uint32_t sampleCount = 0;
    float vMin = 0.0f;
    float vMax = 0.0f;
    std::uint32_t reserved = 0;
};

struct RawSample {
    std::uint64_t timestampMs = 0;
    std::int64_t raw = 0;
    // True when this was reconstructed from a rollup bucket because L0 has been
    // dropped by retention (§11). The value is a real observed sample of the
    // signal, but the timestamp is a bucket edge rather than the arrival time.
    bool approximate = false;
};

struct RollupBucket {
    std::uint64_t startMs = 0;
    std::int64_t min = 0;
    std::int64_t max = 0;
    std::int64_t first = 0;
    std::int64_t last = 0;
    std::int64_t sum = 0;
    std::uint64_t sumHi = 0;
    std::uint32_t count = 0;
    bool present = true;
    bool wide = false;
};

struct SignalInfo {
    std::uint32_t id = 0;
    std::string message;
    std::string name;
    std::string unit;
    double factor = 1.0;
    double offset = 0.0;
    int bits = 0;
    bool isSigned = false;
    double min = 0.0;
    double max = 0.0;
    double nativeHz = 0.0;
    dbc::ValueType valueType = dbc::ValueType::Integer;
    std::uint8_t levels = 0x01;
};

struct SessionManifest {
    std::string id;
    std::uint64_t minMs = 0;
    std::uint64_t maxMs = 0;
    std::string dbcSha;
    std::string dbcLabel;
    std::uint8_t builtLevels = 0x01;
    std::uint64_t samples = 0;
    std::uint64_t frames = 0;
    // Bumped on every index checkpoint, compaction, retention drop and publish.
    // A reader compares it against what it loaded to know its index is stale;
    // see Store::refresh.
    std::uint64_t generation = 0;
    // A live writer owns this session right now. Set at createLive, cleared by
    // finish(); still set after a crash, which is what recoverSession looks for.
    bool live = false;
    // §8.5: add this to a stored video timestamp to get telemetry time.
    // Persisted so a manual nudge survives a restart when RTCP is unavailable.
    std::int64_t videoClockOffsetMs = 0;
    std::string videoClockProv;
    // Retention dropped L0 (§11). Provenance only: the authority is whether
    // l0.pidx carries records, which is what open() acts on.
    bool l0Dropped = false;
};

// Tuning for the live tail writer. The two costs being balanced are visibility
// lag (how long before a reader can see a sample) and page overhead: every
// sealed page costs 32 B of header plus a 48 B index record, so sealing tiny
// pages to shave lag inflates the store. Compaction reclaims that afterwards.
struct LiveOptions {
    // Seal an open L0 page once its oldest sample is this old in feed time,
    // provided the page also carries pageMinSamples.
    std::uint32_t pageMaxAgeMs = 1000;
    // Below this, an aged page waits for pageMaxIdleMs instead. Keeps a 0.3 Hz
    // signal from emitting a 3-sample page every second.
    std::uint32_t pageMinSamples = 256;
    // Backstop: seal regardless of sample count at this age, so a slow signal
    // still becomes visible.
    std::uint32_t pageMaxIdleMs = 60000;
    // Index + manifest rewrite cadence in feed time.
    std::uint32_t checkpointMs = 1000;
    // A checkpoint rewrites every level index in full, so its cost grows with
    // the session. The interval stretches to keep the rewrite under this many
    // bytes per second; without it a long session spends all its time writing
    // indexes.
    std::uint64_t indexRewriteBudgetBps = 4ull * 1024ull * 1024ull;
    // Do not judge a signal's rate, and so which rollup levels to write for it,
    // until it has this much evidence.
    std::uint32_t rateMinSamples = 64;
    std::uint32_t rateMinSpanMs = 5000;
};

struct Query {
    std::uint64_t fromMs = 0;
    std::uint64_t toMs = 0;
    std::vector<std::uint32_t> signalIds;
    std::uint32_t maxPoints = 2000;
    std::atomic<bool>* cancel = nullptr;
};

struct SeriesResult {
    std::uint32_t signalId = 0;
    int level = 0;
    std::uint32_t bucketMs = 0;
    std::optional<RawSample> prior;
    std::optional<RawSample> next;
    std::vector<std::uint64_t> timestamps;
    std::vector<std::int64_t> raw;
    std::vector<RollupBucket> buckets;
};

struct QueryResult {
    int level = 0;
    std::uint32_t bucketMs = 0;
    std::uint64_t fromMs = 0;
    std::uint64_t toMs = 0;
    std::vector<SeriesResult> series;
    bool truncated = false;
};

struct SessionStats {
    std::string id;
    std::uint64_t minMs = 0;
    std::uint64_t maxMs = 0;
    std::uint64_t samples = 0;
    std::uint64_t frames = 0;
    std::uint64_t storeBytes = 0;
};

struct IndexStats {
    std::string sessionId;  // first session built; kept for single-session callers
    std::uint64_t sourceBytes = 0;
    std::uint64_t storeBytes = 0;
    std::uint64_t samples = 0;
    std::uint64_t frames = 0;
    double elapsedS = 0.0;
    // Frames whose CAN ids matched nothing in the DBC. Read and discarded, so
    // they explain a gap between frames seen and samples stored.
    std::uint64_t undecodedFrames = 0;
    std::uint32_t emptySessions = 0;
    std::vector<SessionStats> sessions;
    // False when the walk aborted partway. Sessions already published are
    // valid, but the input was not fully indexed, so callers must not treat a
    // non-zero sample count as success.
    bool complete = false;
};

// A gap this large in the log stream ends a session. The car powering down or
// the logger restarting shows up as a wall-clock hole, and folding two runs
// separated by hours into one session ruins the manifest range that level
// selection depends on.
constexpr std::uint64_t kDefaultSessionGapMs = 300000;

// True when `tMs` is a session boundary relative to `lastMs`. Same threshold
// both ways: a 1 s clock rewind is jitter, not a new log. Live used to split
// on a 1 s reverse jump; the batch indexer already used this full gap.
inline bool isSessionBoundary(std::uint64_t tMs, std::uint64_t lastMs,
                              std::uint64_t gapMs = kDefaultSessionGapMs) {
    if (lastMs == 0 || gapMs == 0) {
        return false;
    }
    const std::uint64_t dt = tMs > lastMs ? tMs - lastMs : lastMs - tMs;
    return dt > gapMs;
}

std::uint32_t bucketMsOf(int level);
std::uint64_t tileSpanMs(int level);
std::int64_t tileIndexOf(std::uint64_t tMs, int level);
bool shouldBuildLevel(double nativeHz, int level);
int pickLevel(std::uint64_t fromMs, std::uint64_t toMs, std::uint32_t maxPoints, std::uint64_t rawCountInRange,
              std::uint8_t builtLevels);

void writeVarint(std::vector<std::uint8_t>& out, std::uint64_t v);
bool readVarint(std::span<const std::uint8_t> in, std::size_t& off, std::uint64_t& v);
void writeZigzag(std::vector<std::uint8_t>& out, std::int64_t v);
bool readZigzag(std::span<const std::uint8_t> in, std::size_t& off, std::int64_t& v);

void encodePageHeader(std::vector<std::uint8_t>& out, const PageHeader& header);
bool decodePageHeader(std::span<const std::uint8_t> bytes, PageHeader& header);

std::vector<std::uint8_t> encodeRawPage(std::uint32_t signalId, std::span<const std::uint64_t> times,
                                        std::span<const std::int64_t> values, bool lastRun = true);
bool decodeRawPage(std::span<const std::uint8_t> bytes, PageHeader& header, std::vector<std::uint64_t>& times,
                   std::vector<std::int64_t>& values);

std::vector<std::uint8_t> encodeRollupPage(std::uint32_t signalId, std::uint8_t level, std::uint64_t baseMs,
                                           std::uint32_t bucketMs, std::span<const RollupBucket> slots,
                                           bool lastRun = true, bool signedSum = false);
bool decodeRollupPage(std::span<const std::uint8_t> bytes, PageHeader& header, std::uint32_t bucketMs,
                      std::vector<RollupBucket>& slots);

std::vector<std::uint8_t> encodeIndex(const IndexHeader& header, std::span<const IndexRecord> records);
bool decodeIndex(std::span<const std::uint8_t> bytes, IndexHeader& header, std::vector<IndexRecord>& records);
std::size_t indexLowerBound(std::span<const IndexRecord> records, std::uint32_t signalId, std::uint64_t startMs);

class Store {
public:
    bool create(const std::filesystem::path& sessionDir, SessionManifest manifest, std::string* error = nullptr);
    // Live tail variant. Builds directly at sessionDir rather than in a
    // .building sibling, and writes session.json / signals.json immediately, so
    // the session is openable and queryable from the first checkpoint. A live
    // session cannot wait for an atomic-rename publish: it has no end.
    bool createLive(const std::filesystem::path& sessionDir, SessionManifest manifest, LiveOptions options,
                    std::string* error = nullptr);
    bool open(const std::filesystem::path& sessionDir, std::string* error = nullptr);
    void close();

    void setSignal(const SignalInfo& info);
    void setFrameCount(std::uint64_t frames) { manifest_.frames = frames; }
    void setVideoClock(std::int64_t offsetMs, std::string provenance) {
        manifest_.videoClockOffsetMs = offsetMs;
        manifest_.videoClockProv = std::move(provenance);
    }
    // Throw away an in-progress build without publishing it.
    void discard();
    void append(std::uint32_t signalId, std::uint64_t timestampMs, std::int64_t raw);
    bool finish(std::string* error = nullptr);

    // Live writer, driven by feed time (the largest sample timestamp seen).
    // tick() checkpoints when due and is cheap otherwise; checkpoint() forces
    // one. Both seal aged pages, then rewrite the level indexes and manifest.
    bool tick(std::uint64_t nowMs, std::string* error = nullptr);
    bool checkpoint(std::uint64_t nowMs, std::string* error = nullptr);
    bool isLive() const { return live_; }
    std::uint64_t checkpointCount() const { return checkpoints_; }
    // Reader side of the live seam: reload indexes, signals and manifest when
    // the writer has checkpointed since this handle last looked. Returns true
    // when a reload happened.
    bool refresh(std::string* error = nullptr);
    std::uint64_t generation() const { return manifest_.generation; }

    QueryResult query(const Query& q) const;
    std::optional<RawSample> prior(std::uint32_t signalId, std::uint64_t atMs) const;
    std::optional<RawSample> next(std::uint32_t signalId, std::uint64_t atMs) const;

    const SessionManifest& manifest() const { return manifest_; }
    const SignalInfo* signal(std::uint32_t id) const;
    const std::unordered_map<std::uint32_t, SignalInfo>& signals() const { return signals_; }
    const std::vector<IndexRecord>& index(int level) const;
    std::uint64_t rawCount(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs) const;
    std::uint8_t levelsOf(std::uint32_t signalId) const;
    std::filesystem::path dir() const { return dir_; }

private:
    std::filesystem::path dir_;       // where pages are written; the temp dir while building
    std::filesystem::path finalDir_;  // publish target, renamed into place by finish()
    SessionManifest manifest_{};
    std::unordered_map<std::uint32_t, SignalInfo> signals_;
    std::vector<IndexRecord> indexes_[kLevelCount]{};
    bool writable_ = false;
    bool finished_ = false;
    bool live_ = false;
    LiveOptions liveOpts_{};
    std::uint64_t lastCheckpointMs_ = 0;
    std::uint64_t checkpoints_ = 0;
    // Records below this offset in indexes_[L] are already sorted, so a
    // checkpoint sorts only the tail and merges. A full sort of a multi-hour
    // index once a second is the difference between viable and not.
    std::size_t sortedCount_[kLevelCount]{};

    struct LevelFile {
        std::uint32_t fileNo = 0;
        std::uint32_t offset = 0;
        std::filesystem::path path;
    };
    struct RollupAcc {
        std::uint32_t bucketMs = 0;
        std::uint8_t level = 0;
        bool open = false;
        RollupBucket cur{};
        std::vector<RollupBucket> pending;
    };
    struct Builder {
        std::uint32_t signalId = 0;
        std::vector<std::uint64_t> times;
        std::vector<std::int64_t> values;
        RollupAcc rollups[kLevelCount]{};
        std::uint64_t firstMs = 0;
        std::uint64_t lastMs = 0;
        std::uint64_t count = 0;
        std::int64_t vMin = 0;
        std::int64_t vMax = 0;
        bool rawConstant = true;
        bool rawStride = true;
        bool rawSpanSaturated = false;
        std::uint64_t rawFirstT = 0;
        std::uint64_t rawPrevT = 0;
        std::int64_t rawFirstV = 0;
        std::int64_t rawPrevV = 0;
        std::int64_t rawFirstDelta = 0;
        std::int64_t rawPrevDelta = 0;
        std::size_t rawTBytes = 0;
        std::size_t rawVConstBytes = 0;
        std::size_t rawVZigzagBytes = 0;
        // Levels this signal has qualified for during a live session. Latched:
        // a rate estimate that dips must not punch a hole in a level that is
        // already being written.
        std::uint8_t liveLevels = 0x01;
    };

    std::unordered_map<std::uint32_t, Builder> builders_;
    LevelFile files_[kLevelCount]{};
    std::filesystem::path lockPath_;

    // append() cannot report failure into a hot decode loop, so the first page
    // write error is latched here and finish() refuses to publish.
    bool writeFailed_ = false;
    std::string writeError_;
    // A live checkpoint rewrites files, and rewriting a level index that has not
    // changed since the last checkpoint is pure cost: the coarse levels of a
    // long session are almost never touched between two checkpoints.
    bool indexDirty_[kLevelCount]{};
    bool signalsDirty_ = true;

    Builder& builder(std::uint32_t signalId);
    void resetRawEnc(Builder& b);
    void noteRawSample(Builder& b, std::uint64_t timestampMs, std::int64_t raw);
    std::size_t rawEncodedPayload(const Builder& b) const;
    bool writePage(int level, std::span<const std::uint8_t> bytes, IndexRecord rec, std::string* error);
    void flushRaw(Builder& b, bool lastRun);
    void addRollup(Builder& b, int level, std::uint64_t t, std::int64_t raw);
    // closeOpen=false leaves the currently accumulating bucket alone. A live
    // checkpoint must not emit an open bucket: more samples can still land in
    // that slot, and a second page carrying the same slot start is a duplicate.
    void flushRollup(Builder& b, int level, bool lastRun, bool closeOpen = true);
    void sortIndexTails();
    bool hasLevel0(std::uint32_t signalId) const;
    std::optional<RawSample> priorFromRollup(std::uint32_t signalId, std::uint64_t atMs) const;
    std::optional<RawSample> nextFromRollup(std::uint32_t signalId, std::uint64_t atMs) const;
    bool readPageBytes(int level, const IndexRecord& rec, std::vector<std::uint8_t>& out) const;
    bool loadIndexes(std::string* error);
    // dirtyOnly skips levels untouched since the last successful write, which is
    // what a live checkpoint wants. finish() and batch publish pass false.
    bool writeIndexes(std::string* error, bool dirtyOnly = false);
    bool writeManifests(std::string* error, bool signalsOnlyIfDirty = false);
    SeriesResult querySignal(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs,
                             std::uint32_t maxPoints, std::atomic<bool>* cancel = nullptr) const;
};

// A non-empty sessionId forces every batch into one session. Left empty, the
// input is split on gaps of sessionGapMs (or on time running backwards).
IndexStats indexLogs(const std::filesystem::path& logs, const std::filesystem::path& outRoot, const dbc::Database& db,
                     std::string sessionId, std::string* error = nullptr,
                     std::uint64_t sessionGapMs = kDefaultSessionGapMs);

// Streams one pass over the logs and checks every session against its store.
// Sessions must not overlap in time.
bool validateSessions(std::span<const Store* const> stores, const std::filesystem::path& logs,
                      const dbc::Database& db, std::string* error = nullptr);

// Unbounded: holds every triple in memory. Tests and small inputs only.
// validateSessions streams instead and is what corpus-scale checks use.
bool collectDecodeTriples(const std::filesystem::path& logs, const dbc::Database& db,
                          std::vector<std::uint32_t>& ids, std::vector<std::uint64_t>& times,
                          std::vector<std::int64_t>& raws, std::string* error = nullptr);

bool validateAgainstDecode(const Store& store, const std::filesystem::path& logs, const dbc::Database& db,
                           std::string* error = nullptr);

// ---------------------------------------------------------------------------
// Phase 5: live tail writer, crash recovery, retention, compaction.
// ---------------------------------------------------------------------------

// The hook the backend needs. One call per CAN frame, in arrival order, with
// non-decreasing timestamps. Frames rather than decoded samples because the
// store holds the raw integer domain (§4) and DecodeBuffer decodes to physical;
// going back from physical loses the exactness the store exists to keep.
class IFrameSink {
public:
    virtual ~IFrameSink() = default;
    virtual void onFrame(std::uint32_t canId, std::uint8_t dlc, const std::uint8_t data[8],
                         std::uint64_t timestampMs) = 0;
};

// Drives a Store in live mode from a frame or sample feed. Owns the DBC decode
// so a caller only has to hand over frames.
class LiveTailWriter final : public IFrameSink {
public:
    bool begin(const std::filesystem::path& root, const std::string& sessionId, const dbc::Database& db,
               LiveOptions options, std::string* error = nullptr);
    void onFrame(std::uint32_t canId, std::uint8_t dlc, const std::uint8_t data[8],
                 std::uint64_t timestampMs) override;
    void onSample(std::uint32_t signalId, std::uint64_t timestampMs, std::int64_t raw);
    // Flush and close cleanly. After this the session is a normal sealed store.
    bool end(std::string* error = nullptr);

    Store& store() { return store_; }
    const Store& store() const { return store_; }
    std::uint64_t samples() const { return samples_; }
    std::uint64_t frames() const { return frames_; }
    std::uint64_t feedMs() const { return feedMs_; }
    bool failed() const { return failed_; }
    const std::string& lastError() const { return lastError_; }
    const std::filesystem::path& dir() const { return dir_; }

private:
    Store store_;
    const dbc::Database* db_ = nullptr;
    std::filesystem::path dir_;
    std::uint64_t samples_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t feedMs_ = 0;
    bool open_ = false;
    bool failed_ = false;
    std::string lastError_;
};

struct RecoveryReport {
    bool ranRecovery = false;
    std::uint32_t filesTruncated = 0;
    std::uint32_t filesRemoved = 0;
    // Index records pointing past the end of their column file. Needs a lost
    // buffered write to happen at all, since pages are written before they are
    // indexed, but a reader that trusts such a record decodes garbage.
    std::uint32_t recordsDropped = 0;
    std::uint64_t bytesDiscarded = 0;
    std::uint64_t generation = 0;
};

// Repair a session left open by a crashed writer, or by a compaction that died
// after writing pages but before swapping the index in. The level indexes are
// the only authority for where a page lives, so anything past the last indexed
// byte of a column file is unreachable and is discarded. Never resumes writing:
// the session becomes sealed as of its last checkpoint and a restarting writer
// opens a new session id.
bool recoverSession(const std::filesystem::path& sessionDir, RecoveryReport& report, std::string* error = nullptr);

struct RetentionPolicy {
    // Drop L0 once the session's newest sample is older than this. 0 disables.
    std::uint64_t l0MaxAgeMs = 0;
    // Drop L0, oldest session first, until total L0 bytes across the root fit.
    // 0 disables.
    std::uint64_t l0BudgetBytes = 0;
    // Before dropping, build L5 for any signal that has no rollup at all, so no
    // signal loses every copy of its data.
    bool sealPyramid = true;
    bool dryRun = false;
};

struct RetentionAction {
    std::string sessionId;
    std::uint64_t maxMs = 0;
    std::uint64_t ageMs = 0;
    std::uint64_t l0Bytes = 0;
    std::uint32_t sealedSignals = 0;
    std::string reason;  // "age" or "budget"
};

struct RetentionReport {
    std::uint32_t sessionsScanned = 0;
    std::uint32_t sessionsSkippedLive = 0;
    std::uint64_t l0BytesBefore = 0;
    std::uint64_t l0BytesFreed = 0;
    std::vector<RetentionAction> dropped;
    bool ok = false;
};

// L0 bytes for one session: its column files plus its page index.
std::uint64_t level0Bytes(const std::filesystem::path& sessionDir);

// Drop L0 for one session. Clears bit 0 from built_levels and from every
// signal's levels array, leaves an empty but well-formed l0.pidx, and bumps the
// generation. With sealPyramid, first builds L5 from L0 for signals that have no
// level above L0, and reports how many.
bool dropLevel0(const std::filesystem::path& sessionDir, bool sealPyramid, std::uint32_t* sealedSignals,
                std::uint64_t* freedBytes, std::string* error = nullptr);

RetentionReport applyRetention(const std::filesystem::path& root, const RetentionPolicy& policy, std::uint64_t nowMs,
                               std::string* error = nullptr);

struct CompactionOptions {
    std::uint32_t targetSamples = kPageSampleCap;
    std::uint32_t targetPayloadBytes = kPagePayloadCap;
    bool levels[kLevelCount] = {true, true, true, true, true, true};
};

struct CompactionReport {
    std::uint32_t pagesBefore[kLevelCount]{};
    std::uint32_t pagesAfter[kLevelCount]{};
    std::uint64_t bytesBefore = 0;
    std::uint64_t bytesAfter = 0;
    std::uint64_t samplesRewritten = 0;
    std::uint64_t generation = 0;
    double elapsedS = 0.0;
};

// Merge the many small pages a live writer leaves behind. Runs level by level,
// merging only runs of two or more adjacent pages of one signal; a page that is
// already alone in its run keeps its original record and is not rewritten.
// Merged pages go into fresh column files with higher file numbers, the index is
// swapped in atomically, and only then are unreferenced old files deleted — so a
// crash at any point leaves either the old index or the new one, never a mix.
bool compactSession(const std::filesystem::path& sessionDir, const CompactionOptions& options,
                    CompactionReport& report, std::string* error = nullptr);

std::string sessionIdFromMs(std::uint64_t timestampMs, int seq = 0);
double physical(const SignalInfo& sig, std::int64_t raw);

// Insert or replace one row in catalog.json. Backfilling or a live session
// publishing itself must not evict other sessions.
bool upsertCatalog(const std::filesystem::path& root, std::string sessionId, std::uint64_t minMs, std::uint64_t maxMs,
                   std::string dbcSha, std::string* error = nullptr);

struct CatalogListing {
    std::string id;
    std::string path;
    std::string dbcSha;
    std::uint64_t minMs = 0;
    std::uint64_t maxMs = 0;
    std::uint64_t sampleCount = 0;
    bool hasVideo = false;
};

// Rows from catalog.json, enriched from session.json / video.vpt when present.
// hasVideo is true only when video.vpt has a GOP overlapping [minMs, maxMs],
// not merely because photon-index copied the live pointer table into the session.
std::vector<CatalogListing> listCatalog(const std::filesystem::path& root);

bool videoVptOverlapsRange(const std::filesystem::path& vpt, std::uint64_t minMs, std::uint64_t maxMs);
bool copySessionVideoVpt(const std::filesystem::path& src, const std::filesystem::path& dest, std::uint64_t minMs,
                         std::uint64_t maxMs);

// Seal every session.json that still says live=true. A crashed writer leaves
// that flag set; recoverSession turns the last checkpoint into a readable store.
std::uint32_t recoverCrashedSessions(const std::filesystem::path& root, std::string* error = nullptr);

}  // namespace photon::tsdb
