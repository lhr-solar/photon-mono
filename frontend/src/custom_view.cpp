#include "photon_ui/custom_view.hpp"

#include "photon/json.hpp"
#include "photon/protocol.hpp"
#include "photon_ui/client.hpp"
#include "photon_ui/clock_format.hpp"
#include "photon_ui/gg_plot.hpp"
#include "photon_ui/plot_axis.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"
#include "implot.h"

#include "photon_ui/gl_api.hpp"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace photon::ui {

std::string gPendingImport;
bool gHaveImport = false;

namespace {

constexpr const char* kStorageKey = "photon-next.custom-views";
constexpr int kMaxSources = 16;
constexpr int kMaxViews = 32;
constexpr int kMaxCells = 128;
constexpr float kToolbarH = 32.0f;
constexpr float kCellHeaderH = 22.0f;
constexpr float kResizeHandle = 14.0f;
constexpr float kMinSeriesWidth = 0.5f;
constexpr float kMaxSeriesWidth = 8.0f;

ViewSignal namedSource(const char* message, const char* signal, const char* label) {
    ViewSignal src;
    src.messageName = message;
    src.signalName = signal;
    src.label = label;
    return src;
}

ViewCell lineCell(const char* id, const char* title, ViewRect rect, ViewSignal src, float windowSec) {
    ViewCell cell;
    cell.id = id;
    cell.title = title;
    cell.kind = ViewCellKind::Line;
    cell.rect = rect;
    cell.timeWindowSec = windowSec;
    cell.sources.push_back(std::move(src));
    return cell;
}

bool shellButton(const char* label, bool selected, const char* id = nullptr) {
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              theme::withAlpha(theme::mixColor(theme::card(), theme::active(), 0.68f), 0.88f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::withAlpha(theme::active(), 0.72f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::withAlpha(theme::active(), 0.88f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::foreground());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(theme::muted(), 0.22f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::withAlpha(theme::active(), 0.52f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::withAlpha(theme::active(), 0.68f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::mutedFg());
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kFrameRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, theme::kFramePadding);
    char buf[192];
    const char* visible = (label != nullptr && label[0] != '\0') ? label : " ";
    const char* shown = visible;
    if (id != nullptr && id[0] != '\0') {
        std::snprintf(buf, sizeof(buf), "%s###%s", visible, id);
        shown = buf;
    }
    const bool hit = ImGui::Button(shown, ImVec2(0.0f, kToolbarH));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return hit;
}

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

int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float seriesWidth(const ViewSignal& src) {
    if (src.width > 0.0f) {
        return clampf(src.width, kMinSeriesWidth, kMaxSeriesWidth);
    }
    return theme::plotLineWeight();
}

float scatterMarkerSize(const ViewSignal& src) {
    if (src.width > 0.0f) {
        return clampf(src.width, 1.5f, kMaxSeriesWidth);
    }
    return 3.5f;
}

int parseYAxis(const json::Value& src) {
    if (!src.contains("yAxis")) {
        return 1;
    }
    const json::Value& axis = src["yAxis"];
    if (axis.isString()) {
        const std::string key = axis.asString();
        if (key == "2" || key == "y2" || key == "Y2") {
            return 2;
        }
        return 1;
    }
    return static_cast<int>(axis.asInt(1)) == 2 ? 2 : 1;
}

bool cellUsesDualY(const ViewCell& cell) {
    return cell.dualY && cell.kind == ViewCellKind::Line;
}

bool pathMentionsDay(const std::string& path, const ctrl::Message::DriveDay& day) {
    if (path.empty()) {
        return false;
    }
    if (!day.path.empty() && path == day.path) {
        return true;
    }
    return !day.date.empty() && path.find(day.date) != std::string::npos;
}

int currentDriveDayIndex(const ClientArena& arena) {
    for (int i = 0; i < static_cast<int>(arena.driveDays.size()); ++i) {
        const auto& day = arena.driveDays[static_cast<std::size_t>(i)];
        if (arena.source == ctrl::SourceKind::LogReplay && pathMentionsDay(arena.sourcePath, day)) {
            return i;
        }
    }
    return arena.driveDays.empty() ? -1 : 0;
}

std::string sanitizeFileToken(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ' || c == '/' || c == '\\') {
            if (out.empty() || out.back() != '-') {
                out.push_back('-');
            }
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out.empty() ? "plot" : out;
}

struct PlotPngPending {
    bool ready = false;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    std::string name;
};

PlotPngPending gPlotPng;

const char* kindKey(ViewCellKind kind) {
    if (kind == ViewCellKind::Value) {
        return "list";
    }
    if (kind == ViewCellKind::Scatter) {
        return "scatter";
    }
    return "line";
}

ViewCellKind kindFromKey(std::string_view key) {
    if (key == "list") {
        return ViewCellKind::Value;
    }
    if (key == "scatter") {
        return ViewCellKind::Scatter;
    }
    return ViewCellKind::Line;
}

std::size_t ringLogicalIndex(const SignalRing& ring, std::size_t logicalOldest) {
    if (ring.count < ring.cap) {
        return logicalOldest;
    }
    return (ring.plotOffset() + logicalOldest) % ring.cap;
}

int formatPlotTime(double tMs, char* buf, int size, void*) {
    return formatLocalClock(tMs, buf, static_cast<std::size_t>(size));
}

struct RangeStats {
    std::size_t n = 0;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    bool ok = false;
};

void holdScrubWindow(CustomView& view, double lo, double hi) {
    if (hi < lo) {
        std::swap(lo, hi);
    }
    for (ViewCell& cell : view.cells) {
        if (cell.kind != ViewCellKind::Line) {
            continue;
        }
        cell.freeze = true;
        cell.scrubHold = true;
        cell.viewMinMs = lo;
        cell.viewMaxMs = hi;
    }
}

void releaseScrubHolds(CustomView& view) {
    for (ViewCell& cell : view.cells) {
        if (!cell.scrubHold) {
            continue;
        }
        cell.scrubHold = false;
        cell.freeze = false;
    }
}

}  // namespace

void CustomViews::releasePlayheadScrub() {
    if (doc_.views.empty()) {
        return;
    }
    releaseScrubHolds(active());
}

void CustomViews::followSessionWindow() {
    plotScrubbing_ = false;
    for (CustomView& view : doc_.views) {
        for (ViewCell& cell : view.cells) {
            if (cell.kind != ViewCellKind::Line) {
                continue;
            }
            cell.freeze = false;
            cell.scrubHold = false;
            cell.rangeActive = false;
            cell.rangeDragging = false;
            cell.viewMinMs = 0.0;
            cell.viewMaxMs = 0.0;
        }
    }
}

namespace {

RangeStats statsInRange(const SignalRing& ring, double t0, double t1) {
    RangeStats out;
    if (t1 < t0) {
        std::swap(t0, t1);
    }
    double sum = 0.0;
    for (std::size_t i = 0; i < ring.count; ++i) {
        const std::size_t p = ringLogicalIndex(ring, i);
        const double t = ring.t[p];
        if (t < t0 || t > t1) {
            continue;
        }
        const double v = ring.v[p];
        if (!std::isfinite(v)) {
            continue;
        }
        if (out.n == 0) {
            out.min = v;
            out.max = v;
        } else {
            out.min = std::min(out.min, v);
            out.max = std::max(out.max, v);
        }
        sum += v;
        ++out.n;
    }
    if (out.n == 0) {
        return out;
    }
    out.mean = sum / static_cast<double>(out.n);
    out.ok = true;
    return out;
}

void binGgCloud(const std::vector<double>& x, const std::vector<double>& y, double cell,
                std::vector<double>& ox, std::vector<double>& oy) {
    ox.clear();
    oy.clear();
    if (x.empty() || x.size() != y.size() || cell <= 0.0) {
        return;
    }
    std::unordered_set<std::uint64_t> seen;
    seen.reserve(x.size());
    ox.reserve(std::min(x.size(), static_cast<std::size_t>(512)));
    oy.reserve(ox.capacity());
    const double inv = 1.0 / cell;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(x[i]) || !std::isfinite(y[i])) {
            continue;
        }
        const auto ix = static_cast<std::int32_t>(std::floor(x[i] * inv));
        const auto iy = static_cast<std::int32_t>(std::floor(y[i] * inv));
        const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(ix)) << 32) |
                         static_cast<std::uint32_t>(iy);
        if (!seen.insert(key).second) {
            continue;
        }
        ox.push_back((static_cast<double>(ix) + 0.5) * cell);
        oy.push_back((static_cast<double>(iy) + 0.5) * cell);
    }
}

