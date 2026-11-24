#include "photon/control_v2.hpp"

#include "photon/json.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>
#include <utility>

namespace photon::ctrl2 {
namespace {

// Control messages nest three levels deep. Deeper input is refused before the
// JSON parser recurses into it.
constexpr std::size_t kMaxNesting = 32;

bool nestingTooDeep(std::string_view text) {
    std::size_t depth = 0;
    bool inString = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (c == '\\') {
                ++i;  // the escaped byte is not a quote
            } else if (c == '"') {
                inString = false;
            }
        } else if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            if (++depth > kMaxNesting) {
                return true;
            }
        } else if ((c == ']' || c == '}') && depth > 0) {
            --depth;
        }
    }
    return false;
}

// Reads a JSON number as an integer of type T. A fraction is cut toward zero; a
// number outside T's range, or not finite, is refused.
template <class T>
bool asInteger(const json::Value& v, T& out) {
    if (!v.isNumber()) {
        return false;
    }
    const double d = v.asNumber();
    const double lo = static_cast<double>(std::numeric_limits<T>::min());
    const double hi = static_cast<double>(std::numeric_limits<T>::max()) + 1.0;
    if (!(d >= lo && d < hi)) {
        return false;
    }
    out = static_cast<T>(d);
    return true;
}

// Reads the fields of one JSON object. A missing field keeps the caller's default.
// A field of the wrong kind sets err (the first error wins) and keeps the default;
// the caller checks err once it has read the object.
class Fields {
public:
    Fields(const json::Value& obj, std::string& err) : obj_(obj), err_(err) {}

    void text(std::string_view key, std::string& out) const {
        if (const json::Value* v = field(key)) {
            if (v->isString()) {
                out = v->asString();
            } else {
                fail(key, "a string");
            }
        }
    }

    void flag(std::string_view key, bool& out) const {
        if (const json::Value* v = field(key)) {
            if (v->isBool()) {
                out = v->asBool();
            } else {
                fail(key, "a boolean");
            }
        }
    }

    void number(std::string_view key, double& out) const {
        if (const json::Value* v = field(key)) {
            if (v->isNumber() && std::isfinite(v->asNumber())) {
                out = v->asNumber();
            } else {
                fail(key, "a finite number");
            }
        }
    }

    template <class T>
    void integer(std::string_view key, T& out) const {
        if (const json::Value* v = field(key)) {
            if (!asInteger(*v, out)) {
                fail(key, "an integer in range");
            }
        }
    }

    void texts(std::string_view key, std::vector<std::string>& out) const {
        const json::Value* v = field(key);
        if (!v) {
            return;
        }
        if (!v->isArray()) {
            fail(key, "an array");
            return;
        }
        std::vector<std::string> items;
        for (const json::Value& item : v->asArray()) {
            if (!item.isString()) {
                fail(key, "an array of strings");
                return;
            }
            items.push_back(item.asString());
        }
        out = std::move(items);
    }

    template <class T>
    void integers(std::string_view key, std::vector<T>& out) const {
        const json::Value* v = field(key);
        if (!v) {
            return;
        }
        if (!v->isArray()) {
            fail(key, "an array");
            return;
        }
        std::vector<T> items;
        for (const json::Value& item : v->asArray()) {
            T x{};
            if (!asInteger(item, x)) {
                fail(key, "an array of integers in range");
                return;
            }
            items.push_back(x);
        }
        out = std::move(items);
    }

    bool has(std::string_view key) const { return field(key) != nullptr; }

    // Calls fn with a Fields for the object at key. A field that is not an object sets err.
    template <class Fn>
    void object(std::string_view key, Fn&& fn) const {
        const json::Value* v = field(key);
        if (!v) {
            return;
        }
        if (!v->isObject()) {
            fail(key, "an object");
            return;
        }
        fn(Fields(*v, err_));
    }

    // Calls fn with a Fields for each object in the array at key.
    template <class Fn>
    void objects(std::string_view key, Fn&& fn) const {
        const json::Value* v = field(key);
        if (!v) {
            return;
        }
        if (!v->isArray()) {
            fail(key, "an array");
            return;
        }
        for (const json::Value& item : v->asArray()) {
            if (!item.isObject()) {
                fail(key, "an array of objects");
                continue;
            }
            fn(Fields(item, err_));
        }
    }

private:
    const json::Value* field(std::string_view key) const {
        const auto& obj = obj_.asObject();
        const auto it = obj.find(std::string(key));
        return it == obj.end() ? nullptr : &it->second;
    }

