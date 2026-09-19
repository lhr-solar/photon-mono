#include "photon/tsdb.hpp"
#include "photon_ui/analysis_workbook.hpp"
#include "test_harness.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

void runAnalysisWorkbookTests();

namespace {

using photon::ui::AnalysisDerivedChannel;
using photon::ui::AnalysisEvent;
using photon::ui::AnalysisRunRole;
using photon::ui::AnalysisTransform;
using photon::ui::AnalysisWorkbook;
using photon::ui::ViewCell;
using photon::ui::ViewCellKind;
using photon::ui::ViewSignal;
using photon::ui::kAnalysisWorkbookSchemaVersion;
using photon::ui::kAnalysisWorkbookStorageKey;
using photon::ui::parseAnalysisWorkbook;
using photon::ui::serializeAnalysisWorkbook;

void testSerializeStampsV2() {
    AnalysisWorkbook doc;
    photon::ui::ensureAnalysisWorkbook(doc);
    ViewCell cell;
    cell.id = "c1";
    cell.kind = ViewCellKind::Line;
    cell.title = "RPM";
    ViewSignal src;
    src.id = 7;
    src.messageId = 256;
    src.messageName = "EngineData";
    src.signalName = "RPM";
    src.label = "EngineData.RPM";
    cell.sources.push_back(src);
    doc.views[0].cells.push_back(cell);
    photon::ui::AnalysisRunBinding bind;
    bind.sessionId = "20260904-120000-00";
    bind.dbcHash = "abc123";
    bind.role = AnalysisRunRole::Primary;
    bind.minMs = 10;
    bind.maxMs = 90;
    doc.runBindings.push_back(bind);

    const std::string json = serializeAnalysisWorkbook(doc);
    CHECK(json.find("\"schemaVersion\":2") != std::string::npos ||
          json.find("\"schemaVersion\": 2") != std::string::npos);
    CHECK(json.find("\"activePanel\"") != std::string::npos);
    CHECK(json.find("\"panels\"") != std::string::npos);
    CHECK(json.find("\"messageId\"") != std::string::npos);
    CHECK(json.find("256") != std::string::npos);
    CHECK(std::string(kAnalysisWorkbookStorageKey) == "photon-next.analysis-workbooks");
    CHECK(kAnalysisWorkbookSchemaVersion == 2);
}

void testRoundTrip() {
    AnalysisWorkbook doc;
    photon::ui::ensureAnalysisWorkbook(doc);
    doc.name = "Race review";
    ViewCell line;
    line.id = "line-1";
    line.kind = ViewCellKind::Line;
    line.title = "Speed";
    ViewSignal s;
    s.messageId = 99;
    s.messageName = "GPS";
    s.signalName = "Speed";
    line.sources.push_back(s);
    doc.views[0].cells.push_back(line);
    ViewCell value;
    value.id = "val-1";
    value.kind = ViewCellKind::Value;
    value.rect.x = 6;
    doc.views[0].cells.push_back(value);

    AnalysisWorkbook back;
    std::string err;
    CHECK(parseAnalysisWorkbook(serializeAnalysisWorkbook(doc), &back, &err));
    CHECK(back.schemaVersion == 2);
    CHECK(back.name == "Race review");
    CHECK(back.views.size() == 1);
    CHECK(back.views[0].cells.size() >= 2);
    bool found = false;
    for (const auto& cell : back.views[0].cells) {
        if (cell.id == "line-1") {
            CHECK(cell.sources[0].messageId == 99);
            CHECK(cell.sources[0].messageName == "GPS");
            found = true;
        }
    }
    CHECK(found);
}

void testAcceptsV1ShapedUpgrade() {
    const char* v1 = R"({
      "schemaVersion": 1,
      "id": "old",
      "name": "Plots dump",
      "activePanel": 0,
      "panels": [{
        "id": "view-1",
        "name": "View 1",
        "layout": {"columns": 12, "rowHeight": 160, "gap": 10},
        "widgets": [{
          "id": "cell-1",
          "kind": "plot",
          "rect": {"x": 0, "y": 0, "w": 6, "h": 2},
          "plot": {
            "type": "line",
            "title": "Plot",
            "timeWindowSeconds": 15,
            "sources": [{"messageId": 4, "messageName": "M", "signalName": "S"}]
          }
        }]
      }]
    })";
    AnalysisWorkbook doc;
    std::string err;
    CHECK(parseAnalysisWorkbook(v1, &doc, &err));
    CHECK(doc.schemaVersion == 2);
    CHECK(doc.runBindings.empty());
    CHECK(doc.views.size() == 1);
    CHECK(doc.views[0].cells.size() == 1);
    CHECK(doc.views[0].cells[0].sources[0].messageId == 4);
    const std::string out = serializeAnalysisWorkbook(doc);
    CHECK(out.find("\"schemaVersion\":2") != std::string::npos || out.find("\"schemaVersion\": 2") != std::string::npos);
}

