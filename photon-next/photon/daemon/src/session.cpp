#include "photon/session.hpp"

#include "session_internal.hpp"

#include "photon/log_sessions.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

namespace photon {
namespace {

std::uint64_t nowMs() {
    using clock = std::chrono::system_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

}  // namespace

SessionHub::SessionHub(std::uint16_t wsPort, std::string daqHost, std::uint16_t daqPort, std::string assetsDir,
                       std::string videoHost, std::uint16_t videoPort, ctrl::SourceKind startupSource,
                       std::string startupLogPath, std::string tsdbDir, std::uint16_t videoUdpPort)
    : wsPort_(wsPort)
    , daqHost_(std::move(daqHost))
    , daqPort_(daqPort)
    , sources_(daqHost_, daqPort_)
    , videoRelay_(videoHost.empty() ? daqHost_ : std::move(videoHost), videoPort == 0 ? 6700 : videoPort,
                  videoUdpPort)
    , catalog_(TrackCatalog::tracksDirFromAssets(assetsDir.empty() ? TrackCatalog::discoverAssetsDir() : assetsDir))
    , assetsDir_(assetsDir.empty() ? TrackCatalog::discoverAssetsDir() : std::move(assetsDir))
    , startupSource_(startupSource)
    , startupLogPath_(std::move(startupLogPath))
    , tsdbDir_(std::move(tsdbDir)) {
    if (tsdbDir_.empty()) {
        if (const char* env =
#if defined(_MSC_VER)
                []() -> const char* {
                    static char buf[512];
                    size_t n = 0;
                    return getenv_s(&n, buf, sizeof(buf), "PHOTON_TSDB") == 0 && n > 1 ? buf : nullptr;
                }()
#else
                std::getenv("PHOTON_TSDB")
#endif
        ) {
            tsdbDir_ = env;
        } else {
            tsdbDir_ = "data/tsdb";
        }
    }
    historyPool_ = std::make_unique<HistoryPool>();
}

SessionHub::~SessionHub() {
    stop();
}

void SessionHub::setClientPassword(std::string password) {
    sources_.setClientPassword(password);
    videoRelay_.setPassword(std::move(password));
}

void SessionHub::setAnalysisCompute(std::string addr) {
    analysisComputeAddr_ = std::move(addr);
}

void SessionHub::setHistoryRemote(std::string host, std::uint16_t port, std::string path, std::string password) {
    historyRemote_.setTarget(std::move(host), port, std::move(path), std::move(password));
    if (historyRemote_.enabled()) {
        std::fprintf(stdout, "[photon-backend] history-remote enabled\n");
    }
}

bool SessionHub::start() {
    listener_ = net::listenLoopback(wsPort_);
    if (!listener_.valid()) {
        std::fprintf(stderr, "[photon-backend] failed to bind 127.0.0.1:%u\n", static_cast<unsigned>(wsPort_));
        return false;
    }
    std::fprintf(stdout, "[photon-backend] websocket listening on ws://127.0.0.1:%u\n",
                 static_cast<unsigned>(listener_.localPort() != 0 ? listener_.localPort() : wsPort_));
    std::fprintf(stdout, "[photon-backend] daq_tcp default %s:%u\n", daqHost_.c_str(), static_cast<unsigned>(daqPort_));
    std::fprintf(stdout, "[photon-backend] video default %s:%u\n", videoRelay_.host().c_str(),
                 static_cast<unsigned>(videoRelay_.port()));
    std::fprintf(stdout, "[photon-backend] track_sim catalog %s\n", catalog_.tracksDir().c_str());
    if (analysisComputeAddr_.empty()) {
        std::fprintf(stdout, "[photon-backend] analysis-compute off\n");
    } else {
        std::fprintf(stdout, "[photon-backend] analysis-compute %s\n", analysisComputeAddr_.c_str());
    }
    std::string dbcError;
    if (loadBundledDbc("carcan_telemcan", dbcError)) {
        std::fprintf(stdout, "[photon-backend] dbc bundled carcan_telemcan (%zu messages)\n",
                     dbc_.messages.size());
    } else {
        std::fprintf(stdout, "[photon-backend] dbc not loaded (%s)\n", dbcError.c_str());
    }
    std::string selectError;
    refreshDriveDays();
    if (!applySelectSource(startupSource_, daqHost_, daqPort_, startupLogPath_, selectError)) {
        std::fprintf(stderr, "[photon-backend] failed to activate --source %s: %s\n",
                     std::string(ctrl::sourceName(startupSource_)).c_str(), selectError.c_str());
        return false;
    }
    recoverCrashedLive();
    sources_.setDecodeWanted({});
    std::fprintf(stdout, "[photon-backend] waiting for UI hello\n");
    return true;
}

bool SessionHub::attachClientLog(ClientSession& client, const std::string& path, std::string& error) {
    const std::string resolved = logsess::resolveReplayPath(path);
    const bool replacing = !sources_.hasLog() || sources_.logPath() != resolved;
    if (replacing) {
        // ReplayCursor holds a pointer into LogReplaySource::archive_. Replacing
        // the log without unbinding every view is a use-after-free on the next
        // tickClientReplays (same tick as this select_source).
        for (auto& c : clients_) {
            sources_.resetReplay(c->replay);
        }
    }
    if (!sources_.openLog(path, error)) {
        if (replacing) {
            for (auto& c : clients_) {
                if (c->helloOk && c->logPointer) {
                    bindClientReplay(*c);
                }
            }
        }
        return false;
    }
    client.logPointer = true;
    if (!replacing) {
        bindClientReplay(client);
        refreshDecodeWanted();
        return true;
    }
    for (auto& c : clients_) {
        if (!c->logPointer) {
            continue;
        }
        bindClientReplay(*c);
        if (c.get() != &client && c->helloOk) {
            sendControl(*c, helloAckMessage(*c));
        }
    }
    refreshDecodeWanted();
    return true;
}

void SessionHub::detachClientLog(ClientSession& client) {
    client.logPointer = false;
    sources_.resetReplay(client.replay);
    refreshDecodeWanted();
    if (client.helloOk) {
        if (sources_.kind() != ctrl::SourceKind::None) {
            sendBinary(client, proto::MsgType::TimelineStatus, proto::encodeTimelineStatus(sources_.timeline()));
        }
        pushSnapshot(client);
    }
}

void SessionHub::stop() {
    cancelAllAnalysisJobs();
    for (auto& job : historyJobs_) {
        if (job.cancel) {
            job.cancel->store(true);
        }
    }
    for (auto& job : historyJobs_) {
        if (job.future.valid()) {
            job.future.wait();
        }
    }
    historyJobs_.clear();
    historyStores_.clear();
    stopLiveWriter();
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
    for (auto& job : arraySimJobs_) {
        if (job.future.valid()) {
            job.future.wait();
        }
    }
    arraySimJobs_.clear();
    cancelAllAnalysisJobs();
    sources_.stop();
    videoRelay_.stop();
    listener_.close();
    if (historyPool_) {
        historyPool_->stop();
    }
    for (int i = 0; i < 200 && exportBusy_.load() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
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
        if (hs == ws::HandshakeStatus::HttpExport) {
            std::string target = client.conn.httpTarget();
            net::Socket sock = client.conn.releaseSocket();
            client.failed = true;
            startCsvExport(std::move(sock), std::move(target));
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
    const auto before = clients_.size();
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
                                      for (auto& [id, slot] : client->historyQueries) {
                                          (void)id;
                                          slot.cancelled = true;
                                          if (slot.cancel) {
                                              slot.cancel->store(true);
                                          }
                                      }
                                      if (historyRemote_.enabled()) {
                                          historyRemote_.cancelHistory(client->id, 0);
                                      }
                                      std::fprintf(stdout, "[photon-backend] client dropped %s\n", client->label.c_str());
                                      return true;
                                  }),
                   clients_.end());
    if (clients_.size() != before) {
        refreshDecodeWanted();
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
        flushHistory(*clients_[i], nowTick);
        if (clients_[i]->conn.hasOutbound()) {
            const auto before = clients_[i]->conn.outboundBytes();
            clients_[i]->conn.flushOutbound();
            noteDrained(*clients_[i], before);
        }
    }

