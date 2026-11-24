#include "photon/protocol_v2.hpp"
#include "photon_ui/history_io.hpp"
#include "photon_ui/tile_cache.hpp"
#include "photon_ui/tile_levels.hpp"
#include "photon_ui/tile_store.hpp"
#include "test_harness.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

void runTilePersistTests();

namespace {

using photon::ui::LoadedTileRange;
using photon::ui::PersistedTile;
using photon::ui::PersistedTileKey;
using photon::ui::TileCache;
using photon::ui::TileState;
namespace proto2 = photon::proto2;

const std::vector<std::int64_t> kLevels = {0, 10, 100, 1000, 10000, 60000, 600000};
constexpr std::uint32_t kId = 7;  // client signal id; the wire index is kId - 1
constexpr std::int64_t kRawSpan = photon::ui::kRawTileSpanMs;
constexpr std::uint8_t kSealedLast = proto2::kTileSealed | proto2::kTileLast;
constexpr std::uint8_t kLastOnly = proto2::kTileLast;
constexpr const char* kRun = "run-1";
constexpr const char* kSha = "sha-a";

// Storage behind the seam. Writes land synchronously so a test can inspect them.
// Reads are answered only when the test calls deliver(), which is what makes the
// asynchronous ordering in TileCache testable without a browser.
struct FakeTileStore : photon::ui::TileStore {
    struct Record {
        std::string runId;
        std::int64_t rev = 0;
        std::string sha;
        std::uint32_t signalId = 0;
        std::uint8_t level = 0;
        std::int64_t tileIndex = 0;
        std::vector<std::uint8_t> blob;
    };
    struct ReadRequest {
        std::uint32_t signalId = 0;
        std::uint8_t level = 0;
        std::int64_t indexFrom = 0;
        std::int64_t indexTo = 0;
    };

    std::string runId;
    std::int64_t rev = 0;
    std::string sha;
    std::vector<Record> records;
    std::vector<LoadedTileRange> inFlight;
    std::vector<LoadedTileRange> delivered;
    std::vector<ReadRequest> readLog;
    std::size_t putCount = 0;
    bool acceptReads = true;
    bool acceptWrites = true;
    // Lets a test hand an out-of-scope record to TileCache, so the cache's own scope
    // check is exercised rather than the store's sweep.
    bool ignoreScope = false;

    bool inScope(const Record& r) const { return r.runId == runId && r.rev == rev && r.sha == sha; }

    void setScope(const std::string& newRun, std::int64_t newRev, const std::string& newSha) override {
        runId = newRun;
        rev = newRev;
        sha = newSha;
        if (ignoreScope) {
            return;
        }
        std::vector<Record> keep;
        for (auto& r : records) {
            if (inScope(r)) {
                keep.push_back(std::move(r));
            }
        }
        records = std::move(keep);
    }

    bool requestRange(std::uint32_t signalId, std::uint8_t level, std::int64_t indexFrom,
                      std::int64_t indexTo) override {
        if (!acceptReads) {
            return false;
        }
        readLog.push_back(ReadRequest{signalId, level, indexFrom, indexTo});
        LoadedTileRange range;
        range.signalId = signalId;
        range.level = level;
        range.indexFrom = indexFrom;
        range.indexTo = indexTo;
        for (const auto& r : records) {
            if (!ignoreScope && !inScope(r)) {
                continue;
            }
            if (r.signalId != signalId || r.level != level || r.tileIndex < indexFrom || r.tileIndex > indexTo) {
                continue;
            }
            range.blobs.push_back(r.blob);
        }
        inFlight.push_back(std::move(range));
        return true;
    }

    // Hands the pending reads over through the wire envelope the JS bridge emits, so every
    // read-back test also covers encodeLoadedRange/decodeLoadedRange.
    void deliver() {
        for (const auto& range : inFlight) {
            LoadedTileRange back;
            if (photon::ui::decodeLoadedRange(photon::ui::encodeLoadedRange(range), back)) {
                delivered.push_back(std::move(back));
            }
        }
        inFlight.clear();
    }

    std::size_t takeCompleted(std::vector<LoadedTileRange>& out) override {
        const std::size_t n = delivered.size();
        for (auto& r : delivered) {
            out.push_back(std::move(r));
        }
        delivered.clear();
        return n;
    }

