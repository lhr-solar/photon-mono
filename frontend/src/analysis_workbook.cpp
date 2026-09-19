#include "photon_ui/analysis_workbook.hpp"

#include "photon_ui/analysis_eval.hpp"
#include "photon/json.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace photon::ui {
namespace {

constexpr int kMaxViews = 32;
constexpr int kMaxCells = 128;
constexpr int kMaxSources = 16;
constexpr int kMaxEvents = 4096;
constexpr int kMaxTransforms = 256;
constexpr int kMaxDerived = 256;

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

bool knownCellKind(std::string_view key) {
    return key.empty() || key == "line" || key == "list" || key == "scatter" || key == "value";
}

ViewCellKind kindFromKey(std::string_view key) {
    if (key == "list" || key == "value") {
        return ViewCellKind::Value;
    }
    if (key == "scatter") {
        return ViewCellKind::Scatter;
    }
    return ViewCellKind::Line;
}

ViewSignal parseSignal(const json::Value& src) {
    ViewSignal sig;
    sig.id = static_cast<std::uint32_t>(src["id"].asInt());
    sig.messageId = static_cast<std::uint32_t>(src["messageId"].asInt());
    if (sig.messageId == 0 && src.contains("message_id")) {
        sig.messageId = static_cast<std::uint32_t>(src["message_id"].asInt());
    }
    sig.messageName = src["messageName"].asString();
    sig.signalName = src["signalName"].asString();
    sig.label = src["label"].asString();
    if (src.contains("color") && src["color"].isString()) {
        sig.color = src["color"].asString();
    }
    if (src.contains("width")) {
        const float width = static_cast<float>(src["width"].asNumber());
        if (width > 0.0f) {
            sig.width = std::min(16.0f, width);
        }
    }
    if (src.contains("hidden")) {
        sig.hidden = src["hidden"].asBool(false);
    }
    if (src.contains("yAxis")) {
        sig.yAxis = static_cast<int>(src["yAxis"].asInt(1));
    }
    return sig;
}

ViewCell parseCellObject(const json::Value& widget, int* nextId) {
    ViewCell cell;
    cell.id = widget["id"].asString();
    if (cell.id.empty()) {
        cell.id = "cell-" + std::to_string(*nextId);
        ++*nextId;
    }
    const json::Value& rect = widget.contains("rect") ? widget["rect"] : widget;
    cell.rect.x = std::max(0, static_cast<int>(rect["x"].asInt()));
    cell.rect.y = std::max(0, static_cast<int>(rect["y"].asInt()));
    cell.rect.w = std::max(1, static_cast<int>(rect["w"].asInt(kDefaultCellW)));
    cell.rect.h = std::max(1, static_cast<int>(rect["h"].asInt(kDefaultCellH)));

    const json::Value& plot = widget["plot"].isObject() ? widget["plot"] : widget;
    const std::string type = plot.contains("type") ? plot["type"].asString("line") : widget["kind"].asString("line");
    cell.kind = kindFromKey(type);
    cell.title = plot["title"].asString(widget["title"].asString(cell.id));
    cell.timeWindowSec = static_cast<float>(plot["timeWindowSeconds"].asNumber(
        widget["timeWindowSeconds"].asNumber(15.0)));
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
        if (options.contains("dualY")) {
            cell.dualY = options["dualY"].asBool(false);
        }
        if (options.contains("yLocked")) {
            cell.yLocked = options["yLocked"].asBool(false);
        }
    }
    const json::Value& sources = plot["sources"].isArray() ? plot["sources"] : widget["sources"];
    if (sources.isArray()) {
        for (const json::Value& src : sources.asArray()) {
            if (cell.sources.size() >= static_cast<std::size_t>(kMaxSources)) {
                break;
            }
            if (!src.isObject()) {
                continue;
            }
            cell.sources.push_back(parseSignal(src));
        }
    }
    if (widget.contains("derivedChannelId") && widget["derivedChannelId"].isString()) {
        cell.derivedChannelId = widget["derivedChannelId"].asString();
    } else if (plot.contains("derivedChannelId") && plot["derivedChannelId"].isString()) {
        cell.derivedChannelId = plot["derivedChannelId"].asString();
    }
    return cell;
}

