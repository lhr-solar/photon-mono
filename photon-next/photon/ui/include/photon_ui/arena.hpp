#pragma once

#include "photon/control.hpp"
#include "photon/control_v2.hpp"
#include "photon/protocol.hpp"
#include "photon_ui/alarms.hpp"
#include "photon_ui/catalog.hpp"
#include "photon_ui/live_v2.hpp"
#include "photon_ui/signal_map.hpp"
#include "photon_ui/tile_cache.hpp"

#include <cstddef>
#include <cstdint>
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

// Video is off in this step (kVideoEnabled); nothing fills this in until step 2.
struct VideoState {
    bool connected = false;
    std::string host;
    std::uint16_t port = 0;
    std::string mode = "live";
    std::uint64_t lastTsMs = 0;
    std::uint64_t lastFrameArrivalMs = 0;
    std::uint64_t frames = 0;
    std::uint64_t dropped = 0;
    bool haveStatus = false;
    bool subscribed = false;
    std::string lastError;
    std::int64_t clockOffsetMs = 0;
    bool haveClockOffset = false;
    bool clockOffsetDirty = false;
    bool clockOffsetUser = false;
    double fps = 0.0;
    // Recent picture intervals, in ms. The rate is their median, so a GOP
    // that arrives in one burst still reads as the camera interval.
    static constexpr int kFpsDeltas = 15;
    std::uint32_t fpsDeltaMs[kFpsDeltas]{};
    int fpsDeltaN = 0;
    int fpsDeltaI = 0;
    bool fpsHaveLast = false;
    std::uint64_t fpsLastPictureMs = 0;
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
    // Runs replay returns in step 2; until then no run is pinned.
    bool catalogPinned() const { return false; }
    float historyRateHz(std::uint32_t signalId) const;
    void updatePlaceholderCells(double nowMs);
    void holdReplayCells();
    void pushLog(std::string text, bool error = false);
    void resetSignalMap();
    void refreshMappedCells();
    void maybePrimeHistory(HistoryIO& io);
    void invalidateHistoryPrime() { primedHistoryKey_.clear(); }
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
        return tiles_.sessionMaxMs();
    }
    const CellPack& cells() const { return cells_; }
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
    CellPack cells_{};
    std::vector<LogLine> logs_;
    std::vector<GapSpan> gaps_;
    bool indexReadyPulse_ = false;
    bool sessionFollowPulse_ = false;
    bool mapsResetPulse_ = false;
    TileCache tiles_{};
    std::string primedHistoryKey_;

    void syncRingSeams();
    void addDefaultRings();
};

}  // namespace photon::ui
