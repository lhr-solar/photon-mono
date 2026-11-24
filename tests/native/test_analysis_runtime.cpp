#include "photon/control_v2.hpp"
#include "photon/protocol_v2.hpp"
#include "photon_ui/analysis_runtime.hpp"
#include "photon_ui/history_io.hpp"
#include "photon_ui/tile_cache.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

void runAnalysisRuntimeTests();

namespace {

using photon::ui::AnalysisBoundRun;
using photon::ui::AnalysisHistoryIO;
using photon::ui::AnalysisRunBinding;
using photon::ui::AnalysisRunRole;
using photon::ui::AnalysisRuntime;
using photon::ui::AnalysisTileRequest;
using photon::ui::TileCache;
namespace proto2 = photon::proto2;
namespace ctrl2 = photon::ctrl2;

// Bucket widths as hello_ack carries them (photon-next/docs/protocol-v2.md).
const std::vector<std::int64_t> kLevels = {0, 10, 100, 1000, 10000, 60000, 600000};
constexpr std::int64_t kRawSpanMs = 10000;
constexpr std::uint8_t kSealedLast = proto2::kTileSealed | proto2::kTileLast;

// The transport under the runtime. It records every request and cancel, and reports
// the queue as full when full is set.
struct ForwardIo final : photon::ui::HistoryIO {
    std::vector<AnalysisTileRequest> sent;
    std::vector<std::uint32_t> cancelled;
    bool full = false;

    void requestTile(std::uint32_t queryId, const std::string& runId, std::uint32_t signalId, int level,
                     std::int64_t index, bool urgent) override {
        sent.push_back(AnalysisTileRequest{queryId, runId, signalId, level, index, urgent});
    }
    void cancelTile(std::uint32_t queryId) override { cancelled.push_back(queryId); }
    bool queueFull() const override { return full; }
};

// The query id of the most recent request for (signalId, level, index) on io, or 0.
std::uint32_t qidOf(const AnalysisHistoryIO& io, std::uint32_t signalId, int level, std::int64_t index) {
    const auto& reqs = io.requests();
    for (auto it = reqs.rbegin(); it != reqs.rend(); ++it) {
        if (it->signalId == signalId && it->level == level && it->index == index) {
            return it->queryId;
        }
    }
    return 0;
}

// A raw tile of signalId as one sealed piece. The wire index is the client id less one.
proto2::TilePiece rawPiece(std::uint32_t qid, std::uint32_t signalId, std::int64_t index,
                           const std::vector<double>& t, const std::vector<double>& v) {
    proto2::TilePiece p;
    p.qid = qid;
    p.signal = signalId - 1;
    p.level = 0;
    p.flags = kSealedLast;
    p.index = index;
    p.offset = 0;
    p.n = static_cast<std::uint32_t>(t.size());
    p.tMs = t;
    p.value = v;
    return p;
}

// Asks the cache for raw tile `index` through io (a cursor in the middle of that tile
// asks for that tile only), then delivers the tile as one piece.
void deliverRaw(TileCache& cache, AnalysisHistoryIO& io, std::uint32_t signalId, std::int64_t index,
                const std::vector<double>& t, const std::vector<double>& v) {
    const std::uint32_t ids[] = {signalId};
    cache.requestCursorWindow(io, ids, static_cast<std::uint64_t>(index * kRawSpanMs + kRawSpanMs / 2));
    const std::uint32_t qid = qidOf(io, signalId, 0, index);
    CHECK(qid != 0);
    if (qid != 0) {
        CHECK(cache.ingestPiece(rawPiece(qid, signalId, index, t, v)));
    }
}

void deliverRaw(AnalysisBoundRun& run, std::uint32_t signalId, std::int64_t index, const std::vector<double>& t,
                const std::vector<double>& v) {
    deliverRaw(run.cache(), run.io(), signalId, index, t, v);
}

// Sets the hello_ack levels on the runtime. Runs bound before or after this call get them.
void useLevels(AnalysisRuntime& runtime) {
    runtime.setLevels(kLevels);
}

void testConstructsWithoutApp() {
    TileCache cache;
    cache.setTileStore(nullptr);
    cache.setRun("run-a", 1, "dbc", 0, 120000);
    CHECK(cache.runId() == "run-a");
    CHECK(cache.catalogSha() == "dbc");
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
    CHECK(run->cache().runId() == "sess-1");
    CHECK(run->cache().catalogSha() == "hash-1");
    CHECK(run->cache().runMinMs() == 1000);
    CHECK(run->cache().runMaxMs() == 90000);
    CHECK(&run->cache() == &runtime.primary()->cache());
}

void testRequestCursorWindowUsesOwnedIo() {
    AnalysisRuntime runtime;
    useLevels(runtime);
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
    if (!run->io().requests().empty()) {
        const auto& first = run->io().requests().front();
        CHECK(first.runId == "sess-io");
        CHECK(first.signalId == 7);
        CHECK(first.level == 0);
        CHECK(first.index == 3);
    }
}

void testGatherPlotOnOwnedCache() {
    AnalysisRuntime runtime;
    useLevels(runtime);
    AnalysisRunBinding bind;
    bind.sessionId = "sess-plot";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);