bool skipUnknownKind(const json::Value& widget) {
    const std::string kind = widget["kind"].asString();
    if (kind == "video" || kind == "map" || kind == "text" || kind == "compute" || kind == "note") {
        return true;
    }
    if (widget["plot"].isObject()) {
        const std::string type = widget["plot"]["type"].asString();
        if (!type.empty() && !knownCellKind(type)) {
            return true;
        }
    } else if (!kind.empty() && kind != "plot" && !knownCellKind(kind)) {
        return true;
    }
    return false;
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
    if (layout.contains("locked")) {
        view.locked = layout["locked"].asBool(false);
    }

    auto pushCell = [&](const json::Value& widget) {
        if (view.cells.size() >= static_cast<std::size_t>(kMaxCells)) {
            return;
        }
        if (!widget.isObject() || skipUnknownKind(widget)) {
            return;
        }
        view.cells.push_back(parseCellObject(widget, nextCell));
    };

    if (root["widgets"].isArray()) {
        for (const json::Value& widget : root["widgets"].asArray()) {
            pushCell(widget);
        }
    } else if (root["cells"].isArray()) {
        for (const json::Value& widget : root["cells"].asArray()) {
            pushCell(widget);
        }
    }
    return view;
}

json::Value signalToJson(const ViewSignal& src) {
    json::Object o;
    if (src.id != 0) {
        o.emplace("id", json::Value(static_cast<int>(src.id)));
    }
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
    if (src.yAxis == 2) {
        o.emplace("yAxis", json::Value(2));
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
    if (cell.dualY) {
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
    if (!cell.derivedChannelId.empty()) {
        widget.emplace("derivedChannelId", json::Value(cell.derivedChannelId));
    }
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

json::Value bindingToJson(const AnalysisRunBinding& bind) {
    json::Object o;
    o.emplace("sessionId", json::Value(bind.sessionId));
    o.emplace("dbcHash", json::Value(bind.dbcHash));
    o.emplace("role", json::Value(bind.role == AnalysisRunRole::Overlay ? "overlay" : "primary"));
    o.emplace("timeOffsetMs", json::Value(bind.timeOffsetMs));
    if (!bind.color.empty()) {
        o.emplace("color", json::Value(bind.color));
    }
    o.emplace("minMs", json::Value(bind.minMs));
    o.emplace("maxMs", json::Value(bind.maxMs));
    return json::Value(std::move(o));
}

AnalysisRunBinding parseBinding(const json::Value& v) {
    AnalysisRunBinding bind;
    bind.sessionId = v["sessionId"].asString();
    if (bind.sessionId.empty()) {
        bind.sessionId = v["session_id"].asString();
    }
    bind.dbcHash = v["dbcHash"].asString();
    if (bind.dbcHash.empty()) {
        bind.dbcHash = v["dbc_hash"].asString();
    }
    const std::string role = v["role"].asString("primary");
    bind.role = role == "overlay" ? AnalysisRunRole::Overlay : AnalysisRunRole::Primary;
    bind.timeOffsetMs = v["timeOffsetMs"].asInt();
    if (v.contains("color") && v["color"].isString()) {
        bind.color = v["color"].asString();
    }
    bind.minMs = static_cast<std::uint64_t>(v["minMs"].asInt());
    bind.maxMs = static_cast<std::uint64_t>(v["maxMs"].asInt());
    return bind;
}

std::optional<double> optNumber(const json::Value& v, const char* key) {
    if (v.contains(key) && v[key].isNumber()) {
        return v[key].asNumber();
    }
    return std::nullopt;
}

json::Value eventToJson(const AnalysisEvent& ev) {
    json::Object o;
    o.emplace("id", json::Value(ev.id));
    o.emplace("tMs", json::Value(ev.tMs));
    if (ev.endMs.has_value()) {
        o.emplace("endMs", json::Value(*ev.endMs));
    }
    if (!ev.source.empty()) {
        o.emplace("source", json::Value(ev.source));
    }
    if (ev.signalId != 0) {
        o.emplace("signalId", json::Value(static_cast<int>(ev.signalId)));
    }
    if (!ev.label.empty()) {
        o.emplace("label", json::Value(ev.label));
    }
    if (ev.value.has_value()) {
        o.emplace("value", json::Value(*ev.value));
    }
    return json::Value(std::move(o));
}

AnalysisEvent parseEvent(const json::Value& v) {
    AnalysisEvent ev;
    ev.id = v["id"].asString();
    ev.tMs = static_cast<std::uint64_t>(v["tMs"].asInt());
    if (v.contains("endMs") && v["endMs"].isNumber()) {
        ev.endMs = static_cast<std::uint64_t>(v["endMs"].asInt());
    }
    ev.source = v["source"].asString();
    ev.signalId = static_cast<std::uint32_t>(v["signalId"].asInt());
    ev.label = v["label"].asString();
    ev.value = optNumber(v, "value");
    return ev;
}

json::Value transformToJson(const AnalysisTransform& xf) {
    json::Object o;
    o.emplace("id", json::Value(xf.id));
    if (!xf.name.empty()) {
        o.emplace("name", json::Value(xf.name));
    }
    if (xf.signalId != 0) {
        o.emplace("signalId", json::Value(static_cast<int>(xf.signalId)));
    }
    if (!xf.messageName.empty()) {
        o.emplace("messageName", json::Value(xf.messageName));
    }
    if (!xf.signalName.empty()) {
        o.emplace("signalName", json::Value(xf.signalName));
    }
    if (!xf.kind.empty()) {
        o.emplace("kind", json::Value(xf.kind));
    }
    if (!xf.spec.empty()) {
        o.emplace("spec", json::Value(xf.spec));
    }
    if (!xf.label.empty()) {
        o.emplace("label", json::Value(xf.label));
    }
    if (xf.threshold.has_value()) {
        o.emplace("threshold", json::Value(*xf.threshold));
    }
    if (xf.min.has_value()) {
        o.emplace("min", json::Value(*xf.min));
    }
    if (xf.max.has_value()) {
        o.emplace("max", json::Value(*xf.max));
    }
    return json::Value(std::move(o));
}

AnalysisTransform parseTransform(const json::Value& v) {
    AnalysisTransform xf;
    xf.id = v["id"].asString();
    xf.name = v["name"].asString();
    xf.signalId = static_cast<std::uint32_t>(v["signalId"].asInt());
    xf.messageName = v["messageName"].asString();
    xf.signalName = v["signalName"].asString();
    xf.kind = v["kind"].asString();
    xf.spec = v["spec"].asString();
    xf.label = v["label"].asString();
    xf.threshold = optNumber(v, "threshold");
    xf.min = optNumber(v, "min");
    xf.max = optNumber(v, "max");
    return xf;
}

json::Value derivedToJson(const AnalysisDerivedChannel& ch) {
    json::Object o;
    o.emplace("id", json::Value(ch.id));
    if (!ch.name.empty()) {
        o.emplace("name", json::Value(ch.name));
    }
    if (!ch.unit.empty()) {
        o.emplace("unit", json::Value(ch.unit));
    }
    if (!ch.kind.empty()) {
        o.emplace("kind", json::Value(ch.kind));
    }
    if (!ch.spec.empty()) {
        o.emplace("spec", json::Value(ch.spec));
    }
    return json::Value(std::move(o));
}

AnalysisDerivedChannel parseDerived(const json::Value& v) {
    AnalysisDerivedChannel ch;
    ch.id = v["id"].asString();
    ch.name = v["name"].asString();
    ch.unit = v["unit"].asString();
    ch.kind = v["kind"].asString();
    ch.spec = v["spec"].asString();
    return ch;
}

template <typename T, typename ParseFn>
void parseObjectArray(const json::Value& root, const char* key, std::vector<T>* out, int maxCount,
                      ParseFn parseFn) {
    if (!root[key].isArray()) {
        return;
    }
    for (const json::Value& row : root[key].asArray()) {
        if (out->size() >= static_cast<std::size_t>(maxCount)) {
            break;
        }
        if (!row.isObject()) {
            continue;
        }
        out->push_back(parseFn(row));
    }
}

}  // namespace

std::string serializeAnalysisWorkbook(const AnalysisWorkbook& doc) {
    json::Array panels;
    for (const CustomView& view : doc.views) {
        panels.push_back(viewToJson(view));
    }
    json::Array bindings;
    for (const AnalysisRunBinding& bind : doc.runBindings) {
        bindings.push_back(bindingToJson(bind));
    }
    json::Array events;
    for (const AnalysisEvent& ev : doc.events) {
        events.push_back(eventToJson(ev));
    }
    json::Array transforms;
    for (const AnalysisTransform& xf : doc.transforms) {
        transforms.push_back(transformToJson(xf));
    }
    json::Array derived;
    for (const AnalysisDerivedChannel& ch : doc.derivedChannels) {
        derived.push_back(derivedToJson(ch));
    }
    json::Object root;
    root.emplace("$schema", json::Value(kAnalysisWorkbookSchema));
    root.emplace("schemaVersion", json::Value(kAnalysisWorkbookSchemaVersion));
    root.emplace("id", json::Value(doc.id.empty() ? "analysis-workbook" : doc.id));
    root.emplace("name", json::Value(doc.name.empty() ? "Analysis" : doc.name));
    root.emplace("activePanel", json::Value(doc.active));
    root.emplace("panels", json::Value(std::move(panels)));
    root.emplace("runBindings", json::Value(std::move(bindings)));
    root.emplace("events", json::Value(std::move(events)));
    root.emplace("transforms", json::Value(std::move(transforms)));
    root.emplace("derivedChannels", json::Value(std::move(derived)));
    return json::stringify(json::Value(std::move(root)));
}

bool parseAnalysisWorkbook(std::string_view text, AnalysisWorkbook* out, std::string* error) {
    if (out == nullptr) {
        return false;
    }
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
            *error = "workbook JSON must be an object";
        }
        return false;
    }
    const int version = static_cast<int>(root["schemaVersion"].asInt(1));
    if (version < 1 || version > kAnalysisWorkbookSchemaVersion) {
        if (error != nullptr) {
            *error = "unsupported schemaVersion";
        }
        return false;
    }
    AnalysisWorkbook doc;
    doc.schemaVersion = kAnalysisWorkbookSchemaVersion;
    doc.id = root["id"].asString("analysis-workbook");
    doc.name = root["name"].asString("Analysis");
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
    if (root["runBindings"].isArray()) {
        for (const json::Value& bind : root["runBindings"].asArray()) {
            if (!bind.isObject()) {
                continue;
            }
            auto row = parseBinding(bind);
            if (!row.sessionId.empty()) {
                doc.runBindings.push_back(std::move(row));
            }
        }
    }
    parseObjectArray(root, "events", &doc.events, kMaxEvents, parseEvent);
    parseObjectArray(root, "transforms", &doc.transforms, kMaxTransforms, parseTransform);
    parseObjectArray(root, "derivedChannels", &doc.derivedChannels, kMaxDerived, parseDerived);
    *out = std::move(doc);
    return true;
}