void pairScatter(const SignalRing& x, const SignalRing& y, std::vector<double>& xs,
                 std::vector<double>& ys) {
    xs.clear();
    ys.clear();
    std::unordered_map<std::uint64_t, double> byTs;
    byTs.reserve(x.count);
    for (std::size_t i = 0; i < x.count; ++i) {
        const std::size_t p = ringLogicalIndex(x, i);
        if (std::isfinite(x.v[p])) {
            byTs[static_cast<std::uint64_t>(x.t[p])] = x.v[p];
        }
    }
    xs.reserve(y.count);
    ys.reserve(y.count);
    for (std::size_t i = 0; i < y.count; ++i) {
        const std::size_t p = ringLogicalIndex(y, i);
        if (!std::isfinite(y.v[p])) {
            continue;
        }
        const auto it = byTs.find(static_cast<std::uint64_t>(y.t[p]));
        if (it != byTs.end()) {
            xs.push_back(it->second);
            ys.push_back(y.v[p]);
        }
    }
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

bool sameSignal(const ViewSignal& a, const ctrl::CatalogSignal& b) {
    if (!a.messageName.empty() && !a.signalName.empty()) {
        return a.messageName == b.message && a.signalName == b.name;
    }
    return a.id != 0 && a.id == b.id;
}

bool occupied(const CustomView& view, int x, int y, int w, int h, int skip) {
    for (int i = 0; i < static_cast<int>(view.cells.size()); ++i) {
        if (i == skip) {
            continue;
        }
        const ViewRect& r = view.cells[static_cast<std::size_t>(i)].rect;
        const bool xOverlap = x < r.x + r.w && x + w > r.x;
        const bool yOverlap = y < r.y + r.h && y + h > r.y;
        if (xOverlap && yOverlap) {
            return true;
        }
    }
    return false;
}

ViewRect nextSlot(const CustomView& view, int w, int h) {
    int maxY = 0;
    for (const ViewCell& cell : view.cells) {
        maxY = std::max(maxY, cell.rect.y + cell.rect.h);
    }
    const int cols = std::max(1, view.columns);
    for (int y = 0; y <= maxY; ++y) {
        for (int x = 0; x + w <= cols; ++x) {
            if (!occupied(view, x, y, w, h, -1)) {
                return ViewRect{x, y, w, h};
            }
        }
    }
    return ViewRect{0, maxY, w, h};
}

ViewCell parseWidget(const json::Value& widget, int* nextId) {
    ViewCell cell;
    cell.id = widget["id"].asString();
    if (cell.id.empty()) {
        cell.id = "cell-" + std::to_string(*nextId);
        ++*nextId;
    }
    const json::Value& rect = widget["rect"];
    cell.rect.x = std::max(0, static_cast<int>(rect["x"].asInt()));
    cell.rect.y = std::max(0, static_cast<int>(rect["y"].asInt()));
    cell.rect.w = std::max(1, static_cast<int>(rect["w"].asInt(1)));
    cell.rect.h = std::max(1, static_cast<int>(rect["h"].asInt(1)));
    const json::Value& plot = widget["plot"];
    cell.kind = kindFromKey(plot["type"].asString("line"));
    cell.title = plot["title"].asString(cell.id);
    cell.timeWindowSec = static_cast<float>(plot["timeWindowSeconds"].asNumber(15.0));
    if (cell.timeWindowSec < 2.0f) {
        cell.timeWindowSec = 2.0f;
    }
    if (cell.timeWindowSec > 7200.0f) {
        cell.timeWindowSec = 7200.0f;
    }
    if (plot["options"].isObject()) {
        const json::Value& options = plot["options"];
        if (options.contains("legend")) {
            cell.legend = options["legend"].asBool(true);
        }
        if (options.contains("dualY") && options["dualY"].isBool()) {
            cell.dualY = cell.kind == ViewCellKind::Line && options["dualY"].asBool(false);
        }
        if (options.contains("yLocked") && options["yLocked"].isBool()) {
            cell.yLocked = options["yLocked"].asBool(false);
        }
    }
    const json::Value& sources = plot["sources"];
    if (sources.isArray()) {
        for (const json::Value& src : sources.asArray()) {
            if (cell.sources.size() >= static_cast<std::size_t>(kMaxSources)) {
                break;
            }
            ViewSignal sig;
            sig.messageId = static_cast<std::uint32_t>(src["messageId"].asInt());
            sig.messageName = src["messageName"].asString();
            sig.signalName = src["signalName"].asString();
            sig.label = src["label"].asString();
            if (src.contains("color") && src["color"].isString()) {
                ImVec4 parsed{};
                if (parseHexColor(src["color"].asString(), &parsed)) {
                    sig.color = formatHexColor(parsed);
                }
            }
            if (src.contains("width") && src["width"].isNumber()) {
                const float width = static_cast<float>(src["width"].asNumber());
                if (width > 0.0f) {
                    sig.width = clampf(width, kMinSeriesWidth, kMaxSeriesWidth);
                }
            }
            if (src.contains("hidden") && src["hidden"].isBool()) {
                sig.hidden = src["hidden"].asBool(false);
            }
            if (cell.kind == ViewCellKind::Line) {
                sig.yAxis = parseYAxis(src);
            }
            cell.sources.push_back(std::move(sig));
        }
    }
    return cell;
}

CustomView parsePanel(const json::Value& root, int* nextCell) {
    CustomView view;
    view.id = root["id"].asString("view");
    view.name = root["name"].asString("View");
    const json::Value& layout = root["layout"];
    view.columns = clampi(static_cast<int>(layout["columns"].asInt(kGridCols)), 1, 48);
    view.rowHeight = static_cast<float>(layout["rowHeight"].asNumber(kGridRowH));
    if (view.rowHeight < 48.0f) {
        view.rowHeight = 48.0f;
    }
    if (view.rowHeight > 1200.0f) {
        view.rowHeight = 1200.0f;
    }
    view.gap = static_cast<float>(layout["gap"].asNumber(10.0));
    if (view.gap < 0.0f) {
        view.gap = 0.0f;
    }
    if (view.gap > 64.0f) {
        view.gap = 64.0f;
    }
    if (layout.contains("locked") && layout["locked"].isBool()) {
        view.locked = layout["locked"].asBool(false);
    }
    const json::Value& widgets = root["widgets"];
    if (!widgets.isArray()) {
        return view;
    }
    for (const json::Value& widget : widgets.asArray()) {
        if (view.cells.size() >= static_cast<std::size_t>(kMaxCells)) {
            break;
        }
        const std::string kind = widget["kind"].asString();
        if (kind != "plot" && !kind.empty()) {
            continue;
        }
        if (!widget["plot"].isObject() && kind != "plot" && !kind.empty()) {
            continue;
        }
        if (!widget["plot"].isObject()) {
            continue;
        }
        view.cells.push_back(parseWidget(widget, nextCell));
    }
    return view;
}

json::Value signalToJson(const ViewSignal& src, ViewCellKind kind) {
    json::Object o;
    o.emplace("messageId", json::Value(static_cast<int>(src.messageId)));
    if (!src.messageName.empty()) {
        o.emplace("messageName", json::Value(src.messageName));
    }
    if (!src.signalName.empty()) {
        o.emplace("signalName", json::Value(src.signalName));
    }
    if (!src.label.empty()) {
        o.emplace("label", json::Value(src.label));
    }
    if (!src.color.empty()) {
        o.emplace("color", json::Value(src.color));
    }
    if (src.width > 0.0f) {
        o.emplace("width", json::Value(static_cast<double>(src.width)));
    }
    if (src.hidden) {
        o.emplace("hidden", json::Value(true));
    }
    if (kind == ViewCellKind::Line && src.yAxis == 2) {
        o.emplace("yAxis", json::Value(2));
    }
    return json::Value(std::move(o));
}

json::Value cellToJson(const ViewCell& cell) {
    json::Array sources;
    for (const ViewSignal& src : cell.sources) {
        sources.push_back(signalToJson(src, cell.kind));
    }
    json::Object options;
    options.emplace("legend", json::Value(cell.legend));
    if (cell.kind == ViewCellKind::Line && cell.dualY) {
        options.emplace("dualY", json::Value(true));
    }
    if (cell.yLocked) {
        options.emplace("yLocked", json::Value(true));
    }
    json::Object plot;
    plot.emplace("type", json::Value(std::string(kindKey(cell.kind))));
    plot.emplace("title", json::Value(cell.title));
    plot.emplace("useSource1TimeAsX", json::Value(cell.kind != ViewCellKind::Scatter));
    plot.emplace("timeWindowSeconds", json::Value(static_cast<double>(cell.timeWindowSec)));
    plot.emplace("sources", json::Value(std::move(sources)));
    plot.emplace("options", json::Value(std::move(options)));
    json::Object rect;
    rect.emplace("x", json::Value(cell.rect.x));
    rect.emplace("y", json::Value(cell.rect.y));
    rect.emplace("w", json::Value(cell.rect.w));
    rect.emplace("h", json::Value(cell.rect.h));
    json::Object widget;
    widget.emplace("id", json::Value(cell.id));
    widget.emplace("kind", json::Value("plot"));
    widget.emplace("rect", json::Value(std::move(rect)));
    widget.emplace("plot", json::Value(std::move(plot)));
    return json::Value(std::move(widget));
}

json::Value viewToJson(const CustomView& view) {
    json::Array widgets;
    for (const ViewCell& cell : view.cells) {
        widgets.push_back(cellToJson(cell));
    }
    json::Object layout;
    layout.emplace("columns", json::Value(view.columns));
    layout.emplace("rowHeight", json::Value(static_cast<double>(view.rowHeight)));
    layout.emplace("gap", json::Value(static_cast<double>(view.gap)));
    if (view.locked) {
        layout.emplace("locked", json::Value(true));
    }
    json::Object panel;
    panel.emplace("id", json::Value(view.id));
    panel.emplace("name", json::Value(view.name));
    panel.emplace("layout", json::Value(std::move(layout)));
    panel.emplace("widgets", json::Value(std::move(widgets)));
    return json::Value(std::move(panel));
}

}  // namespace

}  // namespace photon::ui

#ifdef __EMSCRIPTEN__
EM_JS(char*, photon_ls_get, (const char* key), {
    var v = localStorage.getItem(UTF8ToString(key));
    if (v === null || v === undefined) {
        return 0;
    }
    var len = lengthBytesUTF8(v) + 1;
    var buf = _malloc(len);
    stringToUTF8(v, buf, len);
    return buf;
});

EM_JS(void, photon_ls_set, (const char* key, const char* value), {
    localStorage.setItem(UTF8ToString(key), UTF8ToString(value));
});

EM_JS(void, photon_download_text, (const char* name, const char* text), {
    var blob = new Blob([UTF8ToString(text)], {type: "application/json"});
    var a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = UTF8ToString(name);
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(a.href);
});

EM_JS(void, photon_download_png, (const char* name, int w, int h, const uint8_t* rgba), {
    var width = w | 0;
    var height = h | 0;
    var ptr = rgba >>> 0;
    var nbytes = width * height * 4;
    if (width < 1 || height < 1 || nbytes <= 0) {
        return;
    }
    var heap = (typeof HEAPU8 !== "undefined" && HEAPU8) ? HEAPU8
             : (Module && Module.HEAPU8);
    if (!heap) {
        var mem = (typeof wasmMemory !== "undefined" && wasmMemory) ? wasmMemory
                : (Module && Module.wasmMemory);
        if (!mem || !mem.buffer) {
            return;
        }
        heap = new Uint8Array(mem.buffer);
    }
    var pixels = new Uint8Array(nbytes);
    pixels.set(heap.subarray(ptr, ptr + nbytes));
    var fileName = UTF8ToString(name) || "plot.png";
    var canvas = document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    var ctx = canvas.getContext("2d");
    if (!ctx) {
        return;
    }
    var img = ctx.createImageData(width, height);
    img.data.set(pixels);
    ctx.putImageData(img, 0, 0);
    canvas.toBlob(function(blob) {
        if (!blob) {
            return;
        }
        var a = document.createElement("a");
        a.href = URL.createObjectURL(blob);
        a.download = fileName;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        setTimeout(function() { URL.revokeObjectURL(a.href); }, 1000);
    }, "image/png");
});

EM_JS(void, photon_pick_json, (), {
    var input = document.createElement("input");
    input.type = "file";
    input.accept = ".json,.photon-view.json,application/json";
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
            if (Module["_photon_on_view_import"]) {
                Module["_photon_on_view_import"](buf);
            }
            _free(buf);
        };
        reader.readAsText(input.files[0]);
    };
    input.click();
});
#else
extern "C" {
char* photon_ls_get(const char* key);
void photon_ls_set(const char* key, const char* value);
void photon_download_text(const char* name, const char* text);
void photon_download_png(const char* name, int w, int h, const uint8_t* rgba);
void photon_pick_json();
}
#endif

extern "C" {

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void photon_on_view_import(const char* text) {
    if (text == nullptr) {
        return;
    }
    photon::ui::gPendingImport = text;
    photon::ui::gHaveImport = true;
}

}  // extern "C"

namespace photon::ui {

void flushPlotImageExport() {
    if (!gPlotPng.ready) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float sx = io.DisplayFramebufferScale.x > 0.0f ? io.DisplayFramebufferScale.x : 1.0f;
    const float sy = io.DisplayFramebufferScale.y > 0.0f ? io.DisplayFramebufferScale.y : 1.0f;
    const int fbW = std::max(1, static_cast<int>(io.DisplaySize.x * sx));
    const int fbH = std::max(1, static_cast<int>(io.DisplaySize.y * sy));
    int px = static_cast<int>(std::floor(gPlotPng.x * sx));
    int pyTop = static_cast<int>(std::floor(gPlotPng.y * sy));
    int pw = static_cast<int>(std::ceil(gPlotPng.w * sx));
    int ph = static_cast<int>(std::ceil(gPlotPng.h * sy));
    if (px < 0) {
        pw += px;
        px = 0;
    }
    if (pyTop < 0) {
        ph += pyTop;
        pyTop = 0;
    }
    if (px + pw > fbW) {
        pw = fbW - px;
    }
    if (pyTop + ph > fbH) {
        ph = fbH - pyTop;
    }
    gPlotPng.ready = false;
    if (pw < 8 || ph < 8) {
        return;
    }
    const int py = fbH - (pyTop + ph);
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph) * 4u);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(px, py, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    const std::size_t rowBytes = static_cast<std::size_t>(pw) * 4u;
    std::vector<std::uint8_t> tmp(rowBytes);
    for (int row = 0; row < ph / 2; ++row) {
        std::uint8_t* a = rgba.data() + static_cast<std::size_t>(row) * rowBytes;
        std::uint8_t* b = rgba.data() + static_cast<std::size_t>(ph - 1 - row) * rowBytes;
        std::memcpy(tmp.data(), a, rowBytes);
        std::memcpy(a, b, rowBytes);
        std::memcpy(b, tmp.data(), rowBytes);
    }
    photon_download_png(gPlotPng.name.c_str(), pw, ph, rgba.data());
}