    bool put(const PersistedTileKey& key, const std::vector<std::uint8_t>& blob) override {
        if (!acceptWrites) {
            return false;
        }
        ++putCount;
        for (auto& r : records) {
            if (inScope(r) && r.signalId == key.signalId && r.level == key.level && r.tileIndex == key.tileIndex) {
                r.blob = blob;
                return true;
            }
        }
        records.push_back(Record{runId, rev, sha, key.signalId, key.level, key.tileIndex, blob});
        return true;
    }

    std::size_t storedBytes() const override {
        std::size_t n = 0;
        for (const auto& r : records) {
            n += r.blob.size();
        }
        return n;
    }
    std::size_t byteBudget() const override { return 64u * 1024u * 1024u; }

    const Record* find(std::uint32_t signalId, std::uint8_t level, std::int64_t index) const {
        for (const auto& r : records) {
            if (inScope(r) && r.signalId == signalId && r.level == level && r.tileIndex == index) {
                return &r;
            }
        }
        return nullptr;
    }
};

struct LoggingIO : photon::ui::HistoryIO {
    struct Req {
        std::uint32_t qid = 0;
        std::uint32_t signalId = 0;
        int level = 0;
        std::int64_t index = 0;
    };
    std::vector<Req> requests;
    bool full = false;

    void requestTile(std::uint32_t qid, const std::string&, std::uint32_t signalId, int level, std::int64_t index,
                     bool) override {
        requests.push_back(Req{qid, signalId, level, index});
    }
    void cancelTile(std::uint32_t) override {}
    bool queueFull() const override { return full; }