void testSkipsUnknownKinds() {
    const char* blob = R"({
      "schemaVersion": 2,
      "activePanel": 0,
      "panels": [{
        "id": "v",
        "name": "V",
        "layout": {"columns": 12, "rowHeight": 160, "gap": 10},
        "widgets": [
          {"id": "skip", "kind": "video", "rect": {"x":0,"y":0,"w":6,"h":2}, "plot": {"type": "video"}},
          {"id": "keep", "kind": "plot", "rect": {"x":0,"y":2,"w":6,"h":2},
           "plot": {"type": "scatter", "title": "XY", "sources": []}}
        ]
      }]
    })";
    AnalysisWorkbook doc;
    std::string err;
    CHECK(parseAnalysisWorkbook(blob, &doc, &err));
    CHECK(doc.views[0].cells.size() == 1);
    CHECK(doc.views[0].cells[0].id == "keep");
    CHECK(doc.views[0].cells[0].kind == ViewCellKind::Scatter);
}

void testYLockedRoundTrip() {
    AnalysisWorkbook doc;
    photon::ui::ensureAnalysisWorkbook(doc);
    ViewCell cell;
    cell.id = "auto-y";
    cell.kind = ViewCellKind::Line;
    cell.yLocked = true;
    doc.views[0].cells.push_back(cell);
    const std::string json = serializeAnalysisWorkbook(doc);
    CHECK(json.find("\"yLocked\"") != std::string::npos);
    AnalysisWorkbook back;
    std::string err;
    CHECK(parseAnalysisWorkbook(json, &back, &err));
    bool found = false;
    for (const auto& parsed : back.views[0].cells) {
        if (parsed.id == "auto-y") {
            CHECK(parsed.yLocked);
            found = true;
        }
    }
    CHECK(found);
}

void testCatalogMatchAndBinding() {
    photon::ctrl::Message::CatalogSession row;
    row.id = "20260904-090000-00";
    row.minMs = 10;
    row.maxMs = 90;
    row.dbcHash = "abc";
    CHECK(photon::ui::catalogMatchesListing(row, "20260904-090000-00", 10, 90, "abc"));
    CHECK(!photon::ui::catalogMatchesListing(row, "other", 10, 90, "abc"));
    const auto bind = photon::ui::bindingFromCatalog(row);
    CHECK(bind.sessionId == row.id);
    CHECK(bind.dbcHash == row.dbcHash);
    CHECK(bind.role == AnalysisRunRole::Primary);
}