    void fail(std::string_view key, const char* want) const {
        if (err_.empty()) {
            err_ = std::string(key) + " must be " + want;
        }
    }

    const json::Value& obj_;
    std::string& err_;
};

// JSON has no NaN or infinity. A non-finite double is written as 0, the value the
// server sends for a range it cannot encode.
json::Value finiteValue(double v) {
    return json::Value(std::isfinite(v) ? v : 0.0);
}

json::Object head(const char* type) {
    json::Object obj;
    obj.emplace("type", json::Value(type));
    return obj;
}

json::Value strArray(const std::vector<std::string>& items) {
    json::Array arr;
    arr.reserve(items.size());
    for (const std::string& s : items) {
        arr.emplace_back(s);
    }
    return json::Value(std::move(arr));
}

template <class T>
json::Value intArray(const std::vector<T>& items) {
    json::Array arr;
    arr.reserve(items.size());
    for (const T v : items) {
        arr.emplace_back(static_cast<std::int64_t>(v));
    }
    return json::Value(std::move(arr));
}

// Serializers: each returns the object with its "type" and every field.

json::Object toJson(const Hello& m) {
    auto obj = head("hello");
    obj.emplace("protocol", json::Value(kProtocolVersion));
    obj.emplace("client", json::Value(m.client));
    obj.emplace("token", json::Value(m.token));
    return obj;
}

json::Object toJson(const HelloAck& m) {
    auto obj = head("hello_ack");
    obj.emplace("protocol", json::Value(m.protocol));
    obj.emplace("server", json::Value(m.server));
    obj.emplace("user", json::Value(m.user));
    obj.emplace("server_time_ns", json::Value(m.serverTimeNs));
    obj.emplace("catalog_sha", json::Value(m.catalogSha));
    obj.emplace("live_run", json::Value(m.liveRun));
    obj.emplace("levels", intArray(m.levels));
    obj.emplace("features", strArray(m.features));
    return obj;
}

json::Object toJson(const Catalog& m) {
    auto obj = head("catalog");
    obj.emplace("sha", json::Value(m.sha));
    obj.emplace("buses", strArray(m.buses));
    json::Array messages;
    messages.reserve(m.messages.size());
    for (const CatalogMessage& msg : m.messages) {
        json::Array signals;
        signals.reserve(msg.signals.size());
        for (const CatalogSignal& sig : msg.signals) {
            json::Object so;
            so.emplace("index", json::Value(sig.index));
            so.emplace("name", json::Value(sig.name));
            so.emplace("unit", json::Value(sig.unit));
            so.emplace("min", finiteValue(sig.min));
            so.emplace("max", finiteValue(sig.max));
            so.emplace("integer", json::Value(sig.integer));
            so.emplace("mux", json::Value(sig.mux));
            so.emplace("period_ms", finiteValue(sig.periodMs));
            signals.emplace_back(std::move(so));
        }
        json::Object mo;
        mo.emplace("id", json::Value(msg.id));
        mo.emplace("name", json::Value(msg.name));
        mo.emplace("bus", json::Value(msg.bus));
        mo.emplace("signals", json::Value(std::move(signals)));
        messages.emplace_back(std::move(mo));
    }
    obj.emplace("messages", json::Value(std::move(messages)));
    return obj;
}

json::Object toJson(const Subscribe& m) {
    auto obj = head("subscribe");
    obj.emplace("signals", intArray(m.signals));
    return obj;
}

json::Object toJson(const RunsList&) {
    return head("runs_list");
}

json::Object toJson(const Runs& m) {
    auto obj = head("runs");
    json::Array runs;
    runs.reserve(m.runs.size());
    for (const Run& r : m.runs) {
        json::Object ro;
        ro.emplace("id", json::Value(r.id));
        ro.emplace("label", json::Value(r.label));
        ro.emplace("source", json::Value(r.source));
        ro.emplace("device", json::Value(r.device));
        ro.emplace("dbc_sha", json::Value(r.dbcSha));
        ro.emplace("start_ms", json::Value(r.startMs));
        ro.emplace("end_ms", json::Value(r.endMs));
        ro.emplace("rev", json::Value(r.rev));
        ro.emplace("live", json::Value(r.live));
        runs.emplace_back(std::move(ro));
    }
    obj.emplace("runs", json::Value(std::move(runs)));
    return obj;
}

