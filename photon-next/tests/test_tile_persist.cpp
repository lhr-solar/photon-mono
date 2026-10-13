#include "wire/control.hpp"
#include "wire/protocol.hpp"
#include "store/tsdb.hpp"
#include "ui/history_io.hpp"
#include "ui/tile_cache.hpp"
#include "ui/tile_store.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

void runTilePersistTests();

namespace {

using photon::ui::LoadedTileRange;
using photon::ui::PersistedTile;
using photon::ui::PersistedTileKey;
using photon::ui::TileCache;

constexpr std::uint64_t kL0Span = 60000;

// Storage behind the seam. Writes land synchronously so a test can inspect
// them, but reads are delivered only when the test says so -- that is what makes
// the asynchronous ordering in TileCache testable without a browser.
struct FakeTileStore : photon::ui::TileStore {
    struct Record {
        std::string scope;
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

    std::string scope;
    std::vector<Record> records;
    std::vector<LoadedTileRange> inFlight;
    std::vector<LoadedTileRange> delivered;
    std::vector<ReadRequest> readLog;
    std::size_t putCount = 0;
    std::size_t bytes = 0;
    std::size_t budget = 64u * 1024u * 1024u;
    bool acceptReads = true;
    bool acceptWrites = true;
    // Lets a test hand an out-of-scope record to TileCache so the C++ side's own
    // scope check is exercised rather than the store's sweep.
    bool ignoreScope = false;

    void setScope(const std::string& sessionId, const std::string& dbcHash) override {
        scope = sessionId + "|" + dbcHash;
        // Scope selects records; the shared budget handles eviction.
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
            if (!ignoreScope && r.scope != scope) {
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

    // Round-trips through the wire envelope the JS bridge emits, so every
    // persistence test also covers encodeLoadedRange/decodeLoadedRange.
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
        const auto n = delivered.size();
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
        for (auto& r : records) {
            if (r.scope == scope && r.signalId == key.signalId && r.level == key.level &&
                r.tileIndex == key.tileIndex) {
                bytes = bytes - r.blob.size() + blob.size();
                r.blob = blob;
                ++putCount;
                return true;
            }
        }
        records.push_back(Record{scope, key.signalId, key.level, key.tileIndex, blob});
        bytes += blob.size();
        ++putCount;
        return true;
    }

    std::size_t storedBytes() const override { return bytes; }
    std::size_t byteBudget() const override { return budget; }

    const Record* find(std::uint32_t signalId, std::uint8_t level, std::int64_t index) const {
        for (const auto& r : records) {
            if (r.signalId == signalId && r.level == level && r.tileIndex == index) {
                return &r;
            }
        }
        return nullptr;
    }
};

struct LoggingIO : photon::ui::HistoryIO {
    struct Req {
        std::uint32_t queryId = 0;
        std::uint64_t fromMs = 0;
        std::uint64_t toMs = 0;
        std::uint32_t signalId = 0;
    };
    std::vector<Req> reqs;
    std::vector<std::uint32_t> cancels;

    void requestHistory(std::uint32_t queryId, std::string, std::uint64_t fromMs, std::uint64_t toMs,
                        std::vector<std::uint32_t> signalIds, std::uint32_t) override {
        reqs.push_back(Req{queryId, fromMs, toMs, signalIds.empty() ? 0u : signalIds.front()});
    }
    void cancelHistory(std::uint32_t queryId) override { cancels.push_back(queryId); }

