#include "photon/control.hpp"
#include "photon/protocol.hpp"
#include "photon_ui/analysis_runtime.hpp"
#include "photon_ui/tile_cache.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

void runAnalysisRuntimeTests();

namespace {

using photon::ui::AnalysisBoundRun;
using photon::ui::AnalysisRunBinding;
using photon::ui::AnalysisRunRole;
using photon::ui::AnalysisRuntime;
using photon::ui::TileCache;

photon::ctrl::Message makeResult(std::uint32_t queryId, std::uint32_t signalId, int level, std::uint32_t points,
                                 std::uint64_t fromMs = 0, std::uint64_t toMs = 1000) {
    photon::ctrl::Message::HistorySeries s;
    s.signalId = signalId;
    s.level = level;
    s.points = points;
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

void ingestRaw(TileCache& cache, std::uint32_t signalId, const std::vector<double>& t, const std::vector<float>& v) {
    const auto qid = cache.prepareQuery(signalId, 0, 120000, 0, false, false);
    cache.ingestResult(makeResult(qid, signalId, 0, static_cast<std::uint32_t>(t.size()), 0, 120000));
    cache.ingestChunk(makeRawChunk(qid, signalId, 0, t, v, true));
}

void testConstructsWithoutApp() {
    TileCache cache;
    cache.setSession("run-a", "dbc", 0, 120000);
    CHECK(cache.sessionId() == "run-a");
    CHECK(cache.dbcHash() == "dbc");
    CHECK(cache.budget() == TileCache::kDefaultBudget);

    AnalysisRuntime runtime;
    CHECK(!runtime.hasBoundRun());
    CHECK(runtime.primary() == nullptr);
    const auto empty = runtime.gatherPlot(1, 0, 60000, 0);
    CHECK(empty.t.empty());
}

void testBindOwnsSeparateCache() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "sess-1";
    bind.dbcHash = "hash-1";
    bind.minMs = 1000;
    bind.maxMs = 90000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    CHECK(runtime.hasBoundRun());
    CHECK(runtime.boundCount() == 1);
    CHECK(runtime.primary() == run);
    CHECK(run->cache().sessionId() == "sess-1");
    CHECK(run->cache().dbcHash() == "hash-1");
    CHECK(run->cache().sessionMinMs() == 1000);
    CHECK(run->cache().sessionMaxMs() == 90000);
    CHECK(&run->cache() == &runtime.primary()->cache());
}

void testRequestCursorWindowUsesOwnedIo() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "sess-io";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    CHECK(run->io().requests().empty());

    const std::uint32_t ids[] = {7};
    runtime.requestCursorWindow(ids, 30000);
    CHECK(!run->io().requests().empty());
    CHECK(run->io().requests().front().sessionId == "sess-io");
    CHECK(!run->io().requests().front().signalIds.empty());
    CHECK(run->io().requests().front().signalIds.front() == 7);
}

void testGatherPlotOnOwnedCache() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "sess-plot";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);

    ingestRaw(run->cache(), 3, {10.0, 20.0, 30.0}, {1.0f, 2.0f, 3.0f});
    const auto viaRun = run->gatherPlot(3, 0, 60000, 0);
    CHECK(viaRun.t.size() == 3);
    CHECK(viaRun.y[0] == 1.0);
    CHECK(viaRun.y[2] == 3.0);

    const auto viaRuntime = runtime.gatherPlot(3, 0, 60000, 0);
    CHECK(viaRuntime.t.size() == 3);
    CHECK(viaRuntime.y[1] == 2.0);
}

void testPerRunCachesAreIsolated() {
    AnalysisRuntime runtime;
    AnalysisRunBinding primary;
    primary.sessionId = "primary";
    primary.dbcHash = "dbc-a";
    primary.role = AnalysisRunRole::Primary;
    primary.maxMs = 120000;
    auto* a = runtime.bind(std::move(primary));

    AnalysisRunBinding overlay;
    overlay.sessionId = "overlay";
    overlay.dbcHash = "dbc-b";
    overlay.role = AnalysisRunRole::Overlay;
    overlay.maxMs = 120000;
    auto* b = runtime.bind(std::move(overlay));

    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(a != b);
    CHECK(&a->cache() != &b->cache());
    CHECK(runtime.boundCount() == 2);
    CHECK(runtime.primary() == a);
    CHECK(runtime.find("overlay") == b);

    ingestRaw(a->cache(), 1, {10.0, 20.0}, {5.0f, 6.0f});
    ingestRaw(b->cache(), 1, {10.0}, {99.0f});

    const auto fromPrimary = runtime.gatherPlot(1, 0, 60000, 0);
    CHECK(fromPrimary.t.size() == 2);
    CHECK(fromPrimary.y[0] == 5.0);

    const auto fromOverlay = b->gatherPlot(1, 0, 60000, 0);
    CHECK(fromOverlay.t.size() == 1);
    CHECK(fromOverlay.y[0] == 99.0);
}