void ensureAnalysisWorkbook(AnalysisWorkbook& doc) {
    if (!doc.views.empty()) {
        doc.active = clampi(doc.active, 0, static_cast<int>(doc.views.size()) - 1);
        return;
    }
    CustomView view;
    view.id = "view-1";
    view.name = "View 1";
    ViewCell cell;
    cell.id = "cell-1";
    cell.title = "Plot";
    cell.kind = ViewCellKind::Line;
    cell.rect = ViewRect{0, 0, kDefaultCellW, kDefaultCellH};
    view.cells.push_back(std::move(cell));
    doc.views.push_back(std::move(view));
    doc.active = 0;
    doc.schemaVersion = kAnalysisWorkbookSchemaVersion;
}

CustomView& activeAnalysisView(AnalysisWorkbook& doc) {
    ensureAnalysisWorkbook(doc);
    return doc.views[static_cast<std::size_t>(doc.active)];
}

const CustomView& activeAnalysisView(const AnalysisWorkbook& doc) {
    return doc.views[static_cast<std::size_t>(doc.active)];
}

bool sameAnalysisSignal(const ViewSignal& src, const ctrl::CatalogSignal& sig) {
    if (src.messageId != 0 && sig.messageId != 0 && !src.signalName.empty()) {
        return src.messageId == sig.messageId && src.signalName == sig.name;
    }
    if (!src.messageName.empty() && !src.signalName.empty()) {
        return src.messageName == sig.message && src.signalName == sig.name;
    }
    return src.id != 0 && src.id == sig.id;
}

