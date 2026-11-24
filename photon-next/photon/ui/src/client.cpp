#include "photon_ui/client.hpp"

#include "photon/protocol_v2.hpp"
#include "photon_ui/analysis_runtime.hpp"
#include "photon_ui/page_url.hpp"
#include "photon_ui/platform.hpp"
#include "photon_ui/video_v2.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>
#else
#include "photon/net.hpp"
#include <cstdlib>
#include <memory>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace photon::ui {
namespace {

constexpr double kBackoffMs[] = {200.0, 500.0, 1000.0, 2000.0};
constexpr int kBackoffCount = 4;
constexpr double kPingMs = 1000.0;     // the server closes a socket silent for 5 s
constexpr double kGapLogMs = 5000.0;   // gaps can come in bursts; log one line per interval
constexpr std::uint16_t kCloseRefused = 1008;
constexpr double kRunsRefreshMs = 30000.0;  // runs_list again this often (and on reconnect)
constexpr double kTileTimeoutMs = 60000.0;  // a tile_request unanswered this long is given up
constexpr double kTileErrorLogMs = 5000.0;  // tile errors can come in bursts; one log line per interval

// Unix time in ns on this machine, for the live cursor offset.
std::int64_t wallNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

Client::Client(ClientArena& arena) : arena_(arena), url_(arena.wsUrl), token_(pageStoredToken()) {}

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
            return loginRefused_ ? "password refused" : "disconnected";
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
    if (url == url_) {
        return;
    }
    url_ = std::move(url);
    arena_.wsUrl = url_;
    if (state_ != State::Disconnected) {
        disconnect("server URL changed");
    }
    nextReconnectMs_ = 0.0;
}

