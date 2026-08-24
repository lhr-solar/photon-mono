#include "photon/session.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_set>
#include <utility>

namespace photon {
namespace {

constexpr std::uint64_t kTelemetryPeriodMs = 33;  // ~30 Hz
constexpr std::uint64_t kCanBurstPeriodMs = 100;
constexpr std::uint64_t kTimelinePeriodMs = 1000;
constexpr std::uint64_t kTxWatchdogMs = 3000;
constexpr std::size_t kVideoCongestBytes = 512 * 1024;

std::uint64_t nowMs() {
    using clock = std::chrono::system_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

}  // namespace

SessionHub::SessionHub(std::uint16_t wsPort, std::string daqHost, std::uint16_t daqPort, std::string assetsDir,
                       std::string videoHost, std::uint16_t videoPort, ctrl::SourceKind startupSource,
                       std::string startupLogPath)
    : wsPort_(wsPort)
    , daqHost_(std::move(daqHost))
    , daqPort_(daqPort)
    , sources_(daqHost_, daqPort_)
    , videoRelay_(videoHost.empty() ? daqHost_ : std::move(videoHost), videoPort == 0 ? 6700 : videoPort)
    , catalog_(TrackCatalog::tracksDirFromAssets(assetsDir.empty() ? TrackCatalog::discoverAssetsDir() : assetsDir))
    , assetsDir_(assetsDir.empty() ? TrackCatalog::discoverAssetsDir() : std::move(assetsDir))
    , startupSource_(startupSource)
    , startupLogPath_(std::move(startupLogPath)) {}

bool SessionHub::start() {
    listener_ = net::listenLoopback(wsPort_);
    if (!listener_.valid()) {
        std::fprintf(stderr, "[photon-backend] failed to bind 127.0.0.1:%u\n", static_cast<unsigned>(wsPort_));
        return false;
    }
    std::fprintf(stdout, "[photon-backend] websocket listening on ws://127.0.0.1:%u\n", static_cast<unsigned>(wsPort_));
    std::fprintf(stdout, "[photon-backend] daq_tcp default %s:%u\n", daqHost_.c_str(), static_cast<unsigned>(daqPort_));
    std::fprintf(stdout, "[photon-backend] video default %s:%u\n", videoRelay_.host().c_str(),
                 static_cast<unsigned>(videoRelay_.port()));
    std::fprintf(stdout, "[photon-backend] track_sim catalog %s\n", catalog_.tracksDir().c_str());
    std::string dbcError;
    if (loadBundledDbc("carcan_telemcan", dbcError)) {
        std::fprintf(stdout, "[photon-backend] dbc bundled carcan_telemcan (%zu messages)\n",
                     dbc_.messages.size());
    } else {
        std::fprintf(stdout, "[photon-backend] dbc not loaded (%s)\n", dbcError.c_str());
    }
    std::string selectError;
    if (!applySelectSource(startupSource_, daqHost_, daqPort_, startupLogPath_, selectError)) {
        std::fprintf(stderr, "[photon-backend] failed to activate --source %s: %s\n",
                     std::string(ctrl::sourceName(startupSource_)).c_str(), selectError.c_str());
        return false;
    }
    std::fprintf(stdout, "[photon-backend] waiting for UI hello\n");
    return true;
}

bool SessionHub::applySelectSource(ctrl::SourceKind kind, const std::string& host, std::uint16_t port,
                                   const std::string& path, std::string& error, const pcan::Config& pcanCfg) {
    if (!sources_.select(kind, host, port, path, error, pcanCfg)) {
        return false;
    }
    lastTelemetryMs_ = 0;
    lastCanBurstMs_ = 0;
    lastTimelineMs_ = 0;
    return true;
}

void SessionHub::stop() {
    for (auto& client : clients_) {
        if (client->txArmed) {
            disarm(*client, "server_stop");
        }
        client->conn.queueClose(1001, "server stop");
        client->conn.flushOutbound();
    }
    clients_.clear();
    for (auto& job : trackSimJobs_) {
        if (job.future.valid()) {
            job.future.wait();
        }
    }
    trackSimJobs_.clear();
    sources_.stop();
    videoRelay_.stop();
    listener_.close();
}

void SessionHub::acceptNew() {
    for (;;) {
        net::Socket sock = net::acceptClient(listener_);
        if (!sock.valid()) {
            break;
        }
        auto client = std::make_unique<ClientSession>(std::move(sock));
        client->id = nextClientId_++;
        client->label = client->conn.peer();
        client->lastHeartbeatMs = nowMs();
        std::fprintf(stdout, "[photon-backend] client connected %s\n", client->label.c_str());
        clients_.push_back(std::move(client));
    }
}

void SessionHub::sendControl(ClientSession& client, const ctrl::Message& msg) {
    client.conn.queueText(ctrl::serialize(msg));
}

void SessionHub::sendBinary(ClientSession& client, proto::MsgType type, std::span<const std::uint8_t> payload) {
    const auto frame = proto::encodeFrame(type, binarySeq_++, payload);
    client.conn.queueBinary(frame);
}

void SessionHub::disarm(ClientSession& client, std::string reason) {
    if (!client.txArmed && reason != "client_disconnect") {
        sendControl(client, ctrl::txArmStatus(false, reason));
        return;
    }
    client.txArmed = false;
    std::fprintf(stdout, "[photon-backend] TX disarmed (%s) client=%s\n", reason.c_str(), client.label.c_str());
    if (reason != "client_disconnect") {
        sendControl(client, ctrl::txArmStatus(false, std::move(reason)));
    }
}

void SessionHub::handleText(ClientSession& client, std::string_view text, std::uint64_t nowMs) {
    std::string error;
    auto msg = ctrl::parse(text, &error);
    if (!msg) {
        sendControl(client, ctrl::errorAlert("bad_message", error.empty() ? "invalid JSON control" : error));
        return;
    }
    client.lastHeartbeatMs = nowMs;

    switch (msg->type) {
        case ctrl::Type::Hello:
            if (msg->protocolVersion != ctrl::kProtocolVersion) {
                sendControl(client, ctrl::errorAlert("protocol_mismatch", "server speaks photon-wire v1"));
                return;
            }
            client.helloOk = true;
            sendControl(client, ctrl::helloAck(
                                    {"synthetic", "daq_tcp", "log_replay", "pcan", "dbc", "track_sim", "video"},
                                    sources_.activeSource()));
            sendCatalog(client);
            std::fprintf(stdout, "[photon-backend] hello from %s (%s)\n", client.label.c_str(),
                         msg->client.empty() ? "ui" : msg->client.c_str());
            break;
        case ctrl::Type::SelectSource: {
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            std::string selectError;
            pcan::Config pcanCfg;
            pcanCfg.channel = msg->channel;
            pcanCfg.baud = msg->baud;
            pcanCfg.listenOnly = msg->listenOnly;
            if (!applySelectSource(msg->source, msg->host, msg->port, msg->path, selectError, pcanCfg)) {
                const char* code = "bad_source";
                if (msg->source == ctrl::SourceKind::Pcan) {
                    code = selectError == pcan::kUnavailableMessage ? "pcan_unavailable" : "bad_source";
                }
                sendControl(client, ctrl::errorAlert(code, selectError));
                return;
            }
            break;
        }
        case ctrl::Type::SubscribeSignals:
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            for (const auto id : msg->signalIds) {
                client.subscribed.insert(id);
            }
            break;
        case ctrl::Type::UnsubscribeSignals:
            for (const auto id : msg->signalIds) {
                client.subscribed.erase(id);
            }
            break;
        case ctrl::Type::TimelineControl:
            std::fprintf(stdout, "[photon-backend] timeline_control mode=%s cursor=%llu speed=%.3f\n",
                         std::string(ctrl::timelineName(msg->timelineMode)).c_str(),
                         static_cast<unsigned long long>(msg->cursorMs), msg->speed);
            sources_.controlTimeline(msg->timelineMode, msg->cursorMs, msg->speed);
            break;
        case ctrl::Type::SelectDbc: {
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            std::string dbcError;
            bool ok = false;
            if (!msg->path.empty()) {
                ok = loadDbcFile(msg->path, dbcError);
            } else if (!msg->dbcId.empty()) {
                ok = loadBundledDbc(msg->dbcId, dbcError);
            } else {
                dbcError = "select_dbc requires path or id";
            }
            if (!ok) {
                sendControl(client, ctrl::errorAlert("bad_dbc", dbcError));
                return;
            }
            broadcastCatalog();
            break;
        }
        case ctrl::Type::TxArmRequest:
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            if (msg->armed) {
                client.txArmed = true;
                client.lastHeartbeatMs = nowMs;
                sendControl(client, ctrl::txArmStatus(true, "armed"));
                std::fprintf(stdout, "[photon-backend] TX armed client=%s\n", client.label.c_str());
            } else {
                disarm(client, "client_request");
            }
            break;
        case ctrl::Type::TxCanFrame: {
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            if (sources_.kind() != ctrl::SourceKind::Pcan) {
                sendControl(client, ctrl::errorAlert("tx_refused", "source is not pcan"));
                return;
            }
            if (sources_.listenOnly()) {
                sendControl(client, ctrl::errorAlert("tx_refused", "listen_only is enabled"));
                return;
            }
            if (!client.txArmed) {
                sendControl(client, ctrl::errorAlert("tx_refused", "TX is not armed"));
                return;
            }
            if (!dbcLoaded_) {
                sendControl(client, ctrl::errorAlert("tx_refused", "DBC is not loaded"));
                return;
            }
            std::vector<dbc::SignalValue> values;
            values.reserve(msg->txSignals.size());
            for (const auto& sig : msg->txSignals) {
                values.push_back(dbc::SignalValue{sig.name, sig.value});
            }
            dbc::EncodedFrame frame;
            std::string encError;
            if (!dbc::encodeFrame(dbc_, msg->message, values, frame, &encError)) {
                sendControl(client, ctrl::errorAlert("tx_refused", encError.empty() ? "DBC encode failed" : encError));
                return;
            }
            std::string writeError;
            if (!sources_.writeCan(frame, writeError)) {
                sendControl(client, ctrl::errorAlert("tx_refused", writeError));
                return;
            }
            std::fprintf(stdout, "[photon-backend] tx_can_frame %s id=0x%X dlc=%u client=%s\n",
                         msg->message.c_str(), static_cast<unsigned>(frame.canId),
                         static_cast<unsigned>(frame.dlc), client.label.c_str());
            break;
        }
        case ctrl::Type::Heartbeat:
            break;
        case ctrl::Type::TrackSimList:
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            sendControl(client, ctrl::trackSimListResult(catalog_.list()));
            break;
        case ctrl::Type::TrackSimRequest:
            enqueueTrackSim(client, *msg);
            break;
        case ctrl::Type::VideoSubscribe: {
            if (!client.helloOk) {
                sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
                return;
            }
            client.videoSubscribed = true;
            videoRelay_.subscribe(client.id, msg->videoMode, msg->startMs, msg->host, msg->port);
            streamVideoStatus(nowMs, true);
            std::fprintf(stdout, "[photon-backend] video_subscribe client=%s mode=%s start=%llu\n",
                         client.label.c_str(), std::string(ctrl::videoModeName(msg->videoMode)).c_str(),
                         static_cast<unsigned long long>(msg->startMs));
            break;
        }
        case ctrl::Type::VideoStop:
            videoRelay_.unsubscribe(client.id);
            streamVideoStatus(nowMs, true);
            client.videoSubscribed = false;
            break;
        case ctrl::Type::HelloAck:
        case ctrl::Type::TxArmStatus:
        case ctrl::Type::ErrorAlert:
        case ctrl::Type::TrackSimResult:
        case ctrl::Type::SignalCatalog:
        case ctrl::Type::VideoStatus:
            sendControl(client, ctrl::errorAlert("bad_message", "server-bound message type not accepted"));
            break;
    }
}

void SessionHub::serviceClient(ClientSession& client, std::uint64_t nowTick, short revents) {
    if ((revents & (net::kPollHup | net::kPollErr)) != 0 && (revents & net::kPollIn) == 0) {
        client.conn.queueClose(1001, "peer error");
        return;
    }

    if (!client.conn.handshakeComplete()) {
        const auto hs = client.conn.readHandshake();
        if (hs == ws::HandshakeStatus::Incomplete) {
            return;
        }
        if (hs == ws::HandshakeStatus::BadRequest) {
            const char* resp = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            client.conn.socket().sendSome(resp, std::strlen(resp));
            client.failed = true;
            return;
        }
        if (!client.conn.writeHandshakeResponse()) {
            return;
        }
        std::fprintf(stdout, "[photon-backend] websocket handshake ok %s\n", client.label.c_str());
    }

    if ((revents & net::kPollIn) != 0 || client.conn.handshakeComplete()) {
        if (!client.conn.readAvailable()) {
            client.conn.queueClose(1001, "read error");
            return;
        }
        while (auto frame = client.conn.takeFrame()) {
            if (frame->opcode == ws::Opcode::Text) {
                const std::string_view text(reinterpret_cast<const char*>(frame->payload.data()), frame->payload.size());
                handleText(client, text, nowTick);
            } else if (frame->opcode == ws::Opcode::Close) {
                std::fprintf(stdout, "[photon-backend] client close %s\n", client.label.c_str());
            }
        }
    }

    if ((revents & net::kPollOut) != 0 || client.conn.hasOutbound()) {
        if (!client.conn.flushOutbound()) {
            client.conn.queueClose(1001, "write error");
        }
    }
}

void SessionHub::dropClosed() {
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                  [this](const std::unique_ptr<ClientSession>& client) {
                                      if (!client->failed && !client->conn.wantsClose() && client->conn.socket().valid()) {
                                          return false;
                                      }
                                      if (client->txArmed) {
                                          disarm(*client, "client_disconnect");
                                      }
                                      if (client->videoSubscribed) {
                                          videoRelay_.unsubscribe(client->id);
                                      }
                                      std::fprintf(stdout, "[photon-backend] client dropped %s\n", client->label.c_str());
                                      return true;
                                  }),
                   clients_.end());
}

