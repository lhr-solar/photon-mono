#pragma once

#include "photon/control.hpp"
#include "photon/protocol.hpp"
#include "photon_ui/alarms.hpp"
#include "photon_ui/catalog.hpp"
#include "photon_ui/signal_map.hpp"

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
    std::size_t plotOffset() const;
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
    bool synthetic = true;
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
    std::uint64_t frames = 0;
    std::uint64_t dropped = 0;
    bool haveStatus = false;
    bool subscribed = false;
    double fps = 0.0;
    std::uint64_t fpsWindowStartMs = 0;
    std::uint64_t fpsWindowCount = 0;
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
};

class ClientArena {
public:
    ClientArena();

    void clearTelemetry();
    void ingestTelemetry(std::span<const proto::TelemetryBlock> blocks);
    void ingestCan(std::span<const proto::CanMonitorItem> items);
    void ingestTimeline(const proto::TimelineStatus& status);
    void ingestVideo(const proto::VideoFrame& frame);
    void applyVideoStatus(const ctrl::Message& msg);
    void updatePlaceholderCells(double nowMs);
    void pushLog(std::string text, bool error = false);
    void applyTrackSimList(const ctrl::Message& msg);
    void applyTrackSimResult(const ctrl::Message& msg);
    void applyControlError(const ctrl::Message& msg);
    void resetTrackSimCatalog();
    void beginTrackSimSolve(double timeBudgetS);
    void applySignalCatalog(const ctrl::Message& msg);
    void resetSignalMap();
    void refreshMappedCells();
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
    bool has(std::uint32_t id) const;

    const std::unordered_map<std::uint32_t, SignalRing>& rings() const { return rings_; }
    const std::unordered_map<std::uint32_t, CanRow>& can() const { return can_; }
    const proto::TimelineStatus& timeline() const { return timeline_; }
    const CellPack& cells() const { return cells_; }
    const std::vector<LogLine>& logs() const { return logs_; }

    ctrl::SourceKind source = ctrl::SourceKind::None;
    std::string sourceHost = "127.0.0.1";
    std::uint16_t sourcePort = 6500;
    std::string sourcePath;
    bool haveActiveSource = false;
    std::string wsUrl = std::string(defaultWsUrl());
    bool txArmed = false;
    std::string txReason;
    std::vector<std::string> features;
    TrackSimState trackSim;
    std::vector<ctrl::CatalogSignal> signalCatalog;
    bool haveSignalCatalog = false;
    VideoState video;

private:
    SignalMap signalMap_;
    AlarmEngine alarms_;
    std::unordered_map<std::uint32_t, SignalRing> rings_;
    std::unordered_map<std::uint32_t, CanRow> can_;
    proto::TimelineStatus timeline_{};
    CellPack cells_{};
    std::vector<LogLine> logs_;
};

}  // namespace photon::ui