    std::size_t countRange(std::uint32_t signalId, std::uint64_t fromMs, std::uint64_t toMs) const {
        std::size_t n = 0;
        for (const auto& r : reqs) {
            if (r.signalId == signalId && r.fromMs == fromMs && r.toMs == toMs) {
                ++n;
            }
        }
        return n;
    }
};

photon::ctrl::Message makeResult(std::uint32_t queryId, std::uint32_t signalId, int level, std::uint32_t points) {
    photon::ctrl::Message::HistorySeries s;
    s.signalId = signalId;
    s.level = level;
    s.points = points;
    return photon::ctrl::historyResult(queryId, level, 0, 0, 1000, {s}, false);
}

photon::proto::HistoryChunk makeRawChunk(std::uint32_t queryId, std::uint32_t signalId, const std::vector<double>& t,
                                         const std::vector<float>& v, bool final) {
    photon::proto::HistoryChunk c;
    c.queryId = queryId;
    c.signalId = signalId;
    c.level = 0;
    c.fields = photon::proto::kHistRaw;
    c.flags = photon::proto::kHistDense;
    if (final) {
        c.flags = static_cast<std::uint16_t>(c.flags | photon::proto::kHistFinal);
    }
    c.timestamps = t;
    c.rawV = v;
    c.slotCount = static_cast<std::uint32_t>(t.size());
    c.valueCount = static_cast<std::uint32_t>(v.size());
    if (!t.empty()) {
        c.baseMs = static_cast<std::uint64_t>(t.front());
    }
    return c;
}

// setSession with unchanged arguments does not reset the cache; it just runs one
// persistence pump, which is the only pump entry point that needs no HistoryIO.
void pump(TileCache& cache, std::uint64_t minMs, std::uint64_t maxMs) {
    cache.setSession(cache.sessionId(), cache.dbcHash(), minMs, maxMs);
}

// Ingests one complete L0 tile of raw samples covering [index*60s, index*60s+3s).
void ingestL0Tile(TileCache& cache, std::uint32_t signalId, std::int64_t index) {
    const auto from = static_cast<std::uint64_t>(index) * kL0Span;
    const auto qid = cache.prepareQuery(signalId, from, from + kL0Span, 0, false, false);
    cache.ingestResult(makeResult(qid, signalId, 0, 3));
    cache.ingestChunk(makeRawChunk(qid, signalId,
                                   {static_cast<double>(from + 1000), static_cast<double>(from + 2000),
                                    static_cast<double>(from + 3000)},
                                   {1.0f, 2.0f, 3.0f}, true));
}

// A tile whose actual byte cost is well above a small test budget, so an
// assertion about resident bytes can genuinely fail.
constexpr std::size_t kBigTileSamples = 4000;

void ingestBigL0Tile(TileCache& cache, std::uint32_t signalId, std::int64_t index) {
    const auto from = static_cast<std::uint64_t>(index) * kL0Span;
    std::vector<double> t;
    std::vector<float> v;
    t.reserve(kBigTileSamples);
    v.reserve(kBigTileSamples);
    for (std::size_t i = 0; i < kBigTileSamples; ++i) {
        t.push_back(static_cast<double>(from + 1000 + i * 10));
        v.push_back(static_cast<float>(i));
    }
    const auto qid = cache.prepareQuery(signalId, from, from + kL0Span, 0, false, false);
    cache.ingestResult(makeResult(qid, signalId, 0, static_cast<std::uint32_t>(kBigTileSamples)));
    cache.ingestChunk(makeRawChunk(qid, signalId, t, v, true));
}

// Larger than expectedTileBytes(L0) = 96 KiB so a restore that reserved the
// expected size can still land over a budget that only just covered the reserve.
constexpr std::size_t kHugeTileSamples = 12000;

void ingestHugeL0Tile(TileCache& cache, std::uint32_t signalId, std::int64_t index) {
    const auto from = static_cast<std::uint64_t>(index) * kL0Span;
    std::vector<double> t;
    std::vector<float> v;
    t.reserve(kHugeTileSamples);
    v.reserve(kHugeTileSamples);
    for (std::size_t i = 0; i < kHugeTileSamples; ++i) {
        t.push_back(static_cast<double>(from + 1000 + i * 4));
        v.push_back(static_cast<float>(i));
    }
    const auto qid = cache.prepareQuery(signalId, from, from + kL0Span, 0, false, false);
    cache.ingestResult(makeResult(qid, signalId, 0, static_cast<std::uint32_t>(kHugeTileSamples)));
    cache.ingestChunk(makeRawChunk(qid, signalId, t, v, true));
}

void testTileBlobRoundTrip() {
    PersistedTile p;
    p.sessionId = "20260723-160000-03";
    p.dbcHash = "abc123";
    p.signalId = 0xDEADBEEF;
    p.level = 2;
    p.tileIndex = 1784841600000 / 1024000;
    p.bucketMs = 1000;
    p.t = {1000.0, 2000.5, 3000.0};
    p.minV = {-1.5f, 0.0f, 2.5f};
    p.maxV = {1.5f, 4.0f, 9.5f};
    p.meanV = {0.0f, 2.0f, 6.0f};

    const auto blob = photon::ui::encodeTile(p);
    PersistedTile back;
    CHECK(photon::ui::decodeTile(blob, back));
    CHECK(back.formatVersion == photon::ui::kTileBlobVersion);
    CHECK(back.sessionId == p.sessionId);
    CHECK(back.dbcHash == p.dbcHash);
    CHECK(back.signalId == p.signalId);
    CHECK(back.level == 2);
    CHECK(back.tileIndex == p.tileIndex);
    CHECK(back.bucketMs == 1000);
    CHECK(back.t == p.t);
    CHECK(back.minV == p.minV);
    CHECK(back.maxV == p.maxV);
    CHECK(back.meanV == p.meanV);
    CHECK(back.rawV.empty());

    // A truncated record must not decode: the recorded sample count no longer
    // matches the payload.
    auto shortBlob = blob;
    shortBlob.resize(blob.size() - 8);
    CHECK(!photon::ui::decodeTile(shortBlob, back));

    // Trailing bytes are a spliced record, not a longer one.
    auto longBlob = blob;
    longBlob.push_back(0);
    CHECK(!photon::ui::decodeTile(longBlob, back));

    auto badMagic = blob;
    badMagic[0] = 0x00;
    CHECK(!photon::ui::decodeTile(badMagic, back));

    // Format version is the coarse invalidation lever; a bumped writer must not
    // be readable by this decoder.
    auto badVersion = blob;
    badVersion[4] = static_cast<std::uint8_t>(photon::ui::kTileBlobVersion + 1);
    CHECK(!photon::ui::decodeTile(badVersion, back));

    auto badLevel = blob;
    badLevel[12] = 9;
    CHECK(!photon::ui::decodeTile(badLevel, back));

    CHECK(!photon::ui::decodeTile({}, back));
}

void testRangeEnvelopeRoundTrip() {
    LoadedTileRange range;
    range.signalId = 77;
    range.level = 3;
    range.indexFrom = -0;
    range.indexTo = 5;
    range.blobs.push_back({1, 2, 3});
    range.blobs.push_back({});
    range.blobs.push_back({9, 8});

    const auto env = photon::ui::encodeLoadedRange(range);
    LoadedTileRange back;
    CHECK(photon::ui::decodeLoadedRange(env, back));
    CHECK(back.signalId == 77);
    CHECK(back.level == 3);
    CHECK(back.indexTo == 5);
    CHECK(back.blobs.size() == 3);
    CHECK(back.blobs[0] == std::vector<std::uint8_t>({1, 2, 3}));
    CHECK(back.blobs[1].empty());
    CHECK(back.blobs[2] == std::vector<std::uint8_t>({9, 8}));

    auto truncated = env;
    truncated.pop_back();
    CHECK(!photon::ui::decodeLoadedRange(truncated, back));

    auto badMagic = env;
    badMagic[1] = 0;
    CHECK(!photon::ui::decodeLoadedRange(badMagic, back));
}

// Cold start: a tile that was written before the reload is restored and its
// socket request is not issued, while a neighbouring tile that was never
// persisted still goes to the socket. Both halves matter -- the second is what
// stops the first from passing because nothing was requested at all.
void testColdStartRestoresAndSuppressesRequest() {
    FakeTileStore store;
    {
        TileCache writer;
        writer.setTileStore(&store);
        writer.setSession("s", "d", 0, 600000);
        ingestL0Tile(writer, 7, 0);
        pump(writer, 0, 600000);
        CHECK(writer.persistWrittenCount() == 1);
        CHECK(store.find(7, 0, 0) != nullptr);
    }

    TileCache reader;
    reader.setTileStore(&store);
    reader.setSession("s", "d", 0, 600000);
    LoggingIO io;
    const std::uint32_t ids[] = {7};
    reader.requestCursorWindow(io, ids, 30000);
    CHECK(reader.persistPendingReads() == 1);
    CHECK(io.reqs.empty());

    store.deliver();
    reader.requestCursorWindow(io, ids, 30000);
    CHECK(reader.persistRestoredCount() == 1);
    CHECK(reader.persistRejectedCount() == 0);

    const auto plot = reader.gatherPlot(7, 0, kL0Span - 1, 0);
    CHECK(plot.t.size() == 3);
    CHECK(plot.y[0] == 1.0);
    CHECK(plot.y[2] == 3.0);

    CHECK(io.countRange(7, 0, kL0Span) == 0);
    CHECK(io.countRange(7, kL0Span, 2 * kL0Span) == 1);
}

// A pending read defers the socket request for the tiles it covers, and the
// completion -- even an empty one -- releases it.
void testDiskReadDefersThenReleasesNetwork() {
    FakeTileStore store;
    TileCache cache;
    cache.setTileStore(&store);
    cache.setSession("s", "d", 0, 600000);
    LoggingIO io;
    const std::uint32_t ids[] = {7};

    cache.requestCursorWindow(io, ids, 30000);
    CHECK(io.reqs.empty());
    cache.requestCursorWindow(io, ids, 30000);
    CHECK(io.reqs.empty());
    CHECK(cache.persistPendingReads() == 1);

    store.deliver();
    cache.requestCursorWindow(io, ids, 30000);
    CHECK(cache.persistPendingReads() == 0);
    CHECK(cache.persistRestoredCount() == 0);
    CHECK(io.countRange(7, 0, kL0Span) == 1);
    CHECK(io.countRange(7, kL0Span, 2 * kL0Span) == 1);
}

// A bridge that accepts a read and never answers must not defer the socket
// forever.
void testDiskDeadlineReleasesNetwork() {
    FakeTileStore store;
    TileCache cache;
    cache.setTileStore(&store);
    cache.setSession("s", "d", 0, 600000);
    LoggingIO io;
    const std::uint32_t ids[] = {7};

    cache.requestCursorWindow(io, ids, 30000);
    CHECK(io.reqs.empty());
    for (std::uint64_t i = 0; i < TileCache::kDiskDeadlinePumps + 2; ++i) {
        pump(cache, 0, 600000);
    }
    CHECK(cache.persistPendingReads() == 0);
    cache.requestCursorWindow(io, ids, 30000);
    CHECK(io.countRange(7, 0, kL0Span) == 1);
}

// §9.5's overview-first guarantee outranks deduplication: the overview request
// goes out on the first frame even with an L5 read outstanding.
void testOverviewNotDeferredByPendingRead() {
    FakeTileStore store;
    TileCache cache;
    cache.setTileStore(&store);
    cache.setSession("s", "d", 0, 3'600'000);
    LoggingIO io;
    const std::uint32_t ids[] = {1};

    cache.requestWindow(io, ids, 0, 120000, 2000, false);
    CHECK(cache.persistPendingReads() > 0);
    CHECK(cache.hasOverview(1));
    const auto span = photon::tsdb::tileSpanMs(5);
    CHECK(io.countRange(1, 0, span) == 1);
}

// A tile that is still receiving chunks is not written, and the write happens as
// soon as the final chunk lands.
void testPartialTileIsNotPersisted() {
    FakeTileStore store;
    TileCache cache;
    cache.setTileStore(&store);
    cache.setSession("s", "d", 0, 600000);

    const auto qid = cache.prepareQuery(8, 0, kL0Span, 0, false, false);
    cache.ingestResult(makeResult(qid, 8, 0, 4));
    cache.ingestChunk(makeRawChunk(qid, 8, {1000.0, 2000.0}, {1.0f, 2.0f}, false));
    pump(cache, 0, 600000);
    CHECK(store.putCount == 0);
    CHECK(cache.persistWrittenCount() == 0);

    cache.ingestChunk(makeRawChunk(qid, 8, {3000.0, 4000.0}, {3.0f, 4.0f}, true));
    pump(cache, 0, 600000);
    CHECK(store.putCount == 1);
    CHECK(store.find(8, 0, 0) != nullptr);
}

// "Empty" is the one tile state that is a function of *when* you asked -- a live
// tail can fill a range that was empty a minute ago -- and it is also the state
// that most cheaply advances the seam. So a tile the server has since reported
// empty for its range is not written, even though it still holds samples from an
// earlier query. That is the flush-time re-validation in persistable().
void testEmptyMarkedTileIsNotPersisted() {
    FakeTileStore store;
    TileCache cache;
    cache.setTileStore(&store);
    cache.setSession("s", "d", 0, 600000);

    ingestL0Tile(cache, 5, 0);
    const auto qEmpty = cache.prepareQuery(5, 0, kL0Span, 0, false, false);
    cache.ingestResult(makeResult(qEmpty, 5, 0, 0));
    pump(cache, 0, 600000);
    CHECK(store.putCount == 0);
    CHECK(store.find(5, 0, 0) == nullptr);
    CHECK(cache.persistWrittenCount() == 0);

    // Control: the same shape without the empty report does write, so the check
    // above is not passing because nothing ever writes.
    ingestL0Tile(cache, 6, 0);
    pump(cache, 0, 600000);
    CHECK(store.putCount == 1);
    CHECK(store.find(6, 0, 0) != nullptr);
}

// A tile is only written once the session's write frontier has moved past its
// end, so a range that can still grow is never frozen onto disk.
void testTileNotPersistedUntilFrontierPasses() {
    FakeTileStore store;
    TileCache cache;
    cache.setTileStore(&store);
    cache.setSession("s", "d", 0, kL0Span);
    ingestL0Tile(cache, 7, 0);
    pump(cache, 0, kL0Span);
    CHECK(store.putCount == 0);

    cache.setSession("s", "d", 0, 200000);
    ingestL0Tile(cache, 7, 1);
    pump(cache, 0, 200000);
    CHECK(store.putCount == 1);
    CHECK(store.find(7, 0, 1) != nullptr);
    CHECK(store.find(7, 0, 0) == nullptr);
}

// The subtlest risk in this work: a restored tile must not claim Sealed where
// the socket path would have said Open, because Sealed adjoining the ring
// advances the seam and hides live samples.
void testRestoredTileCannotAdvanceSeamAcrossLive() {
    FakeTileStore store;
    {
        TileCache writer;
        writer.setTileStore(&store);
        writer.setSession("s", "d", 0, 600000);
        ingestL0Tile(writer, 1, 0);
        pump(writer, 0, 600000);
        CHECK(writer.persistWrittenCount() == 1);
    }

    const auto restore = [&store](TileCache& cache, LoggingIO& io) {
        const std::uint32_t ids[] = {1};
        cache.requestCursorWindow(io, ids, 30000);
        store.deliver();
        cache.requestCursorWindow(io, ids, 30000);
    };

    // Ring known before the restore: tile 0 ends at 60000 and the ring starts at
    // 30000, so the tile overlaps live and must land Open.
    {
        TileCache cache;
        cache.setTileStore(&store);
        cache.setSession("s", "d", 0, 600000);
        cache.updateRingOldest(1, 30000);
        LoggingIO io;
        restore(cache, io);
        CHECK(cache.persistRestoredCount() == 1);
        CHECK(cache.seamFor(1) == 0);
    }

    // Ring learned after the restore: the same test has to be re-applied when
    // updateRingOldest arrives, or the tile stays Sealed and moves the seam.
    {
        TileCache cache;
        cache.setTileStore(&store);
        cache.setSession("s", "d", 0, 600000);
        LoggingIO io;
        restore(cache, io);
        CHECK(cache.persistRestoredCount() == 1);
        cache.updateRingOldest(1, 30000);
        CHECK(cache.seamFor(1) == 0);
    }

    // And the guard must not over-suppress: a restored tile that ends exactly at
    // the ring does not overlap it, so the seam is allowed to advance.
    {
        TileCache cache;
        cache.setTileStore(&store);
        cache.setSession("s", "d", 0, 600000);
        cache.updateRingOldest(1, kL0Span);
        LoggingIO io;
        restore(cache, io);
        CHECK(cache.persistRestoredCount() == 1);
        CHECK(cache.seamFor(1) == kL0Span);
    }
}

// The 256 MiB in-memory cap stays hard. Persistence goes through the same
// reservation path, so it cannot be a back door for resident bytes.
void testRestoreRespectsMemoryCap() {
    FakeTileStore store;
    {
        TileCache writer;
        writer.setTileStore(&store);
        writer.setSession("s", "d", 0, 600000);
        for (std::int64_t i = 0; i < 4; ++i) {
            ingestBigL0Tile(writer, 7, i);
        }
        pump(writer, 0, 600000);
        CHECK(writer.persistWrittenCount() == 4);
    }

    // The second drain is a bare pump rather than another requestCursorWindow, so
    // the resident-byte assertions below measure the restore path itself and are
    // not rescued by the eviction that an enqueue would trigger.
    const auto restoreWindow = [&store](TileCache& cache) {
        LoggingIO io;
        const std::uint32_t ids[] = {7};
        cache.requestCursorWindow(io, ids, kL0Span + 30000);
        store.deliver();
        pump(cache, 0, 600000);
    };

    {
        TileCache tight;
        tight.setTileStore(&store);
        tight.setBudget(8 * 1024);
        tight.setSession("s", "d", 0, 600000);
        restoreWindow(tight);
        CHECK(tight.persistRefusedCount() > 0);
        CHECK(tight.persistRestoredCount() == 0);
        // Each stored tile is ~48 kB of samples against an 8 kB budget, so this
        // is a real bound and not a tautology.
        CHECK(tight.residentBytes() <= tight.budget());
    }
    {
        TileCache roomy;
        roomy.setTileStore(&store);
        roomy.setSession("s", "d", 0, 600000);
        restoreWindow(roomy);
        CHECK(roomy.persistRefusedCount() == 0);
        CHECK(roomy.persistRestoredCount() == 3);
        CHECK(roomy.residentBytes() > 3 * kBigTileSamples * sizeof(double));
        CHECK(roomy.residentBytes() <= roomy.budget());
    }
}

void testRestoreDropsWhenActualExceedsReserve() {
    FakeTileStore store;
    {
        TileCache writer;
        writer.setTileStore(&store);
        writer.setSession("s", "d", 0, 600000);
        ingestHugeL0Tile(writer, 9, 0);
        pump(writer, 0, 600000);
        CHECK(writer.persistWrittenCount() == 1);
    }

    TileCache tight;
    tight.setTileStore(&store);
    // Fits the 96 KiB L0 reserve, not the ~144 KiB decoded tile.
    tight.setBudget(110 * 1024);
    tight.setSession("s", "d", 0, 600000);
    LoggingIO io;
    const std::uint32_t ids[] = {9};
    tight.requestCursorWindow(io, ids, kL0Span / 2);
    store.deliver();
    pump(tight, 0, 600000);
    CHECK(tight.persistRefusedCount() > 0);
    CHECK(tight.persistRestoredCount() == 0);
    CHECK(tight.residentBytes() <= tight.budget());
}

// The same signal id under a different DBC is a different signal, so a record
// from another scope must be rejected by the cache itself and not only by the
// store's own sweep.
void testStaleScopeRecordIsRejected() {
    FakeTileStore store;
    {
        TileCache writer;
        writer.setTileStore(&store);
        writer.setSession("s", "d1", 0, 600000);
        ingestL0Tile(writer, 7, 0);
        pump(writer, 0, 600000);
        CHECK(writer.persistWrittenCount() == 1);
    }

    store.ignoreScope = true;
    TileCache reader;
    reader.setTileStore(&store);
    reader.setSession("s", "d2", 0, 600000);
    LoggingIO io;
    const std::uint32_t ids[] = {7};
    reader.requestCursorWindow(io, ids, 30000);
    store.deliver();
    reader.requestCursorWindow(io, ids, 30000);
    CHECK(reader.persistRejectedCount() > 0);
    CHECK(reader.persistRestoredCount() == 0);
    const auto plot = reader.gatherPlot(7, 0, kL0Span - 1, 0);
    CHECK(plot.incomplete);
}

// A record whose payload does not match the key it was filed under would land
// its samples at the wrong time. Reject it.
void testMisfiledRecordIsRejected() {
    FakeTileStore store;
    store.scope = "s|d";
    PersistedTile p;
    p.sessionId = "s";
    p.dbcHash = "d";
    p.signalId = 7;
    p.level = 0;
    p.tileIndex = 99;
    p.t = {1000.0};
    p.rawV = {5.0f};
    store.records.push_back(FakeTileStore::Record{"s|d", 7, 0, 0, photon::ui::encodeTile(p)});

    TileCache cache;
    cache.setTileStore(&store);
    cache.setSession("s", "d", 0, 600000);
    LoggingIO io;
    const std::uint32_t ids[] = {7};
    cache.requestCursorWindow(io, ids, 30000);
    store.deliver();
    cache.requestCursorWindow(io, ids, 30000);
    CHECK(cache.persistRejectedCount() == 1);
    CHECK(cache.persistRestoredCount() == 0);
}

// The store's own budget is separate and larger, but it is still a budget: once
// the store reports itself over, the backlog is shed rather than grown.
void testWritesStopWhenStoreOverBudget() {
    FakeTileStore store;
    store.budget = 128;
    store.bytes = 4096;
    TileCache cache;
    cache.setTileStore(&store);
    cache.setSession("s", "d", 0, 600000);
    ingestL0Tile(cache, 7, 0);
    pump(cache, 0, 600000);
    CHECK(store.putCount == 0);
    CHECK(cache.persistDroppedCount() > 0);
    CHECK(cache.persistWrittenCount() == 0);
}

// Tiles are re-filed whenever a second query overlaps them -- the level+/-1
// prefetch does exactly that -- so a tile already in the store must not be
// rewritten on every such query.
void testTileWrittenOnlyOnce() {
    FakeTileStore store;
    TileCache cache;
    cache.setTileStore(&store);
    cache.setSession("s", "d", 0, 600000);
    ingestL0Tile(cache, 7, 0);
    pump(cache, 0, 600000);
    CHECK(store.putCount == 1);

    ingestL0Tile(cache, 7, 0);
    pump(cache, 0, 600000);
    CHECK(store.putCount == 1);
    CHECK(cache.persistWrittenCount() == 1);
}

// A cold start must not write back what it just read: a restored tile is already
// in the store, so even when a later socket chunk re-files it, it is not
// rewritten.
void testRestoredTileIsNotRewritten() {
    FakeTileStore store;
    {
        TileCache writer;
        writer.setTileStore(&store);
        writer.setSession("s", "d", 0, 600000);
        ingestL0Tile(writer, 7, 0);
        pump(writer, 0, 600000);
        CHECK(store.putCount == 1);
    }

    TileCache reader;
    reader.setTileStore(&store);
    reader.setSession("s", "d", 0, 600000);
    LoggingIO io;
    const std::uint32_t ids[] = {7};
    reader.requestCursorWindow(io, ids, 30000);
    store.deliver();
    reader.requestCursorWindow(io, ids, 30000);
    CHECK(reader.persistRestoredCount() == 1);
    ingestL0Tile(reader, 7, 0);
    pump(reader, 0, 600000);
    CHECK(store.putCount == 1);
    CHECK(reader.persistWrittenCount() == 0);
}

// A tile index already read from disk is not read again, so a steady window does
// not re-issue the same range every frame.
void testDiskRangeAskedOnce() {
    FakeTileStore store;
    TileCache cache;
    cache.setTileStore(&store);
    cache.setSession("s", "d", 0, 600000);
    LoggingIO io;
    const std::uint32_t ids[] = {7};

    cache.requestCursorWindow(io, ids, 30000);
    CHECK(store.readLog.size() == 1);
    CHECK(store.readLog[0].indexFrom == 0);
    CHECK(store.readLog[0].indexTo == 1);
    store.deliver();
    cache.requestCursorWindow(io, ids, 30000);
    CHECK(store.readLog.size() == 1);

    // A window that moves past the asked span reads only the new indices.
    cache.requestCursorWindow(io, ids, 3 * kL0Span);
    CHECK(store.readLog.size() == 2);
    CHECK(store.readLog[1].indexFrom == 2);
    CHECK(store.readLog[1].indexTo == 4);
}

// A read the store refuses must not be recorded as asked, or the tile would be
// neither read from disk nor fetched from the socket.
void testRefusedReadIsRetried() {
    FakeTileStore store;
    store.acceptReads = false;
    TileCache cache;
    cache.setTileStore(&store);
    cache.setSession("s", "d", 0, 600000);
    LoggingIO io;
    const std::uint32_t ids[] = {7};

    cache.requestCursorWindow(io, ids, 30000);
    CHECK(cache.persistPendingReads() == 0);
    CHECK(io.countRange(7, 0, kL0Span) == 1);

    store.acceptReads = true;
    cache.requestCursorWindow(io, ids, 30000);
    CHECK(store.readLog.size() == 1);
    CHECK(store.readLog[0].indexFrom == 0);
}

}  // namespace

void runTilePersistTests() {
    {
        FakeTileStore store;
        TileCache cache;
        cache.setTileStore(&store);
        cache.setSession("a", "dbc", 0, 600000);
        ingestL0Tile(cache, 7, 0);
        pump(cache, 0, 600000);
        cache.setSession("b", "dbc", 0, 600000);
        ingestL0Tile(cache, 7, 0);
        pump(cache, 0, 600000);
        CHECK(store.records.size() == 2);
        TileCache fresh;
        fresh.setTileStore(&store);
        fresh.setSession("a", "dbc", 0, 600000);
        LoggingIO io;
        const std::uint32_t ids[] = {7};
        fresh.requestCursorWindow(io, ids, 2000);
        store.deliver();
        fresh.requestCursorWindow(io, ids, 2000);
        CHECK(fresh.persistRestoredCount() == 1);
        CHECK(fresh.gatherL0(7, 0, 60000).t.size() == 3);
    }
    testTileBlobRoundTrip();
    testRangeEnvelopeRoundTrip();
    testColdStartRestoresAndSuppressesRequest();
    testDiskReadDefersThenReleasesNetwork();
    testDiskDeadlineReleasesNetwork();
    testOverviewNotDeferredByPendingRead();
    testPartialTileIsNotPersisted();
    testEmptyMarkedTileIsNotPersisted();
    testTileNotPersistedUntilFrontierPasses();
    testRestoredTileCannotAdvanceSeamAcrossLive();
    testRestoreRespectsMemoryCap();
    testRestoreDropsWhenActualExceedsReserve();
    testStaleScopeRecordIsRejected();
    testMisfiledRecordIsRejected();
    testWritesStopWhenStoreOverBudget();
    testTileWrittenOnlyOnce();
    testRestoredTileIsNotRewritten();
    testDiskRangeAskedOnce();
    testRefusedReadIsRetried();
}