json::Object toJson(const TileRequest& m) {
    auto obj = head("tile_request");
    obj.emplace("qid", json::Value(m.qid));
    obj.emplace("run", json::Value(m.run));
    obj.emplace("signal", json::Value(m.signal));
    obj.emplace("level", json::Value(m.level));
    obj.emplace("index", json::Value(m.index));
    return obj;
}

json::Object toJson(const TileCancel& m) {
    auto obj = head("tile_cancel");
    obj.emplace("qid", json::Value(m.qid));
    return obj;
}

json::Object toJson(const TileError& m) {
    auto obj = head("tile_error");
    obj.emplace("qid", json::Value(m.qid));
    obj.emplace("message", json::Value(m.message));
    return obj;
}

json::Object toJson(const VideoSubscribe& m) {
    auto obj = head("video_subscribe");
    obj.emplace("mode", json::Value(m.mode));
    obj.emplace("from_ms", json::Value(m.fromMs));
    return obj;
}

json::Object toJson(const VideoSeek& m) {
    auto obj = head("video_seek");
    obj.emplace("qid", json::Value(m.qid));
    obj.emplace("t_ms", json::Value(m.tMs));
    return obj;
}

json::Object toJson(const VideoStatus& m) {
    auto obj = head("video_status");
    obj.emplace("mode", json::Value(m.mode));
    obj.emplace("t_ms", json::Value(m.tMs));
    obj.emplace("clock", json::Value(m.clock));
    if (m.qid) {
        obj.emplace("qid", json::Value(*m.qid));
    }
    if (m.gap) {
        json::Object gap;
        gap.emplace("from_ms", json::Value(m.gap->fromMs));
        gap.emplace("to_ms", json::Value(m.gap->toMs));
        gap.emplace("reason", json::Value(m.gap->reason));
        obj.emplace("gap", json::Value(std::move(gap)));
    }
    return obj;
}

json::Object toJson(const Ping& m) {
    auto obj = head("ping");
    obj.emplace("t_ms", json::Value(m.tMs));
    return obj;
}

json::Object toJson(const Pong& m) {
    auto obj = head("pong");
    obj.emplace("t_ms", json::Value(m.tMs));
    return obj;
}

json::Object toJson(const Error& m) {
    auto obj = head("error");
    obj.emplace("code", json::Value(m.code));
    obj.emplace("message", json::Value(m.message));
    return obj;
}

// Readers: each fills a message from its object. Missing fields keep the defaults.

Hello readHello(const Fields& f) {
    Hello m;
    f.text("client", m.client);
    f.text("token", m.token);
    return m;
}

HelloAck readHelloAck(const Fields& f) {
    HelloAck m;
    f.integer("protocol", m.protocol);
    f.text("server", m.server);
    f.text("user", m.user);
    f.integer("server_time_ns", m.serverTimeNs);
    f.text("catalog_sha", m.catalogSha);
    f.text("live_run", m.liveRun);
    f.integers("levels", m.levels);
    f.texts("features", m.features);
    return m;
}

Catalog readCatalog(const Fields& f) {
    Catalog m;
    f.text("sha", m.sha);
    f.texts("buses", m.buses);
    f.objects("messages", [&](const Fields& mf) {
        CatalogMessage msg;
        mf.integer("id", msg.id);
        mf.text("name", msg.name);
        mf.integer("bus", msg.bus);
        mf.objects("signals", [&](const Fields& sf) {
            CatalogSignal sig;
            sf.integer("index", sig.index);
            sf.text("name", sig.name);
            sf.text("unit", sig.unit);
            sf.number("min", sig.min);
            sf.number("max", sig.max);
            sf.flag("integer", sig.integer);
            sf.integer("mux", sig.mux);
            sf.number("period_ms", sig.periodMs);
            msg.signals.push_back(std::move(sig));
        });
        m.messages.push_back(std::move(msg));
    });
    return m;
}

Subscribe readSubscribe(const Fields& f) {
    Subscribe m;
    f.integers("signals", m.signals);
    return m;
}

Runs readRuns(const Fields& f) {
    Runs m;
    f.objects("runs", [&](const Fields& rf) {
        Run r;
        rf.text("id", r.id);
        rf.text("label", r.label);
        rf.text("source", r.source);
        rf.text("device", r.device);
        rf.text("dbc_sha", r.dbcSha);
        rf.integer("start_ms", r.startMs);
        rf.integer("end_ms", r.endMs);
        rf.integer("rev", r.rev);
        rf.flag("live", r.live);
        m.runs.push_back(std::move(r));
    });
    return m;
}