void SessionHub::streamTelemetry(std::uint64_t nowTick) {
    if (nowTick - lastTelemetryMs_ < kTelemetryPeriodMs) {
        return;
    }
    lastTelemetryMs_ = nowTick;
    std::unordered_set<std::uint32_t> allIds;
    for (const auto& client : clients_) {
        if (client->helloOk) {
            allIds.insert(client->subscribed.begin(), client->subscribed.end());
        }
    }
    if (allIds.empty()) {
        return;
    }
    const auto blocks = sources_.takeTelemetry(allIds);
    if (blocks.empty()) {
        return;
    }
    for (auto& client : clients_) {
        if (!client->helloOk || client->subscribed.empty()) {
            continue;
        }
        std::vector<proto::TelemetryBlock> filtered;
        for (const auto& block : blocks) {
            if (client->subscribed.contains(block.signalId)) {
                filtered.push_back(block);
            }
        }
        if (filtered.empty()) {
            continue;
        }
        const auto payload = proto::encodeTelemetryBatch(filtered);
        sendBinary(*client, proto::MsgType::TelemetryBatch, payload);
    }
}

void SessionHub::streamCan(std::uint64_t nowTick) {
    if (nowTick - lastCanBurstMs_ < kCanBurstPeriodMs) {
        return;
    }
    lastCanBurstMs_ = nowTick;
    auto frames = sources_.takeCanFrames();
    if (frames.empty()) {
        return;
    }
    // Keep bursts small for the UI monitor.
    if (frames.size() > 32) {
        frames.erase(frames.begin(), frames.end() - 32);
    }
    const auto payload = proto::encodeCanMonitorBatch(frames);
    for (auto& client : clients_) {
        if (client->helloOk) {
            sendBinary(*client, proto::MsgType::CanMonitorBatch, payload);
        }
    }
}

