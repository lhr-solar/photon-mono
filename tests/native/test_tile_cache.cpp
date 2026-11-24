#include "photon/protocol_v2.hpp"
#include "photon_ui/history_io.hpp"
#include "photon_ui/tile_cache.hpp"
#include "photon_ui/tile_levels.hpp"
#include "photon_ui/tile_queue.hpp"
#include "test_harness.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

void runTileCacheTests();

namespace {

using photon::ui::TileCache;
using photon::ui::TileLevels;
using photon::ui::TileRequestQueue;
using photon::ui::TileState;
namespace proto2 = photon::proto2;

// Bucket widths as hello_ack carries them today (photon-next/docs/protocol-v2.md).
const std::vector<std::int64_t> kLevels = {0, 10, 100, 1000, 10000, 60000, 600000};
constexpr std::uint32_t kId = 3;  // client signal id; the wire index is kId - 1
constexpr std::uint8_t kSealedLast = proto2::kTileSealed | proto2::kTileLast;
constexpr std::uint8_t kLastOnly = proto2::kTileLast;

// The cache clock is a plain function pointer, so the fake one reads a global.
double gFakeNow = 0.0;
double fakeNow() {
    return gFakeNow;
}

// Installs the fake clock for one test. The steady clock comes back at the end.
struct FakeClock {
    explicit FakeClock(double startMs) {
        gFakeNow = startMs;
        TileCache::setClock(&fakeNow);
    }
    ~FakeClock() { TileCache::setClock(nullptr); }
};

struct RecordingIO : photon::ui::HistoryIO {
    struct Req {
        std::uint32_t qid = 0;
        std::string runId;
        std::uint32_t signalId = 0;
        int level = 0;
        std::int64_t index = 0;
        bool urgent = false;
    };
    std::vector<Req> requests;
    std::vector<std::uint32_t> cancels;
    bool full = false;

    void requestTile(std::uint32_t qid, const std::string& runId, std::uint32_t signalId, int level,
                     std::int64_t index, bool urgent) override {
        requests.push_back(Req{qid, runId, signalId, level, index, urgent});
    }
    void cancelTile(std::uint32_t qid) override { cancels.push_back(qid); }
    bool queueFull() const override { return full; }

