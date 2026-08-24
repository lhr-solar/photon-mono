#include "photon_ui/client.hpp"

#include "photon/protocol.hpp"
#include "photon_ui/video.hpp"

#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>

#include <cstdio>
#include <string>
#include <utility>

namespace photon::ui {
namespace {

constexpr double kBackoffMs[] = {200.0, 500.0, 1000.0, 2000.0};
constexpr int kBackoffCount = 4;

}  // namespace

Client::Client(ClientArena& arena) : arena_(arena), url_(arena.wsUrl) {
    for (const auto& desc : kCatalog) {
        subscribed_.insert(desc.id);
    }
}

const char* Client::stateLabel() const {
    switch (state_) {
        case State::Disconnected:
            return "disconnected";
        case State::Connecting:
            return "connecting";
        case State::Connected:
            return "connected";
        case State::HelloOk:
            return "ready";
    }
    return "unknown";
}

void Client::setUrl(std::string url) {
    url_ = std::move(url);
    arena_.wsUrl = url_;
}

void Client::tick(double nowMs) {
    if (state_ == State::HelloOk && nowMs - lastHeartbeatMs_ >= 1000.0) {
        send(ctrl::heartbeat());
        lastHeartbeatMs_ = nowMs;
    }
    if (state_ == State::Disconnected && nowMs >= nextReconnectMs_) {
        connect();
    }
}

void Client::connect() {
    if (state_ == State::Connecting || state_ == State::Connected || state_ == State::HelloOk) {
        return;
    }
    if (!emscripten_websocket_is_supported()) {
        arena_.pushLog("WebSocket is not supported in this browser", true);
        nextReconnectMs_ = emscripten_get_now() + 2000.0;
        return;
    }
    if (socket_ > 0) {
        emscripten_websocket_delete(socket_);
        socket_ = 0;
    }

    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = url_.c_str();
    attr.protocols = nullptr;
    attr.createOnMainThread = true;

    socket_ = emscripten_websocket_new(&attr);
    if (socket_ <= 0) {
        arena_.pushLog("websocket create failed", true);
        scheduleReconnect(emscripten_get_now());
        return;
    }
    state_ = State::Connecting;
    arena_.pushLog(std::string("connecting ") + url_);
    attachCallbacks();
}

void Client::disconnect(const char* why) {
    handleClose(why, true);
}

namespace {

bool onOpen(int, const EmscriptenWebSocketOpenEvent*, void* user) {
    static_cast<Client*>(user)->handleOpen();
    return true;
}

bool onClose(int, const EmscriptenWebSocketCloseEvent* ev, void* user) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "socket close code=%u", static_cast<unsigned>(ev->code));
    static_cast<Client*>(user)->handleClose(buf, ev->code != 1000);
    return true;
}

bool onError(int, const EmscriptenWebSocketErrorEvent*, void* user) {
    static_cast<Client*>(user)->handleClose("socket error", true);
    return true;
}

bool onMessage(int, const EmscriptenWebSocketMessageEvent* ev, void* user) {
    auto* self = static_cast<Client*>(user);
    if (ev->isText) {
        const char* text = reinterpret_cast<const char*>(ev->data);
        // Emscripten sets numBytes to lengthBytesUTF8(data)+1 (includes NUL).
        std::size_t n = ev->numBytes;
        while (n > 0 && text[n - 1] == '\0') {
            --n;
        }
        self->handleText(std::string_view(text, n));
    } else {
        self->handleBinary(std::span<const std::uint8_t>(ev->data, ev->numBytes));
    }
    return true;
}

}  // namespace

void Client::attachCallbacks() {
    emscripten_websocket_set_onopen_callback(socket_, this, onOpen);
    emscripten_websocket_set_onclose_callback(socket_, this, onClose);
    emscripten_websocket_set_onerror_callback(socket_, this, onError);
    emscripten_websocket_set_onmessage_callback(socket_, this, onMessage);
}

void Client::handleOpen() {
    state_ = State::Connected;
    arena_.pushLog("socket open, sending hello");
    send(ctrl::hello("photon-ui"));
}

void Client::handleClose(const char* why, bool error) {
    if (state_ == State::Disconnected && socket_ <= 0) {
        return;
    }
    const int sock = socket_;
    socket_ = 0;
    if (sock > 0) {
        emscripten_websocket_delete(sock);
    }
    if (state_ != State::Disconnected) {
        arena_.pushLog(why, error);
    }
    state_ = State::Disconnected;
    scheduleReconnect(emscripten_get_now());
}

void Client::scheduleReconnect(double nowMs) {
    const double wait = kBackoffMs[backoffIndex_];
    if (backoffIndex_ + 1 < kBackoffCount) {
        ++backoffIndex_;
    }
    nextReconnectMs_ = nowMs + wait;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "reconnect in %.0f ms", wait);
    arena_.pushLog(buf);
}

