#pragma once

#include "photon/control.hpp"
#include "photon/dbc.hpp"
#include "photon/history_remote.hpp"
#include "photon/net.hpp"
#include "photon/protocol.hpp"
#include "photon/source.hpp"
#include "photon/track_catalog.hpp"
#include "photon/tsdb.hpp"
#include "photon/video_index.hpp"
#include "photon/video_relay.hpp"
#include "photon/websocket.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace photon {

struct HistoryEncodeState {
    tsdb::QueryResult result;
    std::shared_ptr<tsdb::Store> store;
    std::size_t seriesIdx = 0;
    std::size_t off = 0;
    std::uint32_t chunkSeq = 0;
    bool more = false;
};

struct HistoryWork {
    std::uint32_t queryId = 0;
    ctrl::Message result;
    std::vector<std::vector<std::uint8_t>> chunks;
    HistoryEncodeState encode;
};

struct OutboundTag {
    bool history = false;
    std::size_t bytes = 0;
};

struct HistoryQuerySlot {
    enum class Phase : std::uint8_t { Running, Ready };
    Phase phase = Phase::Running;
    bool cancelled = false;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::vector<std::vector<std::uint8_t>> chunks;
    HistoryEncodeState encode;
};

struct ClientSession {
    ws::Connection conn;
    std::uint64_t id = 0;
    bool helloOk = false;
    bool failed = false;
    bool txArmed = false;
    bool videoSubscribed = false;
    bool canMonitor = false;
    std::uint64_t lastHeartbeatMs = 0;
    std::unordered_set<std::uint32_t> subscribed;
    std::string label;
    std::uint64_t historyCreditBytes = 4 * 1024 * 1024;
    std::uint64_t historyUnackedBytes = 0;
    std::uint64_t lastHistoryCreditMs = 0;
    std::unordered_map<std::uint32_t, HistoryQuerySlot> historyQueries;
    std::deque<std::vector<std::uint8_t>> remoteHistoryBin;
    std::deque<OutboundTag> outboundTags;
    ReplayView replay;
    bool logPointer = false;
    // §9.12: compressed GOP bytes for this WS client, loaded from VPT pointers.
    vidx::GopRing videoCache;
    // The live t=0 poll re-locates the same GOP while it grows. These track
    // what has already been read from the store and pushed to the socket, so a
    // poll ships the new tail instead of the whole GOP again.
    std::uint64_t videoSentSeq = 0;
    std::uint32_t videoGopSegment = 0;
    std::uint64_t videoGopOffset = 0;
    std::uint32_t videoGopSent = 0;
    bool videoGopLive = false;

    explicit ClientSession(net::Socket socket) : conn(std::move(socket)) {}
};

class SessionHub {
public:
    SessionHub(std::uint16_t wsPort, std::string daqHost, std::uint16_t daqPort, std::string assetsDir = {},
               std::string videoHost = {}, std::uint16_t videoPort = 6700,
               ctrl::SourceKind startupSource = ctrl::SourceKind::DaqTcp, std::string startupLogPath = {},
               std::string tsdbDir = {}, std::uint16_t videoUdpPort = 0);

    ~SessionHub();

    bool start();
    void tick(std::uint64_t nowMs);
    void stop();
    void setWriteLiveTail(bool on) { writeLiveTail_ = on; }
    void setClientPassword(std::string password);
    void setVideoUdpHost(std::string host) { videoRelay_.setUdpHost(std::move(host)); }
    void setAnalysisCompute(std::string addr);
    void setHistoryRemote(std::string host, std::uint16_t port, std::string path, std::string password);

    std::size_t clientCount() const { return clients_.size(); }
    std::uint16_t listenPort() const { return listener_.localPort(); }

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
    std::vector<TrackSimJob> arraySimJobs_;
    struct AnalysisJob {
        std::uint64_t clientId = 0;
        std::string jobId;
        std::string sidecarClientId;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::future<ctrl::Message> future;
    };
    std::vector<AnalysisJob> analysisJobs_;
    std::string analysisComputeAddr_;
    struct HistoryJob {
        std::uint64_t clientId = 0;
        std::uint32_t queryId = 0;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::shared_ptr<tsdb::Store> store;
        std::future<HistoryWork> future;
    };
    std::uint32_t binarySeq_ = 1;
    std::uint64_t lastTelemetryMs_ = 0;
    std::uint64_t lastCanBurstMs_ = 0;
    std::uint64_t lastTimelineMs_ = 0;
    std::uint64_t lastTxCheckMs_ = 0;
    std::uint64_t lastVideoStatusMs_ = 0;
    std::uint64_t lastLiveCatalogMs_ = 0;
    std::uint64_t lastRetentionMs_ = 0;
    std::unique_ptr<tsdb::LiveTailWriter> liveWriter_;
    std::string liveSessionId_;
    std::uint64_t liveLastMs_ = 0;
    bool writeLiveTail_ = true;