void SessionHub::streamTimeline(std::uint64_t nowTick) {
    if (nowTick - lastTimelineMs_ < kTimelinePeriodMs) {
        return;
    }
    lastTimelineMs_ = nowTick;
    if (sources_.kind() == ctrl::SourceKind::None) {
        return;
    }
    const auto status = sources_.timeline();
    const auto payload = proto::encodeTimelineStatus(status);
    for (auto& client : clients_) {
        if (client->helloOk) {
            sendBinary(*client, proto::MsgType::TimelineStatus, payload);
        }
    }
}

void SessionHub::streamVideo() {
    auto frames = videoRelay_.takeFrames();
    if (frames.empty()) {
        return;
    }
    for (const auto& frame : frames) {
        proto::VideoFrame vf;
        vf.timestampMs = frame.timestampMs;
        vf.flags = frame.flags;
        vf.nal = frame.payload;
        const auto payload = proto::encodeVideoFrame(vf);
        const bool key = proto::videoKeyframe(frame.flags);
        const bool cfg = proto::videoConfig(frame.flags);
        for (auto& client : clients_) {
            if (!client->helloOk || !client->videoSubscribed) {
                continue;
            }
            // Slow-WS-client: drop non-keyframe (and non-config) when the send buffer is congested.
            // Config NALs stay — they are tiny and required for decoder init.
            if (client->conn.outboundBytes() > kVideoCongestBytes && !key && !cfg) {
                videoRelay_.noteDropped();
                continue;
            }
            sendBinary(*client, proto::MsgType::VideoFrame, payload);
        }
    }
}