void Client::handleText(std::string_view text) {
    std::string error;
    auto msg = ctrl::parse(text, &error);
    if (!msg) {
        arena_.pushLog(error.empty() ? "invalid control JSON" : error, true);
        return;
    }
    switch (msg->type) {
        case ctrl::Type::HelloAck:
            applyHelloAck(*msg);
            break;
        case ctrl::Type::ErrorAlert:
            arena_.pushLog(msg->code + ": " + msg->message, true);
            arena_.applyControlError(*msg);
            break;
        case ctrl::Type::TxArmStatus:
            arena_.txArmed = msg->armed;
            arena_.txReason = msg->reason;
            arena_.pushLog(std::string("tx_arm_status armed=") + (msg->armed ? "true" : "false") + " " +
                           msg->reason);
            break;
        case ctrl::Type::TrackSimList:
            arena_.applyTrackSimList(*msg);
            arena_.pushLog("rx track_sim_list");
            break;
        case ctrl::Type::TrackSimResult:
            arena_.applyTrackSimResult(*msg);
            arena_.pushLog(std::string("rx track_sim_result feasible=") + (msg->feasible ? "true" : "false"));
            break;
        case ctrl::Type::SignalCatalog: {
            arena_.applySignalCatalog(*msg);
            arena_.pushLog("rx signal_catalog n=" + std::to_string(msg->catalog.size()));
            std::vector<std::uint32_t> ids;
            ids.reserve(msg->catalog.size());
            for (const auto& sig : msg->catalog) {
                ids.push_back(sig.id);
            }
            ensureSubscribed(ids);
            break;
        }
        case ctrl::Type::VideoStatus:
            arena_.applyVideoStatus(*msg);
            break;
        default:
            arena_.pushLog(std::string("rx ") + std::string(ctrl::typeName(msg->type)));
            break;
    }
}

void Client::adoptActiveSource(const ctrl::Message::ActiveSource& active) {
    ctrl::SourceKind kind = active.kind;
    if (kind == ctrl::SourceKind::None && !active.path.empty()) {
        kind = ctrl::SourceKind::LogReplay;
    }
    if (kind == ctrl::SourceKind::None && !active.host.empty()) {
        kind = ctrl::SourceKind::DaqTcp;
    }
    if (kind == ctrl::SourceKind::None) {
        return;
    }
    pendingSource_ = kind;
    pendingHost_ = active.host;
    pendingPort_ = active.port;
    pendingPath_ = active.path;
    arena_.source = pendingSource_;
    arena_.sourceHost = pendingHost_;
    arena_.sourcePort = pendingPort_;
    arena_.sourcePath = pendingPath_;
    arena_.haveActiveSource = true;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "active_source %s", std::string(ctrl::sourceName(active.kind)).c_str());
    arena_.pushLog(buf);
    if (!active.path.empty()) {
        arena_.pushLog(std::string("active_source path=") + active.path);
    } else if (!active.host.empty()) {
        arena_.pushLog(std::string("active_source ") + active.host + ":" + std::to_string(active.port));
    }
}

void Client::applyHelloAck(const ctrl::Message& msg) {
    state_ = State::HelloOk;
    backoffIndex_ = 0;
    lastHeartbeatMs_ = emscripten_get_now();
    arena_.features = msg.features;
    arena_.resetTrackSimCatalog();
    arena_.haveSignalCatalog = false;
    arena_.signalCatalog.clear();
    arena_.resetSignalMap();
    arena_.pushLog(std::string("hello_ack from ") + (msg.server.empty() ? "photon-backend" : msg.server));
    adoptActiveSource(msg.activeSource);
    sendSubscriptions();
}

void Client::sendSubscriptions() {
    if (!subscribed_.empty()) {
        std::vector<std::uint32_t> ids(subscribed_.begin(), subscribed_.end());
        send(ctrl::subscribe(std::move(ids)));
        arena_.pushLog("subscribe_signals resubscribed");
    }
    if (videoWanted_) {
        send(ctrl::videoSubscribe(videoMode_, videoStartMs_));
        arena_.video.subscribed = true;
    }
}

void Client::resubscribe() {
    ctrl::Message select = ctrl::selectSource(pendingSource_);
    select.host = pendingHost_;
    select.port = pendingPort_;
    select.path = pendingPath_;
    send(select);
    arena_.source = pendingSource_;
    arena_.sourceHost = pendingHost_;
    arena_.sourcePort = pendingPort_;
    arena_.sourcePath = pendingPath_;
    arena_.haveActiveSource = true;
    arena_.clearTelemetry();
    arena_.pushLog(std::string("select_source ") + std::string(ctrl::sourceName(pendingSource_)));
    sendSubscriptions();
}