TileRequest readTileRequest(const Fields& f) {
    TileRequest m;
    f.integer("qid", m.qid);
    f.text("run", m.run);
    f.integer("signal", m.signal);
    f.integer("level", m.level);
    f.integer("index", m.index);
    return m;
}

TileCancel readTileCancel(const Fields& f) {
    TileCancel m;
    f.integer("qid", m.qid);
    return m;
}

TileError readTileError(const Fields& f) {
    TileError m;
    f.integer("qid", m.qid);
    f.text("message", m.message);
    return m;
}

VideoSubscribe readVideoSubscribe(const Fields& f) {
    VideoSubscribe m;
    f.text("mode", m.mode);
    f.integer("from_ms", m.fromMs);
    return m;
}

VideoSeek readVideoSeek(const Fields& f) {
    VideoSeek m;
    f.integer("qid", m.qid);
    f.integer("t_ms", m.tMs);
    return m;
}

VideoStatus readVideoStatus(const Fields& f) {
    VideoStatus m;
    f.text("mode", m.mode);
    f.integer("t_ms", m.tMs);
    f.text("clock", m.clock);
    if (f.has("qid")) {
        std::uint32_t qid = 0;
        f.integer("qid", qid);
        m.qid = qid;
    }
    if (f.has("gap")) {
        VideoGap gap;
        f.object("gap", [&gap](const Fields& g) {
            g.integer("from_ms", gap.fromMs);
            g.integer("to_ms", gap.toMs);
            g.text("reason", gap.reason);
        });
        m.gap = gap;
    }
    return m;
}

Ping readPing(const Fields& f) {
    Ping m;
    f.integer("t_ms", m.tMs);
    return m;
}

Pong readPong(const Fields& f) {
    Pong m;
    f.integer("t_ms", m.tMs);
    return m;
}

Error readError(const Fields& f) {
    Error m;
    f.text("code", m.code);
    f.text("message", m.message);
    return m;
}

std::optional<Message> reject(std::string* error, std::string why) {
    if (error) {
        *error = std::move(why);
    }
    return std::nullopt;
}

// A message whose fields all read cleanly, or a rejection with the first field error.
std::optional<Message> accept(Message msg, const std::string& fieldError, std::string* error) {
    if (!fieldError.empty()) {
        return reject(error, fieldError);
    }
    return msg;
}

}  // namespace

std::string serialize(const Message& msg) {
    return std::visit([](const auto& m) { return json::stringify(json::Value(toJson(m))); }, msg);
}

std::optional<Message> parse(std::string_view text, std::string* error) {
    if (nestingTooDeep(text)) {
        return reject(error, "control message nests too deeply");
    }
    json::ParseError parseError;
    const auto root = json::parse(text, &parseError);
    if (!root) {
        return reject(error, parseError.message.empty() ? "invalid JSON" : parseError.message);
    }
    if (!root->isObject()) {
        return reject(error, "control message must be a JSON object");
    }
    const json::Value& typeValue = (*root)["type"];
    if (!typeValue.isString()) {
        return reject(error, "control message has no type");
    }
    const std::string type = typeValue.asString();

    std::string fieldError;
    const Fields f(*root, fieldError);
    if (type == "hello") {
        return accept(readHello(f), fieldError, error);
    }
    if (type == "hello_ack") {
        return accept(readHelloAck(f), fieldError, error);
    }
    if (type == "catalog") {
        return accept(readCatalog(f), fieldError, error);
    }
    if (type == "subscribe") {
        return accept(readSubscribe(f), fieldError, error);
    }
    if (type == "runs_list") {
        return accept(RunsList{}, fieldError, error);
    }
    if (type == "runs") {
        return accept(readRuns(f), fieldError, error);
    }
    if (type == "tile_request") {
        return accept(readTileRequest(f), fieldError, error);
    }
    if (type == "tile_cancel") {
        return accept(readTileCancel(f), fieldError, error);
    }
    if (type == "tile_error") {
        return accept(readTileError(f), fieldError, error);
    }
    if (type == "video_subscribe") {
        return accept(readVideoSubscribe(f), fieldError, error);
    }
    if (type == "video_seek") {
        return accept(readVideoSeek(f), fieldError, error);
    }
    if (type == "video_status") {
        return accept(readVideoStatus(f), fieldError, error);
    }
    if (type == "ping") {
        return accept(readPing(f), fieldError, error);
    }
    if (type == "pong") {
        return accept(readPong(f), fieldError, error);
    }
    if (type == "error") {
        return accept(readError(f), fieldError, error);
    }
    return reject(error, "unknown control type");
}

}  // namespace photon::ctrl2