void testRebindPrimaryReplacesCache() {
    AnalysisRuntime runtime;
    AnalysisRunBinding first;
    first.sessionId = "old";
    first.dbcHash = "dbc";
    first.maxMs = 120000;
    auto* oldRun = runtime.bind(std::move(first));
    CHECK(oldRun != nullptr);
    ingestRaw(oldRun->cache(), 1, {10.0}, {1.0f});

    AnalysisRunBinding next;
    next.sessionId = "new";
    next.dbcHash = "dbc2";
    next.role = AnalysisRunRole::Primary;
    next.maxMs = 180000;
    auto* newRun = runtime.bind(std::move(next));
    CHECK(newRun != nullptr);
    CHECK(runtime.boundCount() == 1);
    CHECK(runtime.primary() == newRun);
    CHECK(runtime.find("old") == nullptr);
    CHECK(newRun->cache().sessionId() == "new");
    const auto plot = runtime.gatherPlot(1, 0, 60000, 0);
    CHECK(plot.incomplete);
    bool sawOld = false;
    for (const auto y : plot.y) {
        if (y == 1.0) {
            sawOld = true;
        }
    }
    CHECK(!sawOld);
}

void testQueryIdsStayInAnalysisRange() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "qid";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    CHECK(run->queryOrigin() >= photon::ui::kAnalysisQueryOrigin);
    CHECK(run->cache().nextQueryId() >= photon::ui::kAnalysisQueryOrigin);

    const std::uint32_t ids[] = {4};
    runtime.requestCursorWindow(ids, 30000);
    CHECK(!run->io().requests().empty());
    CHECK(photon::ui::isAnalysisQueryId(run->io().requests().front().queryId));
}

void testRequestWindowUsesOwnedIo() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "win";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    const std::uint32_t ids[] = {9};
    runtime.requestWindow(ids, 0, 120000, 200, false);
    CHECK(!run->io().requests().empty());
    CHECK(run->io().requests().front().sessionId == "win");
}

void testIngestRoutesToOwningRun() {
    AnalysisRuntime runtime;
    AnalysisRunBinding primary;
    primary.sessionId = "p";
    primary.dbcHash = "dbc";
    primary.maxMs = 600000;
    auto* a = runtime.bind(std::move(primary));

    AnalysisRunBinding overlay;
    overlay.sessionId = "o";
    overlay.dbcHash = "dbc";
    overlay.role = AnalysisRunRole::Overlay;
    overlay.maxMs = 600000;
    auto* b = runtime.bind(std::move(overlay));
    CHECK(a != nullptr);
    CHECK(b != nullptr);

    const std::uint32_t ids[] = {3};
    a->requestCursorWindow(ids, 30000);
    b->requestCursorWindow(ids, 30000);
    CHECK(!a->io().requests().empty());
    CHECK(!b->io().requests().empty());
    const auto aq = a->io().requests().front().queryId;
    const auto bq = b->io().requests().front().queryId;
    CHECK(aq != bq);
    CHECK(a->cache().hasInflight(aq));
    CHECK(!a->cache().hasInflight(bq));
    CHECK(b->cache().hasInflight(bq));

    CHECK(runtime.ingestResult(makeResult(bq, 3, 0, 2, 0, 120000)));
    CHECK(b->cache().hasInflight(bq));
    CHECK(!runtime.ingestResult(makeResult(aq + 0x00fffffeu, 3, 0, 1)));
}

