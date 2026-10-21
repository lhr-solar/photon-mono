#pragma once

#include "photon/json.hpp"
#include "photon/tracksim.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace photon::ctrl {

inline constexpr int kProtocolVersion = 1;

enum class Type {
    Hello,
    HelloAck,
    SelectSource,
    SubscribeSignals,
    UnsubscribeSignals,
    TimelineControl,
    TxArmRequest,
    TxArmStatus,
    ErrorAlert,
    Heartbeat,
    TrackSimList,
    TrackSimRequest,
    TrackSimResult,
    ArraySimRequest,
    ArraySimResult,
    SelectDbc,
    SignalCatalog,
    TxCanFrame,
    VideoSubscribe,
    VideoStop,
    VideoStatus,
    HistoryQuery,
    HistoryResult,
    HistoryCancel,
    HistoryCredit,
    VideoLocate,
    VideoClock,
    AnalysisComputeRequest,
    AnalysisComputeResult,
    AnalysisComputeCancel,
    CatalogList,
    CatalogResult,
    SubscribeCan,
    UnsubscribeCan,
};

enum class SourceKind {
    None,
    Pcan,
    DaqTcp,
    LogReplay,
    Synthetic,
};

enum class TimelineMode {
    Unknown,
    Pause,
    Play,
    Live,
};

enum class VideoMode {
    Live,
    Replay,
};

struct CatalogSignal {
    std::uint32_t id = 0;
    std::uint32_t messageId = 0;
    std::string message;
    std::string name;
    std::string unit;
    double min = 0.0;
    double max = 0.0;
};

struct TxSignal {
    std::string name;
    double value = 0.0;
};

struct Message {
    Type type = Type::ErrorAlert;
    int protocolVersion = kProtocolVersion;
    std::string client;
    std::string server;
    std::vector<std::string> features;

    SourceKind source = SourceKind::None;
    std::string host;
    std::uint16_t port = 0;
    std::string path;

    std::vector<std::uint32_t> signalIds;

    TimelineMode timelineMode = TimelineMode::Unknown;
    std::uint64_t cursorMs = 0;
    double speed = 1.0;

    bool armed = false;
    std::string reason;

    std::string code;
    std::string message;

    // track_sim_* (additive, protocol v1). Result sizes stay on the JSON
    // control plane; no new binary MsgType.
    std::string trackId;
    std::optional<tracksim::Track> track;
    tracksim::CarOverride carOverride;
    double timeBudgetS = 0.0;
    double initialSoc = 0.90;
    bool feasible = false;
    double totalTimeS = 0.0;
    double energyWh = 0.0;
    double endSoc = 0.0;
    double dEdT = 0.0;
    std::vector<tracksim::SegmentResult> trackSegments;
    std::vector<tracksim::TrackInfo> tracks;
    tracksim::MeasuredDrive measured;

    // One stationary ArrayShading run; paths refer to the backend machine.
    double arrayAzimuthDeg = 125.75;
    double arrayElevationDeg = 72.05125;
    double arrayDniWm2 = 850.0;
    double arrayDhiWm2 = 110.0;
    std::uint32_t arrayDirectSamples = 3;
    std::uint32_t arrayHemisphereSamples = 64;
    std::uint32_t arrayReflectionDepth = 2;
    std::string arrayError;
    std::uint64_t arrayTriangleCount = 0;
    std::uint64_t arrayCellCount = 0;
    std::uint64_t arrayTwoByThreeCount = 0;
    std::uint64_t arrayOneByThreeCount = 0;
    double arrayAreaM2 = 0.0;
    double arrayMeanIrradianceWm2 = 0.0;
    double arrayIncidentPowerW = 0.0;
    struct ArrayCell {
        std::uint32_t row = 0;
        std::uint32_t column = 0;
        std::uint32_t moduleId = 0;
        double directWm2 = 0.0;
        double diffuseWm2 = 0.0;
        double reflectedWm2 = 0.0;
        double totalWm2 = 0.0;
        double activeAreaM2 = 0.0;
        double incidentW = 0.0;
        double directVisibility = 0.0;
    };
    std::vector<ArrayCell> arrayCells;

    // select_dbc / signal_catalog / hello_ack.dbc (additive, protocol v1)
    std::string dbcId;
    std::string dbcPath;
    std::vector<std::string> dbcs;
    std::vector<CatalogSignal> catalog;

    // hello_ack active_source (additive, protocol v1)
    struct ActiveSource {
        SourceKind kind = SourceKind::None;
        std::string host;
        std::uint16_t port = 0;
        std::string path;
    };
    ActiveSource activeSource;

    // hello_ack drive_days (additive, protocol v1): clickable on-track days
    struct DriveSession {
        std::string id;
        std::string path;
        std::uint64_t startMs = 0;
        double durationS = 0.0;
        double speedMaxMps = 0.0;
    };
    struct DriveDay {
        std::string date;
        std::string path;
        int sessionCount = 0;
        double durationS = 0.0;
        std::vector<DriveSession> sessions;
    };
    std::vector<DriveDay> driveDays;

    // catalog_list / catalog_result (additive, protocol v1): TSDB catalog.json rows
    struct CatalogSession {
        std::string id;
        std::string path;
        std::uint64_t minMs = 0;
        std::uint64_t maxMs = 0;
        std::string dbcHash;
        std::uint64_t sampleCount = 0;
        bool hasVideo = false;
    };
    std::vector<CatalogSession> catalogSessions;

    // select_source pcan (additive): channel 1-8, baud 125k/250k/500k/1M, listen-only default true
    int channel = 1;
    int baud = 250000;
    bool listenOnly = true;