    std::size_t count() const { return requests.size(); }
    std::uint32_t lastQid() const { return requests.empty() ? 0u : requests.back().qid; }
};

// The qid of the most recent request for (level, index), or 0.
std::uint32_t qidFor(const RecordingIO& io, int level, std::int64_t index) {
    for (auto it = io.requests.rbegin(); it != io.requests.rend(); ++it) {
        if (it->level == level && it->index == index) {
            return it->qid;
        }
    }
    return 0;
}

// Requests kId over [fromMs, toMs] at the level that fits pixels. Returns the tiles that went out.
std::size_t askWindow(TileCache& cache, RecordingIO& io, std::uint64_t fromMs, std::uint64_t toMs,
                      std::uint32_t pixels) {
    const std::uint32_t ids[] = {kId};
    const std::size_t before = io.count();
    cache.requestWindow(io, ids, fromMs, toMs, pixels, false);
    return io.count() - before;
}

std::size_t askCursor(TileCache& cache, RecordingIO& io, std::uint64_t cursorMs) {
    const std::uint32_t ids[] = {kId};
    const std::size_t before = io.count();
    cache.requestCursorWindow(io, ids, cursorMs);
    return io.count() - before;
}

void configure(TileCache& cache) {
    cache.setLevels(kLevels);
    cache.setRun("run-1", 1, "sha-a", 0, 600000);
}

bool isState(const TileCache& cache, int level, std::int64_t index, TileState state) {
    const auto* tile = cache.find(kId, level, index);
    return tile != nullptr && tile->state == state;
}

// True when the plot holds no finite sample at all.
bool hasNoSamples(const photon::ui::PlotArrays& plot) {
    for (const double y : plot.y) {
        if (std::isfinite(y)) {
            return false;
        }
    }
    return true;
}

std::size_t nanCount(const std::vector<double>& y) {
    std::size_t n = 0;
    for (const double v : y) {
        if (std::isnan(v)) {
            ++n;
        }
    }
    return n;
}

// A raw (level 0) piece for kId's request. Index and offset are the piece's place in the tile.
proto2::TilePiece rawPiece(std::uint32_t qid, std::int64_t index, std::uint32_t offset, std::uint8_t flags,
                           const std::vector<double>& t, const std::vector<double>& v) {
    proto2::TilePiece p;
    p.qid = qid;
    p.signal = kId - 1;
    p.level = 0;
    p.flags = flags;
    p.index = index;
    p.offset = offset;
    p.n = static_cast<std::uint32_t>(t.size());
    p.tMs = t;
    p.value = v;
    return p;
}

// A bucketed (level > 0) piece for kId's request.
proto2::TilePiece bucketPiece(std::uint32_t qid, int level, std::int64_t index, std::uint32_t offset,
                              std::uint8_t flags, const std::vector<double>& t, const std::vector<double>& mean,
                              const std::vector<double>& mn, const std::vector<double>& mx) {
    proto2::TilePiece p;
    p.qid = qid;
    p.signal = kId - 1;
    p.level = static_cast<std::uint8_t>(level);
    p.flags = flags;
    p.index = index;
    p.offset = offset;
    p.n = static_cast<std::uint32_t>(t.size());
    p.tMs = t;
    p.mean = mean;
    p.min = mn;
    p.max = mx;
    return p;
}

// Level helper, per "Levels and tiles" and "Choosing a level" in protocol-v2.md.
void testLevelHelper() {
    const TileLevels lv(kLevels);
    CHECK(lv.valid());
    CHECK(lv.count() == 7);
    CHECK(lv.top() == 6);
    CHECK(lv.spanMs(0) == 10000);
    CHECK(lv.spanMs(1) == 10 * 1024);
    CHECK(lv.spanMs(2) == 100 * 1024);
    CHECK(lv.spanMs(6) == 600000 * 1024);

    // A tile index is floor(t / span).
    CHECK(lv.indexOf(0, 0.0) == 0);
    CHECK(lv.indexOf(0, 9999.0) == 0);
    CHECK(lv.indexOf(0, 10000.0) == 1);
    CHECK(lv.indexOf(0, 25000.0) == 2);
    CHECK(lv.indexOf(0, std::uint64_t{25000}) == 2);
    CHECK(lv.indexOf(1, 10240.0 * 3 + 5) == 3);
    CHECK(lv.indexOf(0, -5.0) == 0);
    CHECK(lv.indexOf(9, 25000.0) == 0);

    // The coarsest level whose bucket is at most visible / pixels, on a 1000 px plot.
    CHECK(lv.chooseLevel(5000, 1000) == 0);       // 5 ms per px: even 10 ms is too coarse
    CHECK(lv.chooseLevel(10000, 1000) == 1);      // 10 ms per px
    CHECK(lv.chooseLevel(600000, 1000) == 2);     // 600 ms per px: 100 ms fits, 1 s does not
    CHECK(lv.chooseLevel(3600000, 1000) == 3);    // 1 h: 3.6 s per px
    CHECK(lv.chooseLevel(60000000, 1000) == 5);   // 60 s per px
    CHECK(lv.chooseLevel(600000000, 1000) == 6);  // 600 s per px
    CHECK(lv.chooseLevel(3600000, 0) == 0);       // no plot to fit

    const TileLevels rawOnly(std::vector<std::int64_t>{0});
    CHECK(rawOnly.valid());
    CHECK(rawOnly.top() == 0);
    CHECK(rawOnly.chooseLevel(3600000, 1000) == 0);

    // A list that does not start with raw is not usable.
    const TileLevels bad(std::vector<std::int64_t>{10, 100});
    CHECK(!bad.valid());
    CHECK(bad.chooseLevel(3600000, 1000) == 0);
    CHECK(bad.indexOf(1, 25000.0) == 0);
}

// Pieces of one request assemble in order; the last one seals the tile.
void testPiecesAssembleInOrder() {
    TileCache cache;
    configure(cache);
    RecordingIO io;
    CHECK(askWindow(cache, io, 0, 5000, 1000) == 1);
    const auto qid = io.lastQid();
    CHECK(cache.ingestPiece(rawPiece(qid, 0, 0, 0, {1000.0, 2000.0}, {1.0, 2.0})));
    CHECK(cache.owns(qid));
    CHECK(isState(cache, 0, 0, TileState::Requested));
    CHECK(cache.ingestPiece(rawPiece(qid, 0, 2, kSealedLast, {3000.0}, {3.0})));
    CHECK(!cache.owns(qid));
    CHECK(isState(cache, 0, 0, TileState::Sealed));

    const auto plot = cache.gatherPlot(kId, 0, 5000, 0);
    CHECK(plot.t.size() == 3);
    CHECK(plot.y.size() == 3 && plot.y[0] == 1.0 && plot.y[1] == 2.0 && plot.y[2] == 3.0);
    CHECK(!plot.incomplete);
}

// A single last piece with no samples is a complete, empty tile: not unknown, not missing.
void testEmptyLastPieceIsAnEmptyTile() {
    TileCache cache;
    configure(cache);
    RecordingIO io;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
    const auto qid = io.lastQid();
    CHECK(cache.ingestPiece(rawPiece(qid, 0, 0, kLastOnly, {}, {})));
    CHECK(!cache.owns(qid));
    const auto* tile = cache.find(kId, 0, 0);
    CHECK(tile != nullptr);
    if (tile != nullptr) {
        CHECK(tile->state == TileState::Empty);
        CHECK(tile->haveData);
        CHECK(tile->t.empty());
    }
    const auto plot = cache.gatherPlot(kId, 0, 5000, 0);
    CHECK(plot.t.empty());
    CHECK(!plot.incomplete);
}

// An offset that does not follow the samples assembled so far fails the request and drops them.
void testOffsetGapFailsTheRequest() {
    TileCache cache;
    configure(cache);
    RecordingIO io;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
    const auto qid = io.lastQid();
    CHECK(cache.ingestPiece(rawPiece(qid, 0, 0, 0, {1000.0, 2000.0}, {1.0, 2.0})));
    // Two samples came before, so the next offset must be 2, not 3.
    CHECK(cache.ingestPiece(rawPiece(qid, 0, 3, kSealedLast, {4000.0}, {4.0})));
    CHECK(!cache.owns(qid));
    CHECK(isState(cache, 0, 0, TileState::Unknown));
    const auto* tile = cache.find(kId, 0, 0);
    CHECK(tile != nullptr && !tile->haveData);
    const auto plot = cache.gatherPlot(kId, 0, 5000, 0);
    CHECK(plot.incomplete);
    CHECK(hasNoSamples(plot));
    CHECK(!cache.ingestPiece(rawPiece(qid, 0, 2, kSealedLast, {3000.0}, {3.0})));
}

// A piece for another tile, signal or level fails the request it arrived on.
void testPieceForAnotherTileFailsTheRequest() {
    {
        TileCache cache;
        configure(cache);
        RecordingIO io;
        askWindow(cache, io, 0, 5000, 0);
        const auto qid = io.lastQid();
        CHECK(cache.ingestPiece(rawPiece(qid, 1, 0, kSealedLast, {1000.0}, {1.0})));
        CHECK(!cache.owns(qid));
        CHECK(isState(cache, 0, 0, TileState::Unknown));
    }
    {
        TileCache cache;
        configure(cache);
        RecordingIO io;
        askWindow(cache, io, 0, 5000, 0);
        const auto qid = io.lastQid();
        auto p = rawPiece(qid, 0, 0, kSealedLast, {1000.0}, {1.0});
        p.signal = kId;  // client id kId + 1
        CHECK(cache.ingestPiece(p));
        CHECK(!cache.owns(qid));
        CHECK(isState(cache, 0, 0, TileState::Unknown));
    }
    {
        TileCache cache;
        configure(cache);
        RecordingIO io;
        askWindow(cache, io, 0, 5000, 0);
        const auto qid = io.lastQid();
        auto p = rawPiece(qid, 0, 0, kSealedLast, {1000.0}, {1.0});
        p.level = 1;
        CHECK(cache.ingestPiece(p));
        CHECK(!cache.owns(qid));
        CHECK(isState(cache, 0, 0, TileState::Unknown));
    }
}

// tile_error drops the pieces received so far; later pieces for that qid are dropped too.
void testErrorDropsPartialPieces() {
    TileCache cache;
    configure(cache);
    RecordingIO io;
    askWindow(cache, io, 0, 5000, 0);
    const auto qid = io.lastQid();
    CHECK(cache.ingestPiece(rawPiece(qid, 0, 0, 0, {1000.0, 2000.0}, {1.0, 2.0})));
    CHECK(cache.ingestError(qid));
    CHECK(!cache.owns(qid));
    CHECK(isState(cache, 0, 0, TileState::Unknown));
    const auto* tile = cache.find(kId, 0, 0);
    CHECK(tile != nullptr && !tile->haveData);
    CHECK(hasNoSamples(cache.gatherPlot(kId, 0, 5000, 0)));
    CHECK(!cache.ingestPiece(rawPiece(qid, 0, 2, kSealedLast, {3000.0}, {3.0})));
    CHECK(!cache.ingestError(qid));
}

// A piece that arrives after cancelAll is dropped; the tile can be asked again under a new qid.
void testPieceAfterCancelIsDropped() {
    TileCache cache;
    configure(cache);
    RecordingIO io;
    askWindow(cache, io, 0, 5000, 0);
    const auto qid = io.lastQid();
    cache.cancelAll(io);
    CHECK(io.cancels.size() == 1 && io.cancels[0] == qid);
    CHECK(!cache.owns(qid));
    CHECK(cache.tileCount() == 0);
    CHECK(!cache.ingestPiece(rawPiece(qid, 0, 0, kSealedLast, {1000.0}, {1.0})));
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
    CHECK(io.lastQid() != qid);
}

// An open (unsealed) tile is asked again only after 2 s; while the refresh is in flight it is not asked again;
// a sealed tile is never asked again.
void testOpenTileAskedAgainAfterTwoSeconds() {
    FakeClock clock(100000.0);
    TileCache cache;
    configure(cache);
    RecordingIO io;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
    const auto q1 = io.lastQid();
    CHECK(cache.ingestPiece(rawPiece(q1, 0, 0, kLastOnly, {1000.0, 2000.0}, {1.0, 2.0})));
    CHECK(isState(cache, 0, 0, TileState::Open));

    gFakeNow += 1999.0;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 0);
    gFakeNow += 1.0;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
    CHECK(isState(cache, 0, 0, TileState::Requested));
    // The refresh is in flight: the samples it will replace stay plotted.
    CHECK(cache.gatherPlot(kId, 0, 5000, 0).t.size() == 2);
    gFakeNow += 5000.0;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 0);

    const auto q2 = io.lastQid();
    CHECK(q2 != q1);
    CHECK(cache.ingestPiece(rawPiece(q2, 0, 0, kSealedLast, {1000.0, 2000.0, 3000.0}, {1.0, 2.0, 3.0})));
    CHECK(isState(cache, 0, 0, TileState::Sealed));
    gFakeNow += 600000.0;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 0);
    CHECK(askCursor(cache, io, 1000) == 0);
    CHECK(io.count() == 2);
}

