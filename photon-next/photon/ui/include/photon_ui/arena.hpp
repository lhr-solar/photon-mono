#pragma once

#include "photon/control.hpp"
#include "photon/protocol.hpp"
#include "photon_ui/alarms.hpp"
#include "photon_ui/catalog.hpp"
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

struct CanRow {
    std::uint32_t canId = 0;
    std::uint8_t dlc = 0;
    std::uint8_t flags = 0;
    std::uint8_t data[8]{};
    std::uint64_t lastTsMs = 0;
    std::uint64_t count = 0;
    std::uint64_t windowCount = 0;
    std::uint64_t windowStartMs = 0;
    float rateHz = 0.0f;
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

struct TrackSimState {
    std::vector<tracksim::TrackInfo> tracks;
    bool catalogReady = false;
    bool catalogPending = false;
    bool solving = false;
    bool haveResult = false;
    std::string error;
    std::string trackId;
    bool feasible = false;
    double timeBudgetS = 0.0;
    double totalTimeS = 0.0;
    double energyWh = 0.0;
    double endSoc = 0.0;
    double dEdT = 0.0;
    std::vector<tracksim::SegmentResult> segments;
    tracksim::MeasuredDrive measured;
};

class ClientArena {
public:
    ClientArena();

    void clearTelemetry();
    void ingestTelemetry(std::span<const proto::TelemetryBlock> blocks);
    void ingestCan(std::span<const proto::CanMonitorItem> items);
    void ingestTimeline(const proto::TimelineStatus& status);
    void pinCatalogSession(const ctrl::Message::CatalogSession& row);
    void clearCatalogPin();
    bool catalogPinned() const { return catalogPinned_; }
    bool catalogPlaying() const {
        return catalogPinned_ && timeline_.mode == static_cast<std::uint8_t>(proto::TimelineMode::Play);
    }
    void applyPinnedTimeline(proto::TimelineMode mode, std::uint64_t cursorMs);
    void tickPinnedPlayback(double nowMs);
    float historyRateHz(std::uint32_t signalId) const;
    const std::string& pinnedCatalogId() const { return pinnedCatalogId_; }
    void ingestVideo(const proto::VideoFrame& frame);
    void applyVideoStatus(const ctrl::Message& msg);
    void updatePlaceholderCells(double nowMs);
    void holdReplayCells();
    void pushLog(std::string text, bool error = false);
    void applyTrackSimList(const ctrl::Message& msg);
    void applyTrackSimResult(const ctrl::Message& msg);
    void applyControlError(const ctrl::Message& msg);
    void resetTrackSimCatalog();
    void beginTrackSimSolve(double timeBudgetS);
    void applySignalCatalog(const ctrl::Message& msg);
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
    const std::unordered_map<std::uint32_t, CanRow>& can() const { return can_; }
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
        // Plot scrub sets a local preview; Pit/live gauges must not follow it
        // while the server is still Live (Pause on DAQ is a no-op).
        if (hasPreviewCursor() && !timelineIsLive()) {
            return previewCursorMs_;
        }
        return timeline_.cursorMs;
    }
    // Video follows the operator pointer: rail drag, Pause/Play time, or the
    // pinned run playhead. Live t=0 is only when none of those is set.
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

    ctrl::SourceKind source = ctrl::SourceKind::None;
    std::string sourceHost = "127.0.0.1";
    std::uint16_t sourcePort = 6500;
    std::string sourcePath;
    bool haveActiveSource = false;
    std::vector<ctrl::Message::DriveDay> driveDays;
    std::string wsUrl = std::string(defaultWsUrl());
    bool txArmed = false;
    std::string txReason;
    std::vector<std::string> features;
    TrackSimState trackSim;
    std::vector<ctrl::CatalogSignal> signalCatalog;
    bool haveSignalCatalog = false;
    std::string dbcId;
    std::string dbcPath;
    std::vector<std::string> dbcs;
    VideoState video;

private:
    SignalMap signalMap_;
    AlarmEngine alarms_;
    std::unordered_map<std::uint32_t, SignalRing> rings_;
    std::unordered_map<std::uint32_t, CanRow> can_;
    proto::TimelineStatus timeline_{};
    std::uint64_t previewCursorMs_ = 0;
    bool previewCursor_ = false;
    std::uint64_t videoFollowMs_ = 0;
    CellPack cells_{};
    std::vector<LogLine> logs_;
    bool indexReadyPulse_ = false;
    bool sessionFollowPulse_ = false;
    bool mapsResetPulse_ = false;
    TileCache tiles_{};
    std::string primedHistoryKey_;
    bool catalogPinned_ = false;
    std::string pinnedCatalogId_;
    double pinnedPlayWallMs_ = 0.0;

    void syncRingSeams();
};

}  // namespace photon::ui