    deliverRaw(*run, 3, 0, {10.0, 20.0, 30.0}, {1.0, 2.0, 3.0});
    const auto viaRun = run->gatherPlot(3, 0, 9999, 0);
    CHECK(viaRun.t.size() == 3 && !viaRun.incomplete);
    CHECK(viaRun.y.size() == 3 && viaRun.y[0] == 1.0 && viaRun.y[2] == 3.0);

    const auto viaRuntime = runtime.gatherPlot(3, 0, 9999, 0);
    CHECK(viaRuntime.t.size() == 3);
    CHECK(viaRuntime.y.size() == 3 && viaRuntime.y[1] == 2.0);
}

void testPerRunCachesAreIsolated() {
    AnalysisRuntime runtime;
    useLevels(runtime);
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
    if (a == nullptr || b == nullptr) {
        return;
    }
    CHECK(a != b);
    CHECK(&a->cache() != &b->cache());
    CHECK(runtime.boundCount() == 2);
    CHECK(runtime.primary() == a);
    CHECK(runtime.find("overlay") == b);

    deliverRaw(*a, 1, 0, {10.0, 20.0}, {5.0, 6.0});
    deliverRaw(*b, 1, 0, {10.0}, {99.0});

    const auto fromPrimary = runtime.gatherPlot(1, 0, 9999, 0);
    CHECK(fromPrimary.t.size() == 2 && fromPrimary.y.size() == 2 && fromPrimary.y[0] == 5.0);

    const auto fromOverlay = b->gatherPlot(1, 0, 9999, 0);
    CHECK(fromOverlay.t.size() == 1 && fromOverlay.y.size() == 1 && fromOverlay.y[0] == 99.0);
}

void testRebindPrimaryReplacesCache() {
    AnalysisRuntime runtime;
    useLevels(runtime);
    AnalysisRunBinding first;
    first.sessionId = "old";
    first.dbcHash = "dbc";
    first.maxMs = 120000;
    auto* oldRun = runtime.bind(std::move(first));
    CHECK(oldRun != nullptr);
    deliverRaw(*oldRun, 1, 0, {10.0}, {1.0});

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
    CHECK(newRun->cache().runId() == "new");
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
    useLevels(runtime);
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
    CHECK(!run->io().requests().empty() && photon::ui::isAnalysisQueryId(run->io().requests().front().queryId));
}

void testRequestWindowUsesOwnedIo() {
    AnalysisRuntime runtime;
    useLevels(runtime);
    AnalysisRunBinding bind;
    bind.sessionId = "win";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    const std::uint32_t ids[] = {9};
    runtime.requestWindow(ids, 0, 120000, 200, false);
    CHECK(!run->io().requests().empty());
    CHECK(!run->io().requests().empty() && run->io().requests().front().runId == "win");
}