std::string serializeCustomViews(const CustomViewDocument& doc) {
    json::Array panels;
    for (const CustomView& view : doc.views) {
        panels.push_back(viewToJson(view));
    }
    json::Object root;
    root.emplace("$schema", json::Value("../docs/config/photon-view.schema.json"));
    root.emplace("schemaVersion", json::Value(1));
    root.emplace("id", json::Value(doc.id.empty() ? "custom-views" : doc.id));
    root.emplace("name", json::Value(doc.name.empty() ? "Custom Views" : doc.name));
    root.emplace("activePanel", json::Value(doc.active));
    root.emplace("panels", json::Value(std::move(panels)));
    return json::stringify(json::Value(std::move(root)));
}

bool isLatGSource(const ViewSignal& src) {
    return src.signalName == "Accel_Y" || src.label == "Lat G";
}

bool isLongGSource(const ViewSignal& src) {
    return src.signalName == "Accel_X" || src.label == "Long G";
}

void migrateGgAxes(ViewCell& cell) {
    if (cell.kind != ViewCellKind::Scatter || cell.sources.size() < 2) {
        return;
    }
    if (isLatGSource(cell.sources[0]) && isLongGSource(cell.sources[1])) {
        std::swap(cell.sources[0], cell.sources[1]);
    }
}

void applyGgImuSources(ViewCell& cell, const GgImu& imu) {
    if (cell.sources.size() < 2) {
        cell.sources.resize(2);
    }
    ViewSignal& lon = cell.sources[0];
    lon.messageName = imu.message;
    lon.signalName = imu.axName;
    lon.label = "Long G";
    lon.id = 0;
    ViewSignal& lat = cell.sources[1];
    lat.messageName = imu.message;
    lat.signalName = imu.ayName;
    lat.label = "Lat G";
    lat.id = 0;
}

int ggImuIndexForCell(const ViewCell& cell) {
    if (cell.sources.empty()) {
        return 0;
    }
    const int fromMsg = ggImuIndexFromMessage(cell.sources[0].messageName);
    return fromMsg >= 0 ? fromMsg : 0;
}

bool cellLooksLikeGgImu(const ViewCell& cell) {
    if (cell.kind != ViewCellKind::Scatter) {
        return false;
    }
    if (cell.sources.size() < 2) {
        return cell.title == "G-G";
    }
    return ggImuIndexFromMessage(cell.sources[0].messageName) >= 0 && isLongGSource(cell.sources[0]) &&
           isLatGSource(cell.sources[1]);
}

bool parseCustomViews(std::string_view text, CustomViewDocument* out, std::string* error) {
    json::ParseError perr;
    const std::optional<json::Value> parsed = json::parse(text, &perr);
    if (!parsed) {
        if (error != nullptr) {
            *error = perr.message;
        }
        return false;
    }
    const json::Value& root = *parsed;
    if (!root.isObject()) {
        if (error != nullptr) {
            *error = "view JSON must be an object";
        }
        return false;
    }
    const int version = static_cast<int>(root["schemaVersion"].asInt(1));
    if (version != 1) {
        if (error != nullptr) {
            *error = "unsupported schemaVersion; expected 1";
        }
        return false;
    }
    CustomViewDocument doc;
    doc.schemaVersion = 1;
    doc.id = root["id"].asString("custom-views");
    doc.name = root["name"].asString("Custom Views");
    int nextCell = 1;
    if (root["panels"].isArray() && !root["panels"].asArray().empty()) {
        for (const json::Value& panel : root["panels"].asArray()) {
            if (doc.views.size() >= static_cast<std::size_t>(kMaxViews)) {
                break;
            }
            if (!panel.isObject()) {
                continue;
            }
            doc.views.push_back(parsePanel(panel, &nextCell));
        }
    } else {
        doc.views.push_back(parsePanel(root, &nextCell));
    }
    if (doc.views.empty()) {
        if (error != nullptr) {
            *error = "no views in document";
        }
        return false;
    }
    doc.active = clampi(static_cast<int>(root["activePanel"].asInt(0)), 0,
                        static_cast<int>(doc.views.size()) - 1);
    for (CustomView& view : doc.views) {
        for (ViewCell& cell : view.cells) {
            migrateGgAxes(cell);
        }
    }
    *out = std::move(doc);
    return true;
}

CustomView& CustomViews::active() { return doc_.views[static_cast<std::size_t>(doc_.active)]; }

const CustomView& CustomViews::active() const {
    return doc_.views[static_cast<std::size_t>(doc_.active)];
}

void CustomViews::pushLayoutUndo() {
    if (doc_.views.empty()) {
        undo_ = {};
        return;
    }
    undo_.have = true;
    undo_.viewIndex = doc_.active;
    undo_.cells = active().cells;
    undo_.selectedCell = selectedCell_;
    undo_.nextCell = nextCell_;
}

void CustomViews::undoLastLayout() {
    if (!undo_.have || undo_.viewIndex < 0 ||
        undo_.viewIndex >= static_cast<int>(doc_.views.size())) {
        undo_ = {};
        status_ = "Nothing to undo";
        return;
    }
    doc_.active = undo_.viewIndex;
    active().cells = undo_.cells;
    selectedCell_ = undo_.selectedCell;
    if (selectedCell_ >= static_cast<int>(active().cells.size())) {
        selectedCell_ = active().cells.empty() ? -1 : 0;
    }
    nextCell_ = undo_.nextCell;
    undo_ = {};
    markDirty();
    status_ = "Undid last layout change";
}

void CustomViews::migrateView(CustomView& view) {
    for (ViewCell& cell : view.cells) {
        migrateGgAxes(cell);
    }
    const int old = std::max(1, view.columns);
    if (old != kGridCols) {
        for (ViewCell& cell : view.cells) {
            cell.rect.x = cell.rect.x * kGridCols / old;
            cell.rect.w = std::max(1, cell.rect.w * kGridCols / old);
            if (cell.rect.x + cell.rect.w > kGridCols) {
                cell.rect.w = std::max(1, kGridCols - cell.rect.x);
            }
        }
    }
    view.columns = kGridCols;
    view.rowHeight = kGridRowH;
}

ViewCell* CustomViews::targetCell() {
    if (doc_.views.empty()) {
        return nullptr;
    }
    CustomView& view = active();
    if (view.cells.empty()) {
        addCell(ViewCellKind::Line);
    }
    if (selectedCell_ < 0 || selectedCell_ >= static_cast<int>(view.cells.size())) {
        selectedCell_ = 0;
    }
    return &view.cells[static_cast<std::size_t>(selectedCell_)];
}

void CustomViews::assignSignal(const ctrl::CatalogSignal& sig) {
    ViewCell* cell = targetCell();
    if (cell == nullptr) {
        return;
    }
    for (auto it = cell->sources.begin(); it != cell->sources.end(); ++it) {
        if (sameSignal(*it, sig)) {
            cell->sources.erase(it);
            markDirty();
            return;
        }
    }
    const int limit = cell->kind == ViewCellKind::Scatter ? 2 : kMaxSources;
    if (static_cast<int>(cell->sources.size()) >= limit) {
        status_ = cell->kind == ViewCellKind::Scatter ? "G-G takes two signals (X then Y)"
                                                     : "Cell is at the 16-signal limit";
        return;
    }
    ViewSignal src;
    src.id = sig.id;
    src.messageName = sig.message;
    src.signalName = sig.name;
    src.label = sig.message + "." + sig.name;
    cell->sources.push_back(std::move(src));
    if (cell->title == "Plot" || cell->title.empty()) {
        cell->title = sig.name;
    }
    markDirty();
}

void CustomViews::updateDrag(ImVec2 origin, float unitW, float unitH) {
    if (active().locked) {
        drag_ = DragKind::None;
        dragIndex_ = -1;
        dragDirty_ = false;
        if (dragUndoPushed_) {
            undo_ = dragUndoPrev_;
            dragUndoPushed_ = false;
            dragUndoPrev_ = {};
        }
        return;
    }
    if (drag_ == DragKind::None || dragIndex_ < 0 ||
        dragIndex_ >= static_cast<int>(active().cells.size())) {
        return;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        drag_ = DragKind::None;
        dragIndex_ = -1;
        if (dragDirty_) {
            markDirty();
            dragDirty_ = false;
        } else if (dragUndoPushed_) {
            undo_ = dragUndoPrev_;
        }
        dragUndoPushed_ = false;
        dragUndoPrev_ = {};
        return;
    }
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const int dx = static_cast<int>(std::floor((mouse.x - dragMouse0_.x) / std::max(1.0f, unitW)));
    const int dy = static_cast<int>(std::floor((mouse.y - dragMouse0_.y) / std::max(1.0f, unitH)));
    ViewRect next = dragStart_;
    if (drag_ == DragKind::Move) {
        next.x = dragStart_.x + dx;
        next.y = dragStart_.y + dy;
        next.x = std::max(0, next.x);
        next.y = std::max(0, next.y);
        if (next.x + next.w > kGridCols) {
            next.x = kGridCols - next.w;
        }
    } else {
        next.w = std::max(1, dragStart_.w + dx);
        next.h = std::max(1, dragStart_.h + dy);
        if (next.x + next.w > kGridCols) {
            next.w = kGridCols - next.x;
        }
    }
    (void)origin;
    CustomView& view = active();
    if (!occupied(view, next.x, next.y, next.w, next.h, dragIndex_)) {
        ViewCell& cell = view.cells[static_cast<std::size_t>(dragIndex_)];
        if (cell.rect.x != next.x || cell.rect.y != next.y || cell.rect.w != next.w ||
            cell.rect.h != next.h) {
            cell.rect = next;
            dragDirty_ = true;
        }
    }
}

CustomView CustomViews::makePitVitalsView() const {
    CustomView view;
    view.id = "view-pit-vitals";
    view.name = "Pit vitals";
    view.columns = kGridCols;
    view.rowHeight = kGridRowH;
    view.gap = theme::kGridGap;
    view.cells.push_back(lineCell("plot-pack-v", "Pack V", ViewRect{0, 0, 6, 2},
                                  namedSource("BPS_Status", "Main_Battery_Voltage", "Pack V"), 30.0f));
    view.cells.push_back(lineCell("plot-pack-i", "Pack I", ViewRect{6, 0, 6, 2},
                                  namedSource("MC_BusMeasurement", "MC_BusCurrent", "Pack I"), 30.0f));
    view.cells.push_back(lineCell("plot-speed", "Speed", ViewRect{0, 2, 6, 2},
                                  namedSource("MC_VelocityMeasurement", "MC_VehicleVelocity", "Speed"),
                                  30.0f));
    view.cells.push_back(lineCell("plot-motor-t", "Motor T", ViewRect{6, 2, 6, 2},
                                  namedSource("MC_Motor_TempMeasurement", "MC_MotorTemp", "Motor T"),
                                  30.0f));
    return view;
}

void CustomViews::addPitVitalsView() {
    for (int i = 0; i < static_cast<int>(doc_.views.size()); ++i) {
        if (doc_.views[static_cast<std::size_t>(i)].id == "view-pit-vitals") {
            doc_.active = i;
            selectedCell_ = doc_.views[static_cast<std::size_t>(i)].cells.empty() ? -1 : 0;
            markDirty();
            return;
        }
    }
    if (static_cast<int>(doc_.views.size()) >= kMaxViews) {
        status_ = "At the 32-view limit";
        return;
    }
    doc_.views.push_back(makePitVitalsView());
    doc_.active = static_cast<int>(doc_.views.size()) - 1;
    selectedCell_ = 0;
    nextCell_ = std::max(nextCell_, 6);
    markDirty();
}