ViewSignal viewSignalFromCatalog(const ctrl::CatalogSignal& sig) {
    ViewSignal src;
    src.id = sig.id;
    src.messageId = sig.messageId;
    src.messageName = sig.message;
    src.signalName = sig.name;
    src.label = sig.message.empty() ? sig.name : sig.message + "." + sig.name;
    return src;
}

void resolveAnalysisSignals(AnalysisWorkbook& doc, const std::vector<ctrl::CatalogSignal>& catalog) {
    for (CustomView& view : doc.views) {
        for (ViewCell& cell : view.cells) {
            for (ViewSignal& src : cell.sources) {
                for (const ctrl::CatalogSignal& sig : catalog) {
                    if (sameAnalysisSignal(src, sig)) {
                        src.id = sig.id;
                        if (src.messageId == 0) {
                            src.messageId = sig.messageId;
                        }
                        if (src.messageName.empty()) {
                            src.messageName = sig.message;
                        }
                        if (src.signalName.empty()) {
                            src.signalName = sig.name;
                        }
                        break;
                    }
                }
            }
        }
    }
}

std::vector<std::uint32_t> collectWorkbookSignalIds(const AnalysisWorkbook& doc) {
    std::vector<std::uint32_t> ids;
    auto push = [&](std::uint32_t id) {
        if (id == 0) {
            return;
        }
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
    };
    if (!doc.views.empty()) {
        const CustomView& view = activeAnalysisView(doc);
        for (const ViewCell& cell : view.cells) {
            for (const ViewSignal& src : cell.sources) {
                push(src.id);
            }
        }
    }
    for (const AnalysisTransform& xf : doc.transforms) {
        push(xf.signalId);
    }
    for (const AnalysisDerivedChannel& ch : doc.derivedChannels) {
        if (ch.kind != "formula" || ch.spec.empty()) {
            continue;
        }
        AnalysisFormulaSpec spec;
        if (parseFormulaSpec(ch.spec, &spec)) {
            collectFormulaSignalIds(spec, &ids);
        }
    }
    return ids;
}