void SessionHub::streamVideoStatus(std::uint64_t nowTick, bool force) {
    const bool changed = videoRelay_.consumeStatusChanged();
    if (!force && !changed) {
        if (!videoRelay_.hasSubscribers()) {
            return;
        }
        if (lastVideoStatusMs_ != 0 && nowTick - lastVideoStatusMs_ < 1000) {
            return;
        }
    }
    lastVideoStatusMs_ = nowTick;
    const auto msg = videoRelay_.statusMessage();
    for (auto& client : clients_) {
        if (client->helloOk && client->videoSubscribed) {
            sendControl(*client, msg);
        }
    }
}

void SessionHub::checkTxWatchdog(std::uint64_t nowTick) {
    if (nowTick - lastTxCheckMs_ < 250) {
        return;
    }
    lastTxCheckMs_ = nowTick;
    for (auto& client : clients_) {
        if (client->txArmed && nowTick - client->lastHeartbeatMs > kTxWatchdogMs) {
            disarm(*client, "heartbeat_timeout");
        }
    }
}

void SessionHub::tick(std::uint64_t nowTick) {
    std::vector<net::PollFd> fds;
    fds.push_back(net::PollFd{listener_.native(), net::kPollIn, 0});
    for (auto& client : clients_) {
        short events = net::kPollIn;
        if (client->conn.hasOutbound()) {
            events = static_cast<short>(events | net::kPollOut);
        }
        fds.push_back(net::PollFd{client->conn.socket().native(), events, 0});
    }
    sources_.attachPoll(fds);
    videoRelay_.attachPoll(fds);
    net::poll(fds.data(), fds.size(), 5);

    const std::size_t nClients = clients_.size();
    if (!fds.empty() && (fds[0].revents & net::kPollIn) != 0) {
        acceptNew();
    }

    for (std::size_t i = 0; i < clients_.size(); ++i) {
        short revents = 0;
        if (i < nClients) {
            const std::size_t fdIndex = i + 1;
            if (fdIndex < fds.size() && fds[fdIndex].fd == clients_[i]->conn.socket().native()) {
                revents = fds[fdIndex].revents;
            }
        }
        serviceClient(*clients_[i], nowTick, revents);
        if (clients_[i]->conn.hasOutbound()) {
            clients_[i]->conn.flushOutbound();
        }
    }

    sources_.onPoll(fds);
    videoRelay_.onPoll(fds);
    sources_.tick(nowTick);
    videoRelay_.tick(nowTick);
    streamTelemetry(nowTick);
    streamCan(nowTick);
    streamTimeline(nowTick);
    streamVideo();
    streamVideoStatus(nowTick, false);
    checkTxWatchdog(nowTick);
    pollTrackSimJobs();
    dropClosed();

    for (auto& client : clients_) {
        if (client->conn.hasOutbound()) {
            client->conn.flushOutbound();
        }
    }
}