void CustomViews::ensureDefault() {
    if (!doc_.views.empty()) {
        for (CustomView& view : doc_.views) {
            migrateView(view);
        }
        return;
    }
    doc_.views.push_back(makePitVitalsView());
    doc_.active = 0;
    selectedCell_ = 0;
    nextView_ = 2;
    nextCell_ = 6;
}

void CustomViews::ensureLoaded() {
    if (loaded_) {
        return;
    }
    loaded_ = true;
    char* raw = photon_ls_get(kStorageKey);
    if (raw != nullptr) {
        std::string err;
        if (applyJson(raw, &err)) {
            const bool emptyStarter = doc_.views.size() == 1 && doc_.views[0].cells.size() == 1 &&
                                      doc_.views[0].cells[0].sources.empty() &&
                                      (doc_.views[0].cells[0].title == "Plot" ||
                                       doc_.views[0].cells[0].title.empty());
            if (emptyStarter) {
                doc_.views.clear();
                ensureDefault();
                save();
                status_ = "Pit vitals starter";
            } else {
                status_ = "Loaded views from localStorage";
            }
            std::free(raw);
            return;
        }
        status_ = "localStorage view JSON ignored: " + err;
        std::free(raw);
    }
    ensureDefault();
}

void CustomViews::markDirty() {
    save();
    dirty_ = false;
}

void CustomViews::touchDirty() { dirty_ = true; }

bool CustomViews::anyScrubHold() const {
    if (doc_.views.empty()) {
        return false;
    }
    for (const ViewCell& cell : active().cells) {
        if (cell.scrubHold) {
            return true;
        }
    }
    return false;
}