void Client::handleBinary(std::span<const std::uint8_t> bytes) {
    const auto frame = proto::decodeFrame(bytes);
    if (!frame) {
        arena_.pushLog("bad binary frame", true);
        return;
    }
    const auto type = static_cast<proto::MsgType>(frame->header.msgType);
    switch (type) {
        case proto::MsgType::TelemetryBatch: {
            const auto blocks = proto::decodeTelemetryBatch(frame->payload);
            if (!blocks) {
                arena_.pushLog("bad TelemetryBatch", true);
                return;
            }
            arena_.ingestTelemetry(*blocks);
            break;
        }
        case proto::MsgType::CanMonitorBatch: {
            const auto items = proto::decodeCanMonitorBatch(frame->payload);
            if (!items) {
                arena_.pushLog("bad CanMonitorBatch", true);
                return;
            }
            arena_.ingestCan(*items);
            break;
        }
        case proto::MsgType::TimelineStatus: {
            const auto status = proto::decodeTimelineStatus(frame->payload);
            if (!status) {
                arena_.pushLog("bad TimelineStatus", true);
                return;
            }
            arena_.ingestTimeline(*status);
            break;
        }
        case proto::MsgType::VideoFrame: {
            const auto vf = proto::decodeVideoFrame(frame->payload);
            if (!vf) {
                arena_.pushLog("bad VideoFrame", true);
                return;
            }
            arena_.ingestVideo(*vf);
            videoDecoder().feed(vf->timestampMs, vf->flags, vf->nal);
            break;
        }
    }
}

bool Client::send(const ctrl::Message& msg) {
    if (socket_ <= 0) {
        return false;
    }
    if (state_ != State::Connected && state_ != State::HelloOk) {
        return false;
    }
    const std::string json = ctrl::serialize(msg);
    if (msg.type != ctrl::Type::Heartbeat) {
        arena_.pushLog(std::string("tx ") + std::string(ctrl::typeName(msg.type)));
    }
    return emscripten_websocket_send_utf8_text(socket_, json.c_str()) >= 0;
}

void Client::selectSource(ctrl::SourceKind kind, std::string host, std::uint16_t port, std::string path) {
    pendingSource_ = kind;
    pendingHost_ = std::move(host);
    pendingPort_ = port;
    pendingPath_ = std::move(path);
    if (state_ == State::HelloOk) {
        resubscribe();
    }
}

void Client::setSubscribed(std::uint32_t id, bool on) {
    if (on) {
        if (subscribed_.insert(id).second && state_ == State::HelloOk) {
            send(ctrl::subscribe({id}));
        }
    } else if (subscribed_.erase(id) > 0 && state_ == State::HelloOk) {
        send(ctrl::unsubscribe({id}));
    }
}

void Client::ensureSubscribed(std::span<const std::uint32_t> ids) {
    std::vector<std::uint32_t> added;
    added.reserve(ids.size());
    for (const std::uint32_t id : ids) {
        if (id == 0) {
            continue;
        }
        if (subscribed_.insert(id).second) {
            added.push_back(id);
        }
    }
    if (!added.empty() && state_ == State::HelloOk) {
        send(ctrl::subscribe(std::move(added)));
        arena_.pushLog("subscribe_signals +" + std::to_string(added.size()));
    }
}

void Client::sendTimeline(ctrl::TimelineMode mode, std::uint64_t cursorMs) {
    send(ctrl::timelineControl(mode, cursorMs));
}

void Client::requestTrackSimList() {
    arena_.trackSim.catalogPending = true;
    if (arena_.trackSim.error == "not connected") {
        arena_.trackSim.error.clear();
    }
    if (!send(ctrl::trackSimList())) {
        arena_.trackSim.catalogPending = false;
        arena_.trackSim.error = "not connected";
    }
}

void Client::requestTrackSim(std::string trackId, double timeBudgetS, double initialSoc,
                             tracksim::CarOverride car) {
    arena_.beginTrackSimSolve(timeBudgetS);
    ctrl::Message msg = ctrl::trackSimRequest(std::move(trackId), timeBudgetS, initialSoc);
    msg.carOverride = std::move(car);
    if (!send(msg)) {
        arena_.trackSim.solving = false;
        arena_.trackSim.error = "not connected";
    }
}

void Client::subscribeVideo(ctrl::VideoMode mode, std::uint64_t startMs) {
    videoWanted_ = true;
    videoMode_ = mode;
    videoStartMs_ = startMs;
    arena_.video.subscribed = true;
    arena_.video.mode = std::string(ctrl::videoModeName(mode));
    send(ctrl::videoSubscribe(mode, startMs));
}

void Client::stopVideo() {
    videoWanted_ = false;
    arena_.video.subscribed = false;
    send(ctrl::videoStop());
    videoDecoder().reset();
}

}  // namespace photon::ui