// An unsealed empty tile is asked again on the same 2 s schedule.
void testEmptyUnsealedTileAskedAgainAfterTwoSeconds() {
    FakeClock clock(300000.0);
    TileCache cache;
    configure(cache);
    RecordingIO io;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
    CHECK(cache.ingestPiece(rawPiece(io.lastQid(), 0, 0, kLastOnly, {}, {})));
    CHECK(isState(cache, 0, 0, TileState::Empty));
    gFakeNow += 1999.0;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 0);
    gFakeNow += 1.0;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
}

// After a gap in the live stream, unsealed tiles over the gap are asked at once; sealed ones are not.
void testNoteGapAsksOpenTilesNow() {
    FakeClock clock(400000.0);
    TileCache cache;
    configure(cache);
    RecordingIO io;
    CHECK(askWindow(cache, io, 0, 15000, 0) == 2);  // raw tiles 1 and 0
    CHECK(cache.ingestPiece(rawPiece(qidFor(io, 0, 0), 0, 0, kLastOnly, {1000.0}, {1.0})));
    CHECK(cache.ingestPiece(rawPiece(qidFor(io, 0, 1), 1, 0, kSealedLast, {11000.0}, {2.0})));
    gFakeNow += 100.0;
    CHECK(askWindow(cache, io, 0, 15000, 0) == 0);
    cache.noteGap(0, 20000);
    CHECK(askWindow(cache, io, 0, 15000, 0) == 1);
    CHECK(io.requests.back().index == 0);
}

