#include "wire/control.hpp"

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







json::Value catalogJson(const std::vector<CatalogSignal>& catalog) {
    json::Array arr;
    arr.reserve(catalog.size());
    for (const auto& sig : catalog) {
        json::Object o;
        o.emplace("id", json::Value(static_cast<std::uint64_t>(sig.id)));
        if (sig.messageId != 0) {
            o.emplace("message_id", json::Value(static_cast<std::uint64_t>(sig.messageId)));
        }
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
        if (item.contains("message_id")) {
            sig.messageId = static_cast<std::uint32_t>(item["message_id"].asInt());
        }
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
        case Type::HistoryQuery:
            return "history_query";
        case Type::HistoryResult:
            return "history_result";
        case Type::HistoryCancel:
            return "history_cancel";
        case Type::HistoryCredit:
            return "history_credit";
        case Type::VideoLocate:
            return "video_locate";
        case Type::VideoClock:
            return "video_clock";
        case Type::AnalysisComputeRequest:
            return "analysis_compute_request";
        case Type::AnalysisComputeResult:
            return "analysis_compute_result";
        case Type::AnalysisComputeCancel:
            return "analysis_compute_cancel";
        case Type::CatalogList:
            return "catalog_list";
        case Type::CatalogResult:
            return "catalog_result";
        case Type::SubscribeCan:
            return "subscribe_can";
        case Type::UnsubscribeCan:
            return "unsubscribe_can";
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
    if (name == "history_query") {
        return Type::HistoryQuery;
    }
    if (name == "history_result") {
        return Type::HistoryResult;
    }
    if (name == "history_cancel") {
        return Type::HistoryCancel;
    }
    if (name == "history_credit") {
        return Type::HistoryCredit;
    }
    if (name == "video_locate") {
        return Type::VideoLocate;
    }
    if (name == "video_clock") {
        return Type::VideoClock;
    }
    if (name == "analysis_compute_request") {
        return Type::AnalysisComputeRequest;
    }
    if (name == "analysis_compute_result") {
        return Type::AnalysisComputeResult;
    }
    if (name == "analysis_compute_cancel") {
        return Type::AnalysisComputeCancel;
    }
    if (name == "catalog_list") {
        return Type::CatalogList;
    }
    if (name == "catalog_result") {
        return Type::CatalogResult;
    }
    if (name == "subscribe_can") {
        return Type::SubscribeCan;
    }
    if (name == "unsubscribe_can") {
        return Type::UnsubscribeCan;
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
    if ((*root)["dbc_path"].isString() && !(*root)["dbc_path"].asString().empty()) {
        msg.dbcPath = (*root)["dbc_path"].asString();
    }
    if ((*root)["dbcs"].isArray()) {
        for (const auto& item : (*root)["dbcs"].asArray()) {
            if (item.isString() && !item.asString().empty()) {
                msg.dbcs.push_back(item.asString());
            }
        }
    }
    if ((*root)["dbc"].isObject()) {
        const auto& dbc = (*root)["dbc"];
        if (dbc["id"].isString() && !dbc["id"].asString().empty()) {
            msg.dbcId = dbc["id"].asString();
        }
        if (dbc["path"].isString() && !dbc["path"].asString().empty()) {
            msg.dbcPath = dbc["path"].asString();
        }
        if (dbc["bundled"].isArray()) {
            msg.dbcs.clear();
            for (const auto& item : dbc["bundled"].asArray()) {
                if (item.isString() && !item.asString().empty()) {
                    msg.dbcs.push_back(item.asString());
                }
            }
        }
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

    if (root->contains("start_ms")) {
        msg.startMs = static_cast<std::uint64_t>((*root)["start_ms"].asInt());
    }
    if (root->contains("at_ms")) {
        msg.startMs = static_cast<std::uint64_t>((*root)["at_ms"].asInt());
    }
    if (root->contains("stream_id")) {
        msg.videoStreamId = static_cast<std::uint8_t>((*root)["stream_id"].asInt());
    }
    if (root->contains("prefetch_gops")) {
        msg.videoPrefetchGops = static_cast<std::uint16_t>((*root)["prefetch_gops"].asInt());
    }
    if (root->contains("offset_ms")) {
        msg.videoClockOffsetMs = (*root)["offset_ms"].asInt();
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
    if ((*root)["drive_days"].isArray()) {
        for (const auto& item : (*root)["drive_days"].asArray()) {
            Message::DriveDay day;
            day.date = item["date"].asString();
            day.path = item["path"].asString();
            day.sessionCount = static_cast<int>(item["session_count"].asInt());
            day.durationS = item["duration_s"].asNumber();
            if (item["sessions"].isArray()) {
                for (const auto& sess : item["sessions"].asArray()) {
                    Message::DriveSession row;
                    row.id = sess["id"].asString();
                    row.path = sess["path"].asString();
                    if (sess.contains("start_ms")) {
                        row.startMs = static_cast<std::uint64_t>(sess["start_ms"].asInt());
                    }
                    row.durationS = sess["duration_s"].asNumber();
                    row.speedMaxMps = sess["speed_max_mps"].asNumber();
                    if (!row.id.empty() || !row.path.empty()) {
                        day.sessions.push_back(std::move(row));
                    }
                }
            }
            if (!day.date.empty() || !day.path.empty()) {
                msg.driveDays.push_back(std::move(day));
            }
        }
    }
    if (root->contains("query_id")) {
        msg.queryId = static_cast<std::uint32_t>((*root)["query_id"].asInt());
    }
    msg.sessionId = (*root)["session_id"].asString();
    if (root->contains("from_ms")) {
        msg.fromMs = static_cast<std::uint64_t>((*root)["from_ms"].asInt());
    }
    if (root->contains("to_ms")) {
        msg.toMs = static_cast<std::uint64_t>((*root)["to_ms"].asInt());
    }
    if (root->contains("max_points")) {
        msg.maxPoints = static_cast<std::uint32_t>((*root)["max_points"].asInt());
    }
    if ((*root)["fields"].isArray()) {
        for (const auto& item : (*root)["fields"].asArray()) {
            if (item.isString()) {
                msg.fields.push_back(item.asString());
            }
        }
    }
    if (root->contains("include_video")) {
        msg.includeVideo = (*root)["include_video"].asBool();
    }
    if (root->contains("credit_bytes")) {
        msg.creditBytes = static_cast<std::uint64_t>((*root)["credit_bytes"].asInt());
    }
    if (root->contains("level")) {
        msg.historyLevel = static_cast<int>((*root)["level"].asInt());
    }
    if (root->contains("bucket_ms")) {
        msg.bucketMs = static_cast<std::uint32_t>((*root)["bucket_ms"].asInt());
    }
    if (root->contains("truncated")) {
        msg.truncated = (*root)["truncated"].asBool();
    }
    if ((*root)["series"].isArray()) {
        for (const auto& item : (*root)["series"].asArray()) {
            if (!item.isObject()) {
                continue;
            }
            Message::HistorySeries row;
            row.signalId = static_cast<std::uint32_t>(item["signal_id"].asInt());
            row.level = static_cast<int>(item["level"].asInt());
            row.bucketMs = static_cast<std::uint32_t>(item["bucket_ms"].asInt());
            row.points = static_cast<std::uint32_t>(item["points"].asInt());
            row.tMin = static_cast<std::uint64_t>(item["t_min"].asInt());
            row.tMax = static_cast<std::uint64_t>(item["t_max"].asInt());
            row.chunks = static_cast<std::uint32_t>(item["chunks"].asInt());
            if (item["prior"].isObject()) {
                row.prior.present = true;
                row.prior.t = static_cast<std::uint64_t>(item["prior"]["t"].asInt());
                row.prior.v = item["prior"]["v"].asNumber();
            }
            if (item["next"].isObject()) {
                row.next.present = true;
                row.next.t = static_cast<std::uint64_t>(item["next"]["t"].asInt());
                row.next.v = item["next"]["v"].asNumber();
            }
            msg.historySeries.push_back(row);
        }
    }
    msg.jobId = (*root)["job_id"].asString();
    msg.dbcHash = (*root)["dbc_hash"].asString();
    if (root->contains("t0_ms")) {
        msg.t0Ms = static_cast<std::uint64_t>((*root)["t0_ms"].asInt());
    } else if (msg.t0Ms == 0 && msg.fromMs != 0) {
        msg.t0Ms = msg.fromMs;
    }
    if (root->contains("t1_ms")) {
        msg.t1Ms = static_cast<std::uint64_t>((*root)["t1_ms"].asInt());
    } else if (msg.t1Ms == 0 && msg.toMs != 0) {
        msg.t1Ms = msg.toMs;
    }
    msg.prompt = (*root)["prompt"].asString();
    msg.recipeId = (*root)["recipe_id"].asString();
    msg.computeKind = (*root)["kind"].asString();
    msg.computeStatus = (*root)["status"].asString();
    msg.summary = (*root)["summary"].asString();
    if ((*root)["derived"].isArray()) {
        for (const auto& item : (*root)["derived"].asArray()) {
            if (!item.isObject()) {
                continue;
            }
            Message::DerivedChannel row;
            row.name = item["name"].asString();
            row.unit = item["unit"].asString();
            row.pointsRef = item["points_ref"].asString();
            if (!row.name.empty() || !row.pointsRef.empty()) {
                msg.derived.push_back(std::move(row));
            }
        }
    }
    if ((*root)["events"].isArray()) {
        for (const auto& item : (*root)["events"].asArray()) {
            if (!item.isObject()) {
                continue;
            }
            Message::ComputeEvent row;
            row.tMs = static_cast<std::uint64_t>(item["t_ms"].asInt());
            row.name = item["name"].asString();
            row.message = item["message"].asString();
            if (!row.name.empty() || !row.message.empty() || row.tMs != 0) {
                msg.events.push_back(std::move(row));
            }
        }
    }
    if (msg.type == Type::CatalogResult && (*root)["sessions"].isArray()) {
        for (const auto& item : (*root)["sessions"].asArray()) {
            if (!item.isObject()) {
                continue;
            }
            Message::CatalogSession row;
            row.id = item["id"].asString();
            row.path = item["path"].asString();
            row.minMs = static_cast<std::uint64_t>(item["min_ms"].asInt());
            row.maxMs = static_cast<std::uint64_t>(item["max_ms"].asInt());
            row.dbcHash = item["dbc_hash"].asString();
            row.decodeId = item["decode_id"].asString();
            for (const auto& d : item["decodes"].asArray()) {
                Message::CatalogSession::DecodeVersion v;
                v.id = d["id"].asString(); v.dbcHash = d["dbc_hash"].asString(); v.state = d["state"].asString();
                row.decodes.push_back(std::move(v));
            }
            if (row.dbcHash.empty()) {
                row.dbcHash = item["dbc_sha"].asString();
            }
            row.sampleCount = static_cast<std::uint64_t>(item["sample_count"].asInt());
            row.hasVideo = item["has_video"].asBool();
            if (!row.id.empty()) {
                msg.catalogSessions.push_back(std::move(row));
            }
        }
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
        if (!msg.driveDays.empty()) {
            json::Array days;
            days.reserve(msg.driveDays.size());
            for (const auto& day : msg.driveDays) {
                json::Object d;
                d.emplace("date", json::Value(day.date));
                putString(d, "path", day.path);
                d.emplace("session_count", json::Value(day.sessionCount));
                d.emplace("duration_s", json::Value(day.durationS));
                if (!day.sessions.empty()) {
                    json::Array sessions;
                    sessions.reserve(day.sessions.size());
                    for (const auto& sess : day.sessions) {
                        json::Object s;
                        putString(s, "id", sess.id);
                        putString(s, "path", sess.path);
                        if (sess.startMs != 0) {
                            s.emplace("start_ms", json::Value(sess.startMs));
                        }
                        s.emplace("duration_s", json::Value(sess.durationS));
                        s.emplace("speed_max_mps", json::Value(sess.speedMaxMps));
                        sessions.emplace_back(std::move(s));
                    }
                    d.emplace("sessions", json::Value(std::move(sessions)));
                }
                days.emplace_back(std::move(d));
            }
            obj.emplace("drive_days", json::Value(std::move(days)));
        }
        if (!msg.dbcId.empty() || !msg.dbcPath.empty() || !msg.dbcs.empty()) {
            json::Object dbc;
            putString(dbc, "id", msg.dbcId);
            putString(dbc, "path", msg.dbcPath);
            if (!msg.dbcs.empty()) {
                json::Array bundled;
                bundled.reserve(msg.dbcs.size());
                for (const auto& id : msg.dbcs) {
                    bundled.emplace_back(id);
                }
                dbc.emplace("bundled", json::Value(std::move(bundled)));
            }
            obj.emplace("dbc", json::Value(std::move(dbc)));
        }
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
        putString(obj, "dbc_id", msg.dbcId);
        putString(obj, "dbc_path", msg.dbcPath);
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
        if (msg.queryId != 0) obj.emplace("query_id", json::Value(static_cast<std::uint64_t>(msg.queryId)));
        putString(obj, "code", msg.code);
        putString(obj, "message", msg.message);
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
    if (msg.type == Type::VideoLocate) {
        if (msg.queryId != 0) obj.emplace("query_id", json::Value(static_cast<std::uint64_t>(msg.queryId)));
        obj.emplace("at_ms", json::Value(static_cast<std::uint64_t>(msg.startMs)));
        obj.emplace("stream_id", json::Value(static_cast<int>(msg.videoStreamId)));
        obj.emplace("prefetch_gops", json::Value(static_cast<int>(msg.videoPrefetchGops)));
    }
    if (msg.type == Type::VideoClock) {
        obj.emplace("offset_ms", json::Value(msg.videoClockOffsetMs));
    }
    if (msg.type == Type::VideoStatus) {
        putString(obj, "reason", msg.reason);
        if (msg.queryId != 0) obj.emplace("query_id", json::Value(static_cast<std::uint64_t>(msg.queryId)));
        obj.emplace("connected", json::Value(msg.videoConnected));
        obj.emplace("host", json::Value(msg.host));
        obj.emplace("port", json::Value(static_cast<int>(msg.port)));
        obj.emplace("mode", json::Value(std::string(videoModeName(msg.videoMode))));
        obj.emplace("last_ts_ms", json::Value(static_cast<std::uint64_t>(msg.lastTsMs)));
        obj.emplace("frames", json::Value(static_cast<std::uint64_t>(msg.videoFrames)));
        obj.emplace("dropped", json::Value(static_cast<std::uint64_t>(msg.videoDropped)));
    }
    if (msg.type == Type::HistoryQuery || msg.type == Type::HistoryCancel || msg.type == Type::HistoryCredit ||
        msg.type == Type::HistoryResult) {
        obj.emplace("query_id", json::Value(static_cast<std::uint64_t>(msg.queryId)));
    }
    if (msg.type == Type::HistoryQuery) {
        putString(obj, "session_id", msg.sessionId);
        obj.emplace("from_ms", json::Value(msg.fromMs));
        obj.emplace("to_ms", json::Value(msg.toMs));
        obj.emplace("signal_ids", idsJson(msg.signalIds));
        obj.emplace("max_points", json::Value(static_cast<int>(msg.maxPoints)));
        if (!msg.fields.empty()) {
            json::Array fields;
            for (const auto& f : msg.fields) {
                fields.emplace_back(f);
            }
            obj.emplace("fields", json::Value(std::move(fields)));
        }
        if (msg.includeVideo) {
            obj.emplace("include_video", json::Value(true));
        }
        obj.emplace("credit_bytes", json::Value(msg.creditBytes));
    }
    if (msg.type == Type::HistoryCredit) {
        obj.emplace("credit_bytes", json::Value(msg.creditBytes));
    }
    if (msg.type == Type::HistoryResult) {
        obj.emplace("level", json::Value(msg.historyLevel));
        obj.emplace("bucket_ms", json::Value(static_cast<int>(msg.bucketMs)));
        obj.emplace("from_ms", json::Value(msg.fromMs));
        obj.emplace("to_ms", json::Value(msg.toMs));
        obj.emplace("truncated", json::Value(msg.truncated));
        json::Array series;
        series.reserve(msg.historySeries.size());
        for (const auto& row : msg.historySeries) {
            json::Object s;
            s.emplace("signal_id", json::Value(static_cast<std::uint64_t>(row.signalId)));
            s.emplace("level", json::Value(row.level));
            s.emplace("bucket_ms", json::Value(static_cast<int>(row.bucketMs)));
            s.emplace("points", json::Value(static_cast<int>(row.points)));
            s.emplace("t_min", json::Value(row.tMin));
            s.emplace("t_max", json::Value(row.tMax));
            s.emplace("chunks", json::Value(static_cast<int>(row.chunks)));
            if (row.prior.present) {
                json::Object p;
                p.emplace("t", json::Value(row.prior.t));
                p.emplace("v", json::Value(row.prior.v));
                s.emplace("prior", json::Value(std::move(p)));
            }
            if (row.next.present) {
                json::Object n;
                n.emplace("t", json::Value(row.next.t));
                n.emplace("v", json::Value(row.next.v));
                s.emplace("next", json::Value(std::move(n)));
            }
            series.emplace_back(std::move(s));
        }
        obj.emplace("series", json::Value(std::move(series)));
    }
    if (msg.type == Type::CatalogResult) {
        json::Array sessions;
        sessions.reserve(msg.catalogSessions.size());
        for (const auto& row : msg.catalogSessions) {
            json::Object s;
            putString(s, "id", row.id);
            putString(s, "path", row.path);
            s.emplace("min_ms", json::Value(row.minMs));
            s.emplace("max_ms", json::Value(row.maxMs));
            putString(s, "dbc_hash", row.dbcHash);
            s.emplace("sample_count", json::Value(row.sampleCount));
            s.emplace("has_video", json::Value(row.hasVideo));
            sessions.emplace_back(std::move(s));
        }
        obj.emplace("sessions", json::Value(std::move(sessions)));
    }
    if (msg.type == Type::AnalysisComputeRequest || msg.type == Type::AnalysisComputeResult ||
        msg.type == Type::AnalysisComputeCancel) {
        putString(obj, "job_id", msg.jobId);
    }
    if (msg.type == Type::AnalysisComputeRequest) {
        putString(obj, "session_id", msg.sessionId);
        putString(obj, "dbc_hash", msg.dbcHash);
        obj.emplace("signal_ids", idsJson(msg.signalIds));
        obj.emplace("t0_ms", json::Value(msg.t0Ms));
        obj.emplace("t1_ms", json::Value(msg.t1Ms));
        obj.emplace("max_points", json::Value(static_cast<int>(msg.maxPoints)));
        putString(obj, "prompt", msg.prompt);
        putString(obj, "recipe_id", msg.recipeId);
        putString(obj, "kind", msg.computeKind);
    }
    if (msg.type == Type::AnalysisComputeResult) {
        putString(obj, "status", msg.computeStatus);
        putString(obj, "summary", msg.summary);
        putString(obj, "code", msg.code);
        putString(obj, "message", msg.message);
        if (!msg.derived.empty()) {
            json::Array derived;
            derived.reserve(msg.derived.size());
            for (const auto& row : msg.derived) {
                json::Object o;
                putString(o, "name", row.name);
                putString(o, "unit", row.unit);
                putString(o, "points_ref", row.pointsRef);
                derived.emplace_back(std::move(o));
            }
            obj.emplace("derived", json::Value(std::move(derived)));
        }
        if (!msg.events.empty()) {
            json::Array events;
            events.reserve(msg.events.size());
            for (const auto& row : msg.events) {
                json::Object o;
                o.emplace("t_ms", json::Value(row.tMs));
                putString(o, "name", row.name);
                putString(o, "message", row.message);
                events.emplace_back(std::move(o));
            }
            obj.emplace("events", json::Value(std::move(events)));
        }
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

Message subscribeCan() {
    Message msg;
    msg.type = Type::SubscribeCan;
    return msg;
}

Message unsubscribeCan() {
    Message msg;
    msg.type = Type::UnsubscribeCan;
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

Message historyQuery(std::uint32_t queryId, std::string sessionId, std::uint64_t fromMs, std::uint64_t toMs,
                     std::vector<std::uint32_t> signalIds, std::uint32_t maxPoints, std::uint64_t creditBytes) {
    Message msg;
    msg.type = Type::HistoryQuery;
    msg.queryId = queryId;
    msg.sessionId = std::move(sessionId);
    msg.fromMs = fromMs;
    msg.toMs = toMs;
    msg.signalIds = std::move(signalIds);
    msg.maxPoints = maxPoints;
    msg.creditBytes = creditBytes;
    return msg;
}

Message historyResult(std::uint32_t queryId, int level, std::uint32_t bucketMs, std::uint64_t fromMs,
                      std::uint64_t toMs, std::vector<Message::HistorySeries> series, bool truncated) {
    Message msg;
    msg.type = Type::HistoryResult;
    msg.queryId = queryId;
    msg.historyLevel = level;
    msg.bucketMs = bucketMs;
    msg.fromMs = fromMs;
    msg.toMs = toMs;
    msg.historySeries = std::move(series);
    msg.truncated = truncated;
    return msg;
}

Message historyCancel(std::uint32_t queryId) {
    Message msg;
    msg.type = Type::HistoryCancel;
    msg.queryId = queryId;
    return msg;
}

Message historyCredit(std::uint32_t queryId, std::uint64_t creditBytes) {
    Message msg;
    msg.type = Type::HistoryCredit;
    msg.queryId = queryId;
    msg.creditBytes = creditBytes;
    return msg;
}

Message videoLocate(std::uint64_t atMs, std::uint8_t streamId, std::uint16_t prefetchGops) {
    Message msg;
    msg.type = Type::VideoLocate;
    msg.startMs = atMs;
    msg.videoStreamId = streamId;
    msg.videoPrefetchGops = prefetchGops == 0 ? 4 : prefetchGops;
    return msg;
}

Message videoClock(std::int64_t offsetMs) {
    Message msg;
    msg.type = Type::VideoClock;
    msg.videoClockOffsetMs = offsetMs;
    return msg;
}

Message analysisComputeRequest(std::string jobId, std::string sessionId, std::uint64_t t0Ms, std::uint64_t t1Ms,
                               std::vector<std::uint32_t> signalIds, std::string recipeId, std::uint32_t maxPoints) {
    Message msg;
    msg.type = Type::AnalysisComputeRequest;
    msg.jobId = std::move(jobId);
    msg.sessionId = std::move(sessionId);
    msg.t0Ms = t0Ms;
    msg.t1Ms = t1Ms;
    msg.signalIds = std::move(signalIds);
    msg.recipeId = std::move(recipeId);
    msg.maxPoints = maxPoints;
    msg.computeKind = "grok";
    return msg;
}

Message analysisComputeResult(std::string jobId, std::string status, std::string summary, std::string code,
                              std::string message) {
    Message msg;
    msg.type = Type::AnalysisComputeResult;
    msg.jobId = std::move(jobId);
    msg.computeStatus = std::move(status);
    msg.summary = std::move(summary);
    msg.code = std::move(code);
    msg.message = std::move(message);
    return msg;
}

Message analysisComputeCancel(std::string jobId) {
    Message msg;
    msg.type = Type::AnalysisComputeCancel;
    msg.jobId = std::move(jobId);
    return msg;
}

Message catalogList() {
    Message msg;
    msg.type = Type::CatalogList;
    return msg;
}

Message catalogResult(std::vector<Message::CatalogSession> sessions) {
    Message msg;
    msg.type = Type::CatalogResult;
    msg.catalogSessions = std::move(sessions);
    return msg;
}

}  // namespace photon::ctrl