    std::size_t count() const { return requests.size(); }
    std::uint32_t lastQid() const { return requests.empty() ? 0u : requests.back().qid; }
    std::size_t countFor(std::uint32_t signalId, int level, std::int64_t index) const {
        std::size_t n = 0;
        for (const auto& r : requests) {
            if (r.signalId == signalId && r.level == level && r.index == index) {
                ++n;
            }
        }
        return n;
    }
};

void configureAt(TileCache& cache, FakeTileStore& store, std::int64_t rev, const std::string& sha) {
    cache.setTileStore(&store);
    cache.setLevels(kLevels);
    cache.setRun(kRun, rev, sha, 0, 600000);
}

void configure(TileCache& cache, FakeTileStore& store) {
    configureAt(cache, store, 1, kSha);
}

// Asks for the raw tile covering cursorMs (and the one before it, near a tile start).
std::size_t askCursor(TileCache& cache, LoggingIO& io, std::uint64_t cursorMs) {
    const std::uint32_t ids[] = {kId};
    const std::size_t before = io.count();
    cache.requestCursorWindow(io, ids, cursorMs);
    return io.count() - before;
}

// Asks for the raw tiles of [fromMs, toMs] (pixels 0 keeps the window at level 0).
std::size_t askRaw(TileCache& cache, LoggingIO& io, std::uint64_t fromMs, std::uint64_t toMs) {
    const std::uint32_t ids[] = {kId};
    const std::size_t before = io.count();
    cache.requestWindow(io, ids, fromMs, toMs, 0, false);
    return io.count() - before;
}

// The same, sent straight to the socket. A cold store is asked first and holds the request
// while it reads, so the writer-side helpers refuse the store's read and go to the socket.
std::size_t askSocket(TileCache& cache, LoggingIO& io, FakeTileStore& store, std::uint64_t fromMs,
                      std::uint64_t toMs) {
    const bool accepting = store.acceptReads;
    store.acceptReads = false;
    const std::size_t n = askRaw(cache, io, fromMs, toMs);
    store.acceptReads = accepting;
    return n;
}

bool isState(const TileCache& cache, std::int64_t index, TileState state) {
    const auto* tile = cache.find(kId, 0, index);
    return tile != nullptr && tile->state == state;
}

// Ingests a sealed raw tile of three samples for tile `index` of kId.
void ingestSealedTile(TileCache& cache, LoggingIO& io, FakeTileStore& store, std::int64_t index) {
    const auto from = static_cast<double>(index * kRawSpan);
    CHECK(askSocket(cache, io, store, static_cast<std::uint64_t>(from),
                    static_cast<std::uint64_t>(from) + kRawSpan - 1) == 1);
    const auto piece = [&] {
        proto2::TilePiece p;
        p.qid = io.lastQid();
        p.signal = kId - 1;
        p.level = 0;
        p.flags = kSealedLast;
        p.index = index;
        p.offset = 0;
        p.tMs = {from + 1000.0, from + 2000.0, from + 3000.0};
        p.value = {1.0, 2.0, 3.0};
        p.n = 3;
        return p;
    }();
    CHECK(cache.ingestPiece(piece));
}

// A sealed raw tile with 4000 samples, so its in-memory cost is well above a small test budget.
constexpr std::size_t kBigSamples = 4000;

void ingestBigSealedTile(TileCache& cache, LoggingIO& io, FakeTileStore& store, std::int64_t index) {
    const auto from = static_cast<double>(index * kRawSpan);
    CHECK(askSocket(cache, io, store, static_cast<std::uint64_t>(from),
                    static_cast<std::uint64_t>(from) + kRawSpan - 1) == 1);
    proto2::TilePiece p;
    p.qid = io.lastQid();
    p.signal = kId - 1;
    p.level = 0;
    p.flags = kSealedLast;
    p.index = index;
    p.offset = 0;
    p.n = static_cast<std::uint32_t>(kBigSamples);
    for (std::size_t i = 0; i < kBigSamples; ++i) {
        p.tMs.push_back(from + 1000.0 + static_cast<double>(i) * 2.0);  // 1 s .. 9 s, inside the 10 s tile
        p.value.push_back(static_cast<double>(i));
    }
    CHECK(cache.ingestPiece(p));
}

void testTileBlobRoundTrip() {
    PersistedTile p;
    p.runId = "run-1";
    p.rev = 1784841600123LL;
    p.sha = "abc123";
    p.signalId = 0xDEADBEEF;
    p.level = 2;
    p.tileIndex = 1784841600000 / 102400;
    p.bucketMs = 100;
    p.t = {1000.0, 2000.5, 3000.0};
    p.minV = {-1.5f, 0.0f, 2.5f};
    p.maxV = {1.5f, 4.0f, 9.5f};
    p.meanV = {0.0f, 2.0f, 6.0f};

    const auto blob = photon::ui::encodeTile(p);
    PersistedTile back;
    CHECK(photon::ui::decodeTile(blob, back));
    CHECK(back.formatVersion == photon::ui::kTileBlobVersion);
    CHECK(back.runId == p.runId);
    CHECK(back.rev == p.rev);
    CHECK(back.sha == p.sha);
    CHECK(back.signalId == p.signalId);
    CHECK(back.level == 2);
    CHECK(back.tileIndex == p.tileIndex);
    CHECK(back.bucketMs == 100);
    CHECK(back.t == p.t);
    CHECK(back.minV == p.minV);
    CHECK(back.maxV == p.maxV);
    CHECK(back.meanV == p.meanV);
    CHECK(back.rawV.empty());

    // Level 0 carries raw values and no envelope arrays.
    PersistedTile raw;
    raw.runId = "run-2";
    raw.rev = 7;
    raw.sha = "s";
    raw.signalId = 1;
    raw.level = 0;
    raw.tileIndex = 0;
    raw.t = {1000.0};
    raw.rawV = {5.5f};
    PersistedTile rawBack;
    CHECK(photon::ui::decodeTile(photon::ui::encodeTile(raw), rawBack));
    CHECK(rawBack.level == 0);
    CHECK(rawBack.rawV == raw.rawV);
    CHECK(rawBack.minV.empty());
}

// Wrong magic, another format version, truncation, spliced trailing bytes and a bad level are refused.
void testTileBlobRejectsBadRecords() {
    PersistedTile p;
    p.runId = "run-1";
    p.rev = 1;
    p.sha = "abc";
    p.signalId = 7;
    p.level = 2;
    p.tileIndex = 3;
    p.bucketMs = 100;
    p.t = {1000.0, 2000.0, 3000.0};
    p.minV = {1.0f, 2.0f, 3.0f};
    p.maxV = {1.0f, 2.0f, 3.0f};
    p.meanV = {1.0f, 2.0f, 3.0f};
    const auto blob = photon::ui::encodeTile(p);
    PersistedTile back;
    CHECK(photon::ui::decodeTile(blob, back));

    auto badMagic = blob;
    badMagic[0] = 0x00;
    CHECK(!photon::ui::decodeTile(badMagic, back));

    auto badVersion = blob;
    badVersion[4] = static_cast<std::uint8_t>(photon::ui::kTileBlobVersion + 1);
    CHECK(!photon::ui::decodeTile(badVersion, back));

    // The record's truncated payload no longer matches its counts.
    auto shortBlob = blob;
    shortBlob.resize(blob.size() - 8);
    CHECK(!photon::ui::decodeTile(shortBlob, back));

    // Trailing bytes are a spliced record, not a longer one.
    auto longBlob = blob;
    longBlob.push_back(0);
    CHECK(!photon::ui::decodeTile(longBlob, back));

    // Level sits after magic, version and signal id: offset 12.
    auto badLevel = blob;
    badLevel[12] = static_cast<std::uint8_t>(photon::ui::kTileBlobMaxLevel + 1);
    CHECK(!photon::ui::decodeTile(badLevel, back));

    CHECK(!photon::ui::decodeTile({}, back));
}

void testRangeEnvelopeRoundTrip() {
    LoadedTileRange range;
    range.signalId = 77;
    range.level = 3;
    range.indexFrom = -3;
    range.indexTo = 5;
    range.blobs.push_back({1, 2, 3});
    range.blobs.push_back({});
    range.blobs.push_back({9, 8});

    const auto env = photon::ui::encodeLoadedRange(range);
    LoadedTileRange back;
    CHECK(photon::ui::decodeLoadedRange(env, back));
    CHECK(back.signalId == 77);
    CHECK(back.level == 3);
    CHECK(back.indexFrom == -3);
    CHECK(back.indexTo == 5);
    CHECK(back.blobs.size() == 3);
    CHECK(back.blobs.size() == 3 && back.blobs[0] == std::vector<std::uint8_t>({1, 2, 3}));
    CHECK(back.blobs.size() == 3 && back.blobs[1].empty());
    CHECK(back.blobs.size() == 3 && back.blobs[2] == std::vector<std::uint8_t>({9, 8}));

    auto truncated = env;
    truncated.pop_back();
    CHECK(!photon::ui::decodeLoadedRange(truncated, back));

    auto badMagic = env;
    badMagic[1] = 0;
    CHECK(!photon::ui::decodeLoadedRange(badMagic, back));
}

// Cold start: a sealed tile written before the reload is restored from the store, and no
// socket request goes out for its key.
void testColdStartRestoresWithoutRequest() {
    FakeTileStore store;
    {
        TileCache writer;
        configure(writer, store);
        LoggingIO io;
        ingestSealedTile(writer, io, store, 0);
        writer.pump();
        CHECK(writer.persistWrittenCount() == 1);
        CHECK(store.find(kId, 0, 0) != nullptr);
    }

    TileCache reader;
    configure(reader, store);
    LoggingIO io;
    CHECK(askCursor(reader, io, 5000) == 0);
    CHECK(reader.persistPendingReads() == 1);
    CHECK(io.count() == 0);  // deferred behind the disk read

    store.deliver();
    reader.pump();
    CHECK(reader.persistRestoredCount() == 1);
    CHECK(askCursor(reader, io, 5000) == 0);
    CHECK(io.count() == 0);
    CHECK(isState(reader, 0, TileState::Sealed));

    const auto plot = reader.gatherPlot(kId, 0, kRawSpan - 1, 0);
    CHECK(plot.t.size() == 3);
    CHECK(plot.y.size() == 3 && plot.y[0] == 1.0 && plot.y[2] == 3.0);
}

// A disk read that answers with nothing releases the socket request for the tile.
void testDiskMissReleasesNetwork() {
    FakeTileStore store;
    TileCache cache;
    configure(cache, store);
    LoggingIO io;
    CHECK(askCursor(cache, io, 5000) == 0);
    CHECK(io.count() == 0);
    CHECK(askCursor(cache, io, 5000) == 0);  // still pending: the store is asked once only
    CHECK(store.readLog.size() == 1);

    store.deliver();
    cache.pump();
    CHECK(askCursor(cache, io, 5000) == 1);
    CHECK(io.countFor(kId, 0, 0) == 1);
    CHECK(store.readLog.size() == 1);
}

// A read the store accepts but never answers must not defer the socket forever.
void testDiskDeadlineReleasesNetwork() {
    FakeTileStore store;
    TileCache cache;
    configure(cache, store);
    LoggingIO io;
    CHECK(askCursor(cache, io, 5000) == 0);
    for (std::uint64_t i = 0; i < TileCache::kDiskDeadlinePumps + 2; ++i) {
        cache.pump();
    }
    CHECK(askCursor(cache, io, 5000) == 1);
    CHECK(io.countFor(kId, 0, 0) == 1);
}

// A read the store refuses is not recorded as asked, so it is not stuck. Here the
// network request still goes out at once.
void testRefusedReadDoesNotDeferNetwork() {
    FakeTileStore store;
    store.acceptReads = false;
    TileCache cache;
    configure(cache, store);
    LoggingIO io;
    CHECK(askCursor(cache, io, 5000) == 1);
    CHECK(io.countFor(kId, 0, 0) == 1);
    CHECK(cache.persistPendingReads() == 0);
    CHECK(store.readLog.empty());
}

// A refused read is asked of the store again on the next call, for as long as the tile is
// still absent. Here the transport is full, so nothing is sent and the tile stays absent.
void testRefusedReadIsAskedAgainWhileTileAbsent() {
    FakeTileStore store;
    store.acceptReads = false;
    TileCache cache;
    configure(cache, store);
    LoggingIO io;
    io.full = true;
    CHECK(askCursor(cache, io, 5000) == 0);
    CHECK(io.count() == 0);

    store.acceptReads = true;
    io.full = false;
    CHECK(askCursor(cache, io, 5000) == 0);
    CHECK(store.readLog.size() == 1);
    CHECK(store.readLog.size() == 1 && store.readLog[0].indexFrom == 0);
    CHECK(io.count() == 0);
}

// A tile that is still receiving pieces is not written; it is written once the last piece lands.
void testPartialTileIsNotPersisted() {
    FakeTileStore store;
    TileCache cache;
    configure(cache, store);
    LoggingIO io;
    CHECK(askSocket(cache, io, store, 0, 9999) == 1);
    const auto qid = io.lastQid();
    proto2::TilePiece first;
    first.qid = qid;
    first.signal = kId - 1;
    first.index = 0;
    first.flags = 0;
    first.tMs = {1000.0, 2000.0};
    first.value = {1.0, 2.0};
    first.n = 2;
    CHECK(cache.ingestPiece(first));
    cache.pump();
    CHECK(store.putCount == 0);
    CHECK(cache.persistWrittenCount() == 0);

    proto2::TilePiece last;
    last.qid = qid;
    last.signal = kId - 1;
    last.index = 0;
    last.offset = 2;
    last.flags = kSealedLast;
    last.tMs = {3000.0};
    last.value = {3.0};
    last.n = 1;
    CHECK(cache.ingestPiece(last));
    cache.pump();
    CHECK(store.putCount == 1);
    CHECK(store.find(kId, 0, 0) != nullptr);
}

// Open (unsealed) and empty tiles are never written. A sealed tile is.
void testOpenAndEmptyTilesAreNotWritten() {
    FakeTileStore store;
    TileCache cache;
    configure(cache, store);
    LoggingIO io;

    CHECK(askSocket(cache, io, store, 0, 9999) == 1);
    proto2::TilePiece openPiece;
    openPiece.qid = io.lastQid();
    openPiece.signal = kId - 1;
    openPiece.index = 0;
    openPiece.flags = kLastOnly;
    openPiece.tMs = {1000.0};
    openPiece.value = {1.0};
    openPiece.n = 1;
    CHECK(cache.ingestPiece(openPiece));
    CHECK(isState(cache, 0, TileState::Open));

    CHECK(askSocket(cache, io, store, 10000, 19999) == 1);
    proto2::TilePiece empty;
    empty.qid = io.lastQid();
    empty.signal = kId - 1;
    empty.index = 1;
    empty.flags = kLastOnly;
    empty.n = 0;
    CHECK(cache.ingestPiece(empty));
    CHECK(isState(cache, 1, TileState::Empty));

    cache.pump();
    cache.pump();
    CHECK(store.putCount == 0);
    CHECK(cache.persistWrittenCount() == 0);

    // Control: a sealed tile in the same cache is written.
    ingestSealedTile(cache, io, store, 2);
    cache.pump();
    CHECK(store.putCount == 1);
    CHECK(store.find(kId, 0, 2) != nullptr);
}

// A record whose payload does not match the key it was filed under lands its samples at the
// wrong time, and a record that does not decode is no record at all. Neither is restored.
void testMisfiledAndCorruptRecordsAreRejected() {
    FakeTileStore store;
    store.setScope(kRun, 1, kSha);

    PersistedTile p;
    p.runId = kRun;
    p.rev = 1;
    p.sha = kSha;
    p.signalId = kId;
    p.level = 0;
    p.tileIndex = 99;  // filed under index 0
    p.t = {1000.0};
    p.rawV = {5.0f};
    const PersistedTileKey misfiledKey{kId, 0, 0};
    const PersistedTileKey corruptKey{kId, 0, 1};
    const std::vector<std::uint8_t> garbage = {0xde, 0xad, 0xbe, 0xef};
    CHECK(store.put(misfiledKey, photon::ui::encodeTile(p)));
    CHECK(store.put(corruptKey, garbage));

    TileCache cache;
    configure(cache, store);
    LoggingIO io;
    const std::uint32_t ids[] = {kId};
    cache.requestWindow(io, ids, 0, 19999, 0, false);
    CHECK(io.count() == 0);  // both reads are deferred behind the store
    store.deliver();
    cache.pump();
    CHECK(cache.persistRestoredCount() == 0);
    CHECK(cache.find(kId, 0, 0) == nullptr);
    CHECK(cache.find(kId, 0, 1) == nullptr);

    // Once the disk has answered with nothing usable, both tiles go to the socket.
    cache.requestWindow(io, ids, 0, 19999, 0, false);
    CHECK(io.countFor(kId, 0, 0) == 1);
    CHECK(io.countFor(kId, 0, 1) == 1);
}

// A record from another rev or another catalog sha is not restored. The store sweeps
// records of a previous scope; when it does not, the cache checks the record itself.
void testRecordFromAnotherRevOrShaIsNotRestored() {
    FakeTileStore store;
    {
        TileCache writer;
        configure(writer, store);
        LoggingIO io;
        ingestSealedTile(writer, io, store, 0);
        writer.pump();
        CHECK(writer.persistWrittenCount() == 1);
    }

    // Rev 2: the store drops the rev 1 record when the scope changes.
    {
        TileCache reader;
        configureAt(reader, store, 2, kSha);
        LoggingIO io;
        CHECK(askCursor(reader, io, 5000) == 0);
        store.deliver();
        reader.pump();
        CHECK(reader.persistRestoredCount() == 0);
        CHECK(askCursor(reader, io, 5000) == 1);
        CHECK(io.countFor(kId, 0, 0) == 1);
    }

    // The store keeps the rev 1 record and hands it over anyway: the cache rejects it.
    store.ignoreScope = true;
    {
        TileCache reader;
        configureAt(reader, store, 1, "sha-b");
        LoggingIO io;
        CHECK(askCursor(reader, io, 5000) == 0);
        store.deliver();
        reader.pump();
        CHECK(reader.persistRestoredCount() == 0);
        CHECK(askCursor(reader, io, 5000) == 1);
        CHECK(io.countFor(kId, 0, 0) == 1);
    }
    store.ignoreScope = false;
}

// A restored tile is already in the store, so it is not written back by a pump.
void testRestoredTileIsNotRewritten() {
    FakeTileStore store;
    {
        TileCache writer;
        configure(writer, store);
        LoggingIO io;
        ingestSealedTile(writer, io, store, 0);
        writer.pump();
        CHECK(store.putCount == 1);
    }

    TileCache reader;
    configure(reader, store);
    LoggingIO io;
    CHECK(askCursor(reader, io, 5000) == 0);
    store.deliver();
    reader.pump();
    CHECK(reader.persistRestoredCount() == 1);
    reader.pump();
    reader.pump();
    CHECK(store.putCount == 1);
    CHECK(reader.persistWrittenCount() == 0);
}

// Each tile is written once it is sealed; a second pump writes nothing more.
void testSealedTileWrittenOnce() {
    FakeTileStore store;
    TileCache cache;
    configure(cache, store);
    LoggingIO io;
    ingestSealedTile(cache, io, store, 0);
    cache.pump();
    CHECK(store.putCount == 1);
    cache.pump();
    CHECK(store.putCount == 1);
    CHECK(cache.persistWrittenCount() == 1);
}

// The write queue drains at most kWritesPerPump tiles per pump; the rest wait for the next one.
void testWritesAreBatchedPerPump() {
    FakeTileStore store;
    TileCache cache;
    configure(cache, store);
    LoggingIO io;
    for (std::int64_t i = 0; i < 30; ++i) {
        ingestSealedTile(cache, io, store, i);
    }
    cache.pump();
    CHECK(store.putCount == TileCache::kWritesPerPump);
    cache.pump();
    CHECK(store.putCount == 30);
    CHECK(cache.persistWrittenCount() == 30);
}

// A write the store refuses is dropped, not retried.
void testRefusedWriteIsDropped() {
    FakeTileStore store;
    store.acceptWrites = false;
    TileCache cache;
    configure(cache, store);
    LoggingIO io;
    ingestSealedTile(cache, io, store, 0);
    cache.pump();
    CHECK(store.putCount == 0);
    CHECK(cache.persistWrittenCount() == 0);

    store.acceptWrites = true;
    cache.pump();
    CHECK(store.putCount == 0);
}

// A restore goes through the same budget as a socket tile: a tight budget evicts what it
// admits, and the resident bytes never pass the budget.
void testRestoreStaysWithinMemoryBudget() {
    FakeTileStore store;
    {
        TileCache writer;
        configure(writer, store);
        LoggingIO io;
        for (std::int64_t i = 0; i < 4; ++i) {
            ingestBigSealedTile(writer, io, store, i);
        }
        writer.pump();
        CHECK(writer.persistWrittenCount() == 4);
    }

    const auto restoreWindow = [&store](TileCache& cache) {
        LoggingIO io;
        const std::uint32_t ids[] = {kId};
        cache.requestWindow(io, ids, 0, 4 * static_cast<std::uint64_t>(kRawSpan) - 1, 0, false);
        store.deliver();
        cache.pump();
    };

    {
        TileCache tight;
        tight.setBudget(8 * 1024);
        configure(tight, store);
        restoreWindow(tight);
        CHECK(tight.residentBytes() <= tight.budget());
        CHECK(tight.tileCount() == 0);
    }
    {
        TileCache roomy;
        configure(roomy, store);
        restoreWindow(roomy);
        CHECK(roomy.persistRestoredCount() == 4);
        CHECK(roomy.tileCount() == 4);
        CHECK(roomy.residentBytes() > 4 * kBigSamples * sizeof(double));
        CHECK(roomy.residentBytes() <= roomy.budget());
    }
}

}  // namespace

void runTilePersistTests() {
    testTileBlobRoundTrip();
    testTileBlobRejectsBadRecords();
    testRangeEnvelopeRoundTrip();
    testColdStartRestoresWithoutRequest();
    testDiskMissReleasesNetwork();
    testDiskDeadlineReleasesNetwork();
    testRefusedReadDoesNotDeferNetwork();
    testRefusedReadIsAskedAgainWhileTileAbsent();
    testPartialTileIsNotPersisted();
    testOpenAndEmptyTilesAreNotWritten();
    testMisfiledAndCorruptRecordsAreRejected();
    testRecordFromAnotherRevOrShaIsNotRestored();
    testRestoredTileIsNotRewritten();
    testSealedTileWrittenOnce();
    testWritesAreBatchedPerPump();
    testRefusedWriteIsDropped();
    testRestoreStaysWithinMemoryBudget();
}