// Errors back off 1 s, 2 s, 4 s, ... capped at 30 s. The retry is not sent a moment early.
void testRetryBackoffDoublesToCap() {
    FakeClock clock(50000.0);
    TileCache cache;
    configure(cache);
    RecordingIO io;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
    const double waits[] = {1000.0, 2000.0, 4000.0, 8000.0, 16000.0, 30000.0, 30000.0};
    for (const double wait : waits) {
        CHECK(cache.ingestError(io.lastQid()));
        gFakeNow += wait - 1.0;
        CHECK(askWindow(cache, io, 0, 5000, 0) == 0);
        gFakeNow += 1.0;
        CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
    }
}

// A success clears the failure streak, so the next error waits 1 s again.
void testSuccessClearsRetryStreak() {
    FakeClock clock(50000.0);
    TileCache cache;
    configure(cache);
    RecordingIO io;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
    CHECK(cache.ingestError(io.lastQid()));  // 1 s
    gFakeNow += 1000.0;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
    CHECK(cache.ingestError(io.lastQid()));  // 2 s
    gFakeNow += 2000.0;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
    CHECK(cache.ingestPiece(rawPiece(io.lastQid(), 0, 0, kLastOnly, {1000.0}, {1.0})));  // Open, streak cleared
    gFakeNow += 2000.0;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);  // open tile refresh
    CHECK(cache.ingestError(io.lastQid()));
    gFakeNow += 999.0;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 0);
    gFakeNow += 1.0;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
}

