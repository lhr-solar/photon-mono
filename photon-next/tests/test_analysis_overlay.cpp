#include "photon/control.hpp"
#include "photon/protocol.hpp"
#include "photon_ui/analysis_eval.hpp"
#include "photon_ui/analysis_runtime.hpp"
#include "photon_ui/analysis_workbook.hpp"
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
using photon::ui::AnalysisRunBinding;
using photon::ui::AnalysisRunRole;
using photon::ui::AnalysisRuntime;
using photon::ui::AnalysisTransform;
using photon::ui::TileCache;
using photon::ui::kAnalysisEventManual;
using photon::ui::kAnalysisEventThreshold;

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

void ingestEnvelope(TileCache& cache, std::uint32_t signalId) {
    photon::proto::HistoryChunk coarse;
    coarse.queryId = cache.prepareQuery(signalId, 0, 120000, 1, false, false);
    cache.ingestResult(makeResult(coarse.queryId, signalId, 1, 3, 0, 120000));
    coarse.signalId = signalId;
    coarse.level = 1;
    coarse.bucketMs = 1000;
    coarse.baseMs = 10000;
    coarse.slotCount = 3;
    coarse.valueCount = 3;
    coarse.flags = static_cast<std::uint16_t>(photon::proto::kHistDense | photon::proto::kHistFinal);
    coarse.minV = {1.0f, 2.0f, 3.0f};
    coarse.maxV = {2.0f, 4.0f, 6.0f};
    coarse.meanV = {1.5f, 3.0f, 4.5f};
    cache.ingestChunk(coarse);
}

void testOverlayQueriesAreDistinct() {
    AnalysisRuntime runtime;
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
    CHECK(a->queryOrigin() != b->queryOrigin());

    const std::uint32_t ids[] = {3};
    runtime.requestWindow(ids, 5000, 30000, 200, false);
    CHECK(!a->io().requests().empty());
    CHECK(!b->io().requests().empty());
    CHECK(a->io().requests().front().queryId != b->io().requests().front().queryId);
    CHECK(photon::ui::isAnalysisQueryId(a->io().requests().front().queryId));
    CHECK(photon::ui::isAnalysisQueryId(b->io().requests().front().queryId));
    CHECK(b->io().requests().front().sessionId == "o");
    CHECK(a->io().requests().front().sessionId == "p");
}

void testOffsetRealignsWithoutRefetch() {
    AnalysisRuntime runtime;
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
    ingestRaw(b->cache(), 1, {10.0, 20.0, 30.0}, {1.0f, 2.0f, 3.0f});

    const std::uint32_t ids[] = {1};
    runtime.requestWindow(ids, 0, 60000, 200, false);
    const auto n = b->io().requests().size();

    const auto aligned = runtime.gatherOverlayPlot(1, 0, 60000, 0);
    CHECK(aligned.t.size() == 3);
    CHECK(aligned.t[0] == 10.0);
    CHECK(aligned.y[1] == 2.0);

    runtime.setOverlayOffsetMs(5000);
    CHECK(b->io().requests().size() == n);
    CHECK(b->cache().sessionId() == "o");
    const auto shifted = runtime.gatherOverlayPlot(1, 0, 60000, 0);
    CHECK(shifted.t.size() == 3);
    CHECK(shifted.t[0] == 5010.0);
    CHECK(shifted.t[2] == 5030.0);
    CHECK(shifted.y[2] == 3.0);
    CHECK(runtime.overlayOffsetMs() == 5000);
}

void testGatherL0IgnoresEnvelope() {
    TileCache cache;
    cache.setSession("s", "dbc", 0, 120000);
    ingestRaw(cache, 4, {10.0, 20.0}, {5.0f, 6.0f});
    ingestEnvelope(cache, 4);
    const auto l0 = cache.gatherL0(4, 0, 120000);
    CHECK(!l0.envelope);
    CHECK(l0.t.size() == 2);
    CHECK(l0.y[0] == 5.0);
    CHECK(l0.y[1] == 6.0);
    for (double y : l0.y) {
        CHECK(y != 1.5 && y != 3.0 && y != 4.5);
    }
}

void testFormulaSampleAligned() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "f";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    ingestRaw(run->cache(), 7, {10.0, 20.0, 30.0}, {2.0f, 4.0f, 6.0f});
    ingestRaw(run->cache(), 8, {10.0, 20.0, 30.0}, {1.0f, 1.0f, 2.0f});

    AnalysisFormulaSpec spec;
    CHECK(photon::ui::parseFormulaSpec("7 + 8", &spec));
    CHECK(spec.leftId == 7);
    CHECK(spec.rightId == 8);
    const auto out = photon::ui::evaluateFormula(*run, spec, 0, 60000);
    CHECK(!out.decimated);
    CHECK(out.plot.t.size() == 3);
    CHECK(out.plot.y[0] == 3.0);
    CHECK(out.plot.y[1] == 5.0);
    CHECK(out.plot.y[2] == 8.0);

    AnalysisFormulaSpec scale;
    CHECK(photon::ui::parseFormulaSpec("$7 * 0.5", &scale));
    const auto scaled = photon::ui::evaluateFormula(*run, scale, 0, 60000);
    CHECK(scaled.plot.y.size() == 3);
    CHECK(scaled.plot.y[1] == 2.0);
}

void testFormulaOnEnvelopeIsLabeled() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "dec";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    ingestEnvelope(run->cache(), 9);
    AnalysisFormulaSpec spec;
    CHECK(photon::ui::parseFormulaSpec("9 + 1.0", &spec));
    const auto out = photon::ui::evaluateFormula(*run, spec, 0, 120000);
    CHECK(out.decimated);
    CHECK(!out.plot.t.empty());
}

void testThresholdUsesL0Only() {
    AnalysisRuntime runtime;
    AnalysisRunBinding bind;
    bind.sessionId = "thr";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    ingestRaw(run->cache(), 5, {10.0, 20.0, 30.0, 40.0}, {1.0f, 2.0f, 10.0f, 2.0f});
    ingestEnvelope(run->cache(), 5);

    AnalysisTransform xf;
    xf.id = "t1";
    xf.signalId = 5;
    xf.kind = "threshold";
    xf.spec = "rising";
    xf.threshold = 5.0;
    xf.label = "over";

    const auto evs = photon::ui::extractThresholdEvents(*run, xf, 0, 60000);
    CHECK(evs.size() == 1);
    CHECK(evs[0].tMs == 30);
    CHECK(evs[0].source == kAnalysisEventThreshold);
    CHECK(evs[0].value.has_value());
    CHECK(*evs[0].value == 10.0);

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
    AnalysisRunBinding bind;
    bind.sessionId = "env";
    bind.dbcHash = "dbc";
    bind.maxMs = 120000;
    auto* run = runtime.bind(std::move(bind));
    CHECK(run != nullptr);
    ingestEnvelope(run->cache(), 6);
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
    CHECK(events[0].source == kAnalysisEventManual);
    CHECK(events[1].id == "new");
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