void CustomViews::flushDirty() {
    if (!dirty_) {
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    if (std::fabs(io.MouseWheel) > 0.01f || std::fabs(io.MouseWheelH) > 0.01f) {
        return;
    }
    save();
    dirty_ = false;
}

void CustomViews::save() const {
    const std::string json = serializeCustomViews(doc_);
    photon_ls_set(kStorageKey, json.c_str());
}

void CustomViews::exportFile() const {
    const std::string json = serializeCustomViews(doc_);
    photon_download_text("custom.photon-view.json", json.c_str());
}

void CustomViews::requestImport() { photon_pick_json(); }

void CustomViews::pollImport() {
    if (!gHaveImport) {
        return;
    }
    gHaveImport = false;
    std::string err;
    if (applyJson(gPendingImport, &err)) {
        status_ = "Imported view JSON";
        save();
    } else {
        status_ = "Import failed: " + err;
    }
    gPendingImport.clear();
}

bool CustomViews::applyJson(std::string_view text, std::string* error) {
    CustomViewDocument next;
    if (!parseCustomViews(text, &next, error)) {
        return false;
    }
    undo_ = {};
    doc_ = std::move(next);
    selectedCell_ = doc_.views.empty() || doc_.views[static_cast<std::size_t>(doc_.active)].cells.empty()
                        ? -1
                        : 0;
    nextView_ = static_cast<int>(doc_.views.size()) + 1;
    nextCell_ = 1;
    for (const CustomView& view : doc_.views) {
        for (const ViewCell& cell : view.cells) {
            if (cell.id.rfind("cell-", 0) == 0 || cell.id.rfind("plot-", 0) == 0) {
                const std::string tail = cell.id.substr(cell.id.find('-') + 1);
                nextCell_ = std::max(nextCell_, std::atoi(tail.c_str()) + 1);
            }
        }
    }
    ensureDefault();
    return true;
}

void CustomViews::addView() {
    CustomView view;
    view.id = "view-" + std::to_string(nextView_);
    view.name = "View " + std::to_string(nextView_);
    ++nextView_;
    migrateView(view);
    ViewCell cell;
    cell.id = "plot-" + std::to_string(nextCell_++);
    cell.title = "Plot";
    cell.rect = ViewRect{0, 0, kDefaultCellW, kDefaultCellH};
    view.cells.push_back(std::move(cell));
    doc_.views.push_back(std::move(view));
    doc_.active = static_cast<int>(doc_.views.size()) - 1;
    selectedCell_ = 0;
    markDirty();
}

void CustomViews::deleteActiveView() {
    if (doc_.views.size() <= 1) {
        status_ = "Keep at least one view";
        return;
    }
    if (undo_.viewIndex == doc_.active) {
        undo_ = {};
    } else if (undo_.viewIndex > doc_.active) {
        --undo_.viewIndex;
    }
    doc_.views.erase(doc_.views.begin() + doc_.active);
    if (doc_.active >= static_cast<int>(doc_.views.size())) {
        doc_.active = static_cast<int>(doc_.views.size()) - 1;
    }
    selectedCell_ = -1;
    markDirty();
}

void CustomViews::addCell(ViewCellKind kind) {
    CustomView& view = active();
    if (static_cast<int>(view.cells.size()) >= kMaxCells) {
        status_ = "View is at the 128-cell limit";
        return;
    }
    pushLayoutUndo();
    ViewCell cell;
    cell.id = "plot-" + std::to_string(nextCell_++);
    cell.kind = kind;
    cell.title = kind == ViewCellKind::Value ? "Value" : (kind == ViewCellKind::Scatter ? "G-G" : "Plot");
    cell.rect = nextSlot(view, kDefaultCellW, kDefaultCellH);
    view.cells.push_back(std::move(cell));
    selectedCell_ = static_cast<int>(view.cells.size()) - 1;
    markDirty();
}

void CustomViews::addGgCell() {
    addCell(ViewCellKind::Scatter);
    ViewCell& cell = active().cells.back();
    cell.title = "G-G";
    ViewSignal lon;
    lon.messageName = "IMU_B_Accel";
    lon.signalName = "Accel_X";
    lon.label = "Long G";
    ViewSignal lat;
    lat.messageName = "IMU_B_Accel";
    lat.signalName = "Accel_Y";
    lat.label = "Lat G";
    cell.sources.push_back(std::move(lon));
    cell.sources.push_back(std::move(lat));
}

void CustomViews::resolve(const ClientArena& arena) {
    if (!arena.haveSignalCatalog) {
        return;
    }
    for (CustomView& view : doc_.views) {
        for (ViewCell& cell : view.cells) {
            for (ViewSignal& src : cell.sources) {
                for (const ctrl::CatalogSignal& cat : arena.signalCatalog) {
                    if (sameSignal(src, cat)) {
                        src.id = cat.id;
                        if (src.messageName.empty()) {
                            src.messageName = cat.message;
                        }
                        if (src.signalName.empty()) {
                            src.signalName = cat.name;
                        }
                        break;
                    }
                }
            }
        }
    }
}

void CustomViews::subscribe(UiContext& ui) {
    std::vector<std::uint32_t> ids;
    for (const ViewCell& cell : active().cells) {
        for (const ViewSignal& src : cell.sources) {
            if (src.id != 0) {
                ids.push_back(src.id);
            }
        }
    }
    if (!ids.empty()) {
        ui.client.ensureSubscribed(ids);
    }
}

void CustomViews::openPicker() {
    pickerOpen_ = true;
    pickerDayIndex_ = -1;
    if (selectedCell_ < 0 && !active().cells.empty()) {
        selectedCell_ = 0;
    }
}

void CustomViews::syncPickerDay(const ClientArena& arena) {
    pickerDayIndex_ = currentDriveDayIndex(arena);
}

bool CustomViews::drawDriveDayCombo(UiContext& ui, const char* id, float width) {
    if (ui.arena.driveDays.empty()) {
        ImGui::TextColored(theme::mutedFg(), "No drive days");
        return false;
    }
    if (pickerDayIndex_ < 0 || pickerDayIndex_ >= static_cast<int>(ui.arena.driveDays.size())) {
        syncPickerDay(ui.arena);
    }
    const auto& days = ui.arena.driveDays;
    const char* preview = "Drive day";
    if (pickerDayIndex_ >= 0 && pickerDayIndex_ < static_cast<int>(days.size())) {
        preview = days[static_cast<std::size_t>(pickerDayIndex_)].date.c_str();
    }
    ImGui::SetNextItemWidth(width);
    bool changed = false;
    if (ImGui::BeginCombo(id, preview)) {
        for (int i = 0; i < static_cast<int>(days.size()); ++i) {
            const auto& day = days[static_cast<std::size_t>(i)];
            const bool sel = i == pickerDayIndex_;
            if (ImGui::Selectable(day.date.c_str(), sel)) {
                pickerDayIndex_ = i;
                ui.client.playDate(day.date);
                status_ = "Drive day " + day.date;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool CustomViews::cellHasSamples(const ClientArena& arena, int index) const {
    if (index < 0 || index >= static_cast<int>(active().cells.size())) {
        return false;
    }
    const ViewCell& cell = active().cells[static_cast<std::size_t>(index)];
    for (const ViewSignal& src : cell.sources) {
        const SignalRing* ring = src.id != 0 ? arena.ring(src.id) : nullptr;
        if (ring != nullptr && ring->count > 8) {
            return true;
        }
    }
    return false;
}

void CustomViews::requestPlotPng(UiContext& ui) {
    syncPickerDay(ui.arena);
    if (selectedCell_ < 0 && !active().cells.empty()) {
        selectedCell_ = 0;
    }
    std::string date = "live";
    std::string path;
    if (pickerDayIndex_ >= 0 && pickerDayIndex_ < static_cast<int>(ui.arena.driveDays.size())) {
        const auto& day = ui.arena.driveDays[static_cast<std::size_t>(pickerDayIndex_)];
        date = day.date.empty() ? date : day.date;
        path = day.path;
    } else if (!ui.arena.sourcePath.empty()) {
        date = sanitizeFileToken(ui.arena.sourcePath);
    }
    std::string title = "plot";
    if (selectedCell_ >= 0 && selectedCell_ < static_cast<int>(active().cells.size())) {
        const ViewCell& cell = active().cells[static_cast<std::size_t>(selectedCell_)];
        title = cell.title.empty() ? cell.id : cell.title;
    }
    export_.active = true;
    export_.cell = selectedCell_;
    export_.date = date;
    export_.path = path;
    export_.filename = "photon-" + sanitizeFileToken(date) + "-" + sanitizeFileToken(title) + ".png";
    export_.startMs = ImGui::GetTime() * 1000.0;
    export_.readySinceMs = 0.0;
    export_.settleFrames = 0;
    pickerOpen_ = false;
    const bool dayReady =
        path.empty() ||
        (ui.arena.source == ctrl::SourceKind::LogReplay && pathMentionsDay(ui.arena.sourcePath, [&] {
             ctrl::Message::DriveDay d;
             d.date = date;
             d.path = path;
             return d;
         }()));
    if (!dayReady && !path.empty()) {
        ui.client.selectSource(ctrl::SourceKind::LogReplay, "", 0, path);
        export_.waitDay = true;
        status_ = "Loading " + date + " for export…";
        return;
    }
    const bool buffering =
        ui.arena.timeline().mode == static_cast<std::uint8_t>(proto::TimelineMode::Buffering);
    if (buffering) {
        export_.waitDay = true;
        status_ = "Waiting for " + date + "…";
        return;
    }
    export_.waitDay = false;
    export_.settleFrames = 2;
    status_ = "Exporting " + export_.filename;
}

void CustomViews::tickPlotExport(UiContext& ui) {
    if (!export_.active) {
        return;
    }
    const double now = ImGui::GetTime() * 1000.0;
    if (export_.waitDay) {
        ctrl::Message::DriveDay want;
        want.date = export_.date;
        want.path = export_.path;
        const bool dayReady =
            export_.path.empty() ||
            (ui.arena.source == ctrl::SourceKind::LogReplay && pathMentionsDay(ui.arena.sourcePath, want));
        const bool buffering =
            ui.arena.timeline().mode == static_cast<std::uint8_t>(proto::TimelineMode::Buffering);
        if (dayReady && !buffering) {
            if (export_.readySinceMs <= 0.0) {
                export_.readySinceMs = now;
            }
            if (cellHasSamples(ui.arena, export_.cell) || now - export_.readySinceMs > 1200.0) {
                export_.waitDay = false;
                export_.settleFrames = 2;
                status_ = "Exporting " + export_.filename;
            } else {
                status_ = "Buffering " + export_.date + "…";
            }
        } else if (now - export_.startMs > 20000.0) {
            status_ = "Export timed out waiting for " + export_.date;
            export_ = {};
        } else {
            status_ = "Loading " + export_.date + "…";
        }
        return;
    }
    if (export_.settleFrames > 0) {
        --export_.settleFrames;
        pickerOpen_ = false;
        if (export_.settleFrames == 0) {
            status_ = "Exporting " + export_.filename;
        }
    }
}

void CustomViews::drawToolbar(UiContext& ui) {
    CustomView& view = active();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(theme::kGridGap, 0.0f));
    bool havePitVitals = false;
    for (int i = 0; i < static_cast<int>(doc_.views.size()); ++i) {
        const CustomView& tab = doc_.views[static_cast<std::size_t>(i)];
        if (tab.id == "view-pit-vitals") {
            havePitVitals = true;
        }
        if (i > 0) {
            ImGui::SameLine();
        }
        const char* tabName = tab.name.empty() ? "View" : tab.name.c_str();
        char tabId[96];
        std::snprintf(tabId, sizeof(tabId), "%s/%d", tab.id.empty() ? "view" : tab.id.c_str(), i);
        if (shellButton(tabName, i == doc_.active, tabId)) {
            doc_.active = i;
            selectedCell_ = active().cells.empty() ? -1 : 0;
            markDirty();
        }
    }
    ImGui::SameLine();
    if (shellButton("+ View", false, "add-view")) {
        addView();
    }
    if (!havePitVitals) {
        ImGui::SameLine();
        if (shellButton("Pit vitals", false, "add-pit-vitals")) {
            addPitVitalsView();
        }
    }
    ImGui::SameLine();
    if (shellButton("Add signal", false, "add-signal")) {
        openPicker();
    }
    ImGui::SameLine();
    if (shellButton("Add plot", false, "add-plot")) {
        addCell(ViewCellKind::Line);
        openPicker();
    }
    ImGui::SameLine();
    if (shellButton("Add value", false, "add-value")) {
        addCell(ViewCellKind::Value);
        openPicker();
    }
    ImGui::SameLine();
    if (shellButton("...", false, "view-more")) {
        ImGui::OpenPopup("##view-more");
    }
    ImGui::SameLine();
    if (shellButton(view.locked ? "Unlock" : "Lock", view.locked, "layout-lock")) {
        view.locked = !view.locked;
        drag_ = DragKind::None;
        dragIndex_ = -1;
        dragDirty_ = false;
        markDirty();
        status_ = view.locked ? "Layout locked" : "Layout unlocked";
    }
    if (undo_.have) {
        ImGui::SameLine();
        if (shellButton("Undo", false, "layout-undo")) {
            undoLastLayout();
        }
    }
    float sharedWindow = 15.0f;
    bool haveLine = false;
    for (const ViewCell& cell : view.cells) {
        if (cell.kind == ViewCellKind::Line) {
            sharedWindow = haveLine ? std::min(sharedWindow, cell.timeWindowSec) : cell.timeWindowSec;
            haveLine = true;
        }
    }
    if (haveLine) {
        ImGui::SameLine();
        if (shellButton("-", false, "win-down")) {
            const double cursorT = static_cast<double>(ui.arena.effectiveCursorMs());
            for (ViewCell& cell : view.cells) {
                if (cell.kind != ViewCellKind::Line) {
                    continue;
                }
                if (cell.freeze && cell.viewMaxMs > cell.viewMinMs) {
                    double lo = cell.viewMinMs;
                    double hi = cell.viewMaxMs;
                    zoomTimeWindowAround(plotZoomAnchorMs(cursorT, lo, hi), lo, hi, 0.5);
                    if (!sanitizePlotWindow(lo, hi)) {
                        continue;
                    }
                    cell.timeWindowSec = static_cast<float>(std::max(2.0, (hi - lo) / 1000.0));
                    cell.viewMinMs = lo;
                    cell.viewMaxMs = hi;
                } else {
                    cell.timeWindowSec = std::max(2.0f, cell.timeWindowSec * 0.5f);
                }
            }
            markDirty();
        }
        ImGui::SameLine();
        char range[32];
        std::snprintf(range, sizeof(range), "RANGE %.0f s", sharedWindow);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::mutedFg(), "%s", range);
        ImGui::SameLine();
        if (shellButton("+", false, "win-up")) {
            const double cursorT = static_cast<double>(ui.arena.effectiveCursorMs());
            for (ViewCell& cell : view.cells) {
                if (cell.kind != ViewCellKind::Line) {
                    continue;
                }
                if (cell.freeze && cell.viewMaxMs > cell.viewMinMs) {
                    double lo = cell.viewMinMs;
                    double hi = cell.viewMaxMs;
                    zoomTimeWindowAround(plotZoomAnchorMs(cursorT, lo, hi), lo, hi, 2.0);
                    if (!sanitizePlotWindow(lo, hi)) {
                        continue;
                    }
                    cell.timeWindowSec = static_cast<float>(std::max(2.0, (hi - lo) / 1000.0));
                    cell.viewMinMs = lo;
                    cell.viewMaxMs = hi;
                } else {
                    cell.timeWindowSec = std::min(7200.0f, cell.timeWindowSec * 2.0f);
                }
            }
            markDirty();
        }
    }
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    if (ImGui::BeginPopup("##view-more")) {
        if (ImGui::MenuItem("Add G-G")) {
            addGgCell();
        }
        if (ImGui::MenuItem("Rename")) {
            std::snprintf(renameBuf_, sizeof(renameBuf_), "%s", view.name.c_str());
            renameOpen_ = true;
        }
        if (ImGui::MenuItem("Delete view")) {
            deleteActiveView();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(view.locked ? "Unlock layout" : "Lock layout")) {
            view.locked = !view.locked;
            drag_ = DragKind::None;
            dragIndex_ = -1;
            dragDirty_ = false;
            markDirty();
        }
        if (ImGui::MenuItem("Undo last layout", "Ctrl+Z", false, undo_.have)) {
            undoLastLayout();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Export plot PNG")) {
            requestPlotPng(ui);
        }
        if (ImGui::MenuItem("Export")) {
            exportFile();
            status_ = "Exported custom.photon-view.json";
        }
        if (ImGui::MenuItem("Import")) {
            requestImport();
        }
        ImGui::EndPopup();
    }

    if (renameOpen_) {
        ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_Appearing);
        if (ImGui::Begin("Rename view", &renameOpen_)) {
            ImGui::InputText("Name", renameBuf_, sizeof(renameBuf_));
            if (ImGui::Button("OK")) {
                if (renameBuf_[0] != '\0') {
                    active().name = renameBuf_;
                    markDirty();
                }
                renameOpen_ = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                renameOpen_ = false;
            }
        }
        ImGui::End();
    }
    if (!status_.empty()) {
        ImGui::TextColored(theme::mutedFg(), "%s", status_.c_str());
    }
}

void CustomViews::drawPicker(UiContext& ui) {
    if (pickerOpen_ && !ImGui::IsPopupOpen("Add signal")) {
        ImGui::OpenPopup("Add signal");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 center = vp != nullptr ? vp->GetCenter() : ImGui::GetCursorScreenPos();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760.0f, 520.0f), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, theme::kContentWindowPadding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme::kPopupRounding);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::card());
    ImGui::PushStyleColor(ImGuiCol_Border, theme::border());
    if (!ImGui::BeginPopupModal("Add signal", &pickerOpen_,
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        return;
    }

    ViewCell* cell = nullptr;
    if (selectedCell_ >= 0 && selectedCell_ < static_cast<int>(active().cells.size())) {
        cell = &active().cells[static_cast<std::size_t>(selectedCell_)];
    }

    const float side = 240.0f;
    const float gap = theme::kGridGap;
    ImGui::BeginChild("##picker-cell", ImVec2(side, -36.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    if (cell == nullptr) {
        ImGui::TextColored(theme::mutedFg(), "Select a plot first.");
        ImGui::Separator();
        ImGui::TextColored(theme::mutedFg(), "Drive day");
        drawDriveDayCombo(ui, "##plot-day", -1.0f);
        if (ImGui::Button("Export PNG", ImVec2(-1.0f, 0.0f))) {
            requestPlotPng(ui);
            ImGui::CloseCurrentPopup();
        }
    } else {
        ImGui::TextColored(theme::mutedFg(), "Plot");
        char title[80];
        std::snprintf(title, sizeof(title), "%s", cell->title.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##title", title, sizeof(title))) {
            cell->title = title;
            markDirty();
        }
        if (ImGui::RadioButton("Line", cell->kind == ViewCellKind::Line)) {
            cell->kind = ViewCellKind::Line;
            markDirty();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Value", cell->kind == ViewCellKind::Value)) {
            cell->kind = ViewCellKind::Value;
            markDirty();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("G-G", cell->kind == ViewCellKind::Scatter)) {
            cell->kind = ViewCellKind::Scatter;
            markDirty();
        }
        if (cell->kind == ViewCellKind::Scatter) {
            theme::pushSmallFont();
            ImGui::TextColored(theme::mutedFg(), "Assign X then Y.");
            theme::popSmallFont();
            int imuIndex = ggImuIndexForCell(*cell);
            if (drawGgImuCombo("##picker-gg-imu", &imuIndex, -1.0f)) {
                applyGgImuSources(*cell, kGgImus[imuIndex]);
                markDirty();
            }
        }
        if (ImGui::SliderFloat("Window", &cell->timeWindowSec, 2.0f, 7200.0f, "%.0f s")) {
            markDirty();
        }
        if (ImGui::Checkbox("Legend", &cell->legend)) {
            markDirty();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Freeze", &cell->freeze);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Hold this time window. Click or drag a plot to scrub;\n"
                              "Play / Live follows the cursor again unless Freeze is on.");
        }
        if (cell->kind != ViewCellKind::Value) {
            bool autoY = !cell->yLocked;
            if (ImGui::Checkbox("Auto Y", &autoY)) {
                cell->yLocked = !autoY;
                markDirty();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Fit Y to the visible window. Uncheck to drag the axis.\n"
                    "Double-click the plot to restore Auto Y.");
            }
        }
        if (cell->kind == ViewCellKind::Line) {
            if (ImGui::Checkbox("Dual Y", &cell->dualY)) {
                markDirty();
            }
        }
        ImGui::Separator();
        ImGui::TextColored(theme::mutedFg(), "Drive day");
        drawDriveDayCombo(ui, "##plot-day", -1.0f);
        if (ImGui::Button("Export PNG", ImVec2(-1.0f, 0.0f))) {
            requestPlotPng(ui);
            ImGui::CloseCurrentPopup();
        }
        theme::pushSmallFont();
        ImGui::TextColored(theme::mutedFg(),
                           "Export captures this plot. Change the day first to export that date.");
        theme::popSmallFont();
        theme::pushSmallFont();
        ImGui::TextColored(theme::mutedFg(), "Shift-drag on a line plot to pick A→B.");
        if (cell->rangeActive) {
            ImGui::Text("Range  %.0f → %.0f ms", cell->rangeA, cell->rangeB);
            if (ImGui::SmallButton("Clear range")) {
                cell->rangeActive = false;
                cell->rangeDragging = false;
            }
        }
        theme::popSmallFont();
        ImGui::Separator();
        ImGui::TextColored(theme::mutedFg(), "Assigned");
        for (int i = 0; i < static_cast<int>(cell->sources.size()); ++i) {
            ImGui::PushID(i);
            ViewSignal& src = cell->sources[static_cast<std::size_t>(i)];
            const std::string label = cellLabel(src);
            ImVec4 col = seriesColor(src, static_cast<std::size_t>(i));
            float rgba[4] = {col.x, col.y, col.z, col.w};
            if (ImGui::ColorEdit4("##c", rgba,
                                 ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
                                     ImGuiColorEditFlags_AlphaBar)) {
                src.color = formatHexColor(ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]));
                markDirty();
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                src.color.clear();
                markDirty();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(src.color.empty() ? "Theme color: edit to pin, right-click to reset"
                                                    : "Pinned color: right-click for theme");
            }
            if (cell->kind != ViewCellKind::Value) {
                ImGui::SameLine();
                float width = seriesWidth(src);
                ImGui::SetNextItemWidth(56.0f);
                if (ImGui::SliderFloat("##w", &width, kMinSeriesWidth, kMaxSeriesWidth, "%.1f")) {
                    src.width = width;
                    markDirty();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(cell->kind == ViewCellKind::Scatter ? "Marker size"
                                                                         : "Line width");
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                cell->sources.erase(cell->sources.begin() + i);
                markDirty();
                ImGui::PopID();
                break;
            }
            ImGui::TextUnformatted(label.c_str());
            if (ImGui::Checkbox("Hide", &src.hidden)) {
                markDirty();
            }
            if (cell->kind == ViewCellKind::Line && cell->dualY) {
                ImGui::SameLine();
                bool y2 = src.yAxis == 2;
                if (ImGui::Checkbox("Y2", &y2)) {
                    src.yAxis = y2 ? 2 : 1;
                    markDirty();
                }
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("Remove plot") && active().cells.size() > 1) {
            pushLayoutUndo();
            active().cells.erase(active().cells.begin() + selectedCell_);
            selectedCell_ = -1;
            cell = nullptr;
            markDirty();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine(0.0f, gap);

    ImGui::BeginChild("##picker-catalog", ImVec2(0.0f, -36.0f),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::Text("Signals");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search message.signal", search_, sizeof(search_));
    theme::pushSmallFont();
    ImGui::TextWrapped("Click a signal to add or remove it on the selected plot.");
    theme::popSmallFont();
    ImGui::Separator();
    if (!ui.arena.haveSignalCatalog) {
        ImGui::TextWrapped("Waiting for signal_catalog from the backend.");
    } else {
        std::map<std::string, std::vector<const ctrl::CatalogSignal*>> groups;
        for (const ctrl::CatalogSignal& sig : ui.arena.signalCatalog) {
            groups[sig.message].push_back(&sig);
        }
        const std::string_view filter = search_;
        ImGui::BeginChild("##catalog-tree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
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
                bool assigned = false;
                if (cell != nullptr) {
                    for (const ViewSignal& src : cell->sources) {
                        if (sameSignal(src, *sig)) {
                            assigned = true;
                            break;
                        }
                    }
                }
                const std::string row = sig->name + (sig->unit.empty() ? std::string() : "  " + sig->unit);
                if (ImGui::Selectable(row.c_str(), assigned)) {
                    assignSignal(*sig);
                }
            }
            ImGui::TreePop();
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    if (ImGui::Button("Done", ImVec2(120.0f, 0.0f))) {
        pickerOpen_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void CustomViews::drawCell(UiContext& ui, ViewCell& cell, ImVec2 size, int index) {
    const bool selected = index == selectedCell_;
    ImGui::PushID(index);
    ImGui::PushStyleColor(ImGuiCol_Border, selected ? theme::primary() : theme::withAlpha(theme::border(), 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    ImGui::BeginChild("cell", size, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        selectedCell_ = index;
    }

    const ImVec2 headerMin = ImGui::GetCursorScreenPos();
    if (ImGui::IsWindowHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        ImGui::GetIO().MousePos.y <= headerMin.y + kCellHeaderH) {
        selectedCell_ = index;
        openPicker();
    }
    const float imuComboW = cellLooksLikeGgImu(cell) ? 96.0f : 0.0f;
    ImGui::InvisibleButton(
        "##move", ImVec2(std::max(8.0f, size.x - kResizeHandle - 8.0f - imuComboW), kCellHeaderH));
    if (!active().locked && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
        drag_ == DragKind::None) {
        dragUndoPrev_ = undo_;
        pushLayoutUndo();
        dragUndoPushed_ = true;
        drag_ = DragKind::Move;
        dragIndex_ = index;
        dragStart_ = cell.rect;
        dragMouse0_ = ImGui::GetIO().MousePos;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    std::string title = cell.title.empty() ? cell.id : cell.title;
    if (export_.active && !export_.waitDay && export_.cell == index && !export_.date.empty()) {
        title += "  ·  ";
        title += export_.date;
    }
    theme::pushSmallFont();
    const ImVec2 tsz = ImGui::CalcTextSize(title.c_str());
    draw->AddText(ImVec2(headerMin.x + 8.0f, headerMin.y + (kCellHeaderH - tsz.y) * 0.5f),
                  theme::u32(theme::mutedFg()), title.c_str());
    theme::popSmallFont();
    if (imuComboW > 0.0f) {
        const float comboH = ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos(ImVec2(headerMin.x + size.x - imuComboW - 8.0f,
                                         headerMin.y + (kCellHeaderH - comboH) * 0.5f));
        ImGui::PushID(index);
        int imuIndex = ggImuIndexForCell(cell);
        if (drawGgImuCombo("##cell-gg-imu", &imuIndex, imuComboW)) {
            applyGgImuSources(cell, kGgImus[imuIndex]);
            markDirty();
        }
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(headerMin.x, headerMin.y + kCellHeaderH));

    if (cell.kind == ViewCellKind::Value) {
        if (cell.sources.empty()) {
            if (ImGui::Selectable("Add signal")) {
                selectedCell_ = index;
                openPicker();
            }
        } else if (cell.sources.size() == 1) {
            const ViewSignal& src = cell.sources[0];
            if (src.hidden) {
                theme::pushSmallFont();
                ImGui::TextColored(theme::mutedFg(), "%s  hidden", cellLabel(src).c_str());
                theme::popSmallFont();
            } else {
                const bool have = src.id != 0 && ui.arena.has(src.id);
                char buf[32];
                if (have) {
                    std::snprintf(buf, sizeof(buf), "%0.3f", ui.arena.readout(src.id));
                } else {
                    std::snprintf(buf, sizeof(buf), "--");
                }
                const ImVec4 col = have ? seriesColor(src, 0) : theme::mutedFg();
                theme::pushLargeFont();
                const ImVec2 ts = ImGui::CalcTextSize(buf);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                     std::max(0.0f, (ImGui::GetContentRegionAvail().x - ts.x) * 0.5f));
                ImGui::TextColored(col, "%s", buf);
                theme::popLargeFont();
                theme::pushSmallFont();
                ImGui::TextColored(theme::mutedFg(), "%s", cellLabel(src).c_str());
                theme::popSmallFont();
            }
        } else {
            for (std::size_t i = 0; i < cell.sources.size(); ++i) {
                const ViewSignal& src = cell.sources[i];
                if (src.hidden) {
                    ImGui::TextColored(theme::mutedFg(), "%s  hidden", cellLabel(src).c_str());
                    continue;
                }
                const bool have = src.id != 0 && ui.arena.has(src.id);
                if (have) {
                    ImGui::TextColored(seriesColor(src, i), "%s  %0.3f", cellLabel(src).c_str(),
                                       ui.arena.readout(src.id));
                } else {
                    ImGui::TextColored(theme::mutedFg(), "%s  --", cellLabel(src).c_str());
                }
            }
        }
    } else if (cell.kind == ViewCellKind::Scatter) {
        const bool xHidden = !cell.sources.empty() && cell.sources[0].hidden;
        const bool yHidden = cell.sources.size() > 1 && cell.sources[1].hidden;
        const SignalRing* xRing = cell.sources.size() > 0 && cell.sources[0].id != 0 && !xHidden
                                      ? ui.arena.ring(cell.sources[0].id)
                                      : nullptr;
        const SignalRing* yRing = cell.sources.size() > 1 && cell.sources[1].id != 0 && !yHidden
                                      ? ui.arena.ring(cell.sources[1].id)
                                      : nullptr;
        std::vector<double> xs;
        std::vector<double> ys;
        std::vector<double> cloudX;
        std::vector<double> cloudY;
        GgPeaks windowPeaks{};
        if (xRing != nullptr && yRing != nullptr && xRing->count > 0 && yRing->count > 0) {
            pairScatter(*xRing, *yRing, xs, ys);
            findGgPeaks(xs, ys, &windowPeaks);
            binGgCloud(xs, ys, 0.05, cloudX, cloudY);
        }
        static std::unordered_map<std::string, std::pair<std::string, GgPeaks>> heldByCell;
        const std::string imuKey = cell.sources.empty() ? "" : cell.sources[0].messageName;
        const std::string session =
            ggPeakSessionKey(ui.arena.tiles().sessionId(), ui.arena.sourcePath) + "|" + imuKey;
        GgPeaks& heldPeaks = heldByCell[cell.id].second;
        if (heldByCell[cell.id].first != session) {
            heldByCell[cell.id].first = session;
            heldPeaks = {};
        }
        mergeGgPeaks(&heldPeaks, windowPeaks);
        const ImPlotFlags plotFlags =
            (cell.legend ? ImPlotFlags_None : ImPlotFlags_NoLegend) | ImPlotFlags_Equal |
            ImPlotFlags_NoMenus;
        const bool autoY = !cell.yLocked;
        theme::pushPlotFont();
        if (ImPlot::BeginPlot("##gg", ImGui::GetContentRegionAvail(), plotFlags)) {
            const bool ggImu = cellLooksLikeGgImu(cell);
            const std::string xName =
                ggImu ? "Long G" : (cell.sources.empty() ? "X" : cellLabel(cell.sources[0]));
            const std::string yName =
                ggImu ? "Lat G" : (cell.sources.size() < 2 ? "Y" : cellLabel(cell.sources[1]));
            ImPlot::SetupAxes(xName.c_str(), yName.c_str(), plotValueAxisFlags(autoY, false),
                              plotValueAxisFlags(autoY, false));
            const double span = ggViewSpan(windowPeaks);
            if (autoY) {
                ImPlot::SetupAxesLimits(-span, span, -span, span, ImPlotCond_Always);
            } else if (cloudX.empty() && cloudY.empty()) {
                ImPlot::SetupAxesLimits(-2.0, 2.0, -2.0, 2.0, ImPlotCond_Once);
            }
            if (xHidden || yHidden) {
                ImPlot::PlotText("hidden", 0.0, 0.0);
            } else if (!cloudX.empty()) {
                const ImVec4 col = cell.sources.empty() ? theme::plotSeries(0)
                                                        : seriesColor(cell.sources[0], 0);
                const float marker = cell.sources.empty() ? 3.5f : scatterMarkerSize(cell.sources[0]);
                ImPlot::SetNextLineStyle(IMPLOT_AUTO_COL, 0.0f);
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, marker, col, 0.0f, col);
                keepPlotSeriesShown();
                ImPlot::PlotScatter("G-G", cloudX.data(), cloudY.data(), static_cast<int>(cloudX.size()));
                if (!cell.sources.empty() &&
                    drawPlotLegendColorPopup("G-G", cell.sources[0].color, 0, &cell.sources[0].hidden)) {
                    markDirty();
                }
                drawGgPeaks(heldPeaks);
                if (!xs.empty() && !ys.empty()) {
                    drawGgNow(xs.back(), ys.back());
                }
            } else {
                ImPlot::PlotText("assign Long G then Lat G", 0.0, 0.0);
            }
            if (ImPlot::IsPlotHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                cell.yLocked = false;
            }
            ImPlot::EndPlot();
        }
        theme::popPlotFont();
    } else {
        double latest = 0.0;
        bool any = false;
        for (const ViewSignal& src : cell.sources) {
            const SignalRing* ring = src.id != 0 ? ui.arena.ring(src.id) : nullptr;
            if (ring != nullptr && ring->hasLatest) {
                latest = std::max(latest, ring->latestTs);
                any = true;
            }
        }
        const auto& tl = ui.arena.timeline();
        const auto mode = static_cast<proto::TimelineMode>(tl.mode);
        const bool live = !ui.arena.catalogPinned() &&
                          (mode == proto::TimelineMode::Live || mode == proto::TimelineMode::Buffering ||
                           mode == proto::TimelineMode::Unknown);
        double xmax = latest;
        const std::uint64_t cursorMs = ui.arena.displayCursorMs();
        if (!live && cursorMs != 0) {
            xmax = static_cast<double>(cursorMs);
        } else if (cursorMs != 0 && latest <= 0.0) {
            xmax = static_cast<double>(cursorMs);
        }
        double xmin = xmax - static_cast<double>(cell.timeWindowSec) * 1000.0;
        if (cell.freeze && cell.viewMaxMs > cell.viewMinMs) {
            xmin = cell.viewMinMs;
            xmax = cell.viewMaxMs;
        }
        double viewMin = xmin;
        double viewMax = xmax;
        if (cell.rangeActive || cell.rangeDragging) {
            const double a = std::min(cell.rangeA, cell.rangeB);
            const double b = std::max(cell.rangeA, cell.rangeB);
            const double span = std::max(1.0, b - a);
            viewMin = a - span * 0.02;
            viewMax = b + span * 0.02;
        }
        double ymin = 0.0;
        double ymax = 1.0;
        double ymin2 = 0.0;
        double ymax2 = 1.0;
        bool haveY = false;
        bool haveY2 = false;
        std::vector<std::uint32_t> histIds;
        histIds.reserve(cell.sources.size());
        for (const ViewSignal& src : cell.sources) {
            if (src.id != 0) {
                histIds.push_back(src.id);
            }
        }
        const auto winFrom = static_cast<std::uint64_t>(std::max(0.0, viewMin));
        const auto winTo = static_cast<std::uint64_t>(std::max(viewMin, viewMax));
        if (!ui.arena.catalogPinned()) {
            for (const ViewSignal& src : cell.sources) {
                const SignalRing* ring = src.id != 0 ? ui.arena.ring(src.id) : nullptr;
                if (ring == nullptr || ring->count == 0) {
                    continue;
                }
                const double ts = ring->firstTs();
                if (std::isfinite(ts) && ts > 0.0) {
                    ui.arena.tiles().updateRingOldest(src.id, static_cast<std::uint64_t>(ts));
                }
            }
        }
        const auto maxPts = static_cast<std::uint32_t>(std::max(32.0f, ImGui::GetContentRegionAvail().x));
        const bool shiftRangeStarting = ImGui::GetIO().KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        const bool pointerActive =
            ImGui::IsMouseDragging(ImGuiMouseButton_Left) || cell.rangeDragging || shiftRangeStarting;
        ui.arena.tiles().setPlayback(1.0, tl.mode == static_cast<std::uint8_t>(proto::TimelineMode::Play) ||
                                              tl.mode == static_cast<std::uint8_t>(proto::TimelineMode::Live));
        if (!histIds.empty() && !live && unixMsLooksLikeWall(winFrom) && unixMsLooksLikeWall(winTo)) {
            if (pointerActive && !plotScrubbing_) {
                ui.arena.tiles().cancelLeaving(ui.client, winFrom, winTo);
            } else {
                ui.arena.tiles().requestWindow(ui.client, histIds, winFrom, winTo, maxPts, false);
                ui.arena.tiles().requestCursorWindow(ui.client, histIds, ui.arena.effectiveCursorMs());
            }
        }

        struct LineSeriesDraw {
            std::size_t index = 0;
            PlotArrays hist;
            const SignalRing* ring = nullptr;
        };
        std::vector<LineSeriesDraw> series;
        series.reserve(cell.sources.size());
        for (std::size_t i = 0; i < cell.sources.size(); ++i) {
            const ViewSignal& src = cell.sources[i];
            LineSeriesDraw row;
            row.index = i;
            if (src.id != 0) {
                row.hist = ui.arena.tiles().gatherPlot(src.id, winFrom, winTo, maxPts);
                row.ring = ui.arena.ring(src.id);
                if (!ui.arena.catalogPinned() && row.ring != nullptr && row.ring->count > 0) {
                    ui.arena.tiles().stitchRing(row.hist, row.ring->t, row.ring->v, row.ring->count,
                                               row.ring->cap, row.ring->write, winFrom, winTo, src.id);
                    capPlotArrays(row.hist, maxPts);
                }
            }
            if (!src.hidden) {
                const bool right = cell.dualY && src.yAxis == 2;
                extendYFromPlot(row.hist, right ? ymin2 : ymin, right ? ymax2 : ymax, right ? haveY2 : haveY);
                if (row.ring != nullptr) {
                    const RangeStats st = statsInRange(*row.ring, viewMin, viewMax);
                    if (st.ok) {
                        double& lo = right ? ymin2 : ymin;
                        double& hi = right ? ymax2 : ymax;
                        bool& have = right ? haveY2 : haveY;
                        if (!have) {
                            lo = st.min;
                            hi = st.max;
                            have = true;
                        } else {
                            lo = std::min(lo, st.min);
                            hi = std::max(hi, st.max);
                        }
                    }
                }
            }
            series.push_back(std::move(row));
        }
        if (haveY) {
            padPlotAxis(ymin, ymax);
        }
        if (haveY2) {
            padPlotAxis(ymin2, ymax2);
        }

        const bool autoY = !cell.yLocked;
        const ImPlotFlags plotFlags = (cell.legend ? ImPlotFlags_None : ImPlotFlags_NoLegend) |
                                      ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMenus;
        theme::pushPlotFont();
        ImVec2 plotSize = ImGui::GetContentRegionAvail();
        plotSize.x = std::max(32.0f, plotSize.x);
        plotSize.y = std::max(32.0f, plotSize.y);
        if (ImPlot::BeginPlot("##plot", plotSize, plotFlags)) {
            const bool dualY = cellUsesDualY(cell);
            ImPlot::SetupAxes("t", "value", plotTimeAxisFlags(), plotValueAxisFlags(autoY));
            if (dualY) {
                ImPlot::SetupAxis(ImAxis_Y2, "Y2",
                                  plotValueAxisFlags(autoY) | ImPlotAxisFlags_AuxDefault);
            }
            ImPlot::SetupAxisFormat(ImAxis_X1, formatPlotTime);
            auto setX = [](double a, double b) {
                if (sanitizePlotWindow(a, b)) {
                    ImPlot::SetupAxisLimits(ImAxis_X1, a, b, ImPlotCond_Always);
                }
            };
            if (cell.rangeActive && !cell.rangeDragging) {
                setX(viewMin, viewMax);
            } else if (cell.freeze && cell.viewMaxMs > cell.viewMinMs && !cell.rangeDragging) {
                setX(viewMin, viewMax);
            } else {
                setX(xmin, xmax);
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
                const std::string name = cellLabel(src);
                const ImVec4 col = seriesColor(src, row.index);
                const float weight = seriesWidth(src);
                if (!src.hidden) {
                    ImPlot::SetNextLineStyle(col, weight);
                    if (dualY) {
                        ImPlot::SetAxes(ImAxis_X1, src.yAxis == 2 ? ImAxis_Y2 : ImAxis_Y1);
                    }
                }
                if (src.hidden) {
                    continue;
                }
                if (row.hist.t.empty() || row.hist.y.empty()) {
                    continue;
                }
                const int n = static_cast<int>(std::min(row.hist.t.size(), row.hist.y.size()));
                if (n <= 0 || !plotArraysAllFinite(row.hist, n)) {
                    continue;
                }
                if (row.hist.envelope && row.hist.yMin.size() == static_cast<std::size_t>(n) &&
                    row.hist.yMax.size() == static_cast<std::size_t>(n)) {
                    keepPlotSeriesShown();
                    ImPlot::PlotLine(("##" + name + " min").c_str(), row.hist.t.data(), row.hist.yMin.data(),
                                     n);
                    ImPlot::SetNextLineStyle(col, weight);
                    keepPlotSeriesShown();
                    ImPlot::PlotLine(("##" + name + " max").c_str(), row.hist.t.data(), row.hist.yMax.data(),
                                     n);
                }
                ImPlot::SetNextLineStyle(col, weight);
                keepPlotSeriesShown();
                ImPlot::PlotLine(name.c_str(), row.hist.t.data(), row.hist.y.data(), n);
                if (drawPlotLegendColorPopup(name.c_str(), src.color, row.index, &src.hidden)) {
                    markDirty();
                }
                legendHot = legendHot || plotLegendEntryHot(name.c_str());
            }
            if (cell.sources.empty()) {
                ImPlot::PlotText("Add signal", 0.5, 0.5);
            }

            const bool hovered = ImPlot::IsPlotHovered();
            const bool xAxisHovered = ImPlot::IsAxisHovered(ImAxis_X1);
            const bool canScrub = (hovered || xAxisHovered) && !ImGui::GetIO().KeyShift &&
                                  !cell.rangeDragging && !legendHot;
            auto scrubTo = [&](double t) {
                t = clampPlotTimeMs(t);
                const double minBound = tl.minTimeMs != 0 ? static_cast<double>(tl.minTimeMs) : 0.0;
                const double maxBound = tl.maxTimeMs > tl.minTimeMs ? static_cast<double>(tl.maxTimeMs) : 0.0;
                double lo = cell.freeze && cell.viewMaxMs > cell.viewMinMs ? cell.viewMinMs : xmin;
                double hi = cell.freeze && cell.viewMaxMs > cell.viewMinMs ? cell.viewMaxMs : xmax;
                if (!(hi > lo)) {
                    lo = t - static_cast<double>(cell.timeWindowSec) * 1000.0;
                    hi = t;
                }
                containPlotTime(t, lo, hi, minBound, maxBound);
                holdScrubWindow(active(), lo, hi);
                ui.arena.setPreviewCursor(static_cast<std::uint64_t>(t), true);
            };
            const double axisLo = cell.freeze && cell.viewMaxMs > cell.viewMinMs ? cell.viewMinMs : xmin;
            const double axisHi = cell.freeze && cell.viewMaxMs > cell.viewMinMs ? cell.viewMaxMs : xmax;
            const double mouseT = plotTimeAtMouse(axisLo, axisHi);
            if (canScrub) {
                const double wheelScrub = plotWheelScrubDeltaMs(axisHi - axisLo);
                if (wheelScrub != 0.0) {
                    double t = static_cast<double>(ui.arena.displayCursorMs());
                    if (!std::isfinite(t) || t <= 0.0) {
                        t = axisHi;
                    }
                    scrubTo(t + wheelScrub);
                    if (!live) {
                        ui.client.sendTimeline(ctrl::TimelineMode::Pause, ui.arena.displayCursorMs());
                    }
                    ImGui::GetIO().MouseWheel = 0.0f;
                    ImGui::GetIO().MouseWheelH = 0.0f;
                } else if (plotWheelIsZoom()) {
                    double lo = axisLo;
                    double hi = axisHi;
                    if (!(hi > lo)) {
                        hi = xmax;
                        lo = xmin;
                    }
                    const double factor = ImGui::GetIO().MouseWheel > 0.0f ? 0.5 : 2.0;
                    const double cursorT = static_cast<double>(ui.arena.effectiveCursorMs());
                    zoomTimeWindowAround(plotZoomAnchorMs(cursorT, lo, hi), lo, hi, factor);
                    if (sanitizePlotWindow(lo, hi)) {
                        const float nextSec = static_cast<float>(std::max(2.0, (hi - lo) / 1000.0));
                        bool changed = std::fabs(nextSec - cell.timeWindowSec) > 0.05f;
                        cell.timeWindowSec = nextSec;
                        if (cell.freeze && cell.viewMaxMs > cell.viewMinMs) {
                            if (std::fabs(cell.viewMinMs - lo) > 0.5 || std::fabs(cell.viewMaxMs - hi) > 0.5) {
                                changed = true;
                            }
                            if (cell.scrubHold) {
                                holdScrubWindow(active(), lo, hi);
                            } else {
                                cell.viewMinMs = lo;
                                cell.viewMaxMs = hi;
                            }
                        }
                        if (changed) {
                            touchDirty();
                        }
                    }
                    ImGui::GetIO().MouseWheel = 0.0f;
                    ImGui::GetIO().MouseWheelH = 0.0f;
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
                if (plotScrubbing_ && !doubleClick && ui.arena.hasPreviewCursor() && !live) {
                    ui.client.sendTimeline(ctrl::TimelineMode::Pause, ui.arena.displayCursorMs());
                }
                plotScrubbing_ = false;
            } else if (canScrub && leftClicked) {
                plotScrubbing_ = true;
            }
            if (plotScrubbing_ && leftDown) {
                double t = mouseT;
                if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
                    t = clampToPlotAxis(mouseT, axisLo, axisHi);
                }
                scrubTo(t);
            }
            double playhead = plotScrubbing_ && leftDown
                                  ? (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)
                                         ? mouseT
                                         : clampToPlotAxis(mouseT, axisLo, axisHi))
                                  : plotPlayheadMs(static_cast<double>(ui.arena.effectiveCursorMs()),
                                                   axisLo, axisHi);
            if (!std::isfinite(playhead) || playhead <= 0.0) {
                playhead = std::isfinite(xmax) && xmax > 0.0 ? xmax : axisHi;
            }
            ImPlot::DragLineX(99, &playhead, theme::chrome(), 1.75f,
                              ImPlotDragToolFlags_NoFit | ImPlotDragToolFlags_NoInputs);
            std::vector<PlotCursorHit> hits;
            hits.reserve(series.size());
            const double cursorT = std::isfinite(playhead) ? playhead : mouseT;
            for (const LineSeriesDraw& row : series) {
                const ViewSignal& src = cell.sources[row.index];
                if (src.hidden) {
                    continue;
                }
                PlotCursorHit hit;
                hit.color = seriesColor(src, row.index);
                hit.yAxis = (dualY && src.yAxis == 2) ? ImAxis_Y2 : ImAxis_Y1;
                if (samplePlotArraysAt(row.hist, cursorT, hit.y)) {
                    hit.ok = true;
                } else if (src.id != 0 && ui.arena.interpolate(src.id, cursorT, hit.y)) {
                    hit.ok = true;
                }
                if (hit.ok) {
                    setPlotCursorLabel(hit, cellLabel(src).c_str(), hit.y);
                }
                hits.push_back(hit);
            }
            if (!hits.empty()) {
                drawPlotCursorReadout(cursorT, hits.data(), static_cast<int>(hits.size()), false);
            }
            if (hovered && ImGui::GetIO().KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                cell.rangeDragging = true;
                cell.rangeA = ImPlot::GetPlotMousePos().x;
                cell.rangeB = cell.rangeA;
                cell.freeze = true;
            }
            if (cell.rangeDragging) {
                cell.rangeB = ImPlot::GetPlotMousePos().x;
                if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || !ImGui::GetIO().KeyShift) {
                    cell.rangeDragging = false;
                    if (std::abs(cell.rangeB - cell.rangeA) > 1.0) {
                        cell.rangeActive = true;
                        if (cell.rangeB < cell.rangeA) {
                            std::swap(cell.rangeA, cell.rangeB);
                        }
                    } else {
                        cell.rangeActive = false;
                    }
                }
            }
            if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                cell.rangeActive = false;
                cell.rangeDragging = false;
                cell.yLocked = false;
                if (cell.scrubHold) {
                    releaseScrubHolds(active());
                    ui.arena.setPreviewCursor(0, false);
                }
            }

            if (cell.rangeActive && !cell.rangeDragging) {
                ImPlot::DragLineX(1, &cell.rangeA, theme::plotSeries(0), 1.5f);
                ImPlot::DragLineX(2, &cell.rangeB, theme::plotSeries(0), 1.5f);
                if (cell.rangeB < cell.rangeA) {
                    std::swap(cell.rangeA, cell.rangeB);
                }
            }

            const bool showRange = cell.rangeActive || cell.rangeDragging;
            if (showRange) {
                const double a = std::min(cell.rangeA, cell.rangeB);
                const double b = std::max(cell.rangeA, cell.rangeB);
                const ImPlotRect lim = ImPlot::GetPlotLimits();
                ImPlot::PushPlotClipRect();
                const ImVec2 p0 = ImPlot::PlotToPixels(a, lim.Y.Max);
                const ImVec2 p1 = ImPlot::PlotToPixels(b, lim.Y.Min);
                ImPlot::GetPlotDrawList()->AddRectFilled(p0, p1, IM_COL32(220, 80, 70, 28));
                ImPlot::PopPlotClipRect();

                char statsLine[256];
                statsLine[0] = '\0';
                bool first = true;
                for (const ViewSignal& src : cell.sources) {
                    if (src.hidden) {
                        continue;
                    }
                    const SignalRing* ring = src.id != 0 ? ui.arena.ring(src.id) : nullptr;
                    if (ring == nullptr) {
                        continue;
                    }
                    const RangeStats st = statsInRange(*ring, a, b);
                    if (!st.ok) {
                        continue;
                    }
                    char piece[128];
                    std::snprintf(piece, sizeof(piece), "%sn=%zu min=%.3g max=%.3g mean=%.3g",
                                  first ? "" : "  |  ", st.n, st.min, st.max, st.mean);
                    first = false;
                    std::strncat(statsLine, piece, sizeof(statsLine) - std::strlen(statsLine) - 1);
                }
                if (statsLine[0] != '\0') {
                    const ImVec2 origin = ImPlot::GetPlotPos();
                    ImDrawList* dl = ImPlot::GetPlotDrawList();
                    const ImVec2 textSz = ImGui::CalcTextSize(statsLine);
                    const ImVec2 pad(8.0f, 4.0f);
                    const ImVec2 boxMin(origin.x + 6.0f, origin.y + 6.0f);
                    const ImVec2 boxMax(boxMin.x + textSz.x + pad.x * 2.0f,
                                        boxMin.y + textSz.y + pad.y * 2.0f);
                    dl->AddRectFilled(boxMin, boxMax, IM_COL32(18, 18, 18, 210), 4.0f);
                    dl->AddRect(boxMin, boxMax, IM_COL32(255, 255, 255, 40), 4.0f);
                    dl->AddText(ImVec2(boxMin.x + pad.x, boxMin.y + pad.y), IM_COL32(240, 240, 240, 255),
                                statsLine);
                }
            }

            ImPlot::EndPlot();
        }
        theme::popPlotFont();
        (void)any;
    }

    if (!active().locked) {
        const ImVec2 handle(headerMin.x + size.x - kResizeHandle - 4.0f,
                            headerMin.y + size.y - kResizeHandle - 6.0f);
        ImGui::SetCursorScreenPos(handle);
        ImGui::InvisibleButton("##resize", ImVec2(kResizeHandle, kResizeHandle));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
            drag_ == DragKind::None) {
            dragUndoPrev_ = undo_;
            pushLayoutUndo();
            dragUndoPushed_ = true;
            drag_ = DragKind::Resize;
            dragIndex_ = index;
            dragStart_ = cell.rect;
            dragMouse0_ = ImGui::GetIO().MousePos;
        }
        const ImVec2 ha = ImGui::GetItemRectMin();
        const ImVec2 hb = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(hb.x, ha.y), hb, ImVec2(ha.x, hb.y),
                                                       theme::u32(theme::mutedFg()));
    }

    ImGui::EndChild();
    if (export_.active && !export_.waitDay && export_.settleFrames == 0 && export_.cell == index) {
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        gPlotPng.ready = true;
        gPlotPng.x = a.x;
        gPlotPng.y = a.y;
        gPlotPng.w = b.x - a.x;
        gPlotPng.h = b.y - a.y;
        gPlotPng.name = export_.filename;
        status_ = "Saved " + export_.filename;
        export_ = {};
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

void CustomViews::drawGrid(UiContext& ui, ImVec2 size) {
    CustomView& view = active();
    ImGui::BeginChild("##view-grid", size, ImGuiChildFlags_None);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int cols = std::max(1, view.columns);
    const float gap = view.gap;
    const float unitW = avail.x / static_cast<float>(cols);
    const float unitH = view.rowHeight;
    int maxRow = 1;
    for (const ViewCell& cell : view.cells) {
        maxRow = std::max(maxRow, cell.rect.y + cell.rect.h);
    }
    ImGui::Dummy(ImVec2(avail.x, static_cast<float>(maxRow) * unitH));
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 scroll(ImGui::GetScrollX(), ImGui::GetScrollY());

    for (int i = 0; i < static_cast<int>(view.cells.size()); ++i) {
        ViewCell& cell = view.cells[static_cast<std::size_t>(i)];
        const float x = static_cast<float>(cell.rect.x) * unitW;
        const float y = static_cast<float>(cell.rect.y) * unitH;
        const float w = std::max(40.0f, static_cast<float>(cell.rect.w) * unitW - gap);
        const float h = std::max(48.0f, static_cast<float>(cell.rect.h) * unitH - gap);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + x - scroll.x, origin.y + y - scroll.y));
        drawCell(ui, cell, ImVec2(w, h), i);
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x - scroll.x, origin.y - scroll.y));
    ImGui::Dummy(ImVec2(avail.x, static_cast<float>(maxRow) * unitH));
    updateDrag(ImVec2(origin.x - scroll.x, origin.y - scroll.y), unitW, unitH);
    if (export_.active && !export_.waitDay && export_.settleFrames == 0 && !gPlotPng.ready) {
        const ImVec2 a = ImGui::GetWindowPos();
        gPlotPng.ready = true;
        gPlotPng.x = a.x;
        gPlotPng.y = a.y;
        gPlotPng.w = avail.x;
        gPlotPng.h = std::min(avail.y, static_cast<float>(maxRow) * unitH);
        gPlotPng.name = export_.filename;
        status_ = "Saved " + export_.filename;
        export_ = {};
    }
    ImGui::EndChild();
}

void CustomViews::draw(UiContext& ui) {
    ensureLoaded();
    pollImport();
    resolve(ui.arena);
    subscribe(ui);
    if (doc_.views.empty()) {
        ensureDefault();
    }
    doc_.active = clampi(doc_.active, 0, static_cast<int>(doc_.views.size()) - 1);
    if (pickerDayIndex_ < 0) {
        syncPickerDay(ui.arena);
    }
    tickPlotExport(ui);

    if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        undoLastLayout();
    }

    drawToolbar(ui);
    drawGrid(ui, ImGui::GetContentRegionAvail());
    if (!export_.active || export_.waitDay || export_.settleFrames > 0) {
        drawPicker(ui);
    }
    const ImGuiIO& io = ImGui::GetIO();
    const bool pointerDown = io.MouseDown[ImGuiMouseButton_Left] || plotScrubbing_;
    if (ui.arena.timelineIsLive() && !pointerDown && anyScrubHold() &&
        std::fabs(io.MouseWheel) < 0.01f && std::fabs(io.MouseWheelH) < 0.01f) {
        releaseScrubHolds(active());
        ui.arena.setPreviewCursor(0, false);
    }
    flushDirty();
}

}  // namespace photon::ui