// A new rev drops every tile and pending request. A new span alone (min/max) keeps them.
void testRunChangeDropsTiles() {
    TileCache cache;
    configure(cache);
    RecordingIO io;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
    CHECK(cache.ingestPiece(rawPiece(io.lastQid(), 0, 0, kSealedLast, {1000.0}, {1.0})));
    CHECK(askWindow(cache, io, 10000, 15000, 0) == 1);  // tile 1 stays in flight
    const auto stale = io.lastQid();
    CHECK(cache.owns(stale));
    CHECK(cache.tileCount() == 2);

    cache.setRun("run-1", 1, "sha-a", 0, 900000);
    CHECK(cache.tileCount() == 2);
    CHECK(cache.runMaxMs() == 900000);

    cache.setRun("run-1", 2, "sha-a", 0, 900000);
    CHECK(cache.tileCount() == 0);
    CHECK(cache.find(kId, 0, 0) == nullptr);
    CHECK(!cache.owns(stale));
    CHECK(!cache.ingestPiece(rawPiece(stale, 1, 0, kSealedLast, {11000.0}, {2.0})));
    CHECK(hasNoSamples(cache.gatherPlot(kId, 0, 5000, 0)));

    // The new rev asks for the tile again, under a new qid.
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
    CHECK(io.lastQid() != stale);

    cache.setRun("run-1", 2, "sha-b", 0, 900000);
    CHECK(cache.tileCount() == 0);
    cache.setRun("run-2", 2, "sha-b", 0, 900000);
    CHECK(cache.tileCount() == 0);
}