void testListCatalogMatchesJson() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "photon-analysis-catalog-test";
    fs::create_directories(root / "20260904-090000-00");
    {
        std::ofstream cat(root / "catalog.json");
        cat << R"({"version":1,"sessions":[{"id":"20260904-090000-00","path":")"
            << (root / "20260904-090000-00").generic_string()
            << R"(","min_ms":1000,"max_ms":50000,"dbc_sha":"deadbeef"}]})";
    }
    {
        std::ofstream sess(root / "20260904-090000-00" / "session.json");
        sess << R"({"id":"20260904-090000-00","min_ms":1000,"max_ms":50000,"dbc_sha":"deadbeef","samples":42,"frames":0,"generation":1,"live":false})";
    }
    {
        std::vector<std::uint8_t> vpt(64 + 40, 0);
        auto poke32 = [&](std::size_t at, std::uint32_t v) {
            vpt[at] = static_cast<std::uint8_t>(v >> 24);
            vpt[at + 1] = static_cast<std::uint8_t>(v >> 16);
            vpt[at + 2] = static_cast<std::uint8_t>(v >> 8);
            vpt[at + 3] = static_cast<std::uint8_t>(v);
        };
        auto poke16 = [&](std::size_t at, std::uint16_t v) {
            vpt[at] = static_cast<std::uint8_t>(v >> 8);
            vpt[at + 1] = static_cast<std::uint8_t>(v);
        };
        auto poke64 = [&](std::size_t at, std::uint64_t v) {
            poke32(at, static_cast<std::uint32_t>(v >> 32));
            poke32(at + 4, static_cast<std::uint32_t>(v));
        };
        poke32(0, 0x56505431u);
        poke16(4, 1);
        poke16(6, 64);
        poke16(8, 40);
        poke32(12, 1);
        poke64(16, 1000);
        poke64(24, 4000);
        poke64(64, 1000);
        poke64(72, 4000);
        std::ofstream out(root / "20260904-090000-00" / "video.vpt", std::ios::binary);
        out.write(reinterpret_cast<const char*>(vpt.data()), static_cast<std::streamsize>(vpt.size()));
    }
    const auto listed = photon::tsdb::listCatalog(root);
    CHECK(listed.size() == 1);
    CHECK(listed[0].id == "20260904-090000-00");
    CHECK(listed[0].minMs == 1000);
    CHECK(listed[0].maxMs == 50000);
    CHECK(listed[0].dbcSha == "deadbeef");
    CHECK(listed[0].sampleCount == 42);
    CHECK(listed[0].hasVideo);
    CHECK(photon::tsdb::videoVptOverlapsRange(root / "20260904-090000-00" / "video.vpt", 1000, 50000));
    CHECK(!photon::tsdb::videoVptOverlapsRange(root / "20260904-090000-00" / "video.vpt", 80000, 90000));
    fs::remove_all(root);
}

void testEmptyCatalogIsEmpty() {
    CHECK(photon::tsdb::listCatalog(std::filesystem::path("Z:/no-such-photon-catalog-dir")).empty());
}

