#pragma once

#include "photon/control.hpp"
#include "photon/dbc.hpp"
#include "photon/net.hpp"
#include "photon/protocol.hpp"
#include "photon/source.hpp"
#include "photon/track_catalog.hpp"
#include "photon/video_relay.hpp"
#include "photon/websocket.hpp"

#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace photon {

struct ClientSession {
    ws::Connection conn;
    std::uint64_t id = 0;
    bool helloOk = false;
    bool failed = false;
    bool txArmed = false;
    bool videoSubscribed = false;
    std::uint64_t lastHeartbeatMs = 0;
    std::unordered_set<std::uint32_t> subscribed;
    std::string label;

    explicit ClientSession(net::Socket socket) : conn(std::move(socket)) {}
};

class SessionHub {
public:
    SessionHub(std::uint16_t wsPort, std::string daqHost, std::uint16_t daqPort, std::string assetsDir = {},
               std::string videoHost = {}, std::uint16_t videoPort = 6700,
               ctrl::SourceKind startupSource = ctrl::SourceKind::Synthetic, std::string startupLogPath = {});

    bool start();
    void tick(std::uint64_t nowMs);
    void stop();

    std::size_t clientCount() const { return clients_.size(); }

private:
    std::uint16_t wsPort_ = 8765;
    std::string daqHost_ = "127.0.0.1";
    std::uint16_t daqPort_ = 6500;
    net::Socket listener_;
    std::vector<std::unique_ptr<ClientSession>> clients_;
    SourceHub sources_;
    VideoRelay videoRelay_;
    TrackCatalog catalog_;
    std::string assetsDir_;
    dbc::Database dbc_;
    bool dbcLoaded_ = false;
    std::uint64_t nextClientId_ = 1;
    struct TrackSimJob {
        std::uint64_t clientId = 0;
        std::future<ctrl::Message> future;
    };
    std::vector<TrackSimJob> trackSimJobs_;
    std::uint32_t binarySeq_ = 1;
    std::uint64_t lastTelemetryMs_ = 0;
    std::uint64_t lastCanBurstMs_ = 0;
    std::uint64_t lastTimelineMs_ = 0;
    std::uint64_t lastTxCheckMs_ = 0;
    std::uint64_t lastVideoStatusMs_ = 0;

    void acceptNew();
    void serviceClient(ClientSession& client, std::uint64_t nowMs, short revents);
    void handleText(ClientSession& client, std::string_view text, std::uint64_t nowMs);
    void sendControl(ClientSession& client, const ctrl::Message& msg);
    void sendBinary(ClientSession& client, proto::MsgType type, std::span<const std::uint8_t> payload);
    void disarm(ClientSession& client, std::string reason);
    void dropClosed();
    void streamTelemetry(std::uint64_t nowMs);
    void streamCan(std::uint64_t nowMs);
    void streamTimeline(std::uint64_t nowMs);
    void streamVideo();
    void streamVideoStatus(std::uint64_t nowMs, bool force);
    void checkTxWatchdog(std::uint64_t nowMs);
    void enqueueTrackSim(ClientSession& client, const ctrl::Message& req);
    void pollTrackSimJobs();
    ClientSession* findClient(std::uint64_t id);
    bool loadDbcFile(const std::string& path, std::string& error);
    bool loadBundledDbc(const std::string& id, std::string& error);
    void sendCatalog(ClientSession& client);
    void broadcastCatalog();
    ctrl::Message catalogMessage() const;
    bool applySelectSource(ctrl::SourceKind kind, const std::string& host, std::uint16_t port,
                           const std::string& path, std::string& error, const pcan::Config& pcanCfg = {});
    ctrl::SourceKind startupSource_ = ctrl::SourceKind::Synthetic;
    std::string startupLogPath_;
};

}  // namespace photon
