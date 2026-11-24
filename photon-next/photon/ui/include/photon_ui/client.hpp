#pragma once

#include "photon/control_v2.hpp"
#include "photon/protocol_v2.hpp"
#include "photon_ui/arena.hpp"
#include "photon_ui/history_io.hpp"
#include "photon_ui/tile_queue.hpp"
#include "photon_ui/video_v2.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef __EMSCRIPTEN__
#include "photon/websocket.hpp"
#endif

namespace photon::ui {

class AnalysisRuntime;

// Protocol v2 client: one WebSocket to the server's /v2. The server decodes
// everything; this client only subscribes to catalog signal indexes (client id
// minus one) and reads decoded values.
class Client : public HistoryIO {
public:
    enum class State { Disconnected, Connecting, Connected, HelloOk };

    explicit Client(ClientArena& arena);
    ~Client();

    void tick(double nowMs);
    void connect();
    void disconnect(const char* why);
    void setUrl(std::string url);
    // The LAN password, sent as hello.token. Empty when there is none.
    void setToken(std::string token);
    const std::string& token() const { return token_; }
    // True after the server refused the hello for its token. Reconnects stop
    // until setToken is called.
    bool loginRefused() const { return loginRefused_; }

    bool send(const ctrl2::Message& msg);

    // Live interest. Ids are client ids (catalog index + 1); 0 is ignored. The
    // subscribe message carries the whole set, sent at most once per tick. The
    // set is trimmed to the ids named between beginInterest and commitInterest
    // when that bracket is used.
    void beginInterest();
    void ensureSubscribed(std::span<const std::uint32_t> ids);
    void setSubscribed(std::uint32_t id, bool on);
    void commitInterest();
    const std::unordered_set<std::uint32_t>& subscribed() const { return subscribed_; }

    void requestRuns();
    // Tile requests (HistoryIO). They go out through TileRequestQueue: at most 24 in
    // flight, visible tiles first. Nothing is sent before hello_ack.
    void requestTile(std::uint32_t queryId, const std::string& runId, std::uint32_t signalId, int level,
                     std::int64_t index, bool urgent) override;
    void cancelTile(std::uint32_t queryId) override;
    bool queueFull() const override { return queue_.full(); }

    // Analysis-owned caches only.
    void bindAnalysisRuntime(AnalysisRuntime* runtime);

    // Ends the requests of every cache when the socket is gone.
    void dropTiles();

    State state() const { return state_; }
    const char* stateLabel() const;
    bool helloOk() const { return state_ == State::HelloOk; }
    const std::string& url() const { return url_; }

    void handleOpen();
    // closeCode and reason are the WebSocket close status and text, when the socket gave them.
    void handleClose(const char* why, bool error, std::uint16_t closeCode = 0, std::string_view reason = {});
    void handleText(std::string_view text);
    void handleBinary(std::span<const std::uint8_t> bytes);

private:
    ClientArena& arena_;
    AnalysisRuntime* analysisRuntime_ = nullptr;
    std::string url_;
    std::string token_;
    bool loginRefused_ = false;
    bool askToken_ = false;  // the web page should ask for the password (from tick)
#ifdef __EMSCRIPTEN__
    int socket_ = 0;
#else
    std::unique_ptr<ws::Connection> ws_;
    bool wsHandshakeOk_ = false;
#endif
    State state_ = State::Disconnected;
    int backoffIndex_ = 0;
    double nextReconnectMs_ = 0.0;
    double lastPingMs_ = 0.0;
    double lastGapLogMs_ = 0.0;
    // Wanted live signals (client ids), and the set the server was last sent.
    std::unordered_set<std::uint32_t> subscribed_;
    std::unordered_set<std::uint32_t> interest_;
    std::vector<std::uint32_t> sentSubscribed_;
    bool interestOpen_ = false;
    bool subscribeDirty_ = true;
    bool catalogSeen_ = false;  // a catalog arrived on this connection
    std::uint32_t serverSignalCount_ = 0;  // signals in the server's catalog (indexes 0..n-1)
    double nowMs_ = 0.0;                   // the time of the last tick
    double lastRunsMs_ = 0.0;
    double lastTileErrorLogMs_ = 0.0;
    TileRequestQueue queue_;
    std::unordered_map<std::uint32_t, double> tileSentMs_;  // sent tile requests, by qid
    VideoSession videoSession_;   // the video stream asked for, and what is awaited
    VideoAssembler videoAssembler_;

    void attachCallbacks();
    void scheduleReconnect(double nowMs);
    void applyHelloAck(const ctrl2::HelloAck& ack);
    void applyCatalog(const ctrl2::Catalog& catalog);
    void applyLiveSamples(std::span<const std::uint8_t> bytes);
    void flushSubscribe();
    void sendPing(double nowMs);
    void pumpTiles();
    void routeTilePiece(const proto2::TilePiece& piece);
    void handleTileError(std::uint32_t qid, const std::string& message);
    void finishTile(std::uint32_t qid);
    TileCache* cacheFor(std::uint32_t qid);
    void applyRuns(std::vector<ctrl2::Run> runs);
    void pumpVideo(double nowMs);
    void handleVideoChunk(std::span<const std::uint8_t> bytes);
    void applyVideoStatus(const ctrl2::VideoStatus& status);
    void resetVideo();
};

}  // namespace photon::ui