void testCacheForQueryRoutesToOwningRun() {
    AnalysisRuntime runtime;
    useLevels(runtime);
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
    if (a == nullptr || b == nullptr) {
        return;
    }

    const std::uint32_t ids[] = {3};
    a->requestCursorWindow(ids, 30000);
    b->requestCursorWindow(ids, 30000);
    CHECK(!a->io().requests().empty());
    CHECK(!b->io().requests().empty());
    if (a->io().requests().empty() || b->io().requests().empty()) {
        return;
    }
    const std::uint32_t aq = a->io().requests().front().queryId;
    const std::uint32_t bq = b->io().requests().front().queryId;
    CHECK(aq != bq);
    CHECK(a->cache().owns(aq));
    CHECK(!a->cache().owns(bq));
    CHECK(b->cache().owns(bq));
    CHECK(runtime.cacheForQuery(aq) == &a->cache());
    CHECK(runtime.cacheForQuery(bq) == &b->cache());
    // No bound run has this id in flight.
    CHECK(runtime.cacheForQuery(0x42u) == nullptr);

    // The overlay's answer (its tile at index 3) lands in the overlay's cache only.
    TileCache* owner = runtime.cacheForQuery(bq);
    CHECK(owner != nullptr);
    if (owner != nullptr) {
        CHECK(owner->ingestPiece(rawPiece(bq, 3, 3, {31000.0}, {7.0})));
    }
    CHECK(b->cache().find(3, 0, 3) != nullptr);
    CHECK(runtime.cacheForQuery(bq) == nullptr);
    CHECK(runtime.cacheForQuery(aq) == &a->cache());
}

void testDoesNotTouchSharedCache() {
    TileCache shared;
    shared.setTileStore(nullptr);
    shared.setLevels(kLevels);
    shared.setRun("live-session", 1, "live-dbc", 0, 999999);
    AnalysisHistoryIO sharedIo;
    deliverRaw(shared, sharedIo, 1, 0, {10.0}, {42.0});
    const auto tilesBefore = shared.tileCount();
    const auto usedBefore = shared.usedBytes();

    AnalysisRuntime runtime;
    useLevels(runtime);
    AnalysisRunBinding bind;
    bind.sessionId = "hist-run";
    bind.dbcHash = "hist-dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    if (run == nullptr) {
        return;
    }
    const std::uint32_t ids[] = {1};
    runtime.requestCursorWindow(ids, 30000);
    deliverRaw(*run, 1, 0, {10.0}, {1.0});

    CHECK(shared.runId() == "live-session");
    CHECK(shared.catalogSha() == "live-dbc");
    CHECK(shared.tileCount() == tilesBefore);
    CHECK(shared.usedBytes() == usedBefore);
    const auto live = shared.gatherPlot(1, 0, 9999, 0);
    CHECK(live.y.size() == 1 && live.y[0] == 42.0);
    CHECK(&run->cache() != &shared);
}

void testTransportForwardsWithoutArena() {
    AnalysisRuntime runtime;
    useLevels(runtime);
    ForwardIo fwd;
    runtime.setTransport(&fwd);
    AnalysisRunBinding bind;
    bind.sessionId = "fwd";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    if (run == nullptr) {
        return;
    }
    CHECK(run->io().forward() == &fwd);
    const std::uint32_t ids[] = {2};
    runtime.requestCursorWindow(ids, 30000);
    CHECK(!run->io().requests().empty());
    CHECK(fwd.sent.size() == run->io().requests().size());
    CHECK(!fwd.sent.empty() && fwd.sent.front().queryId == run->io().requests().front().queryId);
    CHECK(!fwd.sent.empty() && fwd.sent.front().runId == "fwd");
    CHECK(!fwd.sent.empty() && photon::ui::isAnalysisQueryId(fwd.sent.front().queryId));
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
    if (a == nullptr) {
        return;
    }
    CHECK(a->cache().budget() == TileCache::kDefaultBudget);

    AnalysisRunBinding overlay;
    overlay.sessionId = "b";
    overlay.dbcHash = "dbc";
    overlay.role = AnalysisRunRole::Overlay;
    overlay.maxMs = 120000;
    auto* b = runtime.bind(std::move(overlay));
    CHECK(b != nullptr);
    CHECK(a->cache().budget() == TileCache::kDefaultBudget / 2);
    CHECK(b != nullptr && b->cache().budget() == TileCache::kDefaultBudget / 2);
    CHECK(runtime.budget() == TileCache::kDefaultBudget);
}