void upsertPrimaryBinding(AnalysisWorkbook& doc, const AnalysisRunBinding& bind) {
    AnalysisRunBinding row = bind;
    row.role = AnalysisRunRole::Primary;
    for (auto& existing : doc.runBindings) {
        if (existing.role == AnalysisRunRole::Primary) {
            existing = std::move(row);
            return;
        }
    }
    doc.runBindings.insert(doc.runBindings.begin(), std::move(row));
}

void upsertOverlayBinding(AnalysisWorkbook& doc, const AnalysisRunBinding& bind) {
    AnalysisRunBinding row = bind;
    row.role = AnalysisRunRole::Overlay;
    for (auto& existing : doc.runBindings) {
        if (existing.role == AnalysisRunRole::Overlay) {
            existing = std::move(row);
            return;
        }
    }
    doc.runBindings.push_back(std::move(row));
}

AnalysisRunBinding bindingFromCatalog(const ctrl::Message::CatalogSession& row) {
    AnalysisRunBinding bind;
    bind.sessionId = row.id;
    bind.dbcHash = row.dbcHash;
    bind.role = AnalysisRunRole::Primary;
    bind.minMs = row.minMs;
    bind.maxMs = row.maxMs;
    return bind;
}

bool catalogMatchesListing(const ctrl::Message::CatalogSession& row, std::string_view id, std::uint64_t minMs,
                           std::uint64_t maxMs, std::string_view dbcHash) {
    return row.id == id && row.minMs == minMs && row.maxMs == maxMs && row.dbcHash == dbcHash;
}

