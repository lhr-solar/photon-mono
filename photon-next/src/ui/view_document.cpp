#include "ui/view_document.hpp"
#include <algorithm>
#include <optional>
#include "base/json.hpp"
#include "ui/chart_grid_model.hpp"
#include "ui/plot_color.hpp"
#include "ui/plot_time.hpp"
namespace photon::ui {
namespace {
constexpr int kMaxSources = 16, kMaxViews = 32, kMaxCells = 128;
constexpr float kMinSeriesWidth = 0.5f, kMaxSeriesWidth = 8.0f;
int clampi(int v, int lo, int hi) {
    return std::clamp(v, lo, hi);
}
float clampf(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
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

const char* kindKey(ViewCellKind kind) {
    if (kind == ViewCellKind::Value) {
        return "list";
    }
    if (kind == ViewCellKind::Scatter) {
        return "scatter";
    }
    if (kind == ViewCellKind::Histogram) {
        return "histogram";
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
    if (key == "histogram") {
        return ViewCellKind::Histogram;
    }
    return ViewCellKind::Line;
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
    cell.timeWindowSec = clampPlotWindowSec(static_cast<float>(plot["timeWindowSeconds"].asNumber(15.0)));
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
        if (options.contains("yUser") && options["yUser"].isBool()) {
            cell.yUser = options["yUser"].asBool(false);
            cell.yUserLo = options["yUserLo"].asNumber(0.0);
            cell.yUserHi = options["yUserHi"].asNumber(0.0);
            cell.yUserRightLo = options["yUserRightLo"].asNumber(0.0);
            cell.yUserRightHi = options["yUserRightHi"].asNumber(0.0);
            if (!(cell.yUserHi > cell.yUserLo)) {
                cell.yUser = false;
            }
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
                float parsed[4]{};
                if (parseHexColor(src["color"].asString(), parsed)) {
                    sig.color = formatHexColor(parsed[0], parsed[1], parsed[2], parsed[3]);
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

CustomView parsePanel(const json::Value& root, int* nextCell, int version) {
    CustomView view;
    view.id = root["id"].asString("view");
    view.name = root["name"].asString("View");
    const json::Value& layout = root["layout"];
    view.columns = clampi(static_cast<int>(layout["columns"].asInt(version == 1 ? 12 : kGridCols)), 1, 48);
    view.rowHeight = static_cast<float>(layout["rowHeight"].asNumber(version == 1 ? 160.0f : kGridRowH));
    if (view.rowHeight < 24.0f) {
        view.rowHeight = 24.0f;
    }
    if (view.rowHeight > 1200.0f) {
        view.rowHeight = 1200.0f;
    }
    if (!layout.contains("locked") && version == 1) view.locked = false;
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
    if (cell.yUser && cell.yUserHi > cell.yUserLo) {
        options.emplace("yUser", json::Value(true));
        options.emplace("yUserLo", json::Value(cell.yUserLo));
        options.emplace("yUserHi", json::Value(cell.yUserHi));
        if (cell.yUserRightHi > cell.yUserRightLo) {
            options.emplace("yUserRightLo", json::Value(cell.yUserRightLo));
            options.emplace("yUserRightHi", json::Value(cell.yUserRightHi));
        }
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
    layout.emplace("locked", json::Value(view.locked));
    json::Object panel;
    panel.emplace("id", json::Value(view.id));
    panel.emplace("name", json::Value(view.name));
    panel.emplace("layout", json::Value(std::move(layout)));
    panel.emplace("widgets", json::Value(std::move(widgets)));
    return json::Value(std::move(panel));
}

}  // namespace
std::string serializeCustomViews(const CustomViewDocument& doc) {
    json::Array panels;
    for (const CustomView& view : doc.views) {
        panels.push_back(viewToJson(view));
    }
    json::Object root;
    root.emplace("$schema", json::Value("../docs/config/photon-view.schema.json"));
    root.emplace("schemaVersion", json::Value(2));
    root.emplace("id", json::Value(doc.id.empty() ? "custom-views" : doc.id));
    root.emplace("name", json::Value(doc.name.empty() ? "Custom Views" : doc.name));
    root.emplace("activePanel", json::Value(doc.active));
    root.emplace("panels", json::Value(std::move(panels)));
    return json::stringify(json::Value(std::move(root)));
}

void migrateGgAxes(ViewCell& cell) {
    if (cell.kind != ViewCellKind::Scatter || cell.sources.size() < 2) {
        return;
    }
    if ((cell.sources[0].signalName == "Accel_Y" || cell.sources[0].label == "Lat G") &&
        (cell.sources[1].signalName == "Accel_X" || cell.sources[1].label == "Long G")) {
        std::swap(cell.sources[0], cell.sources[1]);
    }
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
    if (version != 1 && version != 2) {
        if (error != nullptr) {
            *error = "unsupported schemaVersion; expected 1 or 2";
        }
        return false;
    }
    CustomViewDocument doc;
    doc.schemaVersion = 2;
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
            doc.views.push_back(parsePanel(panel, &nextCell, version));
        }
    } else {
        doc.views.push_back(parsePanel(root, &nextCell, version));
    }
    if (doc.views.empty()) {
        if (error != nullptr) {
            *error = "no views in document";
        }
        return false;
    }
    doc.active =
        clampi(static_cast<int>(root["activePanel"].asInt(0)), 0, static_cast<int>(doc.views.size()) - 1);
    for (CustomView& view : doc.views) {
        migrateGridUnits(view);
        for (ViewCell& cell : view.cells) {
            migrateGgAxes(cell);
        }
    }
    *out = std::move(doc);
    return true;
}

}  // namespace photon::ui