    sources_.onPoll(fds);
    videoRelay_.onPoll(fds);
    sources_.tick(nowTick);
    tickClientReplays(nowTick);
    if (sources_.hasLog()) {
        for (auto& client : clients_) {
            if (client->helloOk && client->logPointer && sources_.consumeReplaySnapshotDue(client->replay)) {
                pushSnapshot(*client);
            }
        }
    }
    if (sources_.consumeSnapshotDue()) {
        for (auto& client : clients_) {
            if (client->helloOk && !client->logPointer) {
                pushSnapshot(*client);
            }
        }
    }
    videoRelay_.tick(nowTick);
    streamTelemetry(nowTick);
    streamCan(nowTick);
    streamTimeline(nowTick);
    streamVideo();
    streamVideoStatus(nowTick, false);
    checkTxWatchdog(nowTick);
    pollTrackSimJobs();
    pollArraySimJobs();
    pollAnalysisJobs();
    pollHistoryJobs();
    historyRemote_.tick(nowTick);
    flushHistoryRemote();
    tickRetention(nowTick);
    dropClosed();

    for (auto& client : clients_) {
        if (client->conn.hasOutbound()) {
            const auto before = client->conn.outboundBytes();
            client->conn.flushOutbound();
            noteDrained(*client, before);
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

void SessionHub::noteQueued(ClientSession& client, bool history, std::size_t beforeBytes) {
    const auto now = client.conn.outboundBytes();
    if (now > beforeBytes) {
        client.outboundTags.push_back(OutboundTag{history, now - beforeBytes});
    }
}

void SessionHub::noteDrained(ClientSession& client, std::size_t beforeBytes) {
    const auto now = client.conn.outboundBytes();
    if (beforeBytes <= now) {
        return;
    }
    std::size_t drained = beforeBytes - now;
    while (drained > 0 && !client.outboundTags.empty()) {
        auto& tag = client.outboundTags.front();
        if (tag.bytes <= drained) {
            drained -= tag.bytes;
            client.outboundTags.pop_front();
        } else {
            tag.bytes -= drained;
            drained = 0;
        }
    }
}

}  // namespace photon