void testLosslessV2Fields() {
    AnalysisWorkbook doc;
    photon::ui::ensureAnalysisWorkbook(doc);
    doc.views[0].cells[0].derivedChannelId = "pwr";

    photon::ui::AnalysisRunBinding bind;
    bind.sessionId = "20260904-150000-00";
    bind.dbcHash = "deadbeef";
    bind.role = AnalysisRunRole::Overlay;
    bind.timeOffsetMs = -250;
    bind.color = "#ff8800";
    bind.minMs = 10;
    bind.maxMs = 90;
    doc.runBindings.push_back(bind);

    AnalysisEvent ev;
    ev.id = "e1";
    ev.tMs = 12345;
    ev.endMs = 12400;
    ev.source = "threshold";
    ev.signalId = 7;
    ev.label = "over temp";
    ev.value = 61.25;
    doc.events.push_back(ev);

    AnalysisTransform xf;
    xf.id = "t1";
    xf.name = "Hot";
    xf.signalId = 7;
    xf.messageName = "Battery";
    xf.signalName = "Temp";
    xf.kind = "threshold";
    xf.spec = "crossing";
    xf.label = "hot";
    xf.threshold = 60.0;
    xf.min = 0.0;
    xf.max = 120.0;
    doc.transforms.push_back(xf);

    AnalysisDerivedChannel ch;
    ch.id = "pwr";
    ch.name = "Power";
    ch.unit = "W";
    ch.kind = "formula";
    ch.spec = "Voltage * Current";
    doc.derivedChannels.push_back(ch);

    const std::string json = serializeAnalysisWorkbook(doc);
    CHECK(json.find("\"events\"") != std::string::npos);
    CHECK(json.find("\"transforms\"") != std::string::npos);
    CHECK(json.find("\"derivedChannels\"") != std::string::npos);
    CHECK(json.find("\"derivedChannelId\"") != std::string::npos);
    CHECK(json.find("\"#ff8800\"") != std::string::npos);
    CHECK(json.find("over temp") != std::string::npos);
    CHECK(json.find("Voltage * Current") != std::string::npos);

    AnalysisWorkbook back;
    std::string err;
    CHECK(parseAnalysisWorkbook(json, &back, &err));
    CHECK(back.schemaVersion == 2);
    CHECK(back.runBindings.size() == 1);
    CHECK(back.runBindings[0].color == "#ff8800");
    CHECK(back.runBindings[0].role == AnalysisRunRole::Overlay);
    CHECK(back.runBindings[0].timeOffsetMs == -250);
    CHECK(back.events.size() == 1);
    CHECK(back.events[0].id == "e1");
    CHECK(back.events[0].tMs == 12345);
    CHECK(back.events[0].endMs.has_value() && *back.events[0].endMs == 12400);
    CHECK(back.events[0].source == "threshold");
    CHECK(back.events[0].signalId == 7);
    CHECK(back.events[0].label == "over temp");
    CHECK(back.events[0].value.has_value() && near(*back.events[0].value, 61.25));
    CHECK(back.transforms.size() == 1);
    CHECK(back.transforms[0].id == "t1");
    CHECK(back.transforms[0].kind == "threshold");
    CHECK(back.transforms[0].signalName == "Temp");
    CHECK(back.transforms[0].threshold.has_value() && near(*back.transforms[0].threshold, 60.0));
    CHECK(back.transforms[0].min.has_value() && near(*back.transforms[0].min, 0.0));
    CHECK(back.transforms[0].max.has_value() && near(*back.transforms[0].max, 120.0));
    CHECK(back.derivedChannels.size() == 1);
    CHECK(back.derivedChannels[0].id == "pwr");
    CHECK(back.derivedChannels[0].kind == "formula");
    CHECK(back.derivedChannels[0].spec == "Voltage * Current");
    CHECK(!back.views.empty() && !back.views[0].cells.empty());
    CHECK(back.views[0].cells[0].derivedChannelId == "pwr");

    const std::string again = serializeAnalysisWorkbook(back);
    AnalysisWorkbook third;
    CHECK(parseAnalysisWorkbook(again, &third, &err));
    CHECK(third.events[0].label == "over temp");
    CHECK(third.derivedChannels[0].unit == "W");
    CHECK(third.runBindings[0].color == "#ff8800");
}

void testRejectsFutureSchemaVersion() {
    AnalysisWorkbook keep;
    keep.name = "keep-me";
    keep.schemaVersion = 2;
    AnalysisWorkbook doc = keep;
    std::string err;
    const char* v3 = R"({
      "schemaVersion": 3,
      "id": "future",
      "name": "rewritten?",
      "activePanel": 0,
      "panels": [{
        "id": "v",
        "name": "V",
        "layout": {"columns": 12, "rowHeight": 160, "gap": 10},
        "widgets": []
      }],
      "events": [{"id": "e", "tMs": 1, "source": "manual", "label": "x"}]
    })";
    CHECK(!parseAnalysisWorkbook(v3, &doc, &err));
    CHECK(err.find("schemaVersion") != std::string::npos);
    CHECK(doc.name == "keep-me");
    CHECK(doc.schemaVersion == 2);
    CHECK(doc.events.empty());

    const std::string rewritten = serializeAnalysisWorkbook(doc);
    CHECK(rewritten.find("\"schemaVersion\":2") != std::string::npos ||
          rewritten.find("\"schemaVersion\": 2") != std::string::npos);
    CHECK(rewritten.find("rewritten?") == std::string::npos);
}