    void acceptNew();
    void serviceClient(ClientSession& client, std::uint64_t nowMs, short revents);
    void handleText(ClientSession& client, std::string_view text, std::uint64_t nowMs);
    void sendControl(ClientSession& client, const ctrl::Message& msg);
    void sendBinary(ClientSession& client, proto::MsgType type, std::span<const std::uint8_t> payload);
    void sendTelemetryBlocks(ClientSession& client, const std::vector<proto::TelemetryBlock>& blocks,
                             bool ignoreCongest);
    void disarm(ClientSession& client, std::string reason);
    void dropClosed();
    void streamTelemetry(std::uint64_t nowMs);
    void streamCan(std::uint64_t nowMs);
    void refreshDecodeWanted();
    void streamTimeline(std::uint64_t nowMs);
    void streamVideo();
    void streamVideoStatus(std::uint64_t nowMs, bool force);
    void checkTxWatchdog(std::uint64_t nowMs);
    void enqueueTrackSim(ClientSession& client, const ctrl::Message& req);
    void pollTrackSimJobs();
    void enqueueArraySim(ClientSession& client, const ctrl::Message& req);
    void pollArraySimJobs();
    void handleAnalysisCompute(ClientSession& client, const ctrl::Message& req);
    void handleAnalysisCancel(ClientSession& client, const ctrl::Message& req);
    void pollAnalysisJobs();
    void cancelAllAnalysisJobs();
    ClientSession* findClient(std::uint64_t id);
    bool loadDbcFile(const std::string& path, std::string& error);
    bool loadBundledDbc(const std::string& id, std::string& error);
    void sendCatalog(ClientSession& client);
    void handleHistoryQuery(ClientSession& client, const ctrl::Message& msg);
    void flushHistory(ClientSession& client, std::uint64_t nowMs = 0);
    void feedLive(std::span<const proto::CanMonitorItem> frames);
    void startLiveWriter(std::uint64_t firstMs);
    void stopLiveWriter();
    void recoverCrashedLive();
    void publishLiveCatalog();
    void tickRetention(std::uint64_t nowMs);
    bool liveSource() const;
    void pollHistoryJobs();
    void cancelHistory(ClientSession& client, std::uint32_t queryId);
    void flushHistoryRemote();
    void flushRemoteHistoryBin(ClientSession& client);
    void noteQueued(ClientSession& client, bool history, std::size_t beforeBytes);
    void noteDrained(ClientSession& client, std::size_t beforeBytes);
    std::shared_ptr<tsdb::Store> openHistoryStore(const std::string& sessionId, std::uint64_t atMs,
                                                  std::string* error);
    void broadcastCatalog();
    void pushSnapshot(ClientSession& client);
    void broadcastSnapshots();
    void bindClientReplay(ClientSession& client);
    void resetClientReplays();
    void tickClientReplays(std::uint64_t nowMs);
    ctrl::Message catalogMessage() const;
    ctrl::Message historyCatalogMessage() const;
    bool applySelectSource(ctrl::SourceKind kind, const std::string& host, std::uint16_t port,
                           const std::string& path, std::string& error, const pcan::Config& pcanCfg = {});
    void refreshDriveDays();
    ctrl::Message helloAckMessage() const;
    ctrl::Message helloAckMessage(const ClientSession& client) const;
    bool attachClientLog(ClientSession& client, const std::string& path, std::string& error);
    void detachClientLog(ClientSession& client);
    ctrl::SourceKind startupSource_ = ctrl::SourceKind::Synthetic;
    std::string startupLogPath_;
    std::string tsdbDir_;
    std::vector<ctrl::Message::DriveDay> driveDays_;
    // Stores outlive jobs: declared first so they are destroyed last.
    std::unordered_map<std::string, std::shared_ptr<tsdb::Store>> historyStores_;
    std::vector<HistoryJob> historyJobs_;
    class HistoryPool;
    std::unique_ptr<HistoryPool> historyPool_;
    HistoryRemote historyRemote_;
    std::atomic<int> exportBusy_{0};

    void startCsvExport(net::Socket sock, std::string target);
};

}  // namespace photon