void testSampleAtUsesOwnedL0() {
    AnalysisRuntime runtime;
    useLevels(runtime);
    AnalysisRunBinding bind;
    bind.sessionId = "val";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    if (run == nullptr) {
        return;
    }
    deliverRaw(*run, 5, 0, {10.0, 20.0, 30.0}, {1.5, 2.5, 3.5});
    double v = 0.0;
    CHECK(runtime.sampleAt(5, 20, v));
    CHECK(v == 2.5);
    CHECK(run->sampleAt(5, 30, v));
    CHECK(v == 3.5);
}

void testScrubDragIssuesNoHistoryRequest() {
    AnalysisRuntime runtime;
    useLevels(runtime);
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
    useLevels(runtime);
    AnalysisRunBinding bind;
    bind.sessionId = "value";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    if (run == nullptr) {
        return;
    }
    deliverRaw(*run, 3, 0, {10.0, 20.0, 30.0}, {1.0, 2.0, 3.0});
    double v = 0.0;
    CHECK(runtime.sampleAt(3, 20, v));
    CHECK(v == 2.0);
    CHECK(runtime.sampleAt(3, 25, v));
    CHECK(v == 2.0);
}

void testScrubDoesNotEnqueueWhenDragging() {
    AnalysisRuntime runtime;
    useLevels(runtime);
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
    useLevels(runtime);
    ForwardIo fwd;
    runtime.setTransport(&fwd);
    AnalysisRunBinding bind;
    bind.sessionId = "win-cancel";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    if (run == nullptr) {
        return;
    }
    runtime.setWindowMs(10000);
    runtime.setCursorMs(10000);
    const std::uint32_t ids[] = {1};
    runtime.tickHistory(ids, 2000);
    CHECK(!run->io().requests().empty());
    CHECK(run->io().cancels().empty());

    // Cancels only happen when the queue is full. The window has moved, so the tiles
    // that left it are cancelled.
    fwd.full = true;
    runtime.setCursorMs(250000);
    runtime.tickHistory(ids, 2000);
    CHECK(!run->io().cancels().empty());
    CHECK(fwd.cancelled.size() == run->io().cancels().size());
    CHECK(run->cache().inflightCount() == 0);
}

void testTickHistoryNoCancelOnSameWindow() {
    AnalysisRuntime runtime;
    useLevels(runtime);
    ForwardIo fwd;
    runtime.setTransport(&fwd);
    AnalysisRunBinding bind;
    bind.sessionId = "win-same";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    if (run == nullptr) {
        return;
    }
    runtime.setWindowMs(10000);
    runtime.setCursorMs(10000);
    const std::uint32_t ids[] = {1};
    runtime.tickHistory(ids, 2000);
    const auto inflight = run->cache().inflightCount();
    CHECK(inflight > 0);

    // Same window, full queue: nothing leaves the view, so nothing is cancelled.
    fwd.full = true;
    runtime.tickHistory(ids, 2000);
    CHECK(run->io().cancels().empty());
    CHECK(run->cache().inflightCount() == inflight);
}

void testTickHistoryCancelsWhileScrubbing() {
    AnalysisRuntime runtime;
    useLevels(runtime);
    ForwardIo fwd;
    runtime.setTransport(&fwd);
    AnalysisRunBinding bind;
    bind.sessionId = "win-scrub";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    if (run == nullptr) {
        return;
    }
    runtime.setWindowMs(10000);
    runtime.setCursorMs(10000);
    const std::uint32_t ids[] = {1};
    runtime.tickHistory(ids, 2000);
    const auto nReq = run->io().requests().size();

    fwd.full = true;
    runtime.beginScrub();
    runtime.previewScrub(250000);
    runtime.tickHistory(ids, 2000);
    CHECK(!run->io().cancels().empty());
    CHECK(run->io().requests().size() == nReq);
}