namespace {

struct PairSample {
    double t = 0.0;
    double v = 0.0;
};

std::vector<PairSample> finitePairSamples(const PlotArrays& a) {
    std::vector<PairSample> out;
    const std::size_t n = std::min(a.t.size(), a.y.size());
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (std::isfinite(a.t[i]) && std::isfinite(a.y[i])) {
            out.push_back(PairSample{a.t[i], a.y[i]});
        }
    }
    std::sort(out.begin(), out.end(), [](const PairSample& lhs, const PairSample& rhs) { return lhs.t < rhs.t; });
    return out;
}

double medianPositiveDelta(const std::vector<PairSample>& samples) {
    std::vector<double> dt;
    dt.reserve(samples.size());
    for (std::size_t i = 1; i < samples.size(); ++i) {
        const double d = samples[i].t - samples[i - 1].t;
        if (d > 0.0 && std::isfinite(d)) {
            dt.push_back(d);
        }
    }
    if (dt.empty()) {
        return 0.0;
    }
    const auto mid = dt.begin() + static_cast<std::ptrdiff_t>(dt.size() / 2);
    std::nth_element(dt.begin(), mid, dt.end());
    return *mid;
}

double autoScatterPairTolMs(const std::vector<PairSample>& xv, const std::vector<PairSample>& yv) {
    const double spacing = std::max(medianPositiveDelta(xv), medianPositiveDelta(yv));
    if (!(spacing > 0.0)) {
        return kPlotScatterPairDefaultTolMs;
    }
    return std::min(kPlotScatterPairMaxTolMs, std::max(kPlotScatterPairDefaultTolMs, spacing * 1.5));
}

}  // namespace

void pairPlotScatter(const PlotArrays& x, const PlotArrays& y, std::vector<double>& xs, std::vector<double>& ys,
                     double maxDtMs) {
    xs.clear();
    ys.clear();
    const auto xv = finitePairSamples(x);
    const auto yv = finitePairSamples(y);
    if (xv.empty() || yv.empty()) {
        return;
    }
    double tol = maxDtMs;
    if (!(tol > 0.0)) {
        tol = autoScatterPairTolMs(xv, yv);
    }
    tol = std::max(tol, kPlotScatterPairMinTolMs);

    xs.reserve(yv.size());
    ys.reserve(yv.size());
    std::size_t j = 0;
    for (const auto& ySample : yv) {
        while (j + 1 < xv.size() && xv[j + 1].t <= ySample.t) {
            ++j;
        }
        const bool haveNext = j + 1 < xv.size();
        if (haveNext && xv[j].t <= ySample.t && ySample.t <= xv[j + 1].t) {
            const double left = ySample.t - xv[j].t;
            const double right = xv[j + 1].t - ySample.t;
            if (left <= tol && right <= tol) {
                const double span = xv[j + 1].t - xv[j].t;
                if (span > 0.0) {
                    const double a = left / span;
                    xs.push_back(xv[j].v * (1.0 - a) + xv[j + 1].v * a);
                } else {
                    xs.push_back(xv[j].v);
                }
                ys.push_back(ySample.v);
                continue;
            }
        }
        double bestDt = 0.0;
        double bestX = 0.0;
        bool have = false;
        auto consider = [&](std::size_t i) {
            if (i >= xv.size()) {
                return;
            }
            const double dt = std::abs(xv[i].t - ySample.t);
            if (dt <= tol && (!have || dt < bestDt)) {
                bestDt = dt;
                bestX = xv[i].v;
                have = true;
            }
        };
        if (j > 0) {
            consider(j - 1);
        }
        consider(j);
        if (haveNext) {
            consider(j + 1);
        }
        if (have) {
            xs.push_back(bestX);
            ys.push_back(ySample.v);
        }
    }
}

}  // namespace photon::ui
