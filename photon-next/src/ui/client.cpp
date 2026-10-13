#include "ui/client.hpp"
#include "base/json.hpp"

#include "ui/analysis_runtime.hpp"
#include "ui/clock_format.hpp"
#include "ui/page_url.hpp"
#include "ui/platform.hpp"
#include "ui/video.hpp"
#include "wire/protocol.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>
#else
#include <cstdlib>
#include <memory>
#include "net/net.hpp"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace photon::ui {
namespace {

constexpr double kBackoffMs[] = {200.0, 500.0, 1000.0, 2000.0};
constexpr int kBackoffCount = 4;

}  // namespace

Client::Client(ClientArena& arena) : arena_(arena), url_(arena.wsUrl) {
}

Client::~Client() {
#ifdef __EMSCRIPTEN__
    if (socket_ > 0) {
        emscripten_websocket_delete(socket_);
        socket_ = 0;
    }
#else
    ws_.reset();
#endif
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

#ifndef __EMSCRIPTEN__
namespace {

bool parseWsUrl(std::string_view url, std::string& host, std::uint16_t& port, std::string& path) {
    if (url.starts_with("ws://")) {
        url.remove_prefix(5);
    } else if (url.starts_with("wss://")) {
        return false;
    }
    const auto slash = url.find('/');
    const std::string_view hostport = slash == std::string_view::npos ? url : url.substr(0, slash);
    path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
    const auto colon = hostport.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= hostport.size()) {
        host.assign(hostport);
        port = 80;
        return !host.empty();
    }
    host.assign(hostport.substr(0, colon));
    char* end = nullptr;
    const unsigned long v = std::strtoul(std::string(hostport.substr(colon + 1)).c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || v < 1 || v > 65535ul) {
        return false;
    }
    port = static_cast<std::uint16_t>(v);
    return !host.empty();
}

void pumpNativeWs(Client& self, ws::Connection& conn, bool& handshakeOk) {
    if (!conn.readAvailable()) {
        self.handleClose("socket read failed", true);
        return;
    }
    if (!handshakeOk) {
        const auto hs = conn.readClientHandshake();
        if (hs == ws::HandshakeStatus::Incomplete) {
            return;
        }
        if (hs != ws::HandshakeStatus::Ok) {
            self.handleClose("websocket handshake rejected", true);
            return;
        }
        handshakeOk = true;
        if (!conn.flushOutbound()) {
            self.handleClose("socket flush failed", true);
            return;
        }
        self.handleOpen();
        std::fprintf(stdout, "photon-ui websocket handshake ok\n");
        std::fflush(stdout);
        // Fall through and read hello_ack in this same tick.
    }
    if (!conn.flushOutbound()) {
        self.handleClose("socket flush failed", true);
        return;
    }
    int pumped = 0;
    while (auto frame = conn.takeFrame()) {
        if (++pumped > 512) {
            break;
        }
        if (frame->opcode == ws::Opcode::Ping) {
            conn.queuePong(frame->payload);
            continue;
        }
        if (frame->opcode == ws::Opcode::Close) {
            self.handleClose("socket close", false);
            return;
        }
        if (frame->opcode == ws::Opcode::Text) {
            const auto& p = frame->payload;
            self.handleText(std::string_view(reinterpret_cast<const char*>(p.data()), p.size()));
        } else if (frame->opcode == ws::Opcode::Binary) {
            self.handleBinary(frame->payload);
        }
    }
}

}  // namespace
#endif

void Client::tick(double nowMs) {
    pollDbcUpload();
#ifndef __EMSCRIPTEN__
    if (ws_) {
        pumpNativeWs(*this, *ws_, wsHandshakeOk_);
    }
#endif
    if (state_ == State::HelloOk && nowMs - lastHeartbeatMs_ >= 1000.0) {
        send(ctrl::heartbeat());
        lastHeartbeatMs_ = nowMs;
    }
    if (state_ == State::HelloOk) {
        // FPS is a completed-window average and can remain nonzero after a
        // stall. Use actual frame arrival to decide whether a seed is needed.
        const auto received = arena_.video.lastFrameArrivalMs;
        const auto wallNow =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count());
        const bool liveFramesRecent = received != 0 && wallNow >= received && wallNow - received < 2000;
        if (liveFramesRecent && !locateSent_ && arena_.video.subscribed && !arena_.catalogPinned()) {
            std::uint64_t pendingAt = 0;
            if (videoDecoder().pendingLocate(pendingAt) && pendingAt == 0) {
                videoDecoder().cancelPendingLocate();
            }
        }
        if (arena_.video.subscribed && !arena_.catalogPinned() && !locateSent_ &&
            nowMs - lastLiveLocateMs_ >= 400.0 && !liveFramesRecent) {
            videoDecoder().requestLocate(0);
        }
        flushVideoLocate();
        if (arena_.video.clockOffsetDirty) {
            arena_.video.clockOffsetDirty = false;
            setVideoClock(arena_.video.clockOffsetMs);
        }
    }
    if (state_ == State::Disconnected && nowMs >= nextReconnectMs_) {
        connect();
    }
}