using photon::ui::PlotArrays;
using photon::ui::pairPlotScatter;

void testPairPlotScatterExactStillWorks() {
    PlotArrays x;
    PlotArrays y;
    x.t = {1000.0, 2000.0};
    x.y = {1.0, 2.0};
    y.t = {1000.0, 2000.0};
    y.y = {10.0, 20.0};
    std::vector<double> xs;
    std::vector<double> ys;
    pairPlotScatter(x, y, xs, ys);
    CHECK(xs.size() == 2);
    CHECK(near(xs[0], 1.0));
    CHECK(near(ys[0], 10.0));
    CHECK(near(xs[1], 2.0));
    CHECK(near(ys[1], 20.0));
}

void testPairPlotScatterNearestWithinTolerance() {
    PlotArrays x;
    PlotArrays y;
    x.t = {1000.4};
    x.y = {3.5};
    y.t = {1003.1};
    y.y = {9.0};
    std::vector<double> xs;
    std::vector<double> ys;
    pairPlotScatter(x, y, xs, ys, 10.0);
    CHECK(xs.size() == 1);
    CHECK(near(xs[0], 3.5));
    CHECK(near(ys[0], 9.0));
}

void testPairPlotScatterFractionalTimestamps() {
    PlotArrays x;
    PlotArrays y;
    x.t = {1000.9};
    x.y = {4.0};
    y.t = {1001.1};
    y.y = {8.0};
    std::vector<double> xs;
    std::vector<double> ys;
    pairPlotScatter(x, y, xs, ys, 1.0);
    CHECK(xs.size() == 1);
    CHECK(near(xs[0], 4.0));
    CHECK(near(ys[0], 8.0));
}

void testPairPlotScatterInterpolates() {
    PlotArrays x;
    PlotArrays y;
    x.t = {1000.0, 1100.0};
    x.y = {0.0, 10.0};
    y.t = {1050.0};
    y.y = {99.0};
    std::vector<double> xs;
    std::vector<double> ys;
    pairPlotScatter(x, y, xs, ys, 60.0);
    CHECK(xs.size() == 1);
    CHECK(near(xs[0], 5.0));
    CHECK(near(ys[0], 99.0));
}

void testPairPlotScatterRejectsFarTimestamps() {
    PlotArrays x;
    PlotArrays y;
    x.t = {1000.0};
    x.y = {1.0};
    y.t = {5000.0};
    y.y = {2.0};
    std::vector<double> xs;
    std::vector<double> ys;
    pairPlotScatter(x, y, xs, ys, 10.0);
    CHECK(xs.empty());
    CHECK(ys.empty());
}

void testPairPlotScatterDecimatedOffset() {
    PlotArrays x;
    PlotArrays y;
    x.t = {1000.0, 2000.0, 3000.0};
    x.y = {1.0, 2.0, 3.0};
    y.t = {1050.0, 2100.0, 2950.0};
    y.y = {10.0, 20.0, 30.0};
    std::vector<double> xs;
    std::vector<double> ys;
    pairPlotScatter(x, y, xs, ys);
    CHECK(xs.size() == 3);
    CHECK(near(xs[0], 1.0));
    CHECK(near(ys[2], 30.0));
}

}  // namespace

void runAnalysisWorkbookTests() {
    testSerializeStampsV2();
    testRoundTrip();
    testAcceptsV1ShapedUpgrade();
    testSkipsUnknownKinds();
    testYLockedRoundTrip();
    testCatalogMatchAndBinding();
    testListCatalogMatchesJson();
    testEmptyCatalogIsEmpty();
    testLosslessV2Fields();
    testRejectsFutureSchemaVersion();
    testPairPlotScatterExactStillWorks();
    testPairPlotScatterNearestWithinTolerance();
    testPairPlotScatterFractionalTimestamps();
    testPairPlotScatterInterpolates();
    testPairPlotScatterRejectsFarTimestamps();
    testPairPlotScatterDecimatedOffset();
}