ClientSession* SessionHub::findClient(std::uint64_t id) {
    for (auto& client : clients_) {
        if (client && client->id == id) {
            return client.get();
        }
    }
    return nullptr;
}

void SessionHub::enqueueTrackSim(ClientSession& client, const ctrl::Message& req) {
    if (!client.helloOk) {
        sendControl(client, ctrl::errorAlert("bad_message", "hello required first"));
        return;
    }
    if (!(req.timeBudgetS > 0.0)) {
        sendControl(client, ctrl::errorAlert("bad_message", "time_budget_s must be positive"));
        return;
    }

    tracksim::SolveInput input;
    input.timeBudgetS = req.timeBudgetS;
    input.initialSoc = req.initialSoc;
    input.car = tracksim::applyCarOverride(tracksim::defaultCar(), req.carOverride);

    if (req.track) {
        input.track = *req.track;
    } else if (!req.trackId.empty()) {
        std::string loadError;
        auto track = catalog_.load(req.trackId, &loadError);
        if (!track) {
            sendControl(client, ctrl::errorAlert("unknown_track", loadError.empty() ? "unknown track_id" : loadError));
            return;
        }
        input.track = std::move(*track);
    } else {
        sendControl(client, ctrl::errorAlert("bad_message", "track_id or inline track required"));
        return;
    }

    const std::string trackId = input.track.id.empty() ? req.trackId : input.track.id;
    const std::uint64_t clientId = client.id;
    trackSimJobs_.push_back(TrackSimJob{
        clientId,
        std::async(std::launch::async,
                   [input, trackId]() { return ctrl::trackSimResult(tracksim::solve(input), trackId); }),
    });
}