void Client::connect() {
    if (state_ == State::Connecting || state_ == State::Connected || state_ == State::HelloOk) {
        return;
    }
#ifdef __EMSCRIPTEN__
    if (!emscripten_websocket_is_supported()) {
        arena_.pushLog("This browser cannot open the live page connection", true);
        nextReconnectMs_ = nowMs() + 2000.0;
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
        arena_.pushLog("live connection failed", true);
        scheduleReconnect(nowMs());
        return;
    }
    state_ = State::Connecting;
    if (pageIsSharedConsole()) {
        arena_.pushLog("connecting to this website");
    } else {
        arena_.pushLog(std::string("connecting ") + url_);
    }
    attachCallbacks();
#else
    ws_.reset();
    wsHandshakeOk_ = false;
    std::string host;
    std::uint16_t port = 8080;
    std::string path;
    if (!parseWsUrl(url_, host, port, path)) {
        arena_.pushLog(std::string("bad websocket url ") + url_, true);
        scheduleReconnect(nowMs());
        return;
    }
    std::fprintf(stdout, "photon-ui connecting %s:%u%s\n", host.c_str(), static_cast<unsigned>(port),
                 path.c_str());
    std::fflush(stdout);
    auto sock = net::connectTcpTimed(host, port, 800);
    if (!sock.valid()) {
        std::fprintf(stderr, "photon-ui connect failed %s\n", url_.c_str());
        arena_.pushLog(std::string("connect failed ") + url_, true);
        scheduleReconnect(nowMs());
        return;
    }
    sock.setNonBlocking(true);
    sock.setNoDelay(true);
    ws_ = std::make_unique<ws::Connection>(std::move(sock));
    ws_->setClientMode(true);
    const std::string hostHdr = host + ":" + std::to_string(port);
    if (!ws_->beginClientHandshake(path, hostHdr, nativeWsBasicB64())) {
        ws_.reset();
        arena_.pushLog("websocket handshake send failed", true);
        scheduleReconnect(nowMs());
        return;
    }
    state_ = State::Connecting;
    if (pageIsSharedConsole()) {
        arena_.pushLog("connecting to this website");
    } else {
        arena_.pushLog(std::string("connecting ") + url_);
    }
#endif
}

void Client::disconnect(const char* why) {
    handleClose(why, true);
}

#ifdef __EMSCRIPTEN__
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
#endif

void Client::attachCallbacks() {
#ifdef __EMSCRIPTEN__
    emscripten_websocket_set_onopen_callback(socket_, this, onOpen);
    emscripten_websocket_set_onclose_callback(socket_, this, onClose);
    emscripten_websocket_set_onerror_callback(socket_, this, onError);
    emscripten_websocket_set_onmessage_callback(socket_, this, onMessage);
#endif
}

void Client::handleOpen() {
    state_ = State::Connected;
    arena_.pushLog("socket open, sending hello");
    json::Object hello;
    hello.emplace("type", "hello"); hello.emplace("client", "photon-ui"); hello.emplace("protocol_version", 1);
    if (arena_.catalogPinned() && !arena_.tiles().sessionId().empty()) {
        hello.emplace("session_id", arena_.tiles().sessionId());
        hello.emplace("cursor_ms", arena_.displayCursorMs());
        hello.emplace("mode", arena_.catalogPlaying() ? "play" : "pause");
    }
#ifndef __EMSCRIPTEN__
    else if (!nativePageOptions().recordingId.empty()) hello.emplace("session_id", nativePageOptions().recordingId);
    else if (nativePageOptions().pcan) hello.emplace("source", "pcan");
#endif
    sendExtension(json::stringify(json::Value(std::move(hello))));
}

