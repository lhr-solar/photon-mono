#pragma once

#include "photon/control.hpp"
#include "photon/control_v2.hpp"
#include "photon/protocol.hpp"
#include "photon_ui/alarms.hpp"
#include "photon_ui/catalog.hpp"
#include "photon_ui/live_v2.hpp"
#include "photon_ui/signal_map.hpp"
#include "photon_ui/tile_cache.hpp"
#include "photon_ui/video_v2.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace photon::ui {

struct SignalRing {
    std::uint32_t id = 0;
    std::string name;
    std::vector<double> t;
    std::vector<double> v;
    std::size_t cap = kRingCap;
    std::size_t write = 0;
    std::size_t count = 0;
    double latest = 0.0;
    double latestTs = 0.0;
    bool hasLatest = false;

    void push(double ts, double value);
    void resetSeries();
    std::size_t plotOffset() const;
    std::size_t index(std::size_t logical) const;
    double firstTs() const;
    double lastTs() const;
    bool interpolate(double tMs, double& value) const;
};

struct CellPack {
    std::vector<float> voltage;
    std::vector<float> temp;
    std::vector<double> tapTs;
    bool synthetic = false;
};

struct LogLine {
    std::uint64_t tMs = 0;
    bool error = false;
    std::string text;
};

struct PlotSignal {
    std::uint32_t id = 0;
    std::string label;
    std::string unit;
};

// A span of live data the server skipped (protocol v2 Gap), in Unix ns. Step 2
// fills these from tiles.
struct GapSpan {
    std::int64_t fromNs = 0;
    std::int64_t toNs = 0;
};

// What the server said in hello_ack, plus the clock offset for the live cursor.
struct ServerInfo {
    bool helloOk = false;
    std::string server;
    std::string user;
    std::string liveRun;
    std::vector<std::int64_t> levels;
    std::int64_t clockOffsetNs = 0;  // server_time_ns minus this machine's clock at hello_ack
};

// Video over protocol v2 (the /v2 socket). The server says what it streams (mode,
// gap); the client says what it asked for (requested) and when a screen last showed
// video (wantedMs).
struct VideoState {
    bool available = false;                    // hello_ack features contain "video"
    VideoStream requested = VideoStream::Off;  // the stream the client asked for last
    std::string mode = "off";                  // the last video_status mode
    std::uint64_t tMs = 0;                     // the last video_status t_ms
    std::string clock;                         // its clock provenance
    bool haveStatus = false;
    std::optional<ctrl2::VideoGap> gap;        // set while the stream has no video to show
    bool replay = false;                       // the last frame was replayed
    double wantedMs = -1.0e12;                 // the last time a screen showed video
    std::uint64_t frames = 0;                  // frames since the stream (re)started
    std::uint32_t dropped = 0;                 // partial frames the assembler gave up
    double fps = 0.0;                          // pictures per second, from the last full second
    std::uint64_t fpsCount = 0;
    double fpsStartMs = 0.0;
    std::optional<std::uint32_t> qid;          // the qid of the last status, after a seek
};

// Result of applyCatalog. remap maps each old client id to the new client id of
// the same "Message.Signal" name, for the ids that still exist.
struct CatalogChange {
    bool changed = false;
    std::unordered_map<std::uint32_t, std::uint32_t> remap;
    std::uint32_t serverCount = 0;  // signals in the server's catalog
    std::uint32_t duplicates = 0;   // signals left out for a repeated name
};

class ClientArena {
public:
    ClientArena();

    void clearTelemetry();
    void ingestTelemetry(std::span<const proto::TelemetryBlock> blocks);
    void ingestGap(std::int64_t fromNs, std::int64_t toNs);
    // Sets the live cursor to the server's now, in Unix ms. min and max come
    // from the rings the client has.
    void setLiveCursor(std::uint64_t serverNowMs);
    // Replaces the catalog. A new sha clears the rings and re-resolves the
    // signal map and the alarms by name.
    CatalogChange applyCatalog(const ctrl2::Catalog& catalog);
    // A past run is pinned for replay: its tiles, a local cursor, play and pause.
    bool catalogPinned() const { return pinned_; }
    const std::string& pinnedRunId() const { return pinnedRun_; }
    void pinRun(const ctrl2::Run& run);
    void unpinRun();
    // A local playhead for the pinned run (scrub and play/pause). Clamped to the run.
    void applyPinnedTimeline(proto::TimelineMode mode, std::uint64_t cursorMs);
    // Advances the pinned playhead by the wall time since the last call.
    void tickPinnedPlayback(double nowMs);
    // Points the arena's TileCache at the run it should show: the pinned run, or
    // the live run. A change drops the cache's tiles (cancelling them through io).
    void syncRun(HistoryIO* io);
    float historyRateHz(std::uint32_t signalId) const;
    void updatePlaceholderCells(double nowMs);
    void holdReplayCells();
    void pushLog(std::string text, bool error = false);
    void resetSignalMap();
    void refreshMappedCells();
    void maybePrimeHistory(HistoryIO& io);
    void setMappedCells(std::vector<float> voltages, std::vector<float> temps,
                        std::vector<double> tapTs = {});
    std::vector<PlotSignal> plotCatalog() const;
    SignalMap& signalMap() { return signalMap_; }
    const SignalMap& signalMap() const { return signalMap_; }
    AlarmEngine& alarms() { return alarms_; }
    const AlarmEngine& alarms() const { return alarms_; }