void SessionHub::pollTrackSimJobs() {
    for (auto it = trackSimJobs_.begin(); it != trackSimJobs_.end();) {
        if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        auto msg = it->future.get();
        if (auto* client = findClient(it->clientId)) {
            sendControl(*client, msg);
        }
        it = trackSimJobs_.erase(it);
    }
}

namespace {

bool safeDbcId(std::string_view id) {
    if (id.empty() || id.size() > 96) {
        return false;
    }
    for (char c : id) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

}  // namespace

bool SessionHub::loadDbcFile(const std::string& path, std::string& error) {
    auto db = dbc::loadFile(path, &error);
    if (!db) {
        return false;
    }
    dbc_ = std::move(*db);
    dbcLoaded_ = true;
    sources_.setDatabase(&dbc_);
    return true;
}

bool SessionHub::loadBundledDbc(const std::string& id, std::string& error) {
    if (!safeDbcId(id)) {
        error = "invalid bundled DBC id";
        return false;
    }
    const auto path = (std::filesystem::path(assetsDir_) / "dbc" / (id + ".dbc")).string();
    if (!loadDbcFile(path, error)) {
        return false;
    }
    dbc_.label = id;
    return true;
}

ctrl::Message SessionHub::catalogMessage() const {
    std::vector<ctrl::CatalogSignal> entries;
    if (dbcLoaded_) {
        for (const auto& e : dbc_.catalog()) {
            ctrl::CatalogSignal row;
            row.id = e.id;
            row.message = e.message;
            row.name = e.name;
            row.unit = e.unit;
            row.min = e.min;
            row.max = e.max;
            entries.push_back(std::move(row));
        }
    }
    return ctrl::signalCatalog(std::move(entries));
}

void SessionHub::sendCatalog(ClientSession& client) {
    sendControl(client, catalogMessage());
}

void SessionHub::broadcastCatalog() {
    const auto msg = catalogMessage();
    for (auto& client : clients_) {
        if (client->helloOk) {
            sendControl(*client, msg);
        }
    }
}

}  // namespace photon
