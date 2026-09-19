#include "photon_ui/screen.hpp"

#include "photon_ui/analysis_eval.hpp"
#include "photon_ui/analysis_runtime.hpp"
#include "photon_ui/analysis_workbook.hpp"
#include "photon_ui/client.hpp"
#include "photon_ui/plot_axis.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"
#include "implot.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
char* photon_ls_get(const char* key);
void photon_ls_set(const char* key, const char* value);
void photon_download_text(const char* name, const char* text);
}

namespace photon::ui {
namespace {

constexpr float kRailW = 280.0f;
constexpr float kToolbarH = 32.0f;
constexpr float kCellHeaderH = 22.0f;
constexpr float kScrubH = 36.0f;
constexpr float kEventLaneH = 28.0f;
constexpr float kEventTableH = 112.0f;

std::string gPendingImport;
bool gHaveImport = false;

bool containsI(std::string_view hay, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > hay.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            const unsigned char a = static_cast<unsigned char>(hay[i + j]);
            const unsigned char b = static_cast<unsigned char>(needle[j]);
            if (std::tolower(a) != std::tolower(b)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

std::string cellLabel(const ViewSignal& src) {
    if (!src.label.empty()) {
        return src.label;
    }
    if (!src.messageName.empty() && !src.signalName.empty()) {
        return src.messageName + "." + src.signalName;
    }
    if (!src.signalName.empty()) {
        return src.signalName;
    }
    return "signal";
}

int formatPlotTime(double value, char* buff, int size, void*) {
    const auto ms = static_cast<std::int64_t>(value);
    const auto sec = ms / 1000;
    const auto rem = std::abs(ms % 1000);
    return std::snprintf(buff, static_cast<std::size_t>(size), "%lld.%03lld", static_cast<long long>(sec),
                         static_cast<long long>(rem));
}

ViewRect nextSlot(const CustomView& view, int w, int h) {
    int maxY = 0;
    for (const ViewCell& cell : view.cells) {
        maxY = std::max(maxY, cell.rect.y + cell.rect.h);
    }
    const int cols = std::max(1, view.columns);
    for (int y = 0; y <= maxY; ++y) {
        for (int x = 0; x + w <= cols; ++x) {
            bool hit = false;
            for (const ViewCell& cell : view.cells) {
                const bool xOverlap = x < cell.rect.x + cell.rect.w && x + w > cell.rect.x;
                const bool yOverlap = y < cell.rect.y + cell.rect.h && y + h > cell.rect.y;
                if (xOverlap && yOverlap) {
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                return ViewRect{x, y, w, h};
            }
        }
    }
    return ViewRect{0, maxY, w, h};
}

}  // namespace

#ifdef __EMSCRIPTEN__
EM_JS(void, photon_pick_analysis_json, (), {
    var input = document.createElement("input");
    input.type = "file";
    input.accept = ".json,.photon-analysis.json,application/json";
    input.onchange = function() {
        if (!input.files || input.files.length === 0) {
            return;
        }
        var reader = new FileReader();
        reader.onload = function() {
            var s = String(reader.result || "");
            var len = lengthBytesUTF8(s) + 1;
            var buf = _malloc(len);
            stringToUTF8(s, buf, len);
            if (Module["_photon_on_analysis_import"]) {
                Module["_photon_on_analysis_import"](buf);
            }
            _free(buf);
        };
        reader.readAsText(input.files[0]);
    };
    input.click();
});
#else
extern "C" void photon_pick_analysis_json();
#endif

extern "C" {

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void photon_on_analysis_import(const char* text) {
    if (text == nullptr) {
        return;
    }
    gPendingImport = text;
    gHaveImport = true;
}

}  // extern "C"

struct AnalysisScreen::State {
    AnalysisWorkbook doc{};
    int selectedCell = 0;
    int nextCell = 2;
    int nextEvent = 1;
    int nextDerived = 1;
    int nextTransform = 1;
    char search[128]{};
    char catalogFilter[128]{};
    char formula[160]{};
    float threshold = 0.0f;
    int edge = 0;
    bool loaded = false;
    bool catalogAsked = false;
    bool eventsOpen = true;
    bool plotScrubbing = false;
    std::string status;
};

AnalysisScreen::AnalysisScreen()
    : runtime_(std::make_unique<AnalysisRuntime>()), state_(std::make_unique<State>()) {}

AnalysisScreen::~AnalysisScreen() = default;

void AnalysisScreen::attachTransport(Client& client) {
    if (runtime_ == nullptr) {
        return;
    }
    runtime_->setTransport(&client);
    client.bindAnalysisRuntime(runtime_.get());
}

namespace {

void saveWorkbook(const AnalysisWorkbook& doc) {
    const std::string json = serializeAnalysisWorkbook(doc);
    photon_ls_set(kAnalysisWorkbookStorageKey, json.c_str());
}

void loadWorkbook(AnalysisScreen::State& st) {
    if (st.loaded) {
        return;
    }
    st.loaded = true;
    char* raw = photon_ls_get(kAnalysisWorkbookStorageKey);
    if (raw != nullptr) {
        std::string err;
        if (parseAnalysisWorkbook(raw, &st.doc, &err)) {
            ensureAnalysisWorkbook(st.doc);
            st.status = "Loaded analysis workbook";
            std::free(raw);
            return;
        }
        st.status = "localStorage workbook ignored: " + err;
        std::free(raw);
    }
    ensureAnalysisWorkbook(st.doc);
}

void markDirty(AnalysisScreen::State& st) {
    saveWorkbook(st.doc);
}

void bindPrimary(AnalysisRuntime& runtime, AnalysisScreen::State& st, const ctrl::Message::CatalogSession& row) {
    auto* run = runtime.bind(bindingFromCatalog(row));
    if (run == nullptr) {
        st.status = "Could not bind run";
        return;
    }
    upsertPrimaryBinding(st.doc, run->binding());
    runtime.setCursorMs(row.maxMs == 0 ? row.minMs : row.maxMs);
    if (row.maxMs > row.minMs) {
        const auto span = row.maxMs - row.minMs;
        runtime.setWindowMs(std::min<std::uint64_t>(span, 60000));
    }
    markDirty(st);
    st.status = "Bound " + row.id;
}

void bindOverlay(AnalysisRuntime& runtime, AnalysisScreen::State& st, const ctrl::Message::CatalogSession& row) {
    AnalysisRunBinding bind = bindingFromCatalog(row);
    bind.role = AnalysisRunRole::Overlay;
    if (const auto* primary = runtime.primary()) {
        bind.timeOffsetMs = static_cast<std::int64_t>(primary->binding().minMs) -
                            static_cast<std::int64_t>(row.minMs);
    }
    for (const auto& existing : st.doc.runBindings) {
        if (existing.role == AnalysisRunRole::Overlay && existing.sessionId == row.id) {
            bind.timeOffsetMs = existing.timeOffsetMs;
            bind.color = existing.color;
            break;
        }
    }
    auto* run = runtime.bind(std::move(bind));
    if (run == nullptr) {
        st.status = "Overlay must be a different run";
        return;
    }
    upsertOverlayBinding(st.doc, run->binding());
    markDirty(st);
    st.status = "Overlay " + row.id;
}

void assignSignal(AnalysisScreen::State& st, const ctrl::CatalogSignal& sig) {
    CustomView& view = activeAnalysisView(st.doc);
    if (view.cells.empty()) {
        ViewCell cell;
        cell.id = "cell-" + std::to_string(st.nextCell++);
        cell.title = "Plot";
        cell.rect = nextSlot(view, kDefaultCellW, kDefaultCellH);
        view.cells.push_back(std::move(cell));
        st.selectedCell = 0;
    }
    if (st.selectedCell < 0 || st.selectedCell >= static_cast<int>(view.cells.size())) {
        st.selectedCell = 0;
    }
    ViewCell& cell = view.cells[static_cast<std::size_t>(st.selectedCell)];
    for (auto it = cell.sources.begin(); it != cell.sources.end(); ++it) {
        if (sameAnalysisSignal(*it, sig)) {
            cell.sources.erase(it);
            markDirty(st);
            return;
        }
    }
    const int limit = cell.kind == ViewCellKind::Scatter ? 2 : 16;
    if (static_cast<int>(cell.sources.size()) >= limit) {
        st.status = cell.kind == ViewCellKind::Scatter ? "Scatter takes X then Y" : "Cell is at the signal limit";
        return;
    }
    cell.sources.push_back(viewSignalFromCatalog(sig));
    if (cell.title == "Plot" || cell.title.empty()) {
        cell.title = sig.name;
    }
    markDirty(st);
}

void addCell(AnalysisScreen::State& st, ViewCellKind kind) {
    CustomView& view = activeAnalysisView(st.doc);
    ViewCell cell;
    cell.id = "cell-" + std::to_string(st.nextCell++);
    cell.kind = kind;
    cell.title = kind == ViewCellKind::Value ? "Value" : (kind == ViewCellKind::Scatter ? "Scatter" : "Plot");
    cell.rect = nextSlot(view, kDefaultCellW, kind == ViewCellKind::Value ? 1 : kDefaultCellH);
    view.cells.push_back(std::move(cell));
    st.selectedCell = static_cast<int>(view.cells.size()) - 1;
    markDirty(st);
}

constexpr float kMinSeriesWidth = 0.5f;
constexpr float kMaxSeriesWidth = 8.0f;

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

ImVec4 overlaySeriesColor(const AnalysisRunBinding& bind, std::size_t index) {
    ImVec4 parsed{};
    if (parseHexColor(bind.color, &parsed)) {
        return parsed;
    }
    return theme::plotSeries(index + 8);
}

const AnalysisDerivedChannel* findDerived(const AnalysisWorkbook& doc, std::string_view id) {
    for (const auto& ch : doc.derivedChannels) {
        if (ch.id == id) {
            return &ch;
        }
    }
    return nullptr;
}

float seriesWidth(const ViewSignal& src) {
    if (src.width > 0.0f) {
        return clampf(src.width, kMinSeriesWidth, kMaxSeriesWidth);
    }
    return theme::plotLineWeight();
}

bool cellUsesDualY(const ViewCell& cell) {
    return cell.dualY && cell.kind == ViewCellKind::Line;
}

void tickHistory(AnalysisRuntime& runtime, AnalysisScreen::State& st, std::uint32_t maxPts) {
    runtime.tickHistory(collectWorkbookSignalIds(st.doc), maxPts);
}

void drawCatalogRail(UiContext& ui, AnalysisRuntime& runtime, AnalysisScreen::State& st) {
    ImGui::BeginChild("##analysis-catalog", ImVec2(kRailW, 0.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::Text("Runs");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##cat-search", "Filter session id", st.catalogFilter, sizeof(st.catalogFilter));
    theme::pushSmallFont();
    ImGui::TextWrapped("Pick a historical run. Pit keeps the titlebar session.");
    theme::popSmallFont();
    ImGui::Separator();

    if (!ui.client.haveHistoryCatalog()) {
        ImGui::TextColored(theme::mutedFg(), "Loading catalog.json…");
    } else if (ui.client.historyCatalog().empty()) {
        ImGui::TextWrapped("No sessions in catalog.json. Analysis lists historical TSDB runs with bounds and DBC hash.");
    } else {
        ImGui::BeginChild("##catalog-list", ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.48f),
                          ImGuiChildFlags_None);
        const auto* primary = runtime.primary();
        const auto* overlay = runtime.overlay();
        for (const auto& row : ui.client.historyCatalog()) {
            if (!containsI(row.id, st.catalogFilter) && !containsI(row.dbcHash, st.catalogFilter)) {
                continue;
            }
            ImGui::PushID(row.id.c_str());
            const bool selected = primary != nullptr && primary->binding().sessionId == row.id;
            const bool isOverlay = overlay != nullptr && overlay->binding().sessionId == row.id;
            char label[256];
            std::snprintf(label, sizeof(label), "%s##sel", row.id.c_str());
            if (ImGui::Selectable(label, selected, 0, ImVec2(ImGui::GetContentRegionAvail().x - 44.0f, 0.0f))) {
                bindPrimary(runtime, st, row);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("t=[%llu, %llu]\ndbc=%s\nsamples=%llu%s\nClick = primary. O = overlay.",
                                  static_cast<unsigned long long>(row.minMs),
                                  static_cast<unsigned long long>(row.maxMs), row.dbcHash.c_str(),
                                  static_cast<unsigned long long>(row.sampleCount),
                                  row.hasVideo ? "\nvideo" : "");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(isOverlay ? "O*" : "O")) {
                bindOverlay(runtime, st, row);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Bind as overlay run (second analysis TileCache)");
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::Separator();
    ImGui::Text("Signals");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##sig-search", "Search message.signal", st.search, sizeof(st.search));
    if (!ui.arena.haveSignalCatalog) {
        ImGui::TextWrapped("Waiting for signal_catalog from the backend.");
    } else {
        std::map<std::string, std::vector<const ctrl::CatalogSignal*>> groups;
        for (const ctrl::CatalogSignal& sig : ui.arena.signalCatalog) {
            groups[sig.message].push_back(&sig);
        }
        const std::string_view filter = st.search;
        ImGui::BeginChild("##signal-tree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        for (const auto& [message, signals] : groups) {
            bool show = containsI(message, filter);
            if (!show) {
                for (const ctrl::CatalogSignal* sig : signals) {
                    if (containsI(sig->name, filter)) {
                        show = true;
                        break;
                    }
                }
            }
            if (!show) {
                continue;
            }
            if (!ImGui::TreeNodeEx(message.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                continue;
            }
            for (const ctrl::CatalogSignal* sig : signals) {
                if (!filter.empty() && !containsI(sig->name, filter) && !containsI(message, filter)) {
                    continue;
                }
                const std::string row = sig->name + (sig->unit.empty() ? std::string() : "  " + sig->unit);
                if (ImGui::Selectable(row.c_str())) {
                    assignSignal(st, *sig);
                }
            }
            ImGui::TreePop();
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

void drawValueCell(AnalysisRuntime& runtime, const ViewCell& cell) {
    if (cell.sources.empty()) {
        ImGui::TextColored(theme::mutedFg(), "Add signal");
        return;
    }
    for (const ViewSignal& src : cell.sources) {
        if (src.hidden) {
            ImGui::TextColored(theme::mutedFg(), "%s  hidden", cellLabel(src).c_str());
            continue;
        }
        double value = 0.0;
        const bool have = src.id != 0 && runtime.sampleAt(src.id, runtime.windowToMs(), value);
        if (cell.sources.size() == 1) {
            char buf[32];
            if (have) {
                std::snprintf(buf, sizeof(buf), "%0.3f", value);
            } else {
                std::snprintf(buf, sizeof(buf), "--");
            }
            theme::pushLargeFont();
            const ImVec2 ts = ImGui::CalcTextSize(buf);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 std::max(0.0f, (ImGui::GetContentRegionAvail().x - ts.x) * 0.5f));
            ImGui::TextUnformatted(buf);
            theme::popLargeFont();
            theme::pushSmallFont();
            ImGui::TextColored(theme::mutedFg(), "%s", cellLabel(src).c_str());
            theme::popSmallFont();
        } else if (have) {
            ImGui::Text("%s  %0.3f", cellLabel(src).c_str(), value);
        } else {
            ImGui::TextColored(theme::mutedFg(), "%s  --", cellLabel(src).c_str());
        }
    }
}

void drawScatterCell(AnalysisRuntime& runtime, AnalysisScreen::State& st, ViewCell& cell,
                     std::uint32_t maxPts) {
    PlotArrays xa;
    PlotArrays ya;
    if (cell.sources.size() > 0 && cell.sources[0].id != 0) {
        xa = runtime.gatherPlot(cell.sources[0].id, runtime.windowFromMs(), runtime.windowToMs(), maxPts);
    }
    if (cell.sources.size() > 1 && cell.sources[1].id != 0) {
        ya = runtime.gatherPlot(cell.sources[1].id, runtime.windowFromMs(), runtime.windowToMs(), maxPts);
    }
    std::vector<double> xs;
    std::vector<double> ys;
    pairPlotScatter(xa, ya, xs, ys);
    const bool autoY = !cell.yLocked;
    theme::pushPlotFont();
    if (ImPlot::BeginPlot("##xy", ImGui::GetContentRegionAvail(), ImPlotFlags_Equal | ImPlotFlags_NoMenus)) {
        const std::string xName = cell.sources.empty() ? "X" : cellLabel(cell.sources[0]);
        const std::string yName = cell.sources.size() < 2 ? "Y" : cellLabel(cell.sources[1]);
        ImPlot::SetupAxes(xName.c_str(), yName.c_str(), plotValueAxisFlags(autoY, false),
                          plotValueAxisFlags(autoY, false));
        double xmin = -1.0;
        double xmax = 1.0;
        double ymin = -1.0;
        double ymax = 1.0;
        bool haveX = false;
        bool haveY = false;
        extendRange(xs, xmin, xmax, haveX);
        extendRange(ys, ymin, ymax, haveY);
        if (haveX) {
            padPlotAxis(xmin, xmax);
        }
        if (haveY) {
            padPlotAxis(ymin, ymax);
        }
        if (!haveX) {
            xmin = -1.0;
            xmax = 1.0;
        }
        if (!haveY) {
            ymin = -1.0;
            ymax = 1.0;
        }
        squareEqualLimits(xmin, xmax, ymin, ymax);
        if (autoY) {
            ImPlot::SetupAxesLimits(xmin, xmax, ymin, ymax, ImPlotCond_Always);
        }
        if (!xs.empty()) {
            if (!cell.sources.empty()) {
                const ImVec4 col = seriesColor(cell.sources[0], 0);
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 4.0f, col, 0.0f, col);
            }
            keepPlotSeriesShown();
            ImPlot::PlotScatter("XY", xs.data(), ys.data(), static_cast<int>(xs.size()));
            if (!cell.sources.empty() &&
                drawPlotLegendColorPopup("XY", cell.sources[0].color, 0, &cell.sources[0].hidden)) {
                markDirty(st);
            }
        } else {
            ImPlot::PlotText("assign X then Y", 0.0, 0.0);
        }
        if (runtime.overlay() != nullptr && cell.sources.size() >= 2 && cell.sources[0].id != 0 &&
            cell.sources[1].id != 0) {
            const auto ox = runtime.gatherOverlayPlot(cell.sources[0].id, runtime.windowFromMs(),
                                                      runtime.windowToMs(), maxPts);
            const auto oy = runtime.gatherOverlayPlot(cell.sources[1].id, runtime.windowFromMs(),
                                                      runtime.windowToMs(), maxPts);
            std::vector<double> oxs;
            std::vector<double> oys;
            pairPlotScatter(ox, oy, oxs, oys);
            if (!oxs.empty()) {
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 4.0f,
                                           overlaySeriesColor(runtime.overlay()->binding(), 0));
                keepPlotSeriesShown();
                ImPlot::PlotScatter("XY overlay", oxs.data(), oys.data(), static_cast<int>(oxs.size()));
                std::string ovColor = runtime.overlay()->binding().color;
                if (drawPlotLegendColorPopup("XY overlay", ovColor, 8)) {
                    runtime.overlay()->setColor(std::move(ovColor));
                    upsertOverlayBinding(st.doc, runtime.overlay()->binding());
                    markDirty(st);
                }
            }
        }
        if (ImPlot::IsPlotHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            cell.yLocked = false;
        }
        ImPlot::EndPlot();
    }
    theme::popPlotFont();
}

void drawLineCell(AnalysisRuntime& runtime, AnalysisScreen::State& st, ViewCell& cell, std::uint32_t maxPts) {
    const double xmax = static_cast<double>(runtime.windowToMs());
    const double xmin = static_cast<double>(runtime.windowFromMs());
    struct LineSeriesDraw {
        std::size_t index = 0;
        PlotArrays hist;
    };
    std::vector<LineSeriesDraw> series;
    series.reserve(cell.sources.size());
    double ymin = 0.0;
    double ymax = 1.0;
    double ymin2 = 0.0;
    double ymax2 = 1.0;
    bool haveY = false;
    bool haveY2 = false;
    const bool dualY = cellUsesDualY(cell);
    struct OverlayDraw {
        std::size_t index = 0;
        PlotArrays hist;
    };
    struct FormulaDraw {
        std::string name;
        PlotArrays hist;
    };
    std::vector<OverlayDraw> overlays;
    std::vector<FormulaDraw> formulas;
    for (std::size_t i = 0; i < cell.sources.size(); ++i) {
        const ViewSignal& src = cell.sources[i];
        if (src.hidden) {
            continue;
        }
        if (analysisDerivedSource(src)) {
            const auto* ch = findDerived(st.doc, src.signalName);
            if (ch == nullptr || ch->kind != "formula" || runtime.primary() == nullptr) {
                continue;
            }
            AnalysisFormulaSpec spec;
            if (!parseFormulaSpec(ch->spec, &spec)) {
                continue;
            }
            resolveFormulaIds(&spec, {});
            auto eval = evaluateFormula(*runtime.primary(), spec, runtime.windowFromMs(), runtime.windowToMs());
            extendYFromPlot(eval.plot, ymin, ymax, haveY);
            FormulaDraw row;
            row.name = ch->name.empty() ? ch->id : ch->name;
            if (eval.decimated) {
                row.name += " (decimated)";
            }
            row.hist = std::move(eval.plot);
            formulas.push_back(std::move(row));
            continue;
        }
        if (src.id == 0) {
            continue;
        }
        LineSeriesDraw row;
        row.index = i;
        row.hist = runtime.gatherPlot(src.id, runtime.windowFromMs(), runtime.windowToMs(), maxPts);
        const bool right = dualY && src.yAxis == 2;
        extendYFromPlot(row.hist, right ? ymin2 : ymin, right ? ymax2 : ymax, right ? haveY2 : haveY);
        series.push_back(std::move(row));
        if (runtime.overlay() != nullptr) {
            OverlayDraw ov;
            ov.index = i;
            ov.hist = runtime.gatherOverlayPlot(src.id, runtime.windowFromMs(), runtime.windowToMs(), maxPts);
            extendYFromPlot(ov.hist, right ? ymin2 : ymin, right ? ymax2 : ymax, right ? haveY2 : haveY);
            overlays.push_back(std::move(ov));
        }
    }
    if (haveY) {
        padPlotAxis(ymin, ymax);
    }
    if (haveY2) {
        padPlotAxis(ymin2, ymax2);
    }
    const bool autoY = !cell.yLocked;
    theme::pushPlotFont();
    ImVec2 plotSize = ImGui::GetContentRegionAvail();
    plotSize.x = std::max(32.0f, plotSize.x);
    plotSize.y = std::max(32.0f, plotSize.y);
    if (ImPlot::BeginPlot("##plot", plotSize,
                          ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMenus |
                              (cell.legend ? ImPlotFlags_None : ImPlotFlags_NoLegend))) {
        ImPlot::SetupAxes("t", "value", plotTimeAxisFlags(), plotValueAxisFlags(autoY));
        if (dualY) {
            ImPlot::SetupAxis(ImAxis_Y2, "Y2", plotValueAxisFlags(autoY) | ImPlotAxisFlags_AuxDefault);
        }
        ImPlot::SetupAxisFormat(ImAxis_X1, formatPlotTime);
        {
            double x0 = xmin;
            double x1 = xmax <= xmin ? xmin + 1.0 : xmax;
            if (sanitizePlotWindow(x0, x1)) {
                ImPlot::SetupAxisLimits(ImAxis_X1, x0, x1, ImPlotCond_Always);
            }
        }
        if (haveY && autoY) {
            ImPlot::SetupAxisLimits(ImAxis_Y1, ymin, ymax, ImPlotCond_Always);
        }
        if (dualY && haveY2 && autoY) {
            ImPlot::SetupAxisLimits(ImAxis_Y2, ymin2, ymax2, ImPlotCond_Always);
        }
        bool legendHot = false;
        for (const LineSeriesDraw& row : series) {
            ViewSignal& src = cell.sources[row.index];
            if (row.hist.t.empty() || row.hist.y.empty()) {
                continue;
            }
            const int n = static_cast<int>(std::min(row.hist.t.size(), row.hist.y.size()));
            if (n <= 0 || !plotArraysAllFinite(row.hist, n)) {
                continue;
            }
            const std::string name = cellLabel(src);
            const ImVec4 col = seriesColor(src, row.index);
            const float weight = seriesWidth(src);
            if (dualY) {
                ImPlot::SetAxes(ImAxis_X1, src.yAxis == 2 ? ImAxis_Y2 : ImAxis_Y1);
            }
            ImPlot::SetNextLineStyle(col, weight);
            if (row.hist.envelope && row.hist.yMin.size() == static_cast<std::size_t>(n) &&
                row.hist.yMax.size() == static_cast<std::size_t>(n)) {
                keepPlotSeriesShown();
                ImPlot::PlotLine(("##" + name + " min").c_str(), row.hist.t.data(), row.hist.yMin.data(), n);
                ImPlot::SetNextLineStyle(col, weight);
                keepPlotSeriesShown();
                ImPlot::PlotLine(("##" + name + " max").c_str(), row.hist.t.data(), row.hist.yMax.data(), n);
            }
            ImPlot::SetNextLineStyle(col, weight);
            keepPlotSeriesShown();
            ImPlot::PlotLine(name.c_str(), row.hist.t.data(), row.hist.y.data(), n);
            if (drawPlotLegendColorPopup(name.c_str(), src.color, row.index, &src.hidden)) {
                markDirty(st);
            }
            legendHot = legendHot || plotLegendEntryHot(name.c_str());
        }
        if (runtime.overlay() != nullptr) {
            for (const OverlayDraw& ov : overlays) {
                if (ov.hist.t.empty()) {
                    continue;
                }
                const ViewSignal& src = cell.sources[ov.index];
                const std::string name = cellLabel(src) + " overlay";
                const ImVec4 col = overlaySeriesColor(runtime.overlay()->binding(), ov.index);
                ImPlot::SetNextLineStyle(col, theme::plotLineWeight());
                keepPlotSeriesShown();
                ImPlot::PlotLine(name.c_str(), ov.hist.t.data(), ov.hist.y.data(),
                                 static_cast<int>(ov.hist.t.size()));
                std::string ovColor = runtime.overlay()->binding().color;
                if (drawPlotLegendColorPopup(name.c_str(), ovColor, ov.index + 8)) {
                    runtime.overlay()->setColor(std::move(ovColor));
                    upsertOverlayBinding(st.doc, runtime.overlay()->binding());
                    markDirty(st);
                }
                legendHot = legendHot || plotLegendEntryHot(name.c_str());
            }
        }
        for (const FormulaDraw& row : formulas) {
            if (row.hist.t.empty()) {
                continue;
            }
            ImPlot::SetNextLineStyle(theme::accent(), theme::plotLineWeight());
            ImPlot::PlotLine(row.name.c_str(), row.hist.t.data(), row.hist.y.data(),
                             static_cast<int>(row.hist.t.size()));
        }
        for (const AnalysisEvent& ev : st.doc.events) {
            if (ev.tMs < runtime.windowFromMs() || ev.tMs > runtime.windowToMs()) {
                continue;
            }
            double xs[2] = {static_cast<double>(ev.tMs), static_cast<double>(ev.tMs)};
            double ys[2] = {haveY ? ymin : 0.0, haveY ? ymax : 1.0};
            ImPlot::SetNextLineStyle(
                ev.source == kAnalysisEventManual ? theme::warning() : theme::danger(), 1.0f);
            ImPlot::PlotLine(("##ev-" + ev.id).c_str(), xs, ys, 2);
        }
        if (cell.sources.empty()) {
            ImPlot::PlotText("Add signal", 0.5, 0.5);
        }
        const bool hovered = ImPlot::IsPlotHovered();
        const bool xAxisHovered = ImPlot::IsAxisHovered(ImAxis_X1);
        const bool canScrub = (hovered || xAxisHovered) && !legendHot;
        auto scrubTo = [&](double t) {
            t = clampPlotTimeMs(t);
            double minBound = 0.0;
            double maxBound = 0.0;
            if (const auto* primary = runtime.primary()) {
                minBound = static_cast<double>(primary->binding().minMs);
                maxBound = static_cast<double>(primary->binding().maxMs);
                if (maxBound > minBound) {
                    t = std::min(maxBound, std::max(minBound, t));
                } else {
                    maxBound = 0.0;
                }
            }
            double lo = static_cast<double>(runtime.windowFromMs());
            double hi = static_cast<double>(runtime.windowToMs());
            if (!runtime.viewHeld()) {
                runtime.holdView(static_cast<std::uint64_t>(std::max(0.0, lo)),
                                 static_cast<std::uint64_t>(std::max(lo + 1.0, hi)));
                lo = static_cast<double>(runtime.windowFromMs());
                hi = static_cast<double>(runtime.windowToMs());
            }
            containPlotTime(t, lo, hi, minBound, maxBound);
            runtime.holdView(static_cast<std::uint64_t>(lo), static_cast<std::uint64_t>(hi));
            runtime.previewScrub(static_cast<std::uint64_t>(t));
        };
        const double axisLo = xmin;
        const double axisHi = xmax <= xmin ? xmin + 1.0 : xmax;
        const double mouseT = plotTimeAtMouse(axisLo, axisHi);
        if (canScrub) {
            const double wheelScrub = plotWheelScrubDeltaMs(axisHi - axisLo);
            if (wheelScrub != 0.0) {
                double t = static_cast<double>(runtime.displayCursorMs());
                if (!std::isfinite(t) || t <= 0.0) {
                    t = axisHi;
                }
                scrubTo(t + wheelScrub);
                runtime.commitScrub();
            } else if (plotWheelIsZoom()) {
                double lo = static_cast<double>(runtime.windowFromMs());
                double hi = static_cast<double>(runtime.windowToMs());
                const double factor = ImGui::GetIO().MouseWheel > 0.0f ? 0.5 : 2.0;
                const double cursorT = static_cast<double>(runtime.displayCursorMs());
                zoomTimeWindowAround(plotZoomAnchorMs(cursorT, lo, hi), lo, hi, factor);
                if (sanitizePlotWindow(lo, hi)) {
                    const auto span = static_cast<std::uint64_t>(std::max(2000.0, hi - lo));
                    runtime.setWindowMs(span);
                    cell.timeWindowSec = static_cast<float>(span) / 1000.0f;
                    if (runtime.viewHeld()) {
                        runtime.holdView(static_cast<std::uint64_t>(std::max(0.0, lo)),
                                         static_cast<std::uint64_t>(std::max(lo + 1.0, hi)));
                    }
                    markDirty(st);
                }
            }
        }
        const ImGuiIO& io = ImGui::GetIO();
        bool leftDown = io.MouseDown[ImGuiMouseButton_Left];
#ifdef __EMSCRIPTEN__
        const int jsLeft = EM_ASM_INT({
            var b = window.photonPointerButtons;
            if (typeof b !== 'number' || b < 0) return -1;
            return (b | 0) & 1;
        });
        if (jsLeft == 0) {
            leftDown = false;
        }
#endif
        const bool leftClicked = io.MouseClicked[ImGuiMouseButton_Left];
        const bool doubleClick = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (!leftDown || io.MouseReleased[ImGuiMouseButton_Left] || doubleClick) {
            if (st.plotScrubbing && !doubleClick && runtime.scrubbing()) {
                runtime.commitScrub();
            }
            st.plotScrubbing = false;
        } else if (canScrub && leftClicked) {
            st.plotScrubbing = true;
        }
        if (st.plotScrubbing && leftDown) {
            double t = mouseT;
            if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
                t = clampToPlotAxis(mouseT, axisLo, axisHi);
            }
            scrubTo(t);
        }
        double playhead = st.plotScrubbing && leftDown
                              ? (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)
                                     ? mouseT
                                     : clampToPlotAxis(mouseT, axisLo, axisHi))
                              : plotPlayheadMs(static_cast<double>(runtime.displayCursorMs()), axisLo,
                                               axisHi);
        if (!std::isfinite(playhead) || playhead <= 0.0) {
            playhead = std::isfinite(xmax) && xmax > 0.0 ? xmax : axisHi;
        }
        ImPlot::DragLineX(99, &playhead, theme::chrome(), 1.75f,
                          ImPlotDragToolFlags_NoFit | ImPlotDragToolFlags_NoInputs);
        std::vector<PlotCursorHit> hits;
        hits.reserve(series.size() + formulas.size());
        const double cursorT = std::isfinite(playhead) ? playhead : mouseT;
        for (const LineSeriesDraw& row : series) {
            const ViewSignal& src = cell.sources[row.index];
            PlotCursorHit hit;
            hit.color = seriesColor(src, row.index);
            hit.yAxis = (dualY && src.yAxis == 2) ? ImAxis_Y2 : ImAxis_Y1;
            if (samplePlotArraysAt(row.hist, cursorT, hit.y)) {
                hit.ok = true;
            } else if (src.id != 0 &&
                       runtime.sampleAt(src.id, static_cast<std::uint64_t>(std::max(0.0, cursorT)), hit.y)) {
                hit.ok = true;
            }
            if (hit.ok) {
                setPlotCursorLabel(hit, cellLabel(src).c_str(), hit.y);
            }
            hits.push_back(hit);
        }
        for (const FormulaDraw& row : formulas) {
            PlotCursorHit hit;
            hit.color = theme::accent();
            hit.ok = samplePlotArraysAt(row.hist, cursorT, hit.y);
            if (hit.ok) {
                setPlotCursorLabel(hit, row.name.c_str(), hit.y);
            }
            hits.push_back(hit);
        }
        if (!hits.empty()) {
            drawPlotCursorReadout(cursorT, hits.data(), static_cast<int>(hits.size()), false);
        }
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            cell.yLocked = false;
            markDirty(st);
        }
        ImPlot::EndPlot();
    }
    theme::popPlotFont();
}

void drawCell(AnalysisRuntime& runtime, AnalysisScreen::State& st, ViewCell& cell, ImVec2 size, int index) {
    const bool selected = index == st.selectedCell;
    ImGui::PushID(index);
    ImGui::PushStyleColor(ImGuiCol_Border, selected ? theme::primary() : theme::withAlpha(theme::border(), 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    ImGui::BeginChild("cell", size, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        st.selectedCell = index;
    }
    const ImVec2 headerMin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const std::string title = cell.title.empty() ? cell.id : cell.title;
    theme::pushSmallFont();
    const ImVec2 tsz = ImGui::CalcTextSize(title.c_str());
    draw->AddText(ImVec2(headerMin.x + 8.0f, headerMin.y + (kCellHeaderH - tsz.y) * 0.5f),
                  theme::u32(theme::mutedFg()), title.c_str());
    theme::popSmallFont();
    ImGui::SetCursorScreenPos(ImVec2(headerMin.x, headerMin.y + kCellHeaderH));

    const auto maxPts = static_cast<std::uint32_t>(std::max(32.0f, ImGui::GetContentRegionAvail().x));
    if (cell.kind == ViewCellKind::Value) {
        drawValueCell(runtime, cell);
    } else if (cell.kind == ViewCellKind::Scatter) {
        drawScatterCell(runtime, st, cell, maxPts);
    } else {
        drawLineCell(runtime, st, cell, maxPts);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void addFormulaToCell(AnalysisScreen::State& st, const std::vector<ctrl::CatalogSignal>& catalog) {
    AnalysisFormulaSpec spec;
    if (!parseFormulaSpec(st.formula, &spec)) {
        st.status = "Formula must be like 7 + 8 or $id * 2";
        return;
    }
    resolveFormulaIds(&spec, catalog);
    if ((!spec.leftIsConst && spec.leftId == 0) || (!spec.rightIsConst && spec.rightId == 0 && spec.rightName.empty())) {
        st.status = "Could not resolve formula signals";
        return;
    }
    AnalysisDerivedChannel ch;
    ch.id = "d-" + std::to_string(st.nextDerived++);
    ch.name = spec.expr;
    ch.kind = "formula";
    ch.spec = spec.expr;
    st.doc.derivedChannels.push_back(ch);
    CustomView& view = activeAnalysisView(st.doc);
    if (st.selectedCell < 0 || st.selectedCell >= static_cast<int>(view.cells.size())) {
        addCell(st, ViewCellKind::Line);
    }
    ViewCell& cell = view.cells[static_cast<std::size_t>(st.selectedCell)];
    ViewSignal src;
    src.messageName = kAnalysisDerivedMessage;
    src.signalName = ch.id;
    src.label = ch.name;
    cell.sources.push_back(std::move(src));
    markDirty(st);
    st.status = "Added formula " + ch.name;
}

void extractWindowEvents(AnalysisRuntime& runtime, AnalysisScreen::State& st, std::uint32_t signalId) {
    if (runtime.primary() == nullptr || signalId == 0) {
        st.status = "Bind a run and pick a signal first";
        return;
    }
    AnalysisTransform xf;
    xf.id = "xf-" + std::to_string(st.nextTransform++);
    xf.signalId = signalId;
    xf.kind = "threshold";
    xf.spec = st.edge == 1 ? "falling" : (st.edge == 2 ? "both" : "rising");
    xf.threshold = static_cast<double>(st.threshold);
    xf.label = "threshold";
    st.doc.transforms.push_back(xf);
    auto extracted = extractThresholdEvents(*runtime.primary(), xf, runtime.windowFromMs(), runtime.windowToMs());
    replaceThresholdEvents(&st.doc.events, std::move(extracted));
    markDirty(st);
    st.status = "Extracted " + std::to_string(st.doc.events.size()) + " events from L0";
}

void markManualEvent(AnalysisRuntime& runtime, AnalysisScreen::State& st) {
    AnalysisEvent ev;
    ev.id = "m-" + std::to_string(st.nextEvent++);
    ev.tMs = runtime.displayCursorMs();
    ev.source = kAnalysisEventManual;
    ev.label = "marker";
    st.doc.events.push_back(std::move(ev));
    markDirty(st);
    st.status = "Marked t=" + std::to_string(runtime.displayCursorMs());
}

void drawOverlayOffset(AnalysisRuntime& runtime, AnalysisScreen::State& st) {
    auto* overlay = runtime.overlay();
    if (overlay == nullptr) {
        return;
    }
    ImGui::SameLine();
    ImGui::TextColored(theme::mutedFg(), "  ovl %s", overlay->binding().sessionId.c_str());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    int off = static_cast<int>(overlay->binding().timeOffsetMs);
    if (ImGui::SliderInt("##ovl-off", &off, -120000, 120000, "off %d ms")) {
        runtime.setOverlayOffsetMs(off);
        upsertOverlayBinding(st.doc, overlay->binding());
        markDirty(st);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Render-time offset. Resident tiles are not refetched.");
    }
}

void drawFormulaBar(AnalysisRuntime& runtime, AnalysisScreen::State& st,
                    const std::vector<ctrl::CatalogSignal>& catalog) {
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##formula", "7 + 8  or  RPM * 0.1", st.formula, sizeof(st.formula));
    ImGui::SameLine();
    if (ImGui::Button("Add formula", ImVec2(0.0f, kToolbarH))) {
        addFormulaToCell(st, catalog);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::InputFloat("##thr", &st.threshold, 0.0f, 0.0f, "%.3f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::Combo("##edge", &st.edge, "rising\0falling\0both\0");
    ImGui::SameLine();
    if (ImGui::Button("Extract", ImVec2(0.0f, kToolbarH))) {
        std::uint32_t sid = 0;
        CustomView& view = activeAnalysisView(st.doc);
        if (st.selectedCell >= 0 && st.selectedCell < static_cast<int>(view.cells.size())) {
            for (const auto& src : view.cells[static_cast<std::size_t>(st.selectedCell)].sources) {
                if (!analysisDerivedSource(src) && src.id != 0) {
                    sid = src.id;
                    break;
                }
            }
        }
        extractWindowEvents(runtime, st, sid);
    }
    ImGui::SameLine();
    if (ImGui::Button("Mark", ImVec2(0.0f, kToolbarH))) {
        markManualEvent(runtime, st);
    }
}

void drawEventLane(AnalysisRuntime& runtime, AnalysisScreen::State& st) {
    ImGui::BeginChild("##event-lane", ImVec2(0.0f, kEventLaneH), ImGuiChildFlags_Borders);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), theme::u32(theme::withAlpha(theme::card(), 0.6f)));
    const auto from = runtime.windowFromMs();
    const auto to = runtime.windowToMs();
    const double span = static_cast<double>(to > from ? to - from : 1);
    for (const AnalysisEvent& ev : st.doc.events) {
        if (ev.tMs < from || ev.tMs > to) {
            continue;
        }
        const float x = p0.x + static_cast<float>((static_cast<double>(ev.tMs - from) / span) * size.x);
        const ImU32 col = theme::u32(ev.source == kAnalysisEventManual ? theme::warning() : theme::danger());
        draw->AddLine(ImVec2(x, p0.y + 2.0f), ImVec2(x, p0.y + size.y - 2.0f), col, 2.0f);
    }
    theme::pushSmallFont();
    ImGui::TextColored(theme::mutedFg(), "Events  %zu  (click a row to seek)", st.doc.events.size());
    theme::popSmallFont();
    ImGui::EndChild();
}

void drawEventTable(AnalysisRuntime& runtime, AnalysisScreen::State& st) {
    if (!ImGui::CollapsingHeader("Event review", st.eventsOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        st.eventsOpen = false;
        return;
    }
    st.eventsOpen = true;
    ImGui::BeginChild("##event-table", ImVec2(0.0f, kEventTableH), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##events", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("tMs", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("source", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(st.doc.events.size()); ++i) {
            const AnalysisEvent& ev = st.doc.events[static_cast<std::size_t>(i)];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(i);
            char tbuf[32];
            std::snprintf(tbuf, sizeof(tbuf), "%llu", static_cast<unsigned long long>(ev.tMs));
            if (ImGui::Selectable(tbuf, false, ImGuiSelectableFlags_SpanAllColumns)) {
                runtime.seekCursor(ev.tMs);
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(ev.source.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(ev.label.c_str());
            ImGui::TableSetColumnIndex(3);
            if (ev.value.has_value()) {
                ImGui::Text("%.3f", *ev.value);
            } else {
                ImGui::TextUnformatted("--");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void drawScrub(AnalysisRuntime& runtime, AnalysisScreen::State&) {
    const auto* primary = runtime.primary();
    const std::uint64_t minMs = primary == nullptr ? 0 : primary->binding().minMs;
    const std::uint64_t maxMs = primary == nullptr ? 1 : std::max(primary->binding().maxMs, minMs + 1);
    const double lo = static_cast<double>(minMs);
    const double hi = static_cast<double>(maxMs);
    double cur = static_cast<double>(runtime.displayCursorMs());
    if (cur < lo) {
        cur = lo;
    }
    if (cur > hi) {
        cur = hi;
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderScalar("##analysis-cursor", ImGuiDataType_Double, &cur, &lo, &hi, "t=%.0f")) {
        runtime.releaseView();
        runtime.previewScrub(static_cast<std::uint64_t>(std::max(0.0, cur)));
    }
    if (ImGui::IsItemDeactivatedAfterEdit() ||
        (ImGui::IsItemDeactivated() && runtime.scrubbing() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        runtime.commitScrub();
    }
    theme::pushSmallFont();
    ImGui::TextColored(theme::mutedFg(), "Shared analysis cursor  window %.1fs  %s",
                       static_cast<double>(runtime.windowMs()) / 1000.0,
                       runtime.scrubbing() ? "scrub (client-side)" : "committed");
    theme::popSmallFont();
}

}  // namespace

void AnalysisScreen::onPlayOrLive() {
    if (runtime_ != nullptr) {
        runtime_->cancelScrub();
        runtime_->releaseView();
    }
}

void AnalysisScreen::draw(UiContext& ui) {
    if (runtime_ == nullptr || state_ == nullptr) {
        return;
    }
    State& st = *state_;
    loadWorkbook(st);
    if (gHaveImport) {
        std::string err;
        if (parseAnalysisWorkbook(gPendingImport, &st.doc, &err)) {
            ensureAnalysisWorkbook(st.doc);
            markDirty(st);
            st.status = "Imported workbook";
        } else {
            st.status = "Import failed: " + err;
        }
        gPendingImport.clear();
        gHaveImport = false;
    }
    if (ui.client.helloOk() && !st.catalogAsked) {
        ui.client.requestCatalogList();
        st.catalogAsked = true;
    }
    resolveAnalysisSignals(st.doc, ui.arena.signalCatalog);
    if (!st.doc.runBindings.empty() && ui.client.haveHistoryCatalog()) {
        if (!runtime_->hasBoundRun()) {
            for (const auto& bind : st.doc.runBindings) {
                if (bind.role != AnalysisRunRole::Primary) {
                    continue;
                }
                for (const auto& row : ui.client.historyCatalog()) {
                    if (row.id == bind.sessionId) {
                        bindPrimary(*runtime_, st, row);
                        break;
                    }
                }
            }
        }
        if (runtime_->primary() != nullptr && runtime_->overlay() == nullptr) {
            for (const auto& bind : st.doc.runBindings) {
                if (bind.role != AnalysisRunRole::Overlay) {
                    continue;
                }
                for (const auto& row : ui.client.historyCatalog()) {
                    if (row.id == bind.sessionId) {
                        bindOverlay(*runtime_, st, row);
                        runtime_->setOverlayOffsetMs(bind.timeOffsetMs);
                        if (auto* ov = runtime_->overlay()) {
                            ov->setColor(bind.color);
                            upsertOverlayBinding(st.doc, ov->binding());
                        }
                        break;
                    }
                }
            }
        }
    }

    ImGui::BeginChild("##analysis-root", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    drawCatalogRail(ui, *runtime_, st);
    ImGui::SameLine(0.0f, 10.0f);

    ImGui::BeginChild("##analysis-main", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    CustomView& view = activeAnalysisView(st.doc);
    if (ImGui::Button("Line", ImVec2(0.0f, kToolbarH))) {
        addCell(st, ViewCellKind::Line);
    }
    ImGui::SameLine();
    if (ImGui::Button("Value", ImVec2(0.0f, kToolbarH))) {
        addCell(st, ViewCellKind::Value);
    }
    ImGui::SameLine();
    if (ImGui::Button("Scatter", ImVec2(0.0f, kToolbarH))) {
        addCell(st, ViewCellKind::Scatter);
    }
    if (st.selectedCell >= 0 && st.selectedCell < static_cast<int>(view.cells.size())) {
        ViewCell& selected = view.cells[static_cast<std::size_t>(st.selectedCell)];
        if (selected.kind != ViewCellKind::Value) {
            ImGui::SameLine();
            bool autoY = !selected.yLocked;
            if (ImGui::Checkbox("Auto Y", &autoY)) {
                selected.yLocked = !autoY;
                markDirty(st);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Fit Y to the visible window. Uncheck to drag the axis.\n"
                    "Double-click the plot to restore Auto Y.");
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Export", ImVec2(0.0f, kToolbarH))) {
        photon_download_text("analysis.photon-workbook.json", serializeAnalysisWorkbook(st.doc).c_str());
        st.status = "Exported schemaVersion 2 workbook";
    }
    ImGui::SameLine();
    if (ImGui::Button("Import", ImVec2(0.0f, kToolbarH))) {
        photon_pick_analysis_json();
    }
    if (runtime_->hasBoundRun()) {
        ImGui::SameLine();
        const auto& bind = runtime_->primary()->binding();
        ImGui::TextColored(theme::mutedFg(), "  %s", bind.sessionId.c_str());
    }
    drawOverlayOffset(*runtime_, st);
    if (!st.status.empty()) {
        ImGui::SameLine();
        theme::pushSmallFont();
        ImGui::TextColored(theme::mutedFg(), "%s", st.status.c_str());
        theme::popSmallFont();
    }

    drawFormulaBar(*runtime_, st, ui.arena.signalCatalog);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float reviewH = kEventLaneH + (st.eventsOpen ? kEventTableH + 28.0f : 22.0f);
    const float gridH = std::max(80.0f, avail.y - kScrubH - reviewH - 8.0f);
    ImGui::BeginChild("##analysis-grid", ImVec2(avail.x, gridH), ImGuiChildFlags_None);
    const ImVec2 gridAvail = ImGui::GetContentRegionAvail();
    const int cols = std::max(1, view.columns);
    const float unitW = gridAvail.x / static_cast<float>(cols);
    const float unitH = view.rowHeight;
    int maxRow = 1;
    for (const ViewCell& cell : view.cells) {
        maxRow = std::max(maxRow, cell.rect.y + cell.rect.h);
    }
    ImGui::Dummy(ImVec2(gridAvail.x, static_cast<float>(maxRow) * unitH));
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 scroll(ImGui::GetScrollX(), ImGui::GetScrollY());
    for (int i = 0; i < static_cast<int>(view.cells.size()); ++i) {
        ViewCell& cell = view.cells[static_cast<std::size_t>(i)];
        const float x = static_cast<float>(cell.rect.x) * unitW;
        const float y = static_cast<float>(cell.rect.y) * unitH;
        const float w = std::max(40.0f, static_cast<float>(cell.rect.w) * unitW - view.gap);
        const float h = std::max(48.0f, static_cast<float>(cell.rect.h) * unitH - view.gap);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + x - scroll.x, origin.y + y - scroll.y));
        drawCell(*runtime_, st, cell, ImVec2(w, h), i);
    }
    ImGui::EndChild();
    drawEventLane(*runtime_, st);
    drawEventTable(*runtime_, st);
    drawScrub(*runtime_, st);
    tickHistory(*runtime_, st, static_cast<std::uint32_t>(std::max(32.0f, avail.x)));
    ImGui::EndChild();
    ImGui::EndChild();
}

}  // namespace photon::ui
