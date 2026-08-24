#include "photon_ui/custom_view.hpp"

#include "photon/json.hpp"
#include "photon_ui/client.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"
#include "implot.h"

#include <emscripten.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>
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
constexpr float kToolbarH = 28.0f;
constexpr float kCellHeaderH = 22.0f;
constexpr float kResizeHandle = 14.0f;

bool shellButton(const char* label, bool selected) {
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
    const bool hit = ImGui::Button(label, ImVec2(0.0f, kToolbarH));
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
    if (cell.timeWindowSec > 300.0f) {
        cell.timeWindowSec = 300.0f;
    }
    if (plot["options"].isObject() && plot["options"].contains("legend")) {
        cell.legend = plot["options"]["legend"].asBool(true);
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

json::Value signalToJson(const ViewSignal& src) {
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
    return json::Value(std::move(o));
}

json::Value cellToJson(const ViewCell& cell) {
    json::Array sources;
    for (const ViewSignal& src : cell.sources) {
        sources.push_back(signalToJson(src));
    }
    json::Object options;
    options.emplace("legend", json::Value(cell.legend));
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
    json::Object panel;
    panel.emplace("id", json::Value(view.id));
    panel.emplace("name", json::Value(view.name));
    panel.emplace("layout", json::Value(std::move(layout)));
    panel.emplace("widgets", json::Value(std::move(widgets)));
    return json::Value(std::move(panel));
}

}  // namespace

}  // namespace photon::ui

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

extern "C" {

EMSCRIPTEN_KEEPALIVE
void photon_on_view_import(const char* text) {
    if (text == nullptr) {
        return;
    }
    photon::ui::gPendingImport = text;
    photon::ui::gHaveImport = true;
}

}  // extern "C"

namespace photon::ui {

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
    *out = std::move(doc);
    return true;
}

CustomView& CustomViews::active() { return doc_.views[static_cast<std::size_t>(doc_.active)]; }

const CustomView& CustomViews::active() const {
    return doc_.views[static_cast<std::size_t>(doc_.active)];
}

void CustomViews::migrateView(CustomView& view) {
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
        }
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

void CustomViews::ensureDefault() {
    if (!doc_.views.empty()) {
        for (CustomView& view : doc_.views) {
            migrateView(view);
        }
        return;
    }
    CustomView view;
    view.id = "view-1";
    view.name = "View 1";
    migrateView(view);
    ViewCell cell;
    cell.id = "plot-1";
    cell.title = "Plot";
    cell.kind = ViewCellKind::Line;
    cell.rect = ViewRect{0, 0, kDefaultCellW, kDefaultCellH};
    view.cells.push_back(std::move(cell));
    doc_.views.push_back(std::move(view));
    doc_.active = 0;
    selectedCell_ = 0;
    nextView_ = 2;
    nextCell_ = 2;
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
            status_ = "Loaded views from localStorage";
            std::free(raw);
            return;
        }
        status_ = "localStorage view JSON ignored: " + err;
        std::free(raw);
    }
    ensureDefault();
}

void CustomViews::markDirty() {
    dirty_ = true;
    save();
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
    ViewSignal lat;
    lat.messageName = "IMU_B_Accel";
    lat.signalName = "Accel_Y";
    lat.label = "Lat G";
    ViewSignal lon;
    lon.messageName = "IMU_B_Accel";
    lon.signalName = "Accel_X";
    lon.label = "Long G";
    cell.sources.push_back(std::move(lat));
    cell.sources.push_back(std::move(lon));
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

void CustomViews::drawToolbar() {
    CustomView& view = active();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 0.0f));
    for (int i = 0; i < static_cast<int>(doc_.views.size()); ++i) {
        if (i > 0) {
            ImGui::SameLine();
        }
        if (shellButton(doc_.views[static_cast<std::size_t>(i)].name.c_str(), i == doc_.active)) {
            doc_.active = i;
            selectedCell_ = active().cells.empty() ? -1 : 0;
            markDirty();
        }
    }
    ImGui::SameLine();
    if (shellButton("+ View", false)) {
        addView();
    }
    ImGui::SameLine();
    if (shellButton("Add plot", false)) {
        addCell(ViewCellKind::Line);
    }
    ImGui::SameLine();
    if (shellButton("Add value", false)) {
        addCell(ViewCellKind::Value);
    }
    ImGui::SameLine();
    if (shellButton("...", false)) {
        ImGui::OpenPopup("##view-more");
    }
    ImGui::PopStyleVar();
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