// Raw gaps: a hole of more than 3 x period breaks the line with one NaN point between the two runs.
// With period 0 the limit is 3 x the median spacing of the tile.
photon::ui::PlotArrays gatherRawTile(const std::vector<double>& t, double periodMs) {
    TileCache cache;
    configure(cache);
    if (periodMs > 0.0) {
        std::unordered_map<std::uint32_t, double> periods{{kId, periodMs}};
        cache.setPeriods(periods);
    }
    RecordingIO io;
    askWindow(cache, io, 0, 9999, 0);
    std::vector<double> v;
    for (std::size_t i = 0; i < t.size(); ++i) {
        v.push_back(static_cast<double>(i + 1));
    }
    CHECK(cache.ingestPiece(rawPiece(io.lastQid(), 0, 0, kSealedLast, t, v)));
    return cache.gatherPlot(kId, 0, 9999, 0);
}

void testRawGapsBreakLines() {
    // Period 100 ms, a 700 ms hole between 1200 and 1900.
    const auto holed = gatherRawTile({1000.0, 1100.0, 1200.0, 1900.0, 2000.0, 2100.0}, 100.0);
    CHECK(holed.t.size() == 7);
    if (holed.t.size() == 7) {
        CHECK(std::isnan(holed.y[3]));
        CHECK(holed.t[3] == 1550.0);
        CHECK(holed.y[2] == 3.0);
        CHECK(holed.y[4] == 4.0);
    }
    // The same period with no hole: no break.
    const auto even = gatherRawTile({1000.0, 1100.0, 1200.0, 1300.0, 1400.0}, 100.0);
    CHECK(even.t.size() == 5);
    CHECK(nanCount(even.y) == 0);

    // Period unknown (0): the limit is 3 x the median spacing, which is 100 ms here, so the hole breaks.
    const auto medianHole = gatherRawTile(
        {1000.0, 1100.0, 1200.0, 1300.0, 1400.0, 2100.0, 2200.0, 2300.0, 2400.0}, 0.0);
    CHECK(medianHole.t.size() == 10);
    if (medianHole.t.size() == 10) {
        CHECK(std::isnan(medianHole.y[5]));
        CHECK(medianHole.t[5] == 1750.0);
    }
    const auto medianEven = gatherRawTile({1000.0, 1100.0, 1200.0, 1300.0, 1400.0, 1500.0, 1600.0, 1700.0, 1800.0}, 0.0);
    CHECK(medianEven.t.size() == 9);
    CHECK(nanCount(medianEven.y) == 0);
}