    // tx_can_frame (additive): DBC message name lives in `message`; signals are physical values
    std::vector<TxSignal> txSignals;

    // video_subscribe / video_status (additive, protocol v1)
    VideoMode videoMode = VideoMode::Live;
    std::uint64_t startMs = 0;
    bool videoConnected = false;
    std::uint64_t lastTsMs = 0;
    std::uint64_t videoFrames = 0;
    std::uint64_t videoDropped = 0;
    std::uint8_t videoStreamId = 0;
    std::uint16_t videoPrefetchGops = 4;
    std::int64_t videoClockOffsetMs = 0;

    struct HistoryPoint {
        std::uint64_t t = 0;
        double v = 0.0;
        bool present = false;
    };
    struct HistorySeries {
        std::uint32_t signalId = 0;
        int level = 0;
        std::uint32_t bucketMs = 0;
        std::uint32_t points = 0;
        std::uint64_t tMin = 0;
        std::uint64_t tMax = 0;
        std::uint32_t chunks = 0;
        HistoryPoint prior;
        HistoryPoint next;
    };

    std::uint32_t queryId = 0;
    std::string sessionId;
    std::uint64_t fromMs = 0;
    std::uint64_t toMs = 0;
    std::uint32_t maxPoints = 2000;
    std::vector<std::string> fields;
    bool includeVideo = false;
    std::uint64_t creditBytes = 4 * 1024 * 1024;
    int historyLevel = 0;
    std::uint32_t bucketMs = 0;
    bool truncated = false;
    std::vector<HistorySeries> historySeries;

    // analysis_compute_* (additive, protocol v1). No new binary MsgType.
    // Sidecar is loopback HTTP; these JSON fields stay on the control object.
    std::string jobId;
    std::string dbcHash;
    std::uint64_t t0Ms = 0;
    std::uint64_t t1Ms = 0;
    std::string prompt;
    std::string recipeId;
    std::string computeKind;
    std::string computeStatus;
    std::string summary;
    struct DerivedChannel {
        std::string name;
        std::string unit;
        std::string pointsRef;
    };
    std::vector<DerivedChannel> derived;
    struct ComputeEvent {
        std::uint64_t tMs = 0;
        std::string name;
        std::string message;
    };
    std::vector<ComputeEvent> events;
};

std::string_view typeName(Type type);
std::optional<Type> typeFromName(std::string_view name);
std::string_view sourceName(SourceKind kind);
std::optional<SourceKind> sourceFromName(std::string_view name);
std::string_view timelineName(TimelineMode mode);
std::optional<TimelineMode> timelineFromName(std::string_view name);
std::string_view videoModeName(VideoMode mode);
std::optional<VideoMode> videoModeFromName(std::string_view name);

std::optional<Message> parse(std::string_view jsonText, std::string* error = nullptr);
std::string serialize(const Message& msg);

Message hello(std::string client);
Message helloAck(std::vector<std::string> features, Message::ActiveSource active = {});
Message selectSource(SourceKind kind);
Message subscribe(std::vector<std::uint32_t> ids);
Message unsubscribe(std::vector<std::uint32_t> ids);
Message subscribeCan();
Message unsubscribeCan();
Message timelineControl(TimelineMode mode, std::uint64_t cursorMs, double speed = 1.0);
Message selectDbc(std::string pathOrId, bool bundledId = false);
Message signalCatalog(std::vector<CatalogSignal> catalog);
Message txArmRequest(bool armed);
Message txArmStatus(bool armed, std::string reason);
Message errorAlert(std::string code, std::string message);
Message heartbeat();
Message trackSimList();
Message trackSimListResult(std::vector<tracksim::TrackInfo> tracks);
Message trackSimRequest(std::string trackId, double timeBudgetS, double initialSoc = 0.90);
Message trackSimResult(const tracksim::SolveOutput& solved, std::string trackId = {});
Message txCanFrame(std::string messageName, std::vector<TxSignal> signals);
Message videoSubscribe(VideoMode mode, std::uint64_t startMs = 0);
Message videoStop();
Message videoStatus(bool connected, std::string host, std::uint16_t port, VideoMode mode,
                    std::uint64_t lastTsMs, std::uint64_t frames, std::uint64_t dropped);
Message historyQuery(std::uint32_t queryId, std::string sessionId, std::uint64_t fromMs, std::uint64_t toMs,
                     std::vector<std::uint32_t> signalIds, std::uint32_t maxPoints = 2000,
                     std::uint64_t creditBytes = 4 * 1024 * 1024);
Message historyResult(std::uint32_t queryId, int level, std::uint32_t bucketMs, std::uint64_t fromMs,
                      std::uint64_t toMs, std::vector<Message::HistorySeries> series, bool truncated = false);
Message historyCancel(std::uint32_t queryId);
Message historyCredit(std::uint32_t queryId, std::uint64_t creditBytes);
Message videoLocate(std::uint64_t atMs, std::uint8_t streamId = 0, std::uint16_t prefetchGops = 4);
Message videoClock(std::int64_t offsetMs);
Message analysisComputeRequest(std::string jobId, std::string sessionId, std::uint64_t t0Ms, std::uint64_t t1Ms,
                               std::vector<std::uint32_t> signalIds, std::string recipeId = "summarize_window",
                               std::uint32_t maxPoints = 2000);
Message analysisComputeResult(std::string jobId, std::string status, std::string summary = {},
                              std::string code = {}, std::string message = {});
Message analysisComputeCancel(std::string jobId);
Message catalogList();
Message catalogResult(std::vector<Message::CatalogSession> sessions);

}  // namespace photon::ctrl
