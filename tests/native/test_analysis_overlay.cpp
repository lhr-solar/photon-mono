#include "photon/protocol_v2.hpp"
#include "photon_ui/analysis_eval.hpp"
#include "photon_ui/analysis_runtime.hpp"
#include "photon_ui/analysis_workbook.hpp"
#include "photon_ui/history_io.hpp"
#include "photon_ui/tile_cache.hpp"
#include "test_harness.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

void runAnalysisOverlayTests();

namespace {

using photon::ui::AnalysisBoundRun;
using photon::ui::AnalysisEvent;
using photon::ui::AnalysisFormulaSpec;
using photon::ui::AnalysisHistoryIO;
using photon::ui::AnalysisRunBinding;
using photon::ui::AnalysisRunRole;
using photon::ui::AnalysisRuntime;
using photon::ui::AnalysisTransform;
using photon::ui::kAnalysisEventManual;
using photon::ui::kAnalysisEventThreshold;
namespace proto2 = photon::proto2;

// Bucket widths as hello_ack carries them (photon-next/docs/protocol-v2.md).
const std::vector<std::int64_t> kLevels = {0, 10, 100, 1000, 10000, 60000, 600000};
constexpr std::int64_t kRawSpanMs = 10000;
constexpr std::uint8_t kSealedLast = proto2::kTileSealed | proto2::kTileLast;

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

// Asks the run's cache for raw tile `index` of signalId (a cursor in the middle of that
// tile asks for that tile only), then delivers the tile as one piece.
void deliverRaw(AnalysisBoundRun& run, std::uint32_t signalId, std::int64_t index, const std::vector<double>& t,
                const std::vector<double>& v) {
    const std::uint32_t ids[] = {signalId};
    run.requestCursorWindow(ids, static_cast<std::uint64_t>(index * kRawSpanMs + kRawSpanMs / 2));
    const std::uint32_t qid = qidOf(run.io(), signalId, 0, index);
    CHECK(qid != 0);
    if (qid != 0) {
        CHECK(run.cache().ingestPiece(rawPiece(qid, signalId, index, t, v)));
    }
}

// Asks the run's cache for a level 1 tile (10 ms buckets) of signalId at index 0, the
// way a 0..10 s window at 200 pixels asks for it, then delivers it with min/max/mean.
void deliverEnvelope(AnalysisBoundRun& run, std::uint32_t signalId) {
    const std::uint32_t ids[] = {signalId};
    run.requestWindow(ids, 0, 9999, 200, false);
    const std::uint32_t qid = qidOf(run.io(), signalId, 1, 0);
    CHECK(qid != 0);
    if (qid == 0) {
        return;
    }
    proto2::TilePiece p;
    p.qid = qid;
    p.signal = signalId - 1;
    p.level = 1;
    p.flags = kSealedLast;
    p.index = 0;
    p.offset = 0;
    p.n = 3;
    p.tMs = {1000.0, 1010.0, 1020.0};
    p.min = {1.0, 2.0, 3.0};
    p.max = {2.0, 4.0, 6.0};
    p.mean = {1.5, 3.0, 4.5};
    CHECK(run.cache().ingestPiece(p));
}

void testOverlayQueriesAreDistinct() {
    AnalysisRuntime runtime;
    runtime.setLevels(kLevels);
    AnalysisRunBinding primary;
    primary.sessionId = "p";
    primary.dbcHash = "dbc";
    primary.maxMs = 120000;
    auto* a = runtime.bind(std::move(primary));
    AnalysisRunBinding overlay;
    overlay.sessionId = "o";
    overlay.dbcHash = "dbc";
    overlay.role = AnalysisRunRole::Overlay;
    overlay.timeOffsetMs = 1000;
    overlay.maxMs = 120000;
    auto* b = runtime.bind(std::move(overlay));
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    if (a == nullptr || b == nullptr) {
        return;
    }
    CHECK(a->queryOrigin() != b->queryOrigin());

    const std::uint32_t ids[] = {3};
    runtime.requestWindow(ids, 5000, 30000, 200, false);
    CHECK(!a->io().requests().empty());
    CHECK(!b->io().requests().empty());
    if (a->io().requests().empty() || b->io().requests().empty()) {
        return;
    }
    CHECK(a->io().requests().front().queryId != b->io().requests().front().queryId);
    CHECK(photon::ui::isAnalysisQueryId(a->io().requests().front().queryId));
    CHECK(photon::ui::isAnalysisQueryId(b->io().requests().front().queryId));
    CHECK(b->io().requests().front().runId == "o");
    CHECK(a->io().requests().front().runId == "p");
}

void testOffsetRealignsWithoutRefetch() {
    AnalysisRuntime runtime;
    runtime.setLevels(kLevels);
    AnalysisRunBinding primary;
    primary.sessionId = "p";
    primary.dbcHash = "dbc";
    primary.maxMs = 120000;
    CHECK(runtime.bind(std::move(primary)) != nullptr);

    AnalysisRunBinding overlay;
    overlay.sessionId = "o";
    overlay.dbcHash = "dbc";
    overlay.role = AnalysisRunRole::Overlay;
    overlay.timeOffsetMs = 0;
    overlay.maxMs = 120000;
    auto* b = runtime.bind(std::move(overlay));
    CHECK(b != nullptr);
    if (b == nullptr) {
        return;
    }
    deliverRaw(*b, 1, 0, {10.0, 20.0, 30.0}, {1.0, 2.0, 3.0});

    const std::uint32_t ids[] = {1};
    runtime.requestWindow(ids, 0, 60000, 200, false);
    const auto n = b->io().requests().size();

    const auto aligned = runtime.gatherOverlayPlot(1, 0, 9999, 0);
    CHECK(aligned.t.size() == 3 && aligned.y.size() == 3 && aligned.t[0] == 10.0 && aligned.y[1] == 2.0);

    runtime.setOverlayOffsetMs(5000);
    CHECK(b->io().requests().size() == n);
    CHECK(b->cache().runId() == "o");
    // The overlay's native 0..9999 ms shows at 5000..14999 ms of the primary's time.
    const auto shifted = runtime.gatherOverlayPlot(1, 0, 14999, 0);
    CHECK(shifted.t.size() == 3 && shifted.t[0] == 5010.0 && shifted.t[2] == 5030.0 && shifted.y[2] == 3.0);
    CHECK(runtime.overlayOffsetMs() == 5000);
}

void testGatherL0IgnoresEnvelope() {
    AnalysisRuntime runtime;
    runtime.setLevels(kLevels);
    AnalysisRunBinding bind;
    bind.sessionId = "l0";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    if (run == nullptr) {
        return;
    }
    deliverRaw(*run, 4, 0, {10.0, 20.0}, {5.0, 6.0});
    deliverEnvelope(*run, 4);
    const auto l0 = run->gatherL0(4, 0, 120000);
    CHECK(!l0.envelope);
    CHECK(l0.t.size() == 2 && l0.y.size() == 2 && l0.y[0] == 5.0 && l0.y[1] == 6.0);
    for (double y : l0.y) {
        CHECK(y != 1.5 && y != 3.0 && y != 4.5);
    }
}

void testFormulaSampleAligned() {
    AnalysisRuntime runtime;
    runtime.setLevels(kLevels);
    AnalysisRunBinding bind;
    bind.sessionId = "f";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    if (run == nullptr) {
        return;
    }
    deliverRaw(*run, 7, 0, {10.0, 20.0, 30.0}, {2.0, 4.0, 6.0});
    deliverRaw(*run, 8, 0, {10.0, 20.0, 30.0}, {1.0, 1.0, 2.0});

    AnalysisFormulaSpec spec;
    CHECK(photon::ui::parseFormulaSpec("7 + 8", &spec));
    CHECK(spec.leftId == 7);
    CHECK(spec.rightId == 8);
    const auto out = photon::ui::evaluateFormula(*run, spec, 0, 60000);
    CHECK(!out.decimated);
    CHECK(out.plot.t.size() == 3 && out.plot.y.size() == 3);
    CHECK(out.plot.y.size() == 3 && out.plot.y[0] == 3.0 && out.plot.y[1] == 5.0 && out.plot.y[2] == 8.0);

    AnalysisFormulaSpec scale;
    CHECK(photon::ui::parseFormulaSpec("$7 * 0.5", &scale));
    const auto scaled = photon::ui::evaluateFormula(*run, scale, 0, 60000);
    CHECK(scaled.plot.y.size() == 3 && scaled.plot.y[1] == 2.0);
}

void testFormulaOnEnvelopeIsLabeled() {
    AnalysisRuntime runtime;
    runtime.setLevels(kLevels);
    AnalysisRunBinding bind;
    bind.sessionId = "dec";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    if (run == nullptr) {
        return;
    }
    deliverEnvelope(*run, 9);
    AnalysisFormulaSpec spec;
    CHECK(photon::ui::parseFormulaSpec("9 + 1.0", &spec));
    // No raw tile exists, so the formula falls back to the envelope, which is labeled.
    const auto out = photon::ui::evaluateFormula(*run, spec, 0, 120000);
    CHECK(out.decimated);
    CHECK(!out.plot.t.empty());
}

void testThresholdUsesL0Only() {
    AnalysisRuntime runtime;
    runtime.setLevels(kLevels);
    AnalysisRunBinding bind;
    bind.sessionId = "thr";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    if (run == nullptr) {
        return;
    }
    deliverRaw(*run, 5, 0, {10.0, 20.0, 30.0, 40.0}, {1.0, 2.0, 10.0, 2.0});
    deliverEnvelope(*run, 5);

    AnalysisTransform xf;
    xf.id = "t1";
    xf.signalId = 5;
    xf.kind = "threshold";
    xf.spec = "rising";
    xf.threshold = 5.0;
    xf.label = "over";

    const auto evs = photon::ui::extractThresholdEvents(*run, xf, 0, 60000);
    CHECK(evs.size() == 1);
    if (evs.size() != 1) {
        return;
    }
    CHECK(evs[0].tMs == 30);
    CHECK(evs[0].source == kAnalysisEventThreshold);
    CHECK(evs[0].value.has_value());
    CHECK(evs[0].value.has_value() && *evs[0].value == 10.0);

    const auto l0 = run->gatherL0(5, 0, 60000);
    std::size_t manual = 0;
    for (std::size_t i = 1; i < l0.y.size(); ++i) {
        if (l0.y[i - 1] < 5.0 && l0.y[i] >= 5.0) {
            ++manual;
            CHECK(static_cast<std::uint64_t>(l0.t[i]) == evs[0].tMs);
        }
    }
    CHECK(manual == 1);
}

void testEnvelopeCannotProduceThresholdEvents() {
    AnalysisRuntime runtime;
    runtime.setLevels(kLevels);
    AnalysisRunBinding bind;
    bind.sessionId = "env";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    if (run == nullptr) {
        return;
    }
    deliverEnvelope(*run, 6);
    AnalysisTransform xf;
    xf.signalId = 6;
    xf.kind = "threshold";
    xf.spec = "rising";
    xf.threshold = 2.0;
    CHECK(photon::ui::extractThresholdEvents(*run, xf, 0, 120000).empty());
}

void testManualAndSeek() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "seek";
    bind.dbcHash = "dbc";
    bind.minMs = 0;
    bind.maxMs = 90000;
    CHECK(runtime.bind(std::move(bind)) != nullptr);
    runtime.setCursorMs(90000);
    runtime.seekCursor(12345);
    CHECK(runtime.cursorMs() == 12345);
    CHECK(!runtime.viewHeld());