// Bucketed levels: a line breaks where neighbours are more than two bucket widths apart.
// Bucket 10 ms, samples at 0, 10, 40 break between 10 and 40. The period is unknown here, so only
// the bucket rule applies.
void testBucketLevelBreaksAfterTwoBuckets() {
    TileCache cache;
    configure(cache);
    RecordingIO io;
    CHECK(askWindow(cache, io, 0, 10000, 1000) == 1);
    CHECK(io.requests.back().level == 1);
    CHECK(io.requests.back().index == 0);
    CHECK(cache.ingestPiece(bucketPiece(io.lastQid(), 1, 0, 0, kSealedLast, {0.0, 10.0, 40.0}, {1.0, 2.0, 3.0},
                                        {0.0, 1.0, 2.0}, {2.0, 3.0, 4.0})));
    const auto plot = cache.gatherPlot(kId, 0, 10000, 1000);
    CHECK(plot.envelope);
    CHECK(plot.t.size() == 4);
    if (plot.t.size() == 4) {
        CHECK(plot.t[0] == 0.0);
        CHECK(plot.t[1] == 10.0);
        CHECK(std::isnan(plot.y[2]));
        CHECK(plot.t[2] == 25.0);
        CHECK(plot.t[3] == 40.0);
        CHECK(plot.y[3] == 3.0);
        CHECK(plot.yMin[3] == 2.0);
        CHECK(plot.yMax[3] == 4.0);
    }

    // Neighbours within two buckets (0, 10, 20, 30) do not break.
    TileCache even;
    configure(even);
    RecordingIO evenIo;
    CHECK(askWindow(even, evenIo, 0, 10000, 1000) == 1);
    CHECK(even.ingestPiece(bucketPiece(evenIo.lastQid(), 1, 0, 0, kSealedLast, {0.0, 10.0, 20.0, 30.0},
                                       {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0}, {1.0, 2.0, 3.0, 4.0})));
    const auto evenPlot = even.gatherPlot(kId, 0, 10000, 1000);
    CHECK(evenPlot.t.size() == 4);
    CHECK(nanCount(evenPlot.y) == 0);
}

// A span whose tile is not here is incomplete and draws no samples. A coarser cached tile fills it.
void testUnknownTileIsIncompleteAndCoarseFills() {
    TileCache cache;
    configure(cache);
    RecordingIO io;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);  // raw tile 0, still in flight
    auto plot = cache.gatherPlot(kId, 0, 5000, 0);
    CHECK(plot.incomplete);
    CHECK(hasNoSamples(plot));

    CHECK(askWindow(cache, io, 0, 10000, 1000) == 1);  // level 1 (10 ms buckets), tile 0
    CHECK(cache.ingestPiece(bucketPiece(io.lastQid(), 1, 0, 0, kSealedLast, {1000.0, 1010.0, 1020.0},
                                        {4.0, 5.0, 6.0}, {3.0, 4.0, 5.0}, {5.0, 6.0, 7.0})));
    plot = cache.gatherPlot(kId, 0, 5000, 0);
    CHECK(plot.incomplete);  // the raw tile is still missing
    CHECK(plot.envelope);
    CHECK(plot.t.size() == 3);
    CHECK(plot.y.size() == 3 && plot.y[0] == 4.0 && plot.y[2] == 6.0);
    CHECK(plot.t.size() == 3 && plot.t[0] == 1000.0);
}

// Nothing goes out before the levels and the run are known, and nothing while dragging.
void testNothingRequestedBeforeLevelsAndRun() {
    TileCache cache;
    RecordingIO io;
    CHECK(askWindow(cache, io, 0, 5000, 0) == 0);
    cache.setLevels(kLevels);
    CHECK(askWindow(cache, io, 0, 5000, 0) == 0);
    cache.setRun("run-1", 1, "sha-a", 0, 600000);
    const std::uint32_t ids[] = {kId};
    cache.requestWindow(io, ids, 0, 5000, 0, true);
    CHECK(io.count() == 0);
    CHECK(askWindow(cache, io, 0, 5000, 0) == 1);
}

// A window asks for the tiles of the level that fits its pixels, newest first and urgent.
void testWindowRequestsTheFittingLevel() {
    TileCache cache;
    configure(cache);
    RecordingIO io;
    // 10 min over 1000 px: 600 ms per px, so level 2 (100 ms buckets, 102.4 s tiles), indices 0..5.
    CHECK(askWindow(cache, io, 0, 600000, 1000) == 6);
    bool allLevel2 = true;
    bool allUrgent = true;
    for (const auto& r : io.requests) {
        allLevel2 = allLevel2 && r.level == 2;
        allUrgent = allUrgent && r.urgent;
    }
    CHECK(allLevel2);
    CHECK(allUrgent);
    CHECK(io.requests.front().index == 5);
    CHECK(io.requests.back().index == 0);
}