void Client::handleClose(const char* why, bool error) {
    if (state_ == State::Disconnected) {
        return;
    }
#ifdef __EMSCRIPTEN__
    const int sock = socket_;
    socket_ = 0;
    if (sock > 0) {
        emscripten_websocket_delete(sock);
    }
#else
    ws_.reset();
    wsHandshakeOk_ = false;
#endif
    state_ = State::Disconnected;
    arena_.pushLog(why, error);
    scheduleReconnect(nowMs());
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
    if (auto extension = json::parse(text); extension) {
        const auto type = (*extension)["type"].asString();
        if (type == "recording_selected") {
            arena_.tiles().setAggregateOnly(!(*extension)["coarse_history"].asBool());
            selectedDecode_ = (*extension)["decode_id"].asString();
            arena_.tiles().setDecodeId(selectedDecode_);
            arena_.tiles().setSession((*extension)["session_id"].asString(), (*extension)["dbc_hash"].asString(),
                                     static_cast<std::uint64_t>((*extension)["min_ms"].asInt()),
                                     static_cast<std::uint64_t>((*extension)["max_ms"].asInt()));
            return;
        }
        if (type == "dbc_applied" || type == "redecode_status" || type == "event_created") {
            arena_.pushLog(std::string(text));
            return;
        }
    }
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
            if (msg->code == "video_locate" && msg->queryId != 0 && msg->queryId != activeVideoRequest_) {
                break;
            }
            arena_.pushLog(msg->code + ": " + msg->message, true);
            if (msg->code == "video_locate") {
                awaitingVideoBytes_ = false;
                awaitingVideoBatch_ = false;
                arena_.video.seekDelivering = false;
                // A transport failure is not evidence that this run has no
                // camera. Let a later keyframe/lookup recover the connection.
                locateSent_ = false;
                videoDecoder().cancelPendingLocate();
                arena_.video.lastError = msg->message;
                locateBackoffMs_ = 400.0;
                lastLiveLocateMs_ = nowMs();
            }
            break;
        case ctrl::Type::TxArmStatus:
            arena_.txArmed = msg->armed;
            arena_.txReason = msg->reason;
            arena_.pushLog(std::string("tx_arm_status armed=") + (msg->armed ? "true" : "false") + " " +
                           msg->reason);
            break;
        case ctrl::Type::SignalCatalog: {
            arena_.applySignalCatalog(*msg);
            arena_.pushLog("rx signal_catalog n=" + std::to_string(msg->catalog.size()));
            break;
        }
        case ctrl::Type::VideoStatus:
            if (msg->queryId != 0) {
                if (msg->queryId != activeVideoRequest_) break;
                awaitingVideoBytes_ = false;
                arena_.video.seekDelivering = false;
                if (msg->reason != "entry_complete") {
                    awaitingVideoBatch_ = false;
                    std::uint64_t pendingAt = 0;
                    if (videoDecoder().pendingLocate(pendingAt) && pendingAt != 0 &&
                        videoDecoder().hasVideoAt(pendingAt) &&
                        videoDecoder().bufferedNewestMs() >= pendingAt + vidx::kLocateLeadMs) {
                        videoDecoder().cancelPendingLocate();
                    }
                }
            }
            arena_.applyVideoStatus(*msg);
            break;
        case ctrl::Type::VideoClock:
            arena_.video.clockOffsetMs = msg->videoClockOffsetMs;
            arena_.video.haveClockOffset = true;
            videoDecoder().setClockOffsetMs(msg->videoClockOffsetMs);
            break;
        case ctrl::Type::HistoryResult:
            if (analysisRuntime_ == nullptr || !analysisRuntime_->ingestResult(*msg)) {
                arena_.tiles().ingestResult(*msg);
            }
            break;
        case ctrl::Type::CatalogResult:
            historyCatalog_ = msg->catalogSessions;
            haveHistoryCatalog_ = true;
            arena_.pushLog("rx catalog_result n=" + std::to_string(historyCatalog_.size()));
            // History hop just (re)connected. Retry pin tiles if the first
            // prime ran while the remote was down.
            if (arena_.catalogPinned()) {
                arena_.invalidateHistoryPrime();
            }
#ifndef __EMSCRIPTEN__
            if (const char* pin = std::getenv("PHOTON_PIN_SESSION"); pin != nullptr && pin[0] != '\0') {
                for (const auto& row : historyCatalog_) {
                    if (row.id == pin) {
                        playCatalogSession(row);
                        break;
                    }
                }
            }
#endif
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
    // Daq host/port flicker on CAN reconnect. Do not wipe rings — Pit went
    // blank every time the tunnel flapped.
    const bool kindOrPathChanged =
        arena_.haveActiveSource && (arena_.source != kind || arena_.sourcePath != active.path);
    const bool bothDaq = arena_.source == ctrl::SourceKind::DaqTcp && kind == ctrl::SourceKind::DaqTcp;
    if (kindOrPathChanged && !bothDaq) {
        arena_.clearTelemetry();
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
    if (kind == ctrl::SourceKind::LogReplay && arena_.timelineIsLive()) {
        proto::TimelineStatus buffering = arena_.timeline();
        buffering.mode = static_cast<std::uint8_t>(proto::TimelineMode::Buffering);
        buffering.cursorMs = 0;
        arena_.ingestTimeline(buffering);
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "active_source %s", std::string(ctrl::sourceName(active.kind)).c_str());
    arena_.pushLog(buf);
    if (!active.path.empty()) {
        arena_.pushLog(std::string("active_source path=") + active.path);
    } else if (!active.host.empty()) {
        const auto shown = sourceDisplayHost(active.host);
        arena_.pushLog(std::string("active_source ") + shown);
    }
}

void Client::applyHelloAck(const ctrl::Message& msg) {
    const bool first = state_ != State::HelloOk;
    state_ = State::HelloOk;
    backoffIndex_ = 0;
    lastHeartbeatMs_ = nowMs();
    arena_.features = msg.features;
    if (first || !msg.driveDays.empty()) {
        arena_.driveDays = msg.driveDays;
    }
    if (first) {
        arena_.haveSignalCatalog = false;
        arena_.signalCatalog.clear();
        arena_.resetSignalMap();
        arena_.pushLog(std::string("hello_ack from ") + (msg.server.empty() ? "photon-backend" : msg.server));
        if (!msg.driveDays.empty()) {
            arena_.pushLog("drive days " + std::to_string(msg.driveDays.size()));
        }
        if (!msg.dbcs.empty()) {
            arena_.dbcs = msg.dbcs;
        }
        if (!msg.dbcId.empty()) {
            arena_.dbcId = msg.dbcId;
        }
        if (!msg.dbcPath.empty()) {
            arena_.dbcPath = msg.dbcPath;
        }
        if (!arena_.dbcId.empty()) {
            arena_.pushLog(std::string("dbc ") + arena_.dbcId);
        }
        adoptActiveSource(msg.activeSource);
        sendSubscriptions();
        maybeAdoptDriveDay();
        // A hello (including reconnect) joins the existing source. goLive()
        // reselects the hub source and clears the decoded picture, so a brief
        // socket interruption used to become another video buffering cycle.
        // Visible video panels subscribe through markVideoWanted(), while
        // sendSubscriptions() restores an existing subscription on reconnect.
        requestCatalogList();
        return;
    }
    if (!msg.dbcs.empty()) {
        arena_.dbcs = msg.dbcs;
    }
    if (!msg.dbcId.empty()) {
        arena_.dbcId = msg.dbcId;
    }
    if (!msg.dbcPath.empty()) {
        arena_.dbcPath = msg.dbcPath;
    }
    adoptActiveSource(msg.activeSource);
    arena_.pushLog(std::string("source updated ") + std::string(ctrl::sourceName(msg.activeSource.kind)));
    maybeAdoptDriveDay();
}

void Client::maybeAdoptDriveDay() {
    if (pageIsSharedConsole() || userPickedSource_ || arena_.driveDays.empty()) {
        return;
    }
    if (pendingSource_ == ctrl::SourceKind::DaqTcp || pendingSource_ == ctrl::SourceKind::Pcan ||
        pendingSource_ == ctrl::SourceKind::Synthetic) {
        return;
    }
    if (arena_.source == ctrl::SourceKind::DaqTcp || arena_.source == ctrl::SourceKind::Pcan ||
        arena_.source == ctrl::SourceKind::Synthetic) {
        return;
    }
    if (arena_.source == ctrl::SourceKind::LogReplay) {
        return;
    }
    if (pendingSource_ == ctrl::SourceKind::LogReplay && !pendingPath_.empty()) {
        return;
    }
    const ctrl::Message::DriveDay* best = &arena_.driveDays.front();
    for (const auto& day : arena_.driveDays) {
        if (day.durationS > best->durationS) {
            best = &day;
        }
    }
    arena_.pushLog(std::string("auto drive day ") + best->date);
    selectSource(ctrl::SourceKind::LogReplay, "", 0, best->path);
}

void Client::sendSubscriptions() {
    if (!subscribed_.empty()) {
        std::vector<std::uint32_t> ids(subscribed_.begin(), subscribed_.end());
        send(ctrl::subscribe(std::move(ids)));
        arena_.pushLog("subscribe_signals resubscribed");
    }
    if (canWanted_) {
        send(ctrl::subscribeCan());
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
    select.channel = pendingChannel_;
    select.baud = pendingBaud_;
    select.listenOnly = pendingListenOnly_;
    send(select);
    arena_.source = pendingSource_;
    arena_.sourceHost = pendingHost_;
    arena_.sourcePort = pendingPort_;
    arena_.sourcePath = pendingPath_;
    arena_.haveActiveSource = true;
    arena_.clearTelemetry();
    if (pendingSource_ == ctrl::SourceKind::LogReplay && arena_.timelineIsLive()) {
        proto::TimelineStatus buffering = arena_.timeline();
        buffering.mode = static_cast<std::uint8_t>(proto::TimelineMode::Buffering);
        buffering.cursorMs = 0;
        arena_.ingestTimeline(buffering);
    }
    arena_.pushLog(std::string("select_source ") + std::string(ctrl::sourceName(pendingSource_)));
    if (userPickedDbc_ && !pendingDbc_.empty()) {
        send(ctrl::selectDbc(pendingDbc_, pendingDbcBundled_));
        arena_.pushLog(std::string("select_dbc ") + pendingDbc_);
    }
    sendSubscriptions();
}

void Client::handleBinary(std::span<const std::uint8_t> bytes) {
    auto frame = proto::decodeFrame(bytes);
    if (!frame) {
        arena_.pushLog("bad binary frame", true);
        return;
    }
    auto type = static_cast<proto::MsgType>(frame->header.msgType);
    if (type == proto::MsgType::VideoSeekPacket) {
        const auto inner = proto::decodeFrame(frame->payload);
        if (!inner || activeVideoRequest_ == 0 || inner->header.sequence != activeVideoRequest_) return;
        type = static_cast<proto::MsgType>(inner->header.msgType);
        if (type != proto::MsgType::VideoFrame && type != proto::MsgType::VideoLocateReply) return;
        frame = inner;
    } else if (arena_.catalogPinned() &&
               (type == proto::MsgType::VideoFrame || type == proto::MsgType::VideoLocateReply)) {
        // Live packets already queued before video_stop are not replay data.
        return;
    }
    switch (type) {
        case proto::MsgType::VideoSeekPacket:
            return; // Already unwrapped and validated above.
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
            if (!videoDecoder().supported()) {
                break;
            }
            const auto vf = proto::decodeVideoFrame(frame->payload);
            if (!vf) {
                arena_.pushLog("bad VideoFrame", true);
                return;
            }
            arena_.ingestVideo(*vf);
            videoDecoder().feed(vf->timestampMs, vf->flags, vf->nal);
            break;
        }
        case proto::MsgType::HistoryChunk: {
            const auto chunk = proto::decodeHistoryChunk(frame->payload);
            if (!chunk) {
                arena_.pushLog("bad HistoryChunk", true);
                return;
            }
            if (analysisRuntime_ == nullptr || !analysisRuntime_->ingestChunk(*chunk)) {
                arena_.tiles().ingestChunk(*chunk);
            }
            send(ctrl::historyCredit(chunk->queryId, frame->payload.size() + proto::kHeaderSize));
            break;
        }
        case proto::MsgType::VideoLocateReply:
            {
                vidx::LocateReply reply;
                if (vidx::decodeLocateReply(frame->payload, reply)) {
                    if (!reply.found || reply.rows.empty()) {
                        awaitingVideoBytes_ = false;
                        awaitingVideoBatch_ = false;
                        arena_.video.seekDelivering = false;
                    }
                }
            }
            if (!videoDecoder().applyLocateReply(frame->payload)) {
                arena_.pushLog("bad VideoLocateReply", true);
            } else {
                arena_.video.haveClockOffset = true;
                arena_.video.haveStatus = true;
                arena_.video.connected = !videoDecoder().playback().rows().empty();
                arena_.video.mode = arena_.catalogPinned() ? "cursor" : "live";
                arena_.video.lastError.clear();
                locateBackoffMs_ = 0.0;
                if (arena_.video.clockOffsetUser) {
                    videoDecoder().setClockOffsetMs(arena_.video.clockOffsetMs);
                } else {
                    arena_.video.clockOffsetMs = videoDecoder().playback().clockOffsetMs();
                }
                if (arena_.catalogPinned() || nowMs() - lastLocateLogMs_ >= 5000.0) {
                    arena_.pushLog(std::string("video_locate ok rows=") +
                                   std::to_string(videoDecoder().playback().rows().size()));
                }
                if (!arena_.catalogPinned() && arena_.video.mode == "live") {
                    std::uint64_t at = 0;
                    if (videoDecoder().pendingLocate(at) && at != 0) {
                        lastLiveLocateMs_ = 0.0;
                        locateSent_ = false;
                    }
                }
                if (arena_.catalogPinned()) {
                    const auto& pb = videoDecoder().playback();
                    const std::uint64_t cur = arena_.videoCursorMs();
                    const std::int64_t off =
                        arena_.video.clockOffsetUser ? arena_.video.clockOffsetMs : pb.clockOffsetMs();
                    const std::uint64_t minV = vidx::videoMsForCursor(arena_.timeline().minTimeMs, off);
                    const std::uint64_t maxV = arena_.timeline().maxTimeMs == 0
                                                   ? 0
                                                   : vidx::videoMsForCursor(arena_.timeline().maxTimeMs, off);
                    if (!pb.covers(cur)) {
                        std::uint64_t snapV = 0;
                        for (const auto& row : pb.rows()) {
                            if (vidx::rowIsContinuation(row) || row.endMs < minV) {
                                continue;
                            }
                            if (maxV != 0 && row.startMs > maxV) {
                                continue;
                            }
                            snapV = row.startMs < minV ? minV : row.startMs;
                            if (snapV > row.endMs) {
                                snapV = 0;
                                continue;
                            }
                            break;
                        }
                        if (snapV == 0) {
                            snapV = vidx::firstCoveredVideoMs(pb.coverage(), minV, maxV);
                        }
                        const auto pinMode = static_cast<proto::TimelineMode>(arena_.timeline().mode);
                        const bool leadIn =
                            arena_.timeline().minTimeMs == 0 || cur <= arena_.timeline().minTimeMs + 2000;
                        // Skip empty lead-in only. Do not force Play after Pause-at-EOF
                        // or jump back to the first GOP every locate.
                        if (snapV != 0 && pinMode == proto::TimelineMode::Play && leadIn) {
                            const std::uint64_t snapCur = vidx::cursorMsForVideo(snapV, off);
                            if (snapCur != cur) {
                                arena_.applyPinnedTimeline(proto::TimelineMode::Play, snapCur);
                            }
                            videoDecoder().requestLocate(snapV);
                        } else if (snapV == 0) {
                            arena_.applyPinnedTimeline(proto::TimelineMode::Pause, cur);
                        }
                    }
                }
            }
            locateSent_ = false;
            break;
    }
}

bool Client::send(const ctrl::Message& msg) {
#ifdef __EMSCRIPTEN__
    if (socket_ <= 0) {
        return false;
    }
#else
    if (!ws_ || !wsHandshakeOk_) {
        return false;
    }
#endif
    if (state_ != State::Connected && state_ != State::HelloOk) {
        return false;
    }
    const std::string json = ctrl::serialize(msg);
    if (msg.type != ctrl::Type::Heartbeat) {
        arena_.pushLog(std::string("tx ") + std::string(ctrl::typeName(msg.type)));
    }
#ifdef __EMSCRIPTEN__
    return emscripten_websocket_send_utf8_text(socket_, json.c_str()) >= 0;
#else
    ws_->queueText(json);
    return ws_->flushOutbound();
#endif
}

void Client::selectSource(ctrl::SourceKind kind, std::string host, std::uint16_t port, std::string path,
                          int channel, int baud, bool listenOnly) {
    // Log replay is a per-client pointer. It does not take the live ingest source.
    userPickedSource_ = true;
    if (kind == ctrl::SourceKind::DaqTcp) {
        lastDaqHost_ = host.empty() ? lastDaqHost_ : host;
        lastDaqPort_ = port == 0 ? lastDaqPort_ : port;
    }
    pendingSource_ = kind;
    pendingHost_ = std::move(host);
    pendingPort_ = port;
    pendingPath_ = std::move(path);
    pendingChannel_ = channel < 1 ? 1 : (channel > 8 ? 8 : channel);
    pendingBaud_ = baud;
    pendingListenOnly_ = listenOnly;
    if (state_ == State::HelloOk) {
        resubscribe();
    }
}

void Client::selectDbc(std::string idOrPath, bool bundledId) {
    userPickedDbc_ = true;
    pendingDbcBundled_ = bundledId;
    pendingDbc_ = std::move(idOrPath);
    if (state_ != State::HelloOk || pendingDbc_.empty()) {
        return;
    }
    send(ctrl::selectDbc(pendingDbc_, pendingDbcBundled_));
    arena_.pushLog(std::string("select_dbc ") + pendingDbc_);
}

void Client::setSubscribed(std::uint32_t id, bool on) {
    if (on) {
        if (interestOpen_) {
            interest_.insert(id);
        }
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
        if (interestOpen_) {
            interest_.insert(id);
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

void Client::beginInterest() {
    interestOpen_ = true;
    interest_.clear();
    canInterest_ = false;
    videoInterest_ = false;
}

void Client::markCanWanted() {
    canInterest_ = true;
}

void Client::markVideoWanted() {
    videoInterest_ = true;
    // Showing a tile joins the current source; it must not seek/restart the hub.
    if (helloOk() && !arena_.video.subscribed && !arena_.catalogPinned() &&
        arena_.source != ctrl::SourceKind::LogReplay) {
        subscribeVideo(ctrl::VideoMode::Live, 0);
        if (!videoDecoder().hasFrame()) videoDecoder().requestLocate(0);
    }
}

void Client::sendCanMonitor(bool on) {
    if (canWanted_ == on) {
        return;
    }
    canWanted_ = on;
    if (state_ != State::HelloOk) {
        return;
    }
    send(on ? ctrl::subscribeCan() : ctrl::unsubscribeCan());
}

void Client::commitInterest() {
    if (!interestOpen_) {
        return;
    }
    interestOpen_ = false;
    std::vector<std::uint32_t> dropped;
    for (const std::uint32_t id : subscribed_) {
        if (!interest_.contains(id)) {
            dropped.push_back(id);
        }
    }
    for (const std::uint32_t id : dropped) {
        subscribed_.erase(id);
    }
    if (!dropped.empty() && state_ == State::HelloOk) {
        send(ctrl::unsubscribe(std::move(dropped)));
    }
    sendCanMonitor(canInterest_);
    if (!videoInterest_ && arena_.video.subscribed) {
        stopVideo();
    }
}

void Client::sendTimeline(ctrl::TimelineMode mode, std::uint64_t cursorMs) {
    const auto seekVideo = [&](std::uint64_t atCursorMs) {
        lastLiveLocateMs_ = 0.0;
        lastReplayLocateAtMs_ = 0;
        locateSent_ = false;
        locateBackoffMs_ = 0.0;
        if (atCursorMs == 0) {
            return;
        }
        arena_.setVideoFollowMs(atCursorMs);
        const std::int64_t off = arena_.video.clockOffsetUser ? arena_.video.clockOffsetMs
                                                              : videoDecoder().playback().clockOffsetMs();
        std::uint64_t atV = vidx::videoMsForCursor(atCursorMs, off);
        if (atV == 0) {
            atV = atCursorMs;
        }
        // Play/Pause must freeze or resume on the same CAN cursor. Re-locating
        // a GOP already in the ring dumps 30 pictures and the video runs ahead.
        if (!videoDecoder().hasVideoAt(atV)) {
            activeVideoRequest_ = 0;
            videoDecoder().requestLocate(atV);
        }
    };
    if (mode == ctrl::TimelineMode::Live) {
        arena_.clearVideoFollow();
        arena_.setPreviewCursor(0, false);
    }
    if (arena_.catalogPinned()) {
        arena_.applyPinnedTimeline(static_cast<proto::TimelineMode>(mode), cursorMs);
        if (arena_.serverReplay()) send(ctrl::timelineControl(mode, cursorMs));
        if (mode == ctrl::TimelineMode::Pause || mode == ctrl::TimelineMode::Play) {
            seekVideo(cursorMs != 0 ? cursorMs : arena_.videoCursorMs());
        }
        return;
    }
    // Shared-console Pause on DAQ is a no-op. The hour/min/sec rail still has
    // to land on a catalog run so video can follow that cursor.
    if ((mode == ctrl::TimelineMode::Pause || mode == ctrl::TimelineMode::Play) && cursorMs != 0) {
        if (const auto* row = bestCatalogOverlapping(cursorMs, cursorMs); row != nullptr) {
            playCatalogSession(*row, cursorMs);
            if (mode == ctrl::TimelineMode::Play) {
                arena_.applyPinnedTimeline(proto::TimelineMode::Play, cursorMs);
            }
            return;
        }
        seekVideo(cursorMs);
    }
    send(ctrl::timelineControl(mode, cursorMs));
}

const ctrl::Message::CatalogSession* Client::bestCatalogForYmd(const std::string& ymd) const {
    if (ymd.empty()) {
        return nullptr;
    }
    const ctrl::Message::CatalogSession* best = nullptr;
    for (const auto& row : historyCatalog_) {
        if (row.maxMs <= row.minMs || !sessionOverlapsLocalYmd(row.minMs, row.maxMs, ymd)) {
            continue;
        }
        if (best == nullptr) {
            best = &row;
            continue;
        }
        // A later 10 s stub must not hide the run that actually has video.
        if (row.hasVideo != best->hasVideo) {
            if (row.hasVideo) {
                best = &row;
            }
            continue;
        }
        const std::uint64_t rowDur = row.maxMs - row.minMs;
        const std::uint64_t bestDur = best->maxMs - best->minMs;
        if (rowDur > bestDur || (rowDur == bestDur && row.minMs > best->minMs)) {
            best = &row;
        }
    }
    return best;
}

const ctrl::Message::CatalogSession* Client::bestCatalogOverlapping(std::uint64_t minMs,
                                                                    std::uint64_t maxMs) const {
    if (maxMs < minMs) {
        std::swap(minMs, maxMs);
    }
    if (maxMs == minMs) {
        maxMs = minMs + 1;
    }
    const ctrl::Message::CatalogSession* best = nullptr;
    std::uint64_t bestOverlap = 0;
    for (const auto& row : historyCatalog_) {
        if (row.maxMs <= row.minMs) {
            continue;
        }
        const std::uint64_t a = std::max(row.minMs, minMs);
        const std::uint64_t b = std::min(row.maxMs, maxMs);
        if (b <= a) {
            continue;
        }
        const std::uint64_t overlap = b - a;
        if (best == nullptr || overlap > bestOverlap) {
            best = &row;
            bestOverlap = overlap;
        }
    }
    return best;
}

void Client::openExtractLog(std::string path, std::uint64_t cursorMs) {
    userPickedSource_ = true;
    arena_.clearCatalogPin();
    // Keep the live camera. Replay-from-t streams photon-server to EOF
    // (hours of H.264) on the shared relay and aborts every tab.
    selectSource(ctrl::SourceKind::LogReplay, "", 0, std::move(path));
    sendTimeline(ctrl::TimelineMode::Pause, cursorMs);
}

void Client::playLog(std::string path, std::uint64_t cursorMs) {
    std::string ymd;
    const ctrl::Message::DriveSession* stint = nullptr;
    for (const auto& day : arena_.driveDays) {
        if (day.path.empty() && day.date.empty()) {
            continue;
        }
        const bool dayHit = (!day.path.empty() && path == day.path) ||
                            (!day.date.empty() && path.find(day.date) != std::string::npos);
        if (!dayHit) {
            continue;
        }
        ymd = day.date;
        for (const auto& sess : day.sessions) {
            if ((cursorMs != 0 && sess.startMs == cursorMs) || (!sess.path.empty() && sess.path == path)) {
                stint = &sess;
                break;
            }
        }
        break;
    }
    if (stint != nullptr && stint->startMs != 0) {
        const auto endMs = stint->durationS > 0.0
                               ? stint->startMs + static_cast<std::uint64_t>(stint->durationS * 1000.0 + 0.5)
                               : stint->startMs + 1;
        if (const auto* row = bestCatalogOverlapping(stint->startMs, endMs); row != nullptr) {
            playCatalogSession(*row);
            return;
        }
        openExtractLog(std::move(path), cursorMs);
        return;
    }
    if (cursorMs != 0) {
        if (const auto* row = bestCatalogOverlapping(cursorMs, cursorMs); row != nullptr) {
            playCatalogSession(*row);
            return;
        }
        openExtractLog(std::move(path), cursorMs);
        return;
    }
    if (const auto* row = bestCatalogForYmd(ymd); row != nullptr) {
        playCatalogSession(*row);
        return;
    }
    openExtractLog(std::move(path), cursorMs);
}

void Client::playCatalogSession(const ctrl::Message::CatalogSession& row, std::uint64_t cursorMs) {
    userPickedSource_ = true;
    arena_.tiles().cancelAll(*this);
    // Each run owns a separate reference chain, codec, and cursor plan.
    // Retaining compressed bytes/state from the last run can corrupt the
    // next run, including when switching backwards after Pause.
    activeVideoRequest_ = 0;
    videoDecoder().reset();
    videoWanted_ = false;
    arena_.video.subscribed = false;
    send(ctrl::videoStop());
    lastLiveLocateMs_ = 0.0;
    lastReplayLocateAtMs_ = 0;
    locateSent_ = false;
    locateBackoffMs_ = 0.0;
    arena_.pinCatalogSession(row);
    if (arena_.serverReplay()) {
        json::Object select; select.emplace("type", "recording_select"); select.emplace("session_id", row.id);
        if (!row.decodeId.empty()) select.emplace("decode_id", row.decodeId);
        sendExtension(json::stringify(json::Value(std::move(select))));
        send(ctrl::timelineControl(cursorMs == 0 ? ctrl::TimelineMode::Play : ctrl::TimelineMode::Pause,
                                   cursorMs == 0 ? row.minMs : cursorMs));
    }
    if (cursorMs != 0) {
        arena_.applyPinnedTimeline(proto::TimelineMode::Pause, cursorMs);
    }
    arena_.maybePrimeHistory(*this);
    videoDecoder().setReplayWindow(row.minMs, row.maxMs);
    const std::uint64_t at = arena_.videoCursorMs();
    const std::int64_t off =
        arena_.video.clockOffsetUser ? arena_.video.clockOffsetMs : videoDecoder().playback().clockOffsetMs();
    // t=0 means "newest live GOP" on the store. A catalog pin must never
    // send that, or every day shows today's camera.
    std::uint64_t atV = vidx::videoMsForCursor(at, off);
    if (atV == 0) {
        atV = vidx::videoMsForCursor(row.minMs != 0 ? row.minMs : at, off);
    }
    if (!row.hasVideo) {
        return;
    }
    if (atV == 0) {
        arena_.pushLog("catalog pin has no video time; not locating live GOP");
        return;
    }
    videoDecoder().requestLocate(atV);
}

void Client::playDate(std::string ymd) {
    if (ymd.empty()) {
        return;
    }
    // Catalog rows know which run has video. An extract path for the same
    // day does not, and it was winning over the run tagged vid.
    if (const auto* row = bestCatalogForYmd(ymd); row != nullptr && row->hasVideo) {
        playCatalogSession(*row);
        return;
    }
    for (const auto& day : arena_.driveDays) {
        if (day.date == ymd && !day.path.empty()) {
            const std::uint64_t at = day.sessions.empty() ? 0 : day.sessions.front().startMs;
            openExtractLog(day.path, at);
            return;
        }
    }
    if (const auto* row = bestCatalogForYmd(ymd); row != nullptr) {
        playCatalogSession(*row);
        return;
    }
    arena_.pushLog("no log for " + ymd);
}

void Client::goLive() {
    lastReplayLocateAtMs_ = 0;
    arena_.clearCatalogPin();
    arena_.setPreviewCursor(0, false);
    arena_.clearVideoFollow();
    arena_.pushLog("live");
    // Always re-join the hub clock. Skipping this when the chrome already
    // said "Live" left a private replay playhead running, so two windows
    // stayed on different times.
    if (pageIsSharedConsole()) {
        selectSource(ctrl::SourceKind::DaqTcp, "", 0, "");
    } else {
        selectSource(ctrl::SourceKind::DaqTcp, lastDaqHost_, lastDaqPort_, "");
    }
    sendTimeline(ctrl::TimelineMode::Live, 0);
    // Drop the previous ring and the backend seed GOP. Reset-without-stop
    // left locate on "idle skip" with no pictures on the wire.
    stopVideo();
    videoDecoder().resumeLive();
    videoWanted_ = true;
    videoMode_ = ctrl::VideoMode::Live;
    videoStartMs_ = 0;
    send(ctrl::videoSubscribe(ctrl::VideoMode::Live, 0));
    videoDecoder().clearReplayWindow();
    arena_.video.subscribed = true;
    arena_.video.mode = "live";
    arena_.video.connected = false;
    arena_.video.haveStatus = false;
    videoDecoder().requestLocate(0);
    locateSent_ = false;
    locateBackoffMs_ = 0.0;
    lastLiveLocateMs_ = nowMs();
    if (send(ctrl::videoLocate(0, 0, 1))) {
        locateSent_ = true;
        arena_.pushLog("video_locate 0");
    }
}





void Client::subscribeVideo(ctrl::VideoMode mode, std::uint64_t startMs) {
    if (!videoDecoder().supported()) {
        arena_.video.subscribed = false;
        arena_.video.mode = "off";
        return;
    }
    videoWanted_ = true;
    videoMode_ = mode;
    videoStartMs_ = startMs;
    arena_.video.subscribed = true;
    arena_.video.mode = std::string(ctrl::videoModeName(mode));
    send(ctrl::videoSubscribe(mode, startMs));
}

void Client::stopVideo() {
    activeVideoRequest_ = 0;
    videoWanted_ = false;
    arena_.video.subscribed = false;
    locateSent_ = false;
    send(ctrl::videoStop());
    videoDecoder().reset();
}

void Client::setVideoClock(std::int64_t offsetMs) {
    arena_.video.clockOffsetMs = offsetMs;
    arena_.video.haveClockOffset = true;
    videoDecoder().setClockOffsetMs(offsetMs);
    send(ctrl::videoClock(offsetMs));
}

void Client::flushVideoLocate() {
    // A pointer reply arrives before its NALs. Let that entry finish before
    // automatic prefetch assigns a new query ID and rejects the arriving GOP.
    // Explicit timeline seeks reset lastLiveLocateMs_ and bypass this guard.
    if ((awaitingVideoBytes_ || awaitingVideoBatch_) && lastLiveLocateMs_ > 0.0 &&
        nowMs() - lastLiveLocateMs_ < 2500.0) return;
    awaitingVideoBytes_ = false;
    awaitingVideoBatch_ = false;
    arena_.video.seekDelivering = false;
    std::uint64_t atMs = 0;
    if (!videoDecoder().pendingLocate(atMs)) {
        locateSent_ = false;
        return;
    }
    const double now = nowMs();
    if (locateSent_) {
        // Do not sit on Buffering for 8 s. Pointer lookup is ~30 ms; if the
        // GOP is not here in 2 s it is a hole, not a slow store.
        const double waitMs = atMs == 0 ? 2000.0 : 2500.0;
        if (lastLiveLocateMs_ > 0.0 && now - lastLiveLocateMs_ < waitMs) {
            return;
        }
        locateSent_ = false;
        activeVideoRequest_ = 0;
        videoDecoder().cancelPendingLocate();
        if (!videoDecoder().hasFrame()) {
            locateBackoffMs_ = 400.0;
        }
        return;
    }
    double cooldownMs = locateBackoffMs_ > 0.0 ? locateBackoffMs_ : 0.0;
    if (atMs == 0) {
        cooldownMs = std::max(cooldownMs, 400.0);
    } else {
        cooldownMs = std::max(cooldownMs, 50.0);
    }
    if (lastLiveLocateMs_ > 0.0 && now - lastLiveLocateMs_ < cooldownMs) {
        return;
    }
    const std::uint64_t newestMs = videoDecoder().bufferedNewestMs();
    if (atMs != 0 && atMs == lastReplayLocateAtMs_ && newestMs == lastReplayLocateNewestMs_ &&
        lastLiveLocateMs_ > 0.0 && now - lastLiveLocateMs_ < 2500.0) {
        return;
    }
    std::uint16_t prefetch = 0;
    if (atMs != 0) {
        const auto& pb = videoDecoder().playback();
        prefetch = pb.replayWindowOn()
                       ? vidx::replayPrefetchGops(pb.replayMinCursorMs(), pb.replayMaxCursorMs())
                       : static_cast<std::uint16_t>(8);
    }
    auto request = ctrl::videoLocate(atMs, 0, prefetch);
    request.queryId = nextVideoRequest_++;
    if (request.queryId == 0) request.queryId = nextVideoRequest_++;
    if (send(request)) {
        activeVideoRequest_ = request.queryId;
        awaitingVideoBytes_ = atMs != 0;
        awaitingVideoBatch_ = atMs != 0;
        arena_.video.seekDelivering = awaitingVideoBytes_;
        locateSent_ = true;
        lastLiveLocateMs_ = now;
        if (atMs != 0) {
            lastReplayLocateAtMs_ = atMs;
            lastReplayLocateNewestMs_ = newestMs;
        }
        if (atMs != 0 || now - lastLocateLogMs_ >= 5000.0) {
            lastLocateLogMs_ = now;
            arena_.pushLog("video_locate " + std::to_string(atMs));
        }
    }
}

void Client::bindAnalysisRuntime(AnalysisRuntime* runtime) {
    analysisRuntime_ = runtime;
}

void Client::requestCatalogList() {
    send(ctrl::catalogList());
}

void Client::requestHistory(std::uint32_t queryId, std::string sessionId, std::uint64_t fromMs,
                            std::uint64_t toMs, std::vector<std::uint32_t> signalIds,
                            std::uint32_t maxPoints) {
    if (arena_.serverReplay() && sessionId == arena_.tiles().sessionId() && !selectedDecode_.empty()) {
        json::Object query; query.emplace("type", "history_query"); query.emplace("query_id", queryId);
        query.emplace("session_id", sessionId); query.emplace("decode_id", selectedDecode_);
        query.emplace("from_ms", fromMs); query.emplace("to_ms", toMs); query.emplace("max_points", maxPoints);
        json::Array ids; for (auto id : signalIds) ids.emplace_back(id); query.emplace("signal_ids", std::move(ids));
        sendExtension(json::stringify(json::Value(std::move(query))));
    } else send(ctrl::historyQuery(queryId, std::move(sessionId), fromMs, toMs, std::move(signalIds), maxPoints, 0));
}

void Client::cancelHistory(std::uint32_t queryId) {
    send(ctrl::historyCancel(queryId));
}

}  // namespace photon::ui