    std::vector<AnalysisEvent> events;
    AnalysisEvent manual;
    manual.id = "m1";
    manual.tMs = 50;
    manual.source = kAnalysisEventManual;
    events.push_back(manual);
    AnalysisEvent oldThr;
    oldThr.id = "old";
    oldThr.source = kAnalysisEventThreshold;
    events.push_back(oldThr);
    std::vector<AnalysisEvent> extracted;
    AnalysisEvent next;
    next.id = "new";
    next.tMs = 30;
    next.source = kAnalysisEventThreshold;
    extracted.push_back(next);
    photon::ui::replaceThresholdEvents(&events, std::move(extracted));
    CHECK(events.size() == 2);
    CHECK(events.size() == 2 && events[0].source == kAnalysisEventManual);
    CHECK(events.size() == 2 && events[1].id == "new");
}

void testRejectsOverlayOfPrimarySession() {
    AnalysisRuntime runtime;
    AnalysisRunBinding primary;
    primary.sessionId = "same";
    primary.dbcHash = "dbc";
    primary.maxMs = 1000;
    CHECK(runtime.bind(std::move(primary)) != nullptr);
    AnalysisRunBinding overlay;
    overlay.sessionId = "same";
    overlay.dbcHash = "dbc";
    overlay.role = AnalysisRunRole::Overlay;
    CHECK(runtime.bind(std::move(overlay)) == nullptr);
    CHECK(runtime.overlay() == nullptr);
    CHECK(runtime.primary() != nullptr);
}

void testWorkbookCollectsFormulaIds() {
    photon::ui::AnalysisWorkbook doc;
    photon::ui::ensureAnalysisWorkbook(doc);
    photon::ui::AnalysisDerivedChannel ch;
    ch.id = "pwr";
    ch.kind = "formula";
    ch.spec = "7 * 8";
    doc.derivedChannels.push_back(ch);
    photon::ui::AnalysisTransform xf;
    xf.signalId = 11;
    doc.transforms.push_back(xf);
    const auto ids = photon::ui::collectWorkbookSignalIds(doc);
    CHECK(std::find(ids.begin(), ids.end(), 7u) != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), 8u) != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), 11u) != ids.end());
}

}  // namespace

void runAnalysisOverlayTests() {
    testOverlayQueriesAreDistinct();
    testOffsetRealignsWithoutRefetch();
    testGatherL0IgnoresEnvelope();
    testFormulaSampleAligned();
    testFormulaOnEnvelopeIsLabeled();
    testThresholdUsesL0Only();
    testEnvelopeCannotProduceThresholdEvents();
    testManualAndSeek();
    testRejectsOverlayOfPrimarySession();
    testWorkbookCollectsFormulaIds();
}