void testSetLevelsReachesEveryRun() {
    AnalysisRuntime runtime;
    AnalysisRunBinding primary;
    primary.sessionId = "lv-p";
    primary.dbcHash = "dbc";
    primary.maxMs = 600000;
    auto* a = runtime.bind(std::move(primary));
    CHECK(a != nullptr);
    if (a == nullptr) {
        return;
    }
    const std::uint32_t ids[] = {1};
    CHECK(!a->cache().levels().valid());
    runtime.requestCursorWindow(ids, 30000);
    // Without levels nothing can be asked for.
    CHECK(a->io().requests().empty());

    runtime.setLevels(kLevels);
    CHECK(a->cache().levels().valid());
    CHECK(a->cache().levels().count() == 7);

    // A run bound after the levels came in gets them too.
    AnalysisRunBinding overlay;
    overlay.sessionId = "lv-o";
    overlay.dbcHash = "dbc";
    overlay.role = AnalysisRunRole::Overlay;
    overlay.maxMs = 600000;
    auto* b = runtime.bind(std::move(overlay));
    CHECK(b != nullptr);
    CHECK(b != nullptr && b->cache().levels().valid());
    CHECK(b != nullptr && b->cache().levels().count() == 7);

    runtime.requestCursorWindow(ids, 30000);
    CHECK(!a->io().requests().empty());
    CHECK(b != nullptr && !b->io().requests().empty());
}

void testApplyRunsNewRevDropsAndCancels() {
    AnalysisRuntime runtime;
    useLevels(runtime);
    ForwardIo fwd;
    runtime.setTransport(&fwd);
    AnalysisRunBinding changing;
    changing.sessionId = "rev-run";
    changing.dbcHash = "sha";
    changing.rev = 1;
    changing.maxMs = 600000;
    auto* a = runtime.bind(std::move(changing));
    AnalysisRunBinding steady;
    steady.sessionId = "stable";
    steady.dbcHash = "sha";
    steady.rev = 5;
    steady.role = AnalysisRunRole::Overlay;
    steady.maxMs = 600000;
    auto* b = runtime.bind(std::move(steady));
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    if (a == nullptr || b == nullptr) {
        return;
    }

    // Tiles 3 and 2 of each run are asked for. Tile 3 is delivered, tile 2 stays in flight.
    const std::uint32_t ids[] = {1};
    runtime.requestCursorWindow(ids, 30000);
    deliverRaw(*a, 1, 3, {31000.0}, {7.0});
    deliverRaw(*b, 1, 3, {31000.0}, {7.0});
    CHECK(a->cache().tileCount() == 2);
    CHECK(a->cache().inflightCount() == 1);
    const std::uint32_t inflight = qidOf(a->io(), 1, 0, 2);
    CHECK(inflight != 0);

    ctrl2::Run rowA;
    rowA.id = "rev-run";
    rowA.rev = 2;
    ctrl2::Run rowB;
    rowB.id = "stable";
    rowB.rev = 5;
    const std::vector<ctrl2::Run> rows = {rowA, rowB};
    runtime.applyRuns(rows);

    // The run whose rev changed: its in-flight request is cancelled, its tiles are dropped.
    CHECK(a->binding().rev == 2);
    CHECK(a->cache().rev() == 2);
    CHECK(a->cache().tileCount() == 0);
    CHECK(a->cache().inflightCount() == 0);
    CHECK(a->io().cancels().size() == 1 && a->io().cancels().front() == inflight);
    CHECK(fwd.cancelled.size() == 1 && fwd.cancelled.front() == inflight);

    // The run at the same rev is untouched.
    CHECK(b->cache().rev() == 5);
    CHECK(b->cache().tileCount() == 2);
    CHECK(b->cache().inflightCount() == 1);
    CHECK(b->io().cancels().empty());

    // The changed run asks for its tiles again under the new rev.
    const std::size_t before = a->io().requests().size();
    a->requestCursorWindow(ids, 30000);
    CHECK(a->io().requests().size() == before + 2);
    CHECK(!a->io().requests().empty() && a->io().requests().back().runId == "rev-run");
}

