#pragma once

#include "wire/control.hpp"
#include "ui/arena.hpp"
#include "ui/history_io.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifndef __EMSCRIPTEN__
#include "net/websocket.hpp"
#include <future>
#endif

namespace photon::ui {

class AnalysisRuntime;

class Client : public HistoryIO {
public:
    enum class State { Disconnected, Connecting, Connected, HelloOk };

    explicit Client(ClientArena& arena);
    ~Client();

    void tick(double nowMs);
    void connect();
    void disconnect(const char* why);
    void setUrl(std::string url);

    bool send(const ctrl::Message& msg);
    void selectSource(ctrl::SourceKind kind, std::string host, std::uint16_t port, std::string path,
                      int channel = 1, int baud = 250000, bool listenOnly = true);
    void selectDbc(std::string idOrPath, bool bundledId);
    void uploadDbc(std::string path = {});
    void pollDbcUpload();
    bool sendExtension(std::string text);
    const std::string& uploadedDbc() const { return uploadedDbc_; }
    const std::string& dbcUploadStatus() const { return dbcUploadStatus_; }
    void applyUploadedDbc();
    void redecodeWithUploadedDbc();
    void selectDecoding(std::string session, std::string decode);
    const std::string& selectedDecode() const { return selectedDecode_; }
    void setSubscribed(std::uint32_t id, bool on);
    void ensureSubscribed(std::span<const std::uint32_t> ids);
    void beginInterest();
    void markCanWanted();
    void markVideoWanted();
    void commitInterest();
    void sendTimeline(ctrl::TimelineMode mode, std::uint64_t cursorMs);
    void playLog(std::string path, std::uint64_t cursorMs = 0);
    void playCatalogSession(const ctrl::Message::CatalogSession& row, std::uint64_t cursorMs = 0);
    void playDate(std::string ymd);
    void goLive();
    void subscribeVideo(ctrl::VideoMode mode, std::uint64_t startMs = 0);
    void stopVideo();
    void flushVideoLocate();
    void setVideoClock(std::int64_t offsetMs);
    void requestHistory(std::uint32_t queryId, std::string sessionId, std::uint64_t fromMs, std::uint64_t toMs,
                        std::vector<std::uint32_t> signalIds, std::uint32_t maxPoints) override;
    void cancelHistory(std::uint32_t queryId) override;

    // Analysis-owned caches only. Never setSession / selectSource on arena.
    void bindAnalysisRuntime(AnalysisRuntime* runtime);
    void requestCatalogList();
    const std::vector<ctrl::Message::CatalogSession>& historyCatalog() const { return historyCatalog_; }
    bool haveHistoryCatalog() const { return haveHistoryCatalog_; }

    State state() const { return state_; }
    const char* stateLabel() const;
    bool helloOk() const { return state_ == State::HelloOk; }
    const std::unordered_set<std::uint32_t>& subscribed() const { return subscribed_; }
    const std::string& url() const { return url_; }

    void handleOpen();
    void handleClose(const char* why, bool error);
    void handleText(std::string_view text);
    void handleBinary(std::span<const std::uint8_t> bytes);

private:
    ClientArena& arena_;
    AnalysisRuntime* analysisRuntime_ = nullptr;
    std::vector<ctrl::Message::CatalogSession> historyCatalog_;
    bool haveHistoryCatalog_ = false;
    std::string url_;
#ifdef __EMSCRIPTEN__
    int socket_ = 0;
#else
    std::unique_ptr<ws::Connection> ws_;
    bool wsHandshakeOk_ = false;
#endif
    State state_ = State::Disconnected;
    int backoffIndex_ = 0;
    double nextReconnectMs_ = 0.0;
    double lastHeartbeatMs_ = 0.0;
    std::unordered_set<std::uint32_t> subscribed_;
    std::unordered_set<std::uint32_t> interest_;
    bool interestOpen_ = false;
    bool canWanted_ = false;
    bool canInterest_ = false;
    bool videoInterest_ = false;
    ctrl::SourceKind pendingSource_ = ctrl::SourceKind::None;
    bool userPickedSource_ = false;
    std::string pendingHost_ = "127.0.0.1";
    std::uint16_t pendingPort_ = 6500;
    std::string lastDaqHost_ = "127.0.0.1";
    std::uint16_t lastDaqPort_ = 6500;
    std::string pendingPath_;
    int pendingChannel_ = 1;
    int pendingBaud_ = 250000;
    bool pendingListenOnly_ = true;
    bool userPickedDbc_ = false;
    bool pendingDbcBundled_ = true;
    std::string pendingDbc_;
    std::string uploadedDbc_, dbcUploadStatus_;
    std::string selectedDecode_;
#ifndef __EMSCRIPTEN__
    std::future<std::string> dbcUpload_;
#endif
    bool videoWanted_ = false;
    ctrl::VideoMode videoMode_ = ctrl::VideoMode::Live;
    std::uint64_t videoStartMs_ = 0;
    bool locateSent_ = false;
    std::uint32_t nextVideoRequest_ = 1;
    std::uint32_t activeVideoRequest_ = 0;
    bool awaitingVideoBytes_ = false;
    bool awaitingVideoBatch_ = false;
    double lastLiveLocateMs_ = 0.0;
    std::uint64_t lastReplayLocateAtMs_ = 0;
    std::uint64_t lastReplayLocateNewestMs_ = 0;
    double locateBackoffMs_ = 0.0;
    // The live t=0 poll runs 2.5x a second. One log line per poll buried
    // everything else in the drawer, so live locates log on this interval.
    double lastLocateLogMs_ = 0.0;

    void attachCallbacks();
    void scheduleReconnect(double nowMs);
    void resubscribe();
    void sendSubscriptions();
    void sendCanMonitor(bool on);
    void adoptActiveSource(const ctrl::Message::ActiveSource& active);
    void applyHelloAck(const ctrl::Message& msg);
    void maybeAdoptDriveDay();
    const ctrl::Message::CatalogSession* bestCatalogForYmd(const std::string& ymd) const;
    const ctrl::Message::CatalogSession* bestCatalogOverlapping(std::uint64_t minMs,
                                                                std::uint64_t maxMs) const;
    void openExtractLog(std::string path, std::uint64_t cursorMs);
};

}  // namespace photon::ui