void testDoesNotTouchSharedCache() {
    TileCache shared;
    shared.setSession("live-session", "live-dbc", 0, 999999);
    ingestRaw(shared, 1, {10.0}, {42.0f});
    const auto tilesBefore = shared.tileCount();
    const auto usedBefore = shared.usedBytes();

    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "hist-run";
    bind.dbcHash = "hist-dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    const std::uint32_t ids[] = {1};
    runtime.requestCursorWindow(ids, 30000);
    ingestRaw(run->cache(), 1, {10.0}, {1.0f});

    CHECK(shared.sessionId() == "live-session");
    CHECK(shared.dbcHash() == "live-dbc");
    CHECK(shared.tileCount() == tilesBefore);
    CHECK(shared.usedBytes() == usedBefore);
    const auto live = shared.gatherPlot(1, 0, 60000, 0);
    CHECK(live.y.size() == 1);
    CHECK(live.y[0] == 42.0);
    CHECK(&run->cache() != &shared);
}

struct ForwardIo final : photon::ui::HistoryIO {
    std::vector<std::uint32_t> sent;
    std::vector<std::uint32_t> cancelled;
    void requestHistory(std::uint32_t queryId, std::string, std::uint64_t, std::uint64_t,
                        std::vector<std::uint32_t>, std::uint32_t) override {
        sent.push_back(queryId);
    }
    void cancelHistory(std::uint32_t queryId) override { cancelled.push_back(queryId); }
};

void testTransportForwardsWithoutArena() {
    AnalysisRuntime runtime;
    ForwardIo fwd;
    runtime.setTransport(&fwd);
    AnalysisRunBinding bind;
    bind.sessionId = "fwd";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    CHECK(run->io().forward() == &fwd);
    const std::uint32_t ids[] = {2};
    runtime.requestCursorWindow(ids, 30000);
    CHECK(!run->io().requests().empty());
    CHECK(!fwd.sent.empty());
    CHECK(fwd.sent.front() == run->io().requests().front().queryId);
    CHECK(photon::ui::isAnalysisQueryId(fwd.sent.front()));
}

void testBudgetIsSeparateAndSplit() {
    AnalysisRuntime runtime;
    CHECK(runtime.budget() == TileCache::kDefaultBudget);
    CHECK(runtime.overshootBytes() == 0);

    AnalysisRunBinding primary;
    primary.sessionId = "a";
    primary.dbcHash = "dbc";
    primary.maxMs = 120000;
    auto* a = runtime.bind(std::move(primary));
    CHECK(a != nullptr);
    CHECK(a->cache().budget() == TileCache::kDefaultBudget);

    AnalysisRunBinding overlay;
    overlay.sessionId = "b";
    overlay.dbcHash = "dbc";
    overlay.role = AnalysisRunRole::Overlay;
    overlay.maxMs = 120000;
    auto* b = runtime.bind(std::move(overlay));
    CHECK(b != nullptr);
    CHECK(a->cache().budget() == TileCache::kDefaultBudget / 2);
    CHECK(b->cache().budget() == TileCache::kDefaultBudget / 2);
    CHECK(runtime.budget() == TileCache::kDefaultBudget);
}

void testSampleAtUsesOwnedL0() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "val";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    ingestRaw(run->cache(), 5, {10.0, 20.0, 30.0}, {1.5f, 2.5f, 3.5f});
    double v = 0.0;
    CHECK(runtime.sampleAt(5, 20, v));
    CHECK(v == 2.5);
    CHECK(run->sampleAt(5, 30, v));
    CHECK(v == 3.5);
}

void testScrubDragIssuesNoHistoryRequest() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "scrub";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    const std::uint32_t ids[] = {1};
    runtime.requestCursorWindow(ids, 30000);
    const auto n = run->io().requests().size();
    runtime.requestWindow(ids, 0, 60000, 200, true);
    CHECK(run->io().requests().size() == n);
}

void testRejectsEmptySession() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    CHECK(runtime.bind(std::move(bind)) == nullptr);
    CHECK(!runtime.hasBoundRun());
}

void testCursorScrubIsClientSide() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "scrub";
    bind.dbcHash = "dbc";
    bind.minMs = 1000;
    bind.maxMs = 90000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    CHECK(runtime.cursorMs() == 90000);
    CHECK(runtime.windowToMs() == 90000);
    CHECK(runtime.windowFromMs() < runtime.windowToMs());
    CHECK(!runtime.scrubbing());

    runtime.beginScrub();
    runtime.previewScrub(5000);
    CHECK(runtime.scrubbing());
    CHECK(runtime.displayCursorMs() == 5000);
    CHECK(runtime.cursorMs() == 90000);
    runtime.commitScrub();
    CHECK(!runtime.scrubbing());
    CHECK(runtime.cursorMs() == 5000);
    CHECK(runtime.displayCursorMs() == 5000);
}