void CustomViews::drawPicker(UiContext& ui, float width) {
    ImGui::BeginChild("##view-picker", ImVec2(width, 0.0f), ImGuiChildFlags_Borders);
    ImGui::Text("Signals");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search message.signal", search_, sizeof(search_));
    theme::pushSmallFont();
    ImGui::TextWrapped("Click a signal to plot it. Drag a plot to move, corner to resize.");
    theme::popSmallFont();
    ImGui::Separator();

    ViewCell* cell = nullptr;
    if (selectedCell_ >= 0 && selectedCell_ < static_cast<int>(active().cells.size())) {
        cell = &active().cells[static_cast<std::size_t>(selectedCell_)];
        ImGui::TextColored(theme::mutedFg(), "Cell: %s", cell->title.c_str());
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
            ImGui::TextColored(theme::mutedFg(), "Assign X then Y (Lat G, Long G).");
        }
        char title[80];
        std::snprintf(title, sizeof(title), "%s", cell->title.c_str());
        if (ImGui::InputText("Title", title, sizeof(title))) {
            cell->title = title;
            markDirty();
        }
        if (ImGui::SliderFloat("Window", &cell->timeWindowSec, 2.0f, 120.0f, "%.0f s")) {
            markDirty();
        }
        if (ImGui::Checkbox("Legend", &cell->legend)) {
            markDirty();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Freeze", &cell->freeze);
        if (ImGui::SmallButton("Remove cell") && active().cells.size() > 1) {
            active().cells.erase(active().cells.begin() + selectedCell_);
            selectedCell_ = -1;
            cell = nullptr;
            markDirty();
        }
        ImGui::Separator();
        ImGui::TextColored(theme::mutedFg(), "Assigned");
        if (cell != nullptr) {
            for (int i = 0; i < static_cast<int>(cell->sources.size()); ++i) {
                ImGui::PushID(i);
                const std::string label = cellLabel(cell->sources[static_cast<std::size_t>(i)]);
                ImGui::BulletText("%s", label.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    cell->sources.erase(cell->sources.begin() + i);
                    markDirty();
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
        }
        ImGui::Separator();
    } else {
        ImGui::TextColored(theme::mutedFg(), "Select a cell to assign signals.");
    }

    if (!ui.arena.haveSignalCatalog) {
        ImGui::TextWrapped("Waiting for signal_catalog from the backend.");
        ImGui::EndChild();
        return;
    }

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
    ImGui::EndChild();
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
    ImGui::InvisibleButton("##move", ImVec2(std::max(8.0f, size.x - kResizeHandle - 8.0f), kCellHeaderH));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
        drag_ == DragKind::None) {
        drag_ = DragKind::Move;
        dragIndex_ = index;
        dragStart_ = cell.rect;
        dragMouse0_ = ImGui::GetIO().MousePos;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const char* title = cell.title.empty() ? cell.id.c_str() : cell.title.c_str();
    theme::pushSmallFont();
    const ImVec2 tsz = ImGui::CalcTextSize(title);
    draw->AddText(ImVec2(headerMin.x + 8.0f, headerMin.y + (kCellHeaderH - tsz.y) * 0.5f),
                  theme::u32(theme::mutedFg()), title);
    theme::popSmallFont();
    ImGui::SetCursorScreenPos(ImVec2(headerMin.x, headerMin.y + kCellHeaderH));

    if (cell.kind == ViewCellKind::Value) {
        if (cell.sources.empty()) {
            ImGui::TextColored(theme::mutedFg(), "No signals");
        } else if (cell.sources.size() == 1) {
            const ViewSignal& src = cell.sources[0];
            const bool have = src.id != 0 && ui.arena.has(src.id);
            char buf[32];
            if (have) {
                std::snprintf(buf, sizeof(buf), "%0.3f", ui.arena.latest(src.id));
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
        } else {
            for (const ViewSignal& src : cell.sources) {
                const bool have = src.id != 0 && ui.arena.has(src.id);
                if (have) {
                    ImGui::Text("%s  %0.3f", cellLabel(src).c_str(), ui.arena.latest(src.id));
                } else {
                    ImGui::TextColored(theme::mutedFg(), "%s  --", cellLabel(src).c_str());
                }
            }
        }
    } else if (cell.kind == ViewCellKind::Scatter) {
        const SignalRing* xRing = cell.sources.size() > 0 && cell.sources[0].id != 0
                                      ? ui.arena.ring(cell.sources[0].id)
                                      : nullptr;
        const SignalRing* yRing = cell.sources.size() > 1 && cell.sources[1].id != 0
                                      ? ui.arena.ring(cell.sources[1].id)
                                      : nullptr;
        std::vector<double> xs;
        std::vector<double> ys;
        if (xRing != nullptr && yRing != nullptr && xRing->count > 0 && yRing->count > 0) {
            pairScatter(*xRing, *yRing, xs, ys);
        }
        const ImPlotFlags plotFlags =
            (cell.legend ? ImPlotFlags_None : ImPlotFlags_NoLegend) | ImPlotFlags_Equal;
        if (ImPlot::BeginPlot("##gg", ImGui::GetContentRegionAvail(), plotFlags)) {
            const std::string xName = cell.sources.empty() ? "X" : cellLabel(cell.sources[0]);
            const std::string yName = cell.sources.size() < 2 ? "Y" : cellLabel(cell.sources[1]);
            ImPlot::SetupAxes(xName.c_str(), yName.c_str());
            ImPlot::SetupAxesLimits(-2.0, 2.0, -2.0, 2.0, ImPlotCond_Once);
            if (!xs.empty()) {
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, theme::plotSeries(0), IMPLOT_AUTO,
                                           theme::plotSeries(0));
                ImPlot::PlotScatter("G-G", xs.data(), ys.data(), static_cast<int>(xs.size()));
            } else {
                ImPlot::PlotText("assign Lat G then Long G", 0.0, 0.0);
            }
            ImPlot::EndPlot();
        }
    } else {
        double xmax = 0.0;
        bool any = false;
        for (const ViewSignal& src : cell.sources) {
            const SignalRing* ring = src.id != 0 ? ui.arena.ring(src.id) : nullptr;
            if (ring != nullptr && ring->hasLatest) {
                xmax = std::max(xmax, ring->latestTs);
                any = true;
            }
        }
        const double xmin = xmax - static_cast<double>(cell.timeWindowSec) * 1000.0;
        const ImPlotFlags plotFlags = cell.legend ? ImPlotFlags_None : ImPlotFlags_NoLegend;
        if (ImPlot::BeginPlot("##plot", ImGui::GetContentRegionAvail(), plotFlags)) {
            ImPlot::SetupAxes("t", "value", ImPlotAxisFlags_NoLabel, ImPlotAxisFlags_NoLabel);
            if (any && !cell.freeze) {
                ImPlot::SetupAxisLimits(ImAxis_X1, xmin, xmax, ImPlotCond_Always);
            }
            for (std::size_t i = 0; i < cell.sources.size(); ++i) {
                const ViewSignal& src = cell.sources[i];
                const SignalRing* ring = src.id != 0 ? ui.arena.ring(src.id) : nullptr;
                if (ring == nullptr || ring->count == 0) {
                    continue;
                }
                const std::string name = cellLabel(src);
                ImPlot::SetNextLineStyle(theme::plotSeries(i), theme::plotLineWeight());
                ImPlot::PlotLine(name.c_str(), ring->t.data(), ring->v.data(), static_cast<int>(ring->count),
                                 ImPlotLineFlags_None, static_cast<int>(ring->plotOffset()));
            }
            ImPlot::EndPlot();
        }
        (void)any;
    }

    const ImVec2 handle(headerMin.x + size.x - kResizeHandle - 4.0f,
                        headerMin.y + size.y - kResizeHandle - 6.0f);
    ImGui::SetCursorScreenPos(handle);
    ImGui::InvisibleButton("##resize", ImVec2(kResizeHandle, kResizeHandle));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
        drag_ == DragKind::None) {
        drag_ = DragKind::Resize;
        dragIndex_ = index;
        dragStart_ = cell.rect;
        dragMouse0_ = ImGui::GetIO().MousePos;
    }
    const ImVec2 ha = ImGui::GetItemRectMin();
    const ImVec2 hb = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(hb.x, ha.y), hb, ImVec2(ha.x, hb.y),
                                                   theme::u32(theme::mutedFg()));

    ImGui::EndChild();
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
    updateDrag(ImVec2(origin.x - scroll.x, origin.y - scroll.y), unitW, unitH);
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

    drawToolbar();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = 260.0f;
    drawPicker(ui, side);
    ImGui::SameLine();
    drawGrid(ui, ImVec2(avail.x - side - 8.0f, avail.y));
}

}  // namespace photon::ui