void Client::setToken(std::string token) {
    token_ = std::move(token);
    loginRefused_ = false;
    askToken_ = false;
    nextReconnectMs_ = 0.0;
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

// Reads a close frame's payload: a 2-byte status code (big-endian), then text.
void closeInfo(const std::vector<std::uint8_t>& payload, std::uint16_t& code, std::string& reason) {
    code = 0;
    reason.clear();
    if (payload.size() < 2) {
        return;
    }
    code = static_cast<std::uint16_t>((payload[0] << 8) | payload[1]);
    reason.assign(payload.begin() + 2, payload.end());
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
            std::uint16_t code = 0;
            std::string reason;
            closeInfo(frame->payload, code, reason);
            const std::string why = "socket close code=" + std::to_string(code) + " " + reason;
            self.handleClose(why.c_str(), false, code, reason);
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
    nowMs_ = nowMs;
    arena_.noteTick(nowMs);
#ifndef __EMSCRIPTEN__
    if (ws_) {
        pumpNativeWs(*this, *ws_, wsHandshakeOk_);
    }
#endif
    if (state_ == State::HelloOk) {
        if (nowMs - lastPingMs_ >= kPingMs) {
            sendPing(nowMs);
            lastPingMs_ = nowMs;
        }
        if (nowMs - lastRunsMs_ >= kRunsRefreshMs) {
            lastRunsMs_ = nowMs;
            requestRuns();
        }
        // The live cursor is the server's now: this machine's clock plus the
        // offset measured at hello_ack.
        const auto serverNs = wallNs() + arena_.server.clockOffsetNs;
        arena_.setLiveCursor(static_cast<std::uint64_t>(std::max<std::int64_t>(serverNs, 0) / 1000000));
        arena_.syncRun(this);
        flushSubscribe();
        pumpVideo(nowMs);
        // A request nobody answered in time is given up, so its slot is not held for good.
        std::vector<std::uint32_t> stalled;
        for (const auto& [qid, sentMs] : tileSentMs_) {
            if (nowMs - sentMs >= kTileTimeoutMs) {
                stalled.push_back(qid);
            }
        }
        for (const std::uint32_t qid : stalled) {
            send(ctrl2::TileCancel{qid});
            handleTileError(qid, "no answer in time");
        }
        pumpTiles();
    }
    arena_.tiles().pump();
    if (analysisRuntime_ != nullptr) {
        analysisRuntime_->pump();
    }
    if (askToken_) {
        // The server refused the password. The web page asks once; native has
        // no prompt, so it stays refused until setToken is called.
        askToken_ = false;
        const std::string typed = pageAskToken();
        if (!typed.empty()) {
            token_ = typed;
            loginRefused_ = false;
            nextReconnectMs_ = 0.0;
            arena_.pushLog("trying the password again");
        }
    }
    if (state_ == State::Disconnected && !loginRefused_ && nowMs >= nextReconnectMs_) {
        connect();
    }
}

void Client::connect() {
    if (loginRefused_ || state_ == State::Connecting || state_ == State::Connected ||
        state_ == State::HelloOk) {
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
    std::uint16_t port = 6900;
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
    // The LAN password goes in the hello, not in an HTTP header.
    if (!ws_->beginClientHandshake(path, hostHdr, "")) {
        ws_.reset();
        arena_.pushLog("websocket handshake send failed", true);
        scheduleReconnect(nowMs());
        return;
    }
    state_ = State::Connecting;
    arena_.pushLog(std::string("connecting ") + url_);
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
    char buf[200];
    std::snprintf(buf, sizeof(buf), "socket close code=%u %s", static_cast<unsigned>(ev->code), ev->reason);
    static_cast<Client*>(user)->handleClose(buf, ev->code != 1000, ev->code, std::string_view(ev->reason));
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
    // Subscriptions are per connection: the next catalog on this socket is the
    // one the subscribe message must match.
    catalogSeen_ = false;
    sentSubscribed_.clear();
    subscribeDirty_ = true;
    resetVideo();
    arena_.pushLog("socket open, sending hello");
    send(ctrl2::Hello{"photon-ui", token_});
}

void Client::dropTiles() {
    // The requests died with the socket: the caches forget them and ask again later.
    arena_.tiles().abandonRequests();
    if (analysisRuntime_ != nullptr) {
        analysisRuntime_->abandonRequests();
    }
    queue_.clear();
    tileSentMs_.clear();
}

void Client::handleClose(const char* why, bool error, std::uint16_t closeCode, std::string_view reason) {
    if (state_ == State::Disconnected) {
        return;
    }
    dropTiles();
    resetVideo();
    const bool refused = closeCode == kCloseRefused && reason == "bad token" && state_ != State::HelloOk;
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
    if (refused) {
        // Do not retry with a wrong password: each try counts against the
        // server's per-IP limit. Ask (web) or wait for setToken (native).
        loginRefused_ = true;
        askToken_ = true;
        arena_.pushLog("the server refused the password", true);
        return;
    }
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

bool Client::send(const ctrl2::Message& msg) {
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
    const std::string json = ctrl2::serialize(msg);
#ifdef __EMSCRIPTEN__
    return emscripten_websocket_send_utf8_text(socket_, json.c_str()) >= 0;
#else
    ws_->queueText(json);
    return ws_->flushOutbound();
#endif
}

void Client::sendPing(double nowMs) {
    (void)nowMs;
    send(ctrl2::Ping{wallNs() / 1000000});
}

void Client::handleText(std::string_view text) {
    std::string error;
    auto parsed = ctrl2::parse(text, &error);
    if (!parsed) {
        arena_.pushLog(error.empty() ? "invalid control JSON" : error, true);
        return;
    }
    const ctrl2::Message& msg = *parsed;
    if (const auto* ack = std::get_if<ctrl2::HelloAck>(&msg)) {
        applyHelloAck(*ack);
    } else if (const auto* catalog = std::get_if<ctrl2::Catalog>(&msg)) {
        applyCatalog(*catalog);
    } else if (const auto* runs = std::get_if<ctrl2::Runs>(&msg)) {
        applyRuns(runs->runs);
    } else if (const auto* ping = std::get_if<ctrl2::Ping>(&msg)) {
        send(ctrl2::Pong{ping->tMs});
    } else if (const auto* err = std::get_if<ctrl2::Error>(&msg)) {
        arena_.pushLog(err->code + ": " + err->message, true);
        if (err->code == "video" || err->code.starts_with("bad_video")) {
            noteVideoError(videoSession_, nowMs_);
        }
    } else if (const auto* tileErr = std::get_if<ctrl2::TileError>(&msg)) {
        handleTileError(tileErr->qid, tileErr->message);
    } else if (const auto* status = std::get_if<ctrl2::VideoStatus>(&msg)) {
        applyVideoStatus(*status);
    }
    // Pongs only confirm the socket is alive.
}

void Client::applyVideoStatus(const ctrl2::VideoStatus& status) {
    // The status and the pieces travel on different queues at the server, so pieces of the
    // new stream can come before its status. handleVideoChunk takes them by their replay bit.
    noteVideoStatus(videoSession_, status);
    arena_.applyVideoStatus(status);
}

void Client::resetVideo() {
    videoSession_ = VideoSession{};
    videoAssembler_.reset();
    arena_.video.requested = VideoStream::Off;
    arena_.video.gap.reset();
    arena_.pushVideoRestart();
}

void Client::pumpVideo(double nowMs) {
    VideoInputs in;
    in.connected = state_ == State::HelloOk;
    in.serverHasVideo = arena_.video.available;
    in.lastWantedMs = arena_.video.wantedMs;
    in.pinned = arena_.catalogPinned();
    in.playing = arena_.timeline().mode == static_cast<std::uint8_t>(proto::TimelineMode::Play) &&
                 !arena_.hasPreviewCursor();
    in.cursorMs = arena_.videoCursorMs();
    const VideoCommand cmd = decideVideo(in, videoSession_, nowMs);
    arena_.video.requested = videoSession_.stream;
    switch (cmd.op) {
        case VideoOp::None:
            return;
        case VideoOp::SubscribeLive:
            send(ctrl2::VideoSubscribe{"live", 0});
            break;
        case VideoOp::SubscribeReplay:
            send(ctrl2::VideoSubscribe{"replay", static_cast<std::int64_t>(cmd.fromMs)});
            break;
        case VideoOp::Seek:
            send(ctrl2::VideoSeek{cmd.qid, static_cast<std::int64_t>(cmd.fromMs)});
            break;
        case VideoOp::Off:
            send(ctrl2::VideoSubscribe{"off", 0});
            break;
    }
    if (cmd.op != VideoOp::Off) {
        // Telemetry and video are separate: frames of the old stream are dropped by
        // the decoder, and the new stream's frames start from its keyframe.
        videoAssembler_.reset();
        arena_.pushVideoRestart();
    }
}

void Client::handleVideoChunk(std::span<const std::uint8_t> bytes) {
    if (videoSession_.stream == VideoStream::Off) {
        return;
    }
    const auto chunk = proto2::decodeVideoChunk(bytes);
    if (!chunk) {
        arena_.pushLog("bad VideoChunk", true);
        return;
    }
    // While a request is awaited, the pieces of the stream before it are not wanted: those
    // are the other kind (live or replay) by their replay bit.
    if (videoSession_.awaiting &&
        isReplayChunk(chunk->flags) != (videoSession_.awaitingStream == VideoStream::Replay)) {
        return;
    }
    if (auto frame = videoAssembler_.push(*chunk)) {
        arena_.pushVideoFrame(*frame);
    }
    arena_.video.dropped = videoAssembler_.dropped();
}

void Client::applyHelloAck(const ctrl2::HelloAck& ack) {
    const bool first = state_ != State::HelloOk;
    state_ = State::HelloOk;
    backoffIndex_ = 0;
    lastPingMs_ = 0.0;
    arena_.features = ack.features;
    arena_.video.available = std::find(ack.features.begin(), ack.features.end(), "video") != ack.features.end();
    arena_.server.helloOk = true;
    arena_.server.server = ack.server;
    arena_.server.user = ack.user;
    arena_.server.liveRun = ack.liveRun;
    arena_.server.levels = ack.levels;
    arena_.server.clockOffsetNs = ack.serverTimeNs - wallNs();
    arena_.tiles().setLevels(ack.levels);
    if (analysisRuntime_ != nullptr) {
        analysisRuntime_->setLevels(ack.levels);
    }
    arena_.syncRun(this);
    arena_.pushLog("hello_ack from " + (ack.server.empty() ? std::string("photon-server") : ack.server) +
                   " as " + ack.user + (ack.liveRun.empty() ? "" : " live run " + ack.liveRun));
    if (first) {
        arena_.pushLog("protocol " + std::to_string(ack.protocol) + ", features " +
                       std::to_string(ack.features.size()));
    }
    lastRunsMs_ = nowMs_;
    requestRuns();
    pumpTiles();
}

void Client::applyRuns(std::vector<ctrl2::Run> runs) {
    arena_.runs = std::move(runs);
    // The live run is the one the server flags live; hello_ack names it too.
    for (const auto& run : arena_.runs) {
        if (run.live) {
            if (!run.id.empty() && arena_.server.liveRun != run.id) {
                arena_.server.liveRun = run.id;
            }
            break;
        }
    }
    arena_.syncRun(this);
    if (analysisRuntime_ != nullptr) {
        analysisRuntime_->applyRuns(arena_.runs);
    }
    arena_.pushLog("runs " + std::to_string(arena_.runs.size()));
}

void Client::applyCatalog(const ctrl2::Catalog& catalog) {
    const CatalogChange change = arena_.applyCatalog(catalog);
    catalogSeen_ = true;
    serverSignalCount_ = change.serverCount;
    // The server drops every subscription when it sends a catalog, so the
    // whole set is sent again on the next flush.
    sentSubscribed_.clear();
    subscribeDirty_ = true;
    if (change.changed) {
        // Ids changed meaning with the sha. Move the wanted set by name; the
        // signal map and alarms re-resolve from the new catalog.
        std::unordered_set<std::uint32_t> moved;
        for (const std::uint32_t id : subscribed_) {
            if (const auto it = change.remap.find(id); it != change.remap.end()) {
                moved.insert(it->second);
            }
        }
        subscribed_ = std::move(moved);
        std::unordered_set<std::uint32_t> movedInterest;
        for (const std::uint32_t id : interest_) {
            if (const auto it = change.remap.find(id); it != change.remap.end()) {
                movedInterest.insert(it->second);
            }
        }
        interest_ = std::move(movedInterest);
        arena_.pushLog("catalog " + (catalog.sha.empty() ? std::string("(decoding off)") : catalog.sha.substr(0, 12)) +
                       ": " + std::to_string(change.serverCount) + " signals");
    }
    arena_.syncRun(this);
}

void Client::applyLiveSamples(std::span<const std::uint8_t> bytes) {
    const auto live = proto2::decodeLiveSamples(bytes);
    if (!live) {
        arena_.pushLog("bad LiveSamples", true);
        return;
    }
    const auto blocks = liveBlocks(*live, serverSignalCount_);
    if (!blocks.empty()) {
        arena_.ingestTelemetry(blocks);
    }
}

void Client::flushSubscribe() {
    if (state_ != State::HelloOk || !catalogSeen_ || !subscribeDirty_) {
        return;
    }
    subscribeDirty_ = false;
    auto indexes = subscribeIndexes(subscribed_, serverSignalCount_);
    if (indexes == sentSubscribed_) {
        return;
    }
    sentSubscribed_ = indexes;
    send(ctrl2::Subscribe{std::move(indexes)});
}

void Client::handleBinary(std::span<const std::uint8_t> bytes) {
    const auto frame = proto2::decodeFrame(bytes);
    if (!frame) {
        arena_.pushLog("bad binary frame", true);
        return;
    }
    switch (frame->header.type) {
        case static_cast<std::uint8_t>(proto2::MsgType::LiveSamples):
            applyLiveSamples(bytes);
            break;
        case static_cast<std::uint8_t>(proto2::MsgType::Gap): {
            const auto gap = proto2::decodeGap(bytes);
            if (!gap) {
                arena_.pushLog("bad Gap", true);
                return;
            }
            arena_.ingestGap(gap->fromNs, gap->toNs);
            const double now = nowMs();
            if (now - lastGapLogMs_ >= kGapLogMs) {
                lastGapLogMs_ = now;
                arena_.pushLog("live gap: the client fell behind");
            }
            break;
        }
        case static_cast<std::uint8_t>(proto2::MsgType::Tile): {
            const auto piece = proto2::decodeTile(bytes);
            if (!piece) {
                arena_.pushLog("bad Tile piece", true);
                return;
            }
            routeTilePiece(*piece);
            break;
        }
        case static_cast<std::uint8_t>(proto2::MsgType::VideoChunk):
            handleVideoChunk(bytes);
            break;
        default:
            break;
    }
}

void Client::setSubscribed(std::uint32_t id, bool on) {
    if (id == 0) {
        return;
    }
    if (on) {
        if (interestOpen_) {
            interest_.insert(id);
        }
        if (subscribed_.insert(id).second) {
            subscribeDirty_ = true;
        }
    } else if (subscribed_.erase(id) > 0) {
        subscribeDirty_ = true;
    }
}

void Client::ensureSubscribed(std::span<const std::uint32_t> ids) {
    for (const std::uint32_t id : ids) {
        if (id == 0) {
            continue;
        }
        if (interestOpen_) {
            interest_.insert(id);
        }
        if (subscribed_.insert(id).second) {
            subscribeDirty_ = true;
        }
    }
}

void Client::beginInterest() {
    interestOpen_ = true;
    interest_.clear();
}

void Client::commitInterest() {
    if (!interestOpen_) {
        return;
    }
    interestOpen_ = false;
    for (auto it = subscribed_.begin(); it != subscribed_.end();) {
        if (interest_.contains(*it)) {
            ++it;
        } else {
            it = subscribed_.erase(it);
            subscribeDirty_ = true;
        }
    }
}

void Client::requestRuns() {
    send(ctrl2::RunsList{});
}

void Client::requestTile(std::uint32_t queryId, const std::string& runId, std::uint32_t signalId, int level,
                         std::int64_t index, bool urgent) {
    if (queryId == 0 || signalId == 0) {
        return;
    }
    queue_.push(TileRequestQueue::Request{queryId, runId, signalId, level, index, urgent});
    pumpTiles();
}

void Client::cancelTile(std::uint32_t queryId) {
    if (queue_.dropQueued(queryId)) {
        return;
    }
    if (queue_.isInflight(queryId)) {
        send(ctrl2::TileCancel{queryId});
        finishTile(queryId);
    }
}

void Client::pumpTiles() {
    if (state_ != State::HelloOk) {
        return;
    }
    for (const auto& r : queue_.take()) {
        tileSentMs_[r.qid] = nowMs_;
        // The wire signal is the catalog index: the client id minus one.
        send(ctrl2::TileRequest{r.qid, r.run, r.signal - 1, r.level, r.index});
    }
}

void Client::finishTile(std::uint32_t qid) {
    queue_.done(qid);
    tileSentMs_.erase(qid);
}

TileCache* Client::cacheFor(std::uint32_t qid) {
    if (isAnalysisQueryId(qid)) {
        return analysisRuntime_ != nullptr ? analysisRuntime_->cacheForQuery(qid) : nullptr;
    }
    return &arena_.tiles();
}

void Client::routeTilePiece(const proto2::TilePiece& piece) {
    const std::uint32_t qid = piece.qid;
    if (!queue_.isInflight(qid)) {
        // Cancelled, given up, or not ours: a late piece is dropped.
        return;
    }
    const bool last = (piece.flags & proto2::kTileLast) != 0;
    TileCache* cache = cacheFor(qid);
    if (cache != nullptr) {
        cache->ingestPiece(piece);
    }
    if (last) {
        finishTile(qid);
        pumpTiles();
        return;
    }
    if (cache == nullptr || !cache->owns(qid)) {
        // The cache failed the request (a gap in the offsets, or a bad piece). Stop
        // the server from sending the rest.
        send(ctrl2::TileCancel{qid});
        finishTile(qid);
        pumpTiles();
    }
}

void Client::handleTileError(std::uint32_t qid, const std::string& message) {
    if (!queue_.isInflight(qid)) {
        return;
    }
    if (TileCache* cache = cacheFor(qid); cache != nullptr) {
        cache->ingestError(qid);
    }
    finishTile(qid);
    if (nowMs_ - lastTileErrorLogMs_ >= kTileErrorLogMs) {
        lastTileErrorLogMs_ = nowMs_;
        arena_.pushLog("tile " + std::to_string(qid) + ": " + message, true);
    }
    pumpTiles();
}

void Client::bindAnalysisRuntime(AnalysisRuntime* runtime) {
    analysisRuntime_ = runtime;
}

}  // namespace photon::ui