void testHoldViewKeepsWindowWhileScrubbing() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "hold";
    bind.dbcHash = "dbc";
    bind.minMs = 0;
    bind.maxMs = 90000;
    CHECK(runtime.bind(std::move(bind)) != nullptr);
    runtime.setWindowMs(10000);
    runtime.setCursorMs(20000);
    runtime.holdView(10000, 20000);
    CHECK(runtime.viewHeld());
    CHECK(runtime.windowFromMs() == 10000);
    CHECK(runtime.windowToMs() == 20000);
    runtime.previewScrub(15000);
    CHECK(runtime.displayCursorMs() == 15000);
    CHECK(runtime.windowFromMs() == 10000);
    CHECK(runtime.windowToMs() == 20000);
    runtime.releaseView();
    CHECK(!runtime.viewHeld());
    CHECK(runtime.windowToMs() == runtime.displayCursorMs());
}

void testValueSampleIsL0OrPrior() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "value";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    ingestRaw(run->cache(), 3, {10.0, 20.0, 30.0}, {1.0f, 2.0f, 3.0f});
    double v = 0.0;
    CHECK(runtime.sampleAt(3, 20, v));
    CHECK(v == 2.0);
    CHECK(runtime.sampleAt(3, 25, v));
    CHECK(v == 2.0);
}

void testScrubDoesNotEnqueueWhenDragging() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "drag";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    const std::uint32_t ids[] = {1};
    runtime.requestWindow(ids, 0, 60000, 200, true);
    CHECK(run->io().requests().empty());
}

void testTickHistoryCancelsOnWindowChange() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "win-cancel";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    runtime.setWindowMs(10000);
    runtime.setCursorMs(10000);
    const std::uint32_t ids[] = {1};
    runtime.tickHistory(ids, 2000);
    CHECK(!run->io().requests().empty());
    CHECK(run->io().cancels().empty());

    runtime.setCursorMs(250000);
    runtime.tickHistory(ids, 2000);
    CHECK(!run->io().cancels().empty());
}

void testTickHistoryNoCancelOnSameWindow() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "win-same";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    runtime.setWindowMs(10000);
    runtime.setCursorMs(10000);
    const std::uint32_t ids[] = {1};
    runtime.tickHistory(ids, 2000);
    runtime.tickHistory(ids, 2000);
    CHECK(run->io().cancels().empty());
}

void testTickHistoryCancelsWhileScrubbing() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "win-scrub";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    runtime.setWindowMs(10000);
    runtime.setCursorMs(10000);
    const std::uint32_t ids[] = {1};
    runtime.tickHistory(ids, 2000);
    const auto nReq = run->io().requests().size();
    runtime.beginScrub();
    runtime.previewScrub(250000);
    runtime.tickHistory(ids, 2000);
    CHECK(!run->io().cancels().empty());
    CHECK(run->io().requests().size() == nReq);
}

}  // namespace

void runAnalysisRuntimeTests() {
    testConstructsWithoutApp();
    testBindOwnsSeparateCache();
    testRequestCursorWindowUsesOwnedIo();
    testGatherPlotOnOwnedCache();
    testPerRunCachesAreIsolated();
    testRebindPrimaryReplacesCache();
    testQueryIdsStayInAnalysisRange();
    testRequestWindowUsesOwnedIo();
    testIngestRoutesToOwningRun();
    testDoesNotTouchSharedCache();
    testTransportForwardsWithoutArena();
    testBudgetIsSeparateAndSplit();
    testSampleAtUsesOwnedL0();
    testScrubDragIssuesNoHistoryRequest();
    testRejectsEmptySession();
    testCursorScrubIsClientSide();
    testHoldViewKeepsWindowWhileScrubbing();
    testValueSampleIsL0OrPrior();
    testScrubDoesNotEnqueueWhenDragging();
    testTickHistoryCancelsOnWindowChange();
    testTickHistoryNoCancelOnSameWindow();
    testTickHistoryCancelsWhileScrubbing();
}
