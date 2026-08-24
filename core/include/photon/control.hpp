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
    SelectDbc,
    SignalCatalog,
    TxCanFrame,
    VideoSubscribe,
    VideoStop,
    VideoStatus,
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

    // select_dbc / signal_catalog (additive, protocol v1)
    std::string dbcId;
    std::vector<CatalogSignal> catalog;

    // hello_ack active_source (additive, protocol v1)
    struct ActiveSource {
        SourceKind kind = SourceKind::None;
        std::string host;
        std::uint16_t port = 0;
        std::string path;
    };
    ActiveSource activeSource;

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

}  // namespace photon::ctrl
