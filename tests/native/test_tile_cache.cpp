#include "photon/control.hpp"
#include "photon/protocol.hpp"
#include "photon_ui/history_io.hpp"
#include "photon_ui/tile_cache.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

void runTileCacheTests();

namespace {

using photon::ui::TileCache;
using photon::ui::TileState;

photon::ctrl::Message makeResult(std::uint32_t queryId, std::uint32_t signalId, int level, std::uint32_t points,
                                 bool prior = false, bool next = false, std::uint64_t priorT = 0,
                                 std::uint64_t nextT = 0, double priorV = 11.0, double nextV = 99.0,
                                 std::uint64_t fromMs = 0, std::uint64_t toMs = 1000) {
    photon::ctrl::Message::HistorySeries s;
    s.signalId = signalId;
    s.level = level;
    s.points = points;
    if (prior) {
        s.prior.present = true;
        s.prior.t = priorT;
        s.prior.v = priorV;
    }
    if (next) {
        s.next.present = true;
        s.next.t = nextT;
        s.next.v = nextV;
    }
    return photon::ctrl::historyResult(queryId, level, 0, fromMs, toMs, {s}, false);
}

photon::proto::HistoryChunk makeRawChunk(std::uint32_t queryId, std::uint32_t signalId, std::uint8_t level,
                                         const std::vector<double>& t, const std::vector<float>& v, bool final) {
    photon::proto::HistoryChunk c;
    c.queryId = queryId;
    c.signalId = signalId;
    c.level = level;
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

void testFilesByServedLevel() {
    TileCache cache;
    cache.setSession("s", "dbc", 0, 120000);
    const std::uint32_t sid = 3;
    const auto qid = cache.prepareQuery(sid, 0, 60000, 5, false, false);
    cache.ingestResult(makeResult(qid, sid, 0, 3));
    cache.ingestChunk(makeRawChunk(qid, sid, 0, {10.0, 20.0, 30.0}, {1.0f, 2.0f, 3.0f}, true));

    const auto plot = cache.gatherPlot(sid, 0, 60000, 0);
    CHECK(plot.t.size() == 3);
    CHECK(!plot.envelope);
    CHECK(plot.y.size() == 3);
    CHECK(plot.y[0] == 1.0);
    CHECK(plot.y[2] == 3.0);
    CHECK(plot.yMin.empty());
}

void testProgressivePerRange() {
    TileCache cache;
    cache.setSession("s", "dbc", 0, 600000);
    const std::uint32_t sid = 4;
    const auto q0 = cache.prepareQuery(sid, 0, 60000, 0, false, false);
    cache.ingestResult(makeResult(q0, sid, 0, 2));
    cache.ingestChunk(makeRawChunk(q0, sid, 0, {1000.0, 2000.0}, {5.0f, 6.0f}, true));

    photon::proto::HistoryChunk coarse;
    coarse.queryId = cache.prepareQuery(sid, 60000, 200000, 1, false, false);
    cache.ingestResult(makeResult(coarse.queryId, sid, 1, 1));
    coarse.signalId = sid;
    coarse.level = 1;
    coarse.bucketMs = 1000;
    coarse.baseMs = 70000;
    coarse.slotCount = 1;
    coarse.valueCount = 1;
    coarse.flags = static_cast<std::uint16_t>(photon::proto::kHistDense | photon::proto::kHistFinal);
    coarse.minV = {8.0f};
    coarse.maxV = {9.0f};
    coarse.meanV = {8.5f};
    cache.ingestChunk(coarse);

    const auto plot = cache.gatherPlot(sid, 0, 120000, 0);
    CHECK(plot.t.size() >= 2);
    bool sawRaw = false;
    bool sawCoarse = false;
    for (std::size_t i = 0; i < plot.t.size(); ++i) {
        if (plot.t[i] <= 2000.0 && plot.y[i] == 5.0) {
            sawRaw = true;
        }
        if (plot.t[i] >= 60000.0) {
            sawCoarse = true;
        }
    }
    CHECK(sawRaw);
    CHECK(sawCoarse);
    CHECK(plot.envelope);
}

void testPerSignalSeam() {
    {
        TileCache cache;
        cache.setSession("s", "dbc", 0, 180000);
        cache.updateRingOldest(1, 30000);
        cache.updateRingOldest(2, 90000);
        CHECK(cache.seamFor(1) == 0);
        CHECK(cache.seamFor(2) == 0);
    }
    {
        TileCache cache;
        cache.setSession("s", "dbc", 0, 180000);
        cache.updateRingOldest(1, 30000);
        const auto q = cache.prepareQuery(1, 0, 60000, 0, false, false);
        cache.ingestResult(makeResult(q, 1, 0, 1));
        cache.ingestChunk(makeRawChunk(q, 1, 0, {1000.0}, {1.0f}, true));
        CHECK(cache.seamFor(1) == 0);
    }
    {
        TileCache cache;
        cache.setSession("s", "dbc", 0, 180000);
        cache.updateRingOldest(1, 60000);
        const auto q = cache.prepareQuery(1, 0, 60000, 0, false, false);
        cache.ingestResult(makeResult(q, 1, 0, 1));
        cache.ingestChunk(makeRawChunk(q, 1, 0, {1000.0}, {1.0f}, true));
        CHECK(cache.seamFor(1) == 60000);
        CHECK(cache.seamFor(2) == 0);
    }
    {
        TileCache cache;
        cache.setSession("s", "dbc", 0, 180000);
        cache.updateRingOldest(2, 120000);
        const auto q = cache.prepareQuery(2, 0, 60000, 0, false, false);
        cache.ingestResult(makeResult(q, 2, 0, 1));
        cache.ingestChunk(makeRawChunk(q, 2, 0, {1000.0}, {1.0f}, true));
        CHECK(cache.seamFor(2) == 0);
    }
}

void testNearestBoundSample() {
    TileCache cache;
    cache.setSession("s", "dbc", 0, 10000);
    const std::uint32_t sid = 8;
    const auto q1 = cache.prepareQuery(sid, 5000, 8000, 0, false, false);
    cache.ingestResult(makeResult(q1, sid, 0, 0, true, false, 1000, 0, 11.0, 0.0, 5000, 8000));
    const auto q2 = cache.prepareQuery(sid, 0, 2000, 0, false, false);
    cache.ingestResult(makeResult(q2, sid, 0, 0, true, false, 400, 0, 7.0, 0.0, 0, 2000));

    double v = 0;
    CHECK(cache.sampleAt(sid, 1500, v));
    CHECK(v == 7.0);
    CHECK(cache.sampleAt(sid, 6000, v));
    CHECK(v == 11.0);
}

void testClientFold() {
    TileCache cache;
    cache.setSession("s", "dbc", 0, 60000);
    const std::uint32_t sid = 9;
    const auto qid = cache.prepareQuery(sid, 0, 60000, 0, false, false);
    std::vector<double> t;
    std::vector<float> v;
    for (int i = 0; i < 16; ++i) {
        t.push_back(static_cast<double>(i * 10));
        v.push_back(static_cast<float>(i == 3 ? 40 : i));
    }
    cache.ingestResult(makeResult(qid, sid, 0, 16));
    cache.ingestChunk(makeRawChunk(qid, sid, 0, t, v, true));
    const auto folded = cache.gatherPlot(sid, 0, 60000, 4);
    CHECK(folded.t.size() <= 4);
    CHECK(folded.envelope);
    CHECK(folded.yMin.size() == folded.t.size());
    CHECK(folded.yMax.size() == folded.t.size());
    double mn = folded.yMin[0];
    double mx = folded.yMax[0];
    for (std::size_t i = 1; i < folded.yMin.size(); ++i) {
        mn = std::min(mn, folded.yMin[i]);
        mx = std::max(mx, folded.yMax[i]);
    }
    CHECK(mn == 0.0);
    CHECK(mx == 40.0);
}

struct RecordingIO : photon::ui::HistoryIO {
    std::unordered_set<std::uint32_t> requested;
    std::unordered_set<std::uint32_t> cancelled;
    void requestHistory(std::uint32_t queryId, std::string, std::uint64_t, std::uint64_t,
                        std::vector<std::uint32_t> signalIds, std::uint32_t) override {
        requested.insert(queryId);
        for (const auto id : signalIds) {
            (void)id;
        }
    }
    void cancelHistory(std::uint32_t queryId) override { cancelled.insert(queryId); }
};

void testOverviewPerSignal() {
    TileCache cache;
    cache.setSession("s", "dbc", 0, 600000);
    CHECK(!cache.hasOverview(1));
    CHECK(!cache.hasOverview(2));
    RecordingIO io;
    const std::uint32_t first[] = {1};
    cache.requestWindow(io, first, 0, 10000, 2000, false);
    CHECK(cache.hasOverview(1));
    CHECK(!cache.hasOverview(2));
    const std::uint32_t second[] = {2};
    cache.requestWindow(io, second, 0, 10000, 2000, false);
    CHECK(cache.hasOverview(2));
    CHECK(!io.requested.empty());
}

void testCancelLeaving() {
    TileCache cache;
    cache.setSession("s", "dbc", 0, 600000);
    RecordingIO io;
    const std::uint32_t ids[] = {1};
    cache.requestWindow(io, ids, 0, 10000, 2000, false);
    CHECK(!io.requested.empty());
    cache.cancelLeaving(io, 200000, 260000);
    CHECK(!io.cancelled.empty());
}

void testMemoryCapAndOverviewOvershoot() {
    TileCache cache;
    cache.setBudget(48 * 1024);
    cache.setSession("s", "dbc", 0, 3'600'000);
    RecordingIO io;
    std::vector<std::uint32_t> ids;
    ids.reserve(100);
    for (std::uint32_t i = 1; i <= 100; ++i) {
        ids.push_back(i);
    }
    cache.requestWindow(io, ids, 0, 120000, 2000, false);
    CHECK(cache.residentBytes() <= cache.budget());
    CHECK(cache.usedBytes() + cache.boundBytes() == cache.residentBytes());
    CHECK(cache.overshootBytes() == 0);
    CHECK(cache.tileCount() < 100);
    std::size_t marked = 0;
    for (const auto id : ids) {
        if (cache.hasOverview(id)) {
            ++marked;
        }
    }
    CHECK(marked < ids.size());
}

void testOverviewRefusalIsRetriable() {
    TileCache cache;
    cache.setBudget(1024);
    cache.setSession("s", "dbc", 0, 3'600'000);
    RecordingIO io;
    const std::uint32_t ids[] = {1};
    cache.requestWindow(io, ids, 0, 120000, 2000, false);
    CHECK(!cache.hasOverview(1));
    CHECK(cache.overviewRefuseCount() > 0);
    const auto plot = cache.gatherPlot(1, 0, 120000, 0);
    CHECK(plot.incomplete);
    CHECK(!plot.t.empty());

    cache.setBudget(256 * 1024);
    io.requested.clear();
    cache.requestWindow(io, ids, 0, 120000, 2000, false);
    CHECK(cache.hasOverview(1));
    CHECK(!io.requested.empty());
}

void testEvictRequestedSendsCancel() {
    TileCache cache;
    cache.setBudget(200 * 1024);
    cache.setSession("s", "dbc", 0, 180000);
    RecordingIO io;
    const std::uint32_t a[] = {1};
    cache.requestCursorWindow(io, a, 30000);
    CHECK(!io.requested.empty());
    const auto first = *io.requested.begin();
    const std::uint32_t b[] = {2};
    cache.requestCursorWindow(io, b, 30000);
    const std::uint32_t c[] = {3};
    cache.requestCursorWindow(io, c, 30000);
    CHECK(io.cancelled.contains(first));
    CHECK(cache.residentBytes() <= cache.budget());
}

void testCursorZeroUsesSessionMax() {
    TileCache cache;
    cache.setSession("s", "dbc", 0, 120000);
    RecordingIO none;
    const std::uint32_t ids[] = {1};
    {
        TileCache empty;
        empty.requestCursorWindow(none, ids, 0);
        CHECK(none.requested.empty());
    }
    RecordingIO io;
    cache.requestCursorWindow(io, ids, 0);
    CHECK(!io.requested.empty());
}

void testPrimeStartupIsNarrowerThanWindow() {
    TileCache primed;
    primed.setSession("s", "dbc", 0, 600000);
    RecordingIO primeIo;
    const std::uint32_t ids[] = {1, 2};
    primed.primeStartup(primeIo, ids, 0);
    CHECK(!primeIo.requested.empty());

    TileCache full;
    full.setSession("s", "dbc", 0, 600000);
    RecordingIO fullIo;
    full.requestWindow(fullIo, ids, 540000, 600000, 800, false);
    CHECK(primeIo.requested.size() <= fullIo.requested.size());
}

void testWrappedRingAndUnsortedTilesStayMonotonic() {
    TileCache cache;
    cache.setSession("s", "dbc", 0, 180000);
    const auto q = cache.prepareQuery(1, 0, 60000, 0, false, false);
    cache.ingestResult(makeResult(q, 1, 0, 4));
    cache.ingestChunk(makeRawChunk(q, 1, 0, {55000.0, 40000.0, 20000.0, 1000.0}, {4.0f, 3.0f, 2.0f, 1.0f}, true));
    CHECK(cache.seamFor(1) == 0);

    auto plot = cache.gatherPlot(1, 0, 60000, 0);
    CHECK(plot.t.size() == 4);
    for (std::size_t i = 1; i < plot.t.size(); ++i) {
        CHECK(plot.t[i] >= plot.t[i - 1]);
    }
    CHECK(plot.t.front() == 1000.0);
    CHECK(plot.t.back() == 55000.0);

    constexpr std::size_t cap = 4;
    std::vector<double> rt(cap, 0.0);
    std::vector<double> rv(cap, 0.0);
    std::size_t write = 0;
    std::size_t count = 0;
    auto push = [&](double t, double v) {
        if (count < cap) {
            rt[count] = t;
            rv[count] = v;
            ++count;
            write = count;
            return;
        }
        rt[write % cap] = t;
        rv[write % cap] = v;
        ++write;
        count = cap;
    };
    push(10000.0, 1.0);
    push(20000.0, 2.0);
    push(30000.0, 10.0);
    push(40000.0, 11.0);
    push(50000.0, 12.0);
    push(60000.0, 13.0);
    CHECK(count == cap);
    CHECK(write > cap);

    cache.stitchRing(plot, rt, rv, count, cap, write, 0, 60000, 1);
    CHECK(!plot.t.empty());
    for (std::size_t i = 1; i < plot.t.size(); ++i) {
        CHECK(plot.t[i] >= plot.t[i - 1]);
    }
    CHECK(plot.t.front() == 1000.0);
    CHECK(plot.t.back() == 60000.0);
}

void testOpenTilesDoNotWrapIntoRing() {
    TileCache cache;
    cache.setSession("s", "dbc", 0, 180000);
    cache.updateRingOldest(1, 30000);
    const auto q = cache.prepareQuery(1, 0, 60000, 0, false, false);
    cache.ingestResult(makeResult(q, 1, 0, 4));
    cache.ingestChunk(makeRawChunk(q, 1, 0, {1000.0, 20000.0, 40000.0, 55000.0}, {1.0f, 2.0f, 3.0f, 4.0f}, true));
    CHECK(cache.seamFor(1) == 0);

    auto plot = cache.gatherPlot(1, 0, 60000, 0);
    CHECK(plot.t.size() == 2);
    CHECK(plot.t[0] == 1000.0);
    CHECK(plot.t[1] == 20000.0);
    CHECK(plot.y[0] == 1.0);
    CHECK(plot.y[1] == 2.0);
    for (double t : plot.t) {
        CHECK(t < 30000.0);
    }

    const auto liveOnly = cache.gatherPlot(1, 40000, 60000, 0);
    CHECK(liveOnly.t.empty());

    const std::vector<double> rt{30000.0, 40000.0, 55000.0, 59000.0};
    const std::vector<double> rv{10.0, 11.0, 12.0, 13.0};
    cache.stitchRing(plot, rt, rv, rt.size(), rt.size(), rt.size(), 0, 60000, 1);
    CHECK(plot.t.size() == 6);
    for (std::size_t i = 1; i < plot.t.size(); ++i) {
        CHECK(plot.t[i] >= plot.t[i - 1]);
    }
    CHECK(plot.t.front() == 1000.0);
    CHECK(plot.t.back() == 59000.0);
    CHECK(plot.y[2] == 10.0);
}

void testSwitchingSessionDropsPreviousLog() {
    TileCache cache;
    cache.setTileStore(nullptr);
    cache.setSession("2026-07-23-01", "dbc", 5000, 7000);
    const auto q = cache.prepareQuery(1, 5000, 7000, 0, false, false);
    cache.ingestResult(makeResult(q, 1, 0, 2, false, false, 0, 0, 11.0, 99.0, 5000, 7000));
    cache.ingestChunk(makeRawChunk(q, 1, 0, {5000.0, 7000.0}, {120.0f, 121.0f}, true));
    CHECK(cache.gatherPlot(1, 5000, 7000, 0).t.size() == 2);

    cache.setSession("2026-07-18-01", "dbc", 100, 300);
    CHECK(cache.tileCount() == 0);
    const auto leftover = cache.gatherPlot(1, 100, 7000, 0);
    bool finite = false;
    for (double y : leftover.y) {
        if (std::isfinite(y)) {
            finite = true;
        }
    }
    CHECK(!finite);
    CHECK(cache.sessionId() == "2026-07-18-01");
    CHECK(cache.sessionMinMs() == 100);
    CHECK(cache.sessionMaxMs() == 300);
}

void testRunSwitchDoesNotClipNewSession() {
    TileCache cache;
    cache.setSession("run-a", "dbc", 1000, 5000);
    cache.updateRingOldest(1, 4000);
    const auto qa = cache.prepareQuery(1, 1000, 5000, 0, false, false);
    cache.ingestResult(makeResult(qa, 1, 0, 2, false, false, 0, 0, 11.0, 99.0, 1000, 5000));
    cache.ingestChunk(makeRawChunk(qa, 1, 0, {2000.0, 4500.0}, {1.0f, 2.0f}, true));
    CHECK(cache.gatherPlot(1, 1000, 5000, 0).t.size() == 1);

    cache.setSession("run-b", "dbc", 10000, 20000);
    CHECK(cache.tileCount() == 0);
    CHECK(cache.seamFor(1) == 0);
    const auto qb = cache.prepareQuery(1, 10000, 20000, 0, false, false);
    cache.ingestResult(makeResult(qb, 1, 0, 2, false, false, 0, 0, 11.0, 99.0, 10000, 20000));
    cache.ingestChunk(makeRawChunk(qb, 1, 0, {12000.0, 18000.0}, {8.0f, 9.0f}, true));
    const auto plot = cache.gatherPlot(1, 10000, 20000, 0);
    CHECK(plot.t.size() == 2);
    CHECK(plot.t[0] == 12000.0);
    CHECK(plot.t[1] == 18000.0);
}

void testBoundBytesInBudget() {
    TileCache cache;
    cache.setBudget(256 * 1024);
    cache.setSession("s", "dbc", 0, 60000);
    const auto qid = cache.prepareQuery(1, 0, 2000, 0, false, false);
    cache.ingestResult(makeResult(qid, 1, 0, 0, true, true, 100, 5000, 7.0, 9.0, 0, 2000));
    CHECK(cache.boundBytes() > 0);
    CHECK(cache.residentBytes() == cache.usedBytes() + cache.boundBytes());
    CHECK(cache.residentBytes() > cache.usedBytes());

    const auto beforeBounds = cache.boundBytes();
    cache.setBudget(32);
    RecordingIO io;
    const std::uint32_t more[] = {2};
    cache.requestCursorWindow(io, more, 10000);
    CHECK(cache.residentBytes() <= cache.budget() || cache.overshootBytes() > 0);
    CHECK(cache.residentBytes() == cache.usedBytes() + cache.boundBytes());
    CHECK(cache.boundBytes() < beforeBounds);
}

}  // namespace

void runTileCacheTests() {
    testFilesByServedLevel();
    testProgressivePerRange();
    testPerSignalSeam();
    testNearestBoundSample();
    testClientFold();
    testOverviewPerSignal();
    testCancelLeaving();
    testMemoryCapAndOverviewOvershoot();
    testOverviewRefusalIsRetriable();
    testEvictRequestedSendsCancel();
    testCursorZeroUsesSessionMax();
    testPrimeStartupIsNarrowerThanWindow();
    testOpenTilesDoNotWrapIntoRing();
    testWrappedRingAndUnsortedTilesStayMonotonic();
    testSwitchingSessionDropsPreviousLog();
    testRunSwitchDoesNotClipNewSession();
    testBoundBytesInBudget();
}