    SignalRing* ring(std::uint32_t id);
    const SignalRing* ring(std::uint32_t id) const;
    double latest(std::uint32_t id, double fallback = 0.0) const;
    double readout(std::uint32_t id, double fallback = 0.0) const;
    bool interpolate(std::uint32_t id, double tMs, double& value) const;
    bool has(std::uint32_t id) const;
    bool consumeIndexReady();
    bool consumeSessionFollow();
    bool consumeMapsReset();
    TileCache& tiles() { return tiles_; }
    const TileCache& tiles() const { return tiles_; }
    bool cursorValue(std::uint32_t id, double& value) const;

    const std::unordered_map<std::uint32_t, SignalRing>& rings() const { return rings_; }
    const std::vector<GapSpan>& gaps() const { return gaps_; }
    const proto::TimelineStatus& timeline() const { return timeline_; }
    void setPreviewCursor(std::uint64_t cursorMs, bool on) {
        previewCursor_ = on;
        previewCursorMs_ = cursorMs;
    }
    bool hasPreviewCursor() const { return previewCursor_ && previewCursorMs_ != 0; }
    std::uint64_t previewCursorMs() const { return previewCursorMs_; }
    void setVideoFollowMs(std::uint64_t cursorMs) { videoFollowMs_ = cursorMs; }
    void clearVideoFollow() { videoFollowMs_ = 0; }
    std::uint64_t videoFollowMs() const { return videoFollowMs_; }
    bool timelineIsLive() const {
        return timeline_.mode == static_cast<std::uint8_t>(proto::TimelineMode::Live);
    }
    std::uint64_t displayCursorMs() const {
        // Plot scrub sets a local preview; live gauges must not follow it while
        // the cursor is live.
        if (hasPreviewCursor() && !timelineIsLive()) {
            return previewCursorMs_;
        }
        return timeline_.cursorMs;
    }
    // Video follows the operator pointer: rail drag, or the live cursor.
    std::uint64_t videoCursorMs() const {
        if (hasPreviewCursor()) {
            return previewCursorMs_;
        }
        if (videoFollowMs_ != 0) {
            return videoFollowMs_;
        }
        return displayCursorMs();
    }
    // Readout / history prime when the timeline cursor has not arrived yet.
    std::uint64_t effectiveCursorMs() const {
        const auto live = displayCursorMs();
        if (live != 0) {
            return live;
        }
        if (timeline_.maxTimeMs != 0) {
            return timeline_.maxTimeMs;
        }
        return tiles_.runMaxMs();
    }
    const CellPack& cells() const { return cells_; }
    // Video (protocol v2). The client pushes restarts and frames in order; the decoder
    // takes them with takeVideoEvents on the UI thread.
    void pushVideoRestart();
    void pushVideoFrame(const AssembledFrame& frame);
    std::deque<VideoEvent> takeVideoEvents();
    void applyVideoStatus(const ctrl2::VideoStatus& status);
    // A screen is showing video now. The client reads it with the clock of its tick.
    void markVideoWanted() { video.wantedMs = tickMs_; }
    void noteTick(double nowMs) { tickMs_ = nowMs; }
    bool videoOn() const { return video.available; }
    const std::vector<LogLine>& logs() const { return logs_; }

    std::string wsUrl = std::string(defaultWsUrl());
    ServerInfo server;
    std::vector<std::string> features;
    std::vector<ctrl2::Run> runs;  // runs_list reply, newest first
    std::vector<ctrl::CatalogSignal> signalCatalog;  // id = catalog index + 1
    std::vector<SignalExtra> signalExtra;            // parallel to signalCatalog
    std::vector<std::string> catalogBuses;
    std::string catalogSha;
    bool haveSignalCatalog = false;
    VideoState video;

private:
    SignalMap signalMap_;
    AlarmEngine alarms_;
    std::unordered_map<std::uint32_t, SignalRing> rings_;
    proto::TimelineStatus timeline_{};
    std::uint64_t previewCursorMs_ = 0;
    bool previewCursor_ = false;
    std::uint64_t videoFollowMs_ = 0;
    std::deque<VideoEvent> videoEvents_;
    std::size_t videoBytes_ = 0;
    double tickMs_ = 0.0;
    CellPack cells_{};
    std::vector<LogLine> logs_;
    std::vector<GapSpan> gaps_;
    bool indexReadyPulse_ = false;
    bool sessionFollowPulse_ = false;
    bool mapsResetPulse_ = false;
    TileCache tiles_{};
    bool pinned_ = false;
    std::string pinnedRun_;
    double pinnedPlayWallMs_ = 0.0;

    void syncRingSeams();
    void addDefaultRings();
};

}  // namespace photon::ui