// Tiles that left the view are cancelled, but only when the transport cannot take more requests.
void testCancelLeavingOnlyWhenQueueFull() {
    TileCache cache;
    configure(cache);
    RecordingIO io;
    CHECK(askWindow(cache, io, 0, 59999, 0) == 6);  // raw tiles 0..5 in flight
    cache.cancelLeaving(io, 50000, 60000);
    CHECK(io.cancels.empty());
    io.full = true;
    cache.cancelLeaving(io, 50000, 60000);
    // Tile 5 spans [50000, 60000) and stays; tiles 0..4 left the view.
    CHECK(io.cancels.size() == 5);
    for (const auto& r : io.requests) {
        CHECK(cache.owns(r.qid) == (r.index == 5));
    }
}

TileRequestQueue::Request queuedRequest(std::uint32_t qid, bool urgent) {
    TileRequestQueue::Request r;
    r.qid = qid;
    r.run = "run-1";
    r.signal = kId;
    r.level = 0;
    r.index = qid;
    r.urgent = urgent;
    return r;
}

// At most 24 tile_request messages are in flight; one finishing lets exactly one more go.
void testRequestQueueCapsInFlight() {
    static_assert(TileRequestQueue::kMaxInFlight == 24);
    TileRequestQueue q;
    for (std::uint32_t i = 1; i <= 30; ++i) {
        q.push(queuedRequest(i, false));
    }
    const auto first = q.take();
    CHECK(first.size() == 24);
    CHECK(q.inflight() == 24);
    CHECK(q.queued() == 6);
    CHECK(q.take().empty());

    q.done(first[0].qid);
    CHECK(q.inflight() == 23);
    const auto next = q.take();
    CHECK(next.size() == 1);
    CHECK(next.size() == 1 && next[0].qid == 25);
    CHECK(q.inflight() == 24);
    CHECK(q.queued() == 5);
    CHECK(q.take().empty());

    // A queued request can be dropped; one already sent cannot.
    CHECK(q.dropQueued(30));
    CHECK(!q.dropQueued(2));
}

// Visible (urgent) requests go before priming ones; clear() hands back everything held.
void testRequestQueueSendsVisibleFirst() {
    TileRequestQueue q;
    q.push(queuedRequest(100, false));
    q.push(queuedRequest(101, false));
    q.push(queuedRequest(1, true));
    const auto out = q.take();
    CHECK(out.size() == 3);
    CHECK(out.size() == 3 && out[0].qid == 1 && out[1].qid == 100 && out[2].qid == 101);
    CHECK(out.size() == 3 && out[0].urgent && !out[1].urgent);

    q.push(queuedRequest(102, false));
    const auto held = q.clear();
    CHECK(held.size() == 4);  // 1, 100 and 101 in flight, 102 queued
    CHECK(q.queued() == 0);
    CHECK(q.inflight() == 0);
}

}  // namespace

void runTileCacheTests() {
    testLevelHelper();
    testPiecesAssembleInOrder();
    testEmptyLastPieceIsAnEmptyTile();
    testOffsetGapFailsTheRequest();
    testPieceForAnotherTileFailsTheRequest();
    testErrorDropsPartialPieces();
    testPieceAfterCancelIsDropped();
    testOpenTileAskedAgainAfterTwoSeconds();
    testEmptyUnsealedTileAskedAgainAfterTwoSeconds();
    testNoteGapAsksOpenTilesNow();
    testRetryBackoffDoublesToCap();
    testSuccessClearsRetryStreak();
    testRunChangeDropsTiles();
    testRawGapsBreakLines();
    testBucketLevelBreaksAfterTwoBuckets();
    testUnknownTileIsIncompleteAndCoarseFills();
    testNothingRequestedBeforeLevelsAndRun();
    testWindowRequestsTheFittingLevel();
    testCancelLeavingOnlyWhenQueueFull();
    testRequestQueueCapsInFlight();
    testRequestQueueSendsVisibleFirst();
}