void testCacheForQueryFollowsInFlight() {
    AnalysisRuntime runtime;
    useLevels(runtime);
    AnalysisRunBinding primary;
    primary.sessionId = "cq-p";
    primary.dbcHash = "dbc";
    primary.maxMs = 600000;
    auto* p = runtime.bind(std::move(primary));
    AnalysisRunBinding overlay;
    overlay.sessionId = "cq-o";
    overlay.dbcHash = "dbc";
    overlay.role = AnalysisRunRole::Overlay;
    overlay.maxMs = 600000;
    auto* o = runtime.bind(std::move(overlay));
    CHECK(p != nullptr);
    CHECK(o != nullptr);
    if (p == nullptr || o == nullptr) {
        return;
    }

    const std::uint32_t ids[] = {2};
    runtime.requestCursorWindow(ids, 30000);
    const std::uint32_t pq = qidOf(p->io(), 2, 0, 3);
    const std::uint32_t pqOther = qidOf(p->io(), 2, 0, 2);
    const std::uint32_t oq = qidOf(o->io(), 2, 0, 3);
    CHECK(pq != 0 && pqOther != 0 && oq != 0);
    CHECK(runtime.cacheForQuery(pq) == &p->cache());
    CHECK(runtime.cacheForQuery(oq) == &o->cache());
    CHECK(runtime.cacheForQuery(0x42u) == nullptr);

    // A tile that completes releases its query.
    TileCache* owner = runtime.cacheForQuery(pq);
    CHECK(owner != nullptr);
    if (owner != nullptr) {
        CHECK(owner->ingestPiece(rawPiece(pq, 2, 3, {31000.0}, {1.0})));
    }
    CHECK(runtime.cacheForQuery(pq) == nullptr);
    CHECK(runtime.cacheForQuery(pqOther) == &p->cache());

    // A cancelled query is released too: a new rev of the overlay cancels its tiles.
    ctrl2::Run row;
    row.id = "cq-o";
    row.rev = 9;
    const std::vector<ctrl2::Run> rows = {row};
    runtime.applyRuns(rows);
    CHECK(runtime.cacheForQuery(oq) == nullptr);
    CHECK(runtime.cacheForQuery(pqOther) == &p->cache());
}

void testAbandonRequestsForgetsWithoutCancel() {
    AnalysisRuntime runtime;
    useLevels(runtime);
    ForwardIo fwd;
    runtime.setTransport(&fwd);
    AnalysisRunBinding bind;
    bind.sessionId = "abandon";
    bind.dbcHash = "dbc";
    bind.maxMs = 600000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    if (run == nullptr) {
        return;
    }
    const std::uint32_t ids[] = {1};
    runtime.requestCursorWindow(ids, 30000);
    CHECK(run->cache().inflightCount() == 2);
    const std::uint32_t lost = fwd.sent.empty() ? 0u : fwd.sent.front().queryId;
    const std::size_t sentBefore = fwd.sent.size();

    // The socket is gone: the requests are forgotten, and nothing is sent to cancel them.
    runtime.abandonRequests();
    CHECK(run->cache().inflightCount() == 0);
    CHECK(run->cache().tileCount() == 0);
    CHECK(lost != 0 && runtime.cacheForQuery(lost) == nullptr);
    CHECK(run->io().cancels().empty());
    CHECK(fwd.cancelled.empty());

    // The abandoned tiles are asked for again.
    runtime.requestCursorWindow(ids, 30000);
    CHECK(fwd.sent.size() == sentBefore + 2);
}

// A run bound in the other role replaces the one it had: no run keeps two primaries.
void testBindingInOtherRoleReplacesRun() {
    AnalysisRuntime runtime;
    useLevels(runtime);
    AnalysisRunBinding primary;
    primary.sessionId = "primary-run";
    primary.dbcHash = "dbc";
    primary.maxMs = 120000;
    CHECK(runtime.bind(std::move(primary)) != nullptr);
    AnalysisRunBinding overlay;
    overlay.sessionId = "other-run";
    overlay.dbcHash = "dbc";
    overlay.role = AnalysisRunRole::Overlay;
    overlay.maxMs = 120000;
    CHECK(runtime.bind(std::move(overlay)) != nullptr);
    CHECK(runtime.boundCount() == 2);
    AnalysisRunBinding promoted;
    promoted.sessionId = "other-run";
    promoted.dbcHash = "dbc";
    promoted.maxMs = 120000;
    CHECK(runtime.bind(std::move(promoted)) != nullptr);
    CHECK(runtime.boundCount() == 1);
    CHECK(runtime.primary() != nullptr && runtime.primary()->binding().sessionId == "other-run");
    CHECK(runtime.overlay() == nullptr);
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
    testCacheForQueryRoutesToOwningRun();
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
    testSetLevelsReachesEveryRun();
    testApplyRunsNewRevDropsAndCancels();
    testCacheForQueryFollowsInFlight();
    testAbandonRequestsForgetsWithoutCancel();
    testBindingInOtherRoleReplacesRun();
}
