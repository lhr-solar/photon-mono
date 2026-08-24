#include "photon/control.hpp"

namespace photon::ctrl {
namespace {

void putString(json::Object& obj, const char* key, const std::string& value) {
    if (!value.empty()) {
        obj.emplace(key, json::Value(value));
    }
}

std::vector<std::uint32_t> readIds(const json::Value& value) {
    std::vector<std::uint32_t> ids;
    for (const auto& item : value.asArray()) {
        if (item.isNumber()) {
            ids.push_back(static_cast<std::uint32_t>(item.asInt()));
        }
    }
    return ids;
}

json::Value idsJson(const std::vector<std::uint32_t>& ids) {
    json::Array arr;
    arr.reserve(ids.size());
    for (const auto id : ids) {
        arr.emplace_back(static_cast<std::uint64_t>(id));
    }
    return json::Value(std::move(arr));
}

json::Value segmentResultsJson(const std::vector<tracksim::SegmentResult>& rows) {
    json::Array arr;
    arr.reserve(rows.size());
    for (const auto& row : rows) {
        json::Object o;
        o.emplace("index", json::Value(row.index));
        o.emplace("length_m", json::Value(row.lengthM));
        o.emplace("grade", json::Value(row.grade));
        o.emplace("speed_mps", json::Value(row.speedMps));
        o.emplace("t_cum_s", json::Value(row.tCumS));
        o.emplace("wh_cum", json::Value(row.whCum));
        arr.emplace_back(std::move(o));
    }
    return json::Value(std::move(arr));
}

std::vector<tracksim::SegmentResult> readSegmentResults(const json::Value& value) {
    std::vector<tracksim::SegmentResult> rows;
    if (!value.isArray()) {
        return rows;
    }
    for (const auto& item : value.asArray()) {
        if (!item.isObject()) {
            continue;
        }
        tracksim::SegmentResult row;
        row.index = static_cast<int>(item["index"].asInt());
        row.lengthM = item["length_m"].asNumber();
        row.grade = item["grade"].asNumber();
        row.speedMps = item["speed_mps"].asNumber();
        row.tCumS = item["t_cum_s"].asNumber();
        row.whCum = item["wh_cum"].asNumber();
        rows.push_back(row);
    }
    return rows;
}

json::Value trackInfosJson(const std::vector<tracksim::TrackInfo>& tracks) {
    json::Array arr;
    arr.reserve(tracks.size());
    for (const auto& t : tracks) {
        json::Object o;
        o.emplace("id", json::Value(t.id));
        o.emplace("name", json::Value(t.name));
        o.emplace("length_m", json::Value(t.lengthM));
        o.emplace("segment_count", json::Value(t.segmentCount));
        arr.emplace_back(std::move(o));
    }
    return json::Value(std::move(arr));
}

json::Value catalogJson(const std::vector<CatalogSignal>& catalog) {
    json::Array arr;
    arr.reserve(catalog.size());
    for (const auto& sig : catalog) {
        json::Object o;
        o.emplace("id", json::Value(static_cast<std::uint64_t>(sig.id)));
        o.emplace("message", json::Value(sig.message));
        o.emplace("name", json::Value(sig.name));
        o.emplace("unit", json::Value(sig.unit));
        o.emplace("min", json::Value(sig.min));
        o.emplace("max", json::Value(sig.max));
        arr.emplace_back(std::move(o));
    }
    return json::Value(std::move(arr));
}

std::vector<CatalogSignal> readCatalog(const json::Value& value) {
    std::vector<CatalogSignal> catalog;
    if (!value.isArray()) {
        return catalog;
    }
    for (const auto& item : value.asArray()) {
        if (!item.isObject()) {
            continue;
        }
        CatalogSignal sig;
        sig.id = static_cast<std::uint32_t>(item["id"].asInt());
        sig.message = item["message"].asString();
        sig.name = item["name"].asString();
        sig.unit = item["unit"].asString();
        sig.min = item["min"].asNumber();
        sig.max = item["max"].asNumber();
        catalog.push_back(std::move(sig));
    }
    return catalog;
}

std::vector<TxSignal> readTxSignals(const json::Value& value) {
    std::vector<TxSignal> signals;
    if (!value.isArray()) {
        return signals;
    }
    for (const auto& item : value.asArray()) {
        if (!item.isObject()) {
            continue;
        }
        TxSignal sig;
        sig.name = item["name"].asString();
        sig.value = item["value"].asNumber();
        if (!sig.name.empty()) {
            signals.push_back(std::move(sig));
        }
    }
    return signals;
}

json::Value txSignalsJson(const std::vector<TxSignal>& signals) {
    json::Array arr;
    arr.reserve(signals.size());
    for (const auto& sig : signals) {
        json::Object o;
        o.emplace("name", json::Value(sig.name));
        o.emplace("value", json::Value(sig.value));
        arr.emplace_back(std::move(o));
    }
    return json::Value(std::move(arr));
}

void readPcanFields(const json::Value& obj, Message& msg) {
    if (obj.contains("channel")) {
        msg.channel = static_cast<int>(obj["channel"].asInt());
    }
    if (obj.contains("baud")) {
        msg.baud = static_cast<int>(obj["baud"].asInt());
    }
    if (obj.contains("listen_only")) {
        msg.listenOnly = obj["listen_only"].asBool();
    }
}

std::vector<tracksim::TrackInfo> readTrackInfos(const json::Value& value) {
    std::vector<tracksim::TrackInfo> tracks;
    if (!value.isArray()) {
        return tracks;
    }
    for (const auto& item : value.asArray()) {
        if (item.isString()) {
            tracksim::TrackInfo info;
            info.id = item.asString();
            info.name = info.id;
            tracks.push_back(std::move(info));
            continue;
        }
        if (!item.isObject()) {
            continue;
        }
        tracksim::TrackInfo info;
        info.id = item["id"].asString();
        info.name = item["name"].asString();
        if (info.name.empty()) {
            info.name = info.id;
        }
        info.lengthM = item["length_m"].asNumber();
        info.segmentCount = static_cast<int>(item["segment_count"].asInt());
        tracks.push_back(std::move(info));
    }
    return tracks;
}

}  // namespace

std::string_view typeName(Type type) {
    switch (type) {
        case Type::Hello:
            return "hello";
        case Type::HelloAck:
            return "hello_ack";
        case Type::SelectSource:
            return "select_source";
        case Type::SubscribeSignals:
            return "subscribe_signals";
        case Type::UnsubscribeSignals:
            return "unsubscribe_signals";
        case Type::TimelineControl:
            return "timeline_control";
        case Type::TxArmRequest:
            return "tx_arm_request";
        case Type::TxArmStatus:
            return "tx_arm_status";
        case Type::ErrorAlert:
            return "error_alert";
        case Type::Heartbeat:
            return "heartbeat";
        case Type::TrackSimList:
            return "track_sim_list";
        case Type::TrackSimRequest:
            return "track_sim_request";
        case Type::TrackSimResult:
            return "track_sim_result";
        case Type::SelectDbc:
            return "select_dbc";
        case Type::SignalCatalog:
            return "signal_catalog";
        case Type::TxCanFrame:
            return "tx_can_frame";
        case Type::VideoSubscribe:
            return "video_subscribe";
        case Type::VideoStop:
            return "video_stop";
        case Type::VideoStatus:
            return "video_status";
    }
    return "error_alert";
}

std::optional<Type> typeFromName(std::string_view name) {
    if (name == "hello") {
        return Type::Hello;
    }
    if (name == "hello_ack") {
        return Type::HelloAck;
    }
    if (name == "select_source") {
        return Type::SelectSource;
    }
    if (name == "subscribe_signals") {
        return Type::SubscribeSignals;
    }
    if (name == "unsubscribe_signals") {
        return Type::UnsubscribeSignals;
    }
    if (name == "timeline_control") {
        return Type::TimelineControl;
    }
    if (name == "tx_arm_request") {
        return Type::TxArmRequest;
    }
    if (name == "tx_arm_status") {
        return Type::TxArmStatus;
    }
    if (name == "error_alert") {
        return Type::ErrorAlert;
    }
    if (name == "heartbeat") {
        return Type::Heartbeat;
    }
    if (name == "track_sim_list") {
        return Type::TrackSimList;
    }
    if (name == "track_sim_request") {
        return Type::TrackSimRequest;
    }
    if (name == "track_sim_result") {
        return Type::TrackSimResult;
    }
    if (name == "select_dbc") {
        return Type::SelectDbc;
    }
    if (name == "signal_catalog") {
        return Type::SignalCatalog;
    }
    if (name == "tx_can_frame") {
        return Type::TxCanFrame;
    }
    if (name == "video_subscribe") {
        return Type::VideoSubscribe;
    }
    if (name == "video_stop") {
        return Type::VideoStop;
    }
    if (name == "video_status") {
        return Type::VideoStatus;
    }
    return std::nullopt;
}

std::string_view sourceName(SourceKind kind) {
    switch (kind) {
        case SourceKind::Pcan:
            return "pcan";
        case SourceKind::DaqTcp:
            return "daq_tcp";
        case SourceKind::LogReplay:
            return "log_replay";
        case SourceKind::Synthetic:
            return "synthetic";
        case SourceKind::None:
            break;
    }
    return "none";
}

std::optional<SourceKind> sourceFromName(std::string_view name) {
    if (name == "pcan") {
        return SourceKind::Pcan;
    }
    if (name == "daq_tcp") {
        return SourceKind::DaqTcp;
    }
    if (name == "log_replay") {
        return SourceKind::LogReplay;
    }
    if (name == "synthetic") {
        return SourceKind::Synthetic;
    }
    return std::nullopt;
}

std::string_view timelineName(TimelineMode mode) {
    switch (mode) {
        case TimelineMode::Pause:
            return "pause";
        case TimelineMode::Play:
            return "play";
        case TimelineMode::Live:
            return "live";
        case TimelineMode::Unknown:
            break;
    }
    return "unknown";
}

std::optional<TimelineMode> timelineFromName(std::string_view name) {
    if (name == "pause") {
        return TimelineMode::Pause;
    }
    if (name == "play") {
        return TimelineMode::Play;
    }
    if (name == "live") {
        return TimelineMode::Live;
    }
    return std::nullopt;
}

std::string_view videoModeName(VideoMode mode) {
    switch (mode) {
        case VideoMode::Replay:
            return "replay";
        case VideoMode::Live:
            break;
    }
    return "live";
}

std::optional<VideoMode> videoModeFromName(std::string_view name) {
    if (name == "live") {
        return VideoMode::Live;
    }
    if (name == "replay") {
        return VideoMode::Replay;
    }
    return std::nullopt;
}

std::optional<Message> parse(std::string_view jsonText, std::string* error) {
    json::ParseError parseError;
    const auto root = json::parse(jsonText, &parseError);
    if (!root || !root->isObject()) {
        if (error) {
            *error = parseError.message.empty() ? "control message must be a JSON object" : parseError.message;
        }
        return std::nullopt;
    }
    const auto type = typeFromName((*root)["type"].asString());
    if (!type) {
        if (error) {
            *error = "unknown control type";
        }
        return std::nullopt;
    }

    Message msg;
    msg.type = *type;
    if (root->contains("protocol_version")) {
        msg.protocolVersion = static_cast<int>((*root)["protocol_version"].asInt());
    }
    msg.client = (*root)["client"].asString();
    msg.server = (*root)["server"].asString();
    if ((*root)["features"].isArray()) {
        for (const auto& item : (*root)["features"].asArray()) {
            if (item.isString()) {
                msg.features.push_back(item.asString());
            }
        }
    }
    if (root->contains("source")) {
        if (const auto kind = sourceFromName((*root)["source"].asString())) {
            msg.source = *kind;
        }
    }
    msg.host = (*root)["host"].asString();
    if (root->contains("port")) {
        msg.port = static_cast<std::uint16_t>((*root)["port"].asInt());
    }
    msg.path = (*root)["path"].asString();
    if ((*root)["signal_ids"].isArray()) {
        msg.signalIds = readIds((*root)["signal_ids"]);
    }
    if (root->contains("mode")) {
        const auto modeName = (*root)["mode"].asString();
        if (const auto mode = timelineFromName(modeName)) {
            msg.timelineMode = *mode;
        }
        if (const auto vmode = videoModeFromName(modeName)) {
            msg.videoMode = *vmode;
        }
    }
    if (root->contains("cursor_ms")) {
        msg.cursorMs = static_cast<std::uint64_t>((*root)["cursor_ms"].asInt());
    }
    if (root->contains("speed")) {
        msg.speed = (*root)["speed"].asNumber();
    }
    if ((*root)["id"].isString()) {
        msg.dbcId = (*root)["id"].asString();
    }
    if ((*root)["dbc_id"].isString() && !(*root)["dbc_id"].asString().empty()) {
        msg.dbcId = (*root)["dbc_id"].asString();
    }
    if ((*root)["signals"].isArray()) {
        if (*type == Type::TxCanFrame) {
            msg.txSignals = readTxSignals((*root)["signals"]);
        } else {
            msg.catalog = readCatalog((*root)["signals"]);
        }
    }
    if ((*root)["pcan"].isObject()) {
        readPcanFields((*root)["pcan"], msg);
    }
    readPcanFields(*root, msg);
    msg.armed = (*root)["armed"].asBool();
    msg.reason = (*root)["reason"].asString();
    msg.code = (*root)["code"].asString();
    msg.message = (*root)["message"].asString();

    msg.trackId = (*root)["track_id"].asString();
    if (root->contains("time_budget_s")) {
        msg.timeBudgetS = (*root)["time_budget_s"].asNumber();
    }
    if (root->contains("initial_soc")) {
        msg.initialSoc = (*root)["initial_soc"].asNumber();
    }
    if ((*root)["track"].isObject()) {
        std::string trackError;
        if (auto track = tracksim::trackFromValue((*root)["track"], &trackError)) {
            msg.track = std::move(*track);
        } else if (error && *type == Type::TrackSimRequest) {
            *error = trackError.empty() ? "invalid inline track" : trackError;
            return std::nullopt;
        }
    }
    if ((*root)["car"].isObject()) {
        msg.carOverride = tracksim::carOverrideFromValue((*root)["car"]);
    }
    if (root->contains("feasible")) {
        msg.feasible = (*root)["feasible"].asBool();
    }
    if (root->contains("total_time_s")) {
        msg.totalTimeS = (*root)["total_time_s"].asNumber();
    }
    if (root->contains("energy_wh")) {
        msg.energyWh = (*root)["energy_wh"].asNumber();
    }
    if (root->contains("end_soc")) {
        msg.endSoc = (*root)["end_soc"].asNumber();
    }
    if (root->contains("dE_dT")) {
        msg.dEdT = (*root)["dE_dT"].asNumber();
    }
    if ((*root)["segments"].isArray()) {
        msg.trackSegments = readSegmentResults((*root)["segments"]);
    }
    if ((*root)["tracks"].isArray()) {
        msg.tracks = readTrackInfos((*root)["tracks"]);
    }
    if (root->contains("start_ms")) {
        msg.startMs = static_cast<std::uint64_t>((*root)["start_ms"].asInt());
    }
    if (root->contains("connected")) {
        msg.videoConnected = (*root)["connected"].asBool();
    }
    if (root->contains("last_ts_ms")) {
        msg.lastTsMs = static_cast<std::uint64_t>((*root)["last_ts_ms"].asInt());
    }
    if (root->contains("frames")) {
        msg.videoFrames = static_cast<std::uint64_t>((*root)["frames"].asInt());
    }
    if (root->contains("dropped")) {
        msg.videoDropped = static_cast<std::uint64_t>((*root)["dropped"].asInt());
    }
    if ((*root)["active_source"].isObject()) {
        const auto& as = (*root)["active_source"];
        if (const auto kind = sourceFromName(as["kind"].asString())) {
            msg.activeSource.kind = *kind;
        }
        msg.activeSource.host = as["host"].asString();
        if (as.contains("port")) {
            msg.activeSource.port = static_cast<std::uint16_t>(as["port"].asInt());
        }
        msg.activeSource.path = as["path"].asString();
    }
    return msg;
}

std::string serialize(const Message& msg) {
    json::Object obj;
    obj.emplace("type", json::Value(std::string(typeName(msg.type))));
    obj.emplace("protocol_version", json::Value(msg.protocolVersion));

    putString(obj, "client", msg.client);
    putString(obj, "server", msg.server);
    if (!msg.features.empty()) {
        json::Array features;
        for (const auto& feature : msg.features) {
            features.emplace_back(feature);
        }
        obj.emplace("features", json::Value(std::move(features)));
    }
    if (msg.type == Type::HelloAck) {
        json::Object as;
        as.emplace("kind", json::Value(std::string(sourceName(msg.activeSource.kind))));
        putString(as, "host", msg.activeSource.host);
        if (msg.activeSource.port != 0) {
            as.emplace("port", json::Value(static_cast<int>(msg.activeSource.port)));
        }
        putString(as, "path", msg.activeSource.path);
        obj.emplace("active_source", json::Value(std::move(as)));
    }
    if (msg.type == Type::SelectSource) {
        obj.emplace("source", json::Value(std::string(sourceName(msg.source))));
        putString(obj, "host", msg.host);
        if (msg.port != 0) {
            obj.emplace("port", json::Value(static_cast<int>(msg.port)));
        }
        putString(obj, "path", msg.path);
        if (msg.source == SourceKind::Pcan) {
            obj.emplace("channel", json::Value(msg.channel));
            obj.emplace("baud", json::Value(msg.baud));
            obj.emplace("listen_only", json::Value(msg.listenOnly));
        }
    }
    if (msg.type == Type::SubscribeSignals || msg.type == Type::UnsubscribeSignals) {
        obj.emplace("signal_ids", idsJson(msg.signalIds));
    }
    if (msg.type == Type::TimelineControl) {
        obj.emplace("mode", json::Value(std::string(timelineName(msg.timelineMode))));
        obj.emplace("cursor_ms", json::Value(static_cast<std::uint64_t>(msg.cursorMs)));
        obj.emplace("speed", json::Value(msg.speed));
    }
    if (msg.type == Type::SelectDbc) {
        putString(obj, "path", msg.path);
        putString(obj, "id", msg.dbcId);
        putString(obj, "dbc_id", msg.dbcId);
    }
    if (msg.type == Type::SignalCatalog) {
        obj.emplace("signals", catalogJson(msg.catalog));
    }
    if (msg.type == Type::TxCanFrame) {
        putString(obj, "message", msg.message);
        obj.emplace("signals", txSignalsJson(msg.txSignals));
    }
    if (msg.type == Type::TxArmRequest || msg.type == Type::TxArmStatus) {
        obj.emplace("armed", json::Value(msg.armed));
        putString(obj, "reason", msg.reason);
    }
    if (msg.type == Type::ErrorAlert) {
        putString(obj, "code", msg.code);
        putString(obj, "message", msg.message);
    }
    if (msg.type == Type::TrackSimList) {
        if (!msg.tracks.empty()) {
            obj.emplace("tracks", trackInfosJson(msg.tracks));
        }
    }
    if (msg.type == Type::TrackSimRequest) {
        putString(obj, "track_id", msg.trackId);
        obj.emplace("time_budget_s", json::Value(msg.timeBudgetS));
        obj.emplace("initial_soc", json::Value(msg.initialSoc));
        if (msg.track) {
            obj.emplace("track", tracksim::trackToValue(*msg.track));
        }
        if (!tracksim::carOverrideEmpty(msg.carOverride)) {
            obj.emplace("car", tracksim::carOverrideToValue(msg.carOverride));
        }
    }
    if (msg.type == Type::TrackSimResult) {
        putString(obj, "track_id", msg.trackId);
        obj.emplace("feasible", json::Value(msg.feasible));
        obj.emplace("total_time_s", json::Value(msg.totalTimeS));
        obj.emplace("energy_wh", json::Value(msg.energyWh));
        obj.emplace("end_soc", json::Value(msg.endSoc));
        obj.emplace("dE_dT", json::Value(msg.dEdT));
        obj.emplace("segments", segmentResultsJson(msg.trackSegments));
    }
    if (msg.type == Type::VideoSubscribe) {
        obj.emplace("mode", json::Value(std::string(videoModeName(msg.videoMode))));
        if (msg.videoMode == VideoMode::Replay || msg.startMs != 0) {
            obj.emplace("start_ms", json::Value(static_cast<std::uint64_t>(msg.startMs)));
        }
        putString(obj, "host", msg.host);
        if (msg.port != 0) {
            obj.emplace("port", json::Value(static_cast<int>(msg.port)));
        }
    }
    if (msg.type == Type::VideoStatus) {
        obj.emplace("connected", json::Value(msg.videoConnected));
        obj.emplace("host", json::Value(msg.host));
        obj.emplace("port", json::Value(static_cast<int>(msg.port)));
        obj.emplace("mode", json::Value(std::string(videoModeName(msg.videoMode))));
        obj.emplace("last_ts_ms", json::Value(static_cast<std::uint64_t>(msg.lastTsMs)));
        obj.emplace("frames", json::Value(static_cast<std::uint64_t>(msg.videoFrames)));
        obj.emplace("dropped", json::Value(static_cast<std::uint64_t>(msg.videoDropped)));
    }
    return json::stringify(json::Value(std::move(obj)));
}

Message hello(std::string client) {
    Message msg;
    msg.type = Type::Hello;
    msg.client = std::move(client);
    return msg;
}

Message helloAck(std::vector<std::string> features, Message::ActiveSource active) {
    Message msg;
    msg.type = Type::HelloAck;
    msg.server = "photon-backend";
    msg.features = std::move(features);
    msg.activeSource = std::move(active);
    return msg;
}

Message selectSource(SourceKind kind) {
    Message msg;
    msg.type = Type::SelectSource;
    msg.source = kind;
    return msg;
}

Message subscribe(std::vector<std::uint32_t> ids) {
    Message msg;
    msg.type = Type::SubscribeSignals;
    msg.signalIds = std::move(ids);
    return msg;
}

Message unsubscribe(std::vector<std::uint32_t> ids) {
    Message msg;
    msg.type = Type::UnsubscribeSignals;
    msg.signalIds = std::move(ids);
    return msg;
}

Message timelineControl(TimelineMode mode, std::uint64_t cursorMs, double speed) {
    Message msg;
    msg.type = Type::TimelineControl;
    msg.timelineMode = mode;
    msg.cursorMs = cursorMs;
    msg.speed = speed;
    return msg;
}

Message selectDbc(std::string pathOrId, bool bundledId) {
    Message msg;
    msg.type = Type::SelectDbc;
    if (bundledId) {
        msg.dbcId = std::move(pathOrId);
    } else {
        msg.path = std::move(pathOrId);
    }
    return msg;
}

Message signalCatalog(std::vector<CatalogSignal> catalog) {
    Message msg;
    msg.type = Type::SignalCatalog;
    msg.catalog = std::move(catalog);
    return msg;
}

Message txArmRequest(bool armed) {
    Message msg;
    msg.type = Type::TxArmRequest;
    msg.armed = armed;
    return msg;
}

Message txArmStatus(bool armed, std::string reason) {
    Message msg;
    msg.type = Type::TxArmStatus;
    msg.armed = armed;
    msg.reason = std::move(reason);
    return msg;
}

Message errorAlert(std::string code, std::string message) {
    Message msg;
    msg.type = Type::ErrorAlert;
    msg.code = std::move(code);
    msg.message = std::move(message);
    return msg;
}

Message heartbeat() {
    Message msg;
    msg.type = Type::Heartbeat;
    return msg;
}

Message trackSimList() {
    Message msg;
    msg.type = Type::TrackSimList;
    return msg;
}

Message trackSimListResult(std::vector<tracksim::TrackInfo> tracks) {
    Message msg;
    msg.type = Type::TrackSimList;
    msg.tracks = std::move(tracks);
    return msg;
}

Message trackSimRequest(std::string trackId, double timeBudgetS, double initialSoc) {
    Message msg;
    msg.type = Type::TrackSimRequest;
    msg.trackId = std::move(trackId);
    msg.timeBudgetS = timeBudgetS;
    msg.initialSoc = initialSoc;
    return msg;
}

Message trackSimResult(const tracksim::SolveOutput& solved, std::string trackId) {
    Message msg;
    msg.type = Type::TrackSimResult;
    msg.trackId = std::move(trackId);
    msg.feasible = solved.feasible;
    msg.totalTimeS = solved.totalTimeS;
    msg.energyWh = solved.energyWh;
    msg.endSoc = solved.endSoc;
    msg.dEdT = solved.dEdT;
    msg.trackSegments = solved.segments;
    return msg;
}

Message txCanFrame(std::string messageName, std::vector<TxSignal> signals) {
    Message msg;
    msg.type = Type::TxCanFrame;
    msg.message = std::move(messageName);
    msg.txSignals = std::move(signals);
    return msg;
}

Message videoSubscribe(VideoMode mode, std::uint64_t startMs) {
    Message msg;
    msg.type = Type::VideoSubscribe;
    msg.videoMode = mode;
    msg.startMs = startMs;
    return msg;
}

Message videoStop() {
    Message msg;
    msg.type = Type::VideoStop;
    return msg;
}

Message videoStatus(bool connected, std::string host, std::uint16_t port, VideoMode mode,
                    std::uint64_t lastTsMs, std::uint64_t frames, std::uint64_t dropped) {
    Message msg;
    msg.type = Type::VideoStatus;
    msg.videoConnected = connected;
    msg.host = std::move(host);
    msg.port = port;
    msg.videoMode = mode;
    msg.lastTsMs = lastTsMs;
    msg.videoFrames = frames;
    msg.videoDropped = dropped;
    return msg;
}

}  // namespace photon::ctrl
