#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#if defined(_MSC_VER)
#define PHOTON_PACKED_BEGIN __pragma(pack(push, 1))
#define PHOTON_PACKED_END __pragma(pack(pop))
#define PHOTON_PACKED
#else
#define PHOTON_PACKED_BEGIN
#define PHOTON_PACKED_END
#define PHOTON_PACKED __attribute__((packed))
#endif

namespace photon::proto {

inline constexpr std::uint16_t kMagic = 0x5057;  // "PW"
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 12;

enum class MsgType : std::uint8_t {
    TelemetryBatch = 0x01,
    CanMonitorBatch = 0x02,
    VideoFrame = 0x04,
    TimelineStatus = 0x05,
};

inline constexpr std::uint8_t kVideoFlagKeyframe = 1 << 0;
inline constexpr std::uint8_t kVideoFlagConfig = 1 << 1;
inline constexpr std::size_t kVideoFrameHeaderSize = 12;

enum class TimelineMode : std::uint8_t {
    Unknown = 0,
    Pause = 1,
    Play = 2,
    Live = 3,
    Buffering = 4,  // additive: log_replay index in progress
};

PHOTON_PACKED_BEGIN
struct PHOTON_PACKED FrameHeader {
    std::uint16_t magic;
    std::uint8_t version;
    std::uint8_t msgType;
    std::uint32_t sequence;
    std::uint32_t payloadLen;
};

struct PHOTON_PACKED TelemetryBlockHeader {
    std::uint32_t signalId;
    std::uint16_t sampleCount;
    std::uint16_t reserved;
};

struct PHOTON_PACKED CanMonitorItem {
    std::uint32_t canId;
    std::uint8_t dlc;
    std::uint8_t flags;
    std::uint16_t reserved;
    std::uint64_t timestampMs;
    std::uint8_t data[8];
};

struct PHOTON_PACKED TimelineStatus {
    std::uint8_t mode;
    std::uint64_t cursorMs;
    std::uint64_t minTimeMs;
    std::uint64_t maxTimeMs;
    std::uint64_t totalBatches;
};

// VideoFrame payload prefix (little-endian, same as other photon-wire v1 types).
// Followed by Annex-B NAL bytes (including 00 00 00 01).
struct PHOTON_PACKED VideoFrameHeader {
    std::uint64_t timestampMs;
    std::uint8_t flags;
    std::uint8_t reserved[3];
};
PHOTON_PACKED_END

static_assert(sizeof(FrameHeader) == 12, "FrameHeader must be 12 bytes");
static_assert(offsetof(FrameHeader, magic) == 0);
static_assert(offsetof(FrameHeader, version) == 2);
static_assert(offsetof(FrameHeader, msgType) == 3);
static_assert(offsetof(FrameHeader, sequence) == 4);
static_assert(offsetof(FrameHeader, payloadLen) == 8);

static_assert(sizeof(TelemetryBlockHeader) == 8);
static_assert(offsetof(TelemetryBlockHeader, signalId) == 0);
static_assert(offsetof(TelemetryBlockHeader, sampleCount) == 4);
static_assert(offsetof(TelemetryBlockHeader, reserved) == 6);

static_assert(sizeof(CanMonitorItem) == 24);
static_assert(offsetof(CanMonitorItem, canId) == 0);
static_assert(offsetof(CanMonitorItem, dlc) == 4);
static_assert(offsetof(CanMonitorItem, flags) == 5);
static_assert(offsetof(CanMonitorItem, reserved) == 6);
static_assert(offsetof(CanMonitorItem, timestampMs) == 8);
static_assert(offsetof(CanMonitorItem, data) == 16);

static_assert(sizeof(TimelineStatus) == 33);
static_assert(offsetof(TimelineStatus, mode) == 0);
static_assert(offsetof(TimelineStatus, cursorMs) == 1);
static_assert(offsetof(TimelineStatus, minTimeMs) == 9);
static_assert(offsetof(TimelineStatus, maxTimeMs) == 17);
static_assert(offsetof(TimelineStatus, totalBatches) == 25);

static_assert(sizeof(VideoFrameHeader) == 12, "VideoFrameHeader must be 12 bytes");
static_assert(offsetof(VideoFrameHeader, timestampMs) == 0);
static_assert(offsetof(VideoFrameHeader, flags) == 8);
static_assert(offsetof(VideoFrameHeader, reserved) == 9);

struct TelemetryBlock {
    std::uint32_t signalId = 0;
    std::uint16_t reserved = 0;
    std::vector<double> timestamps;
    std::vector<double> values;
};

std::vector<std::uint8_t> encodeHeader(const FrameHeader& header);
std::optional<FrameHeader> decodeHeader(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> encodeFrame(MsgType type, std::uint32_t sequence,
                                      std::span<const std::uint8_t> payload);
struct DecodedFrame {
    FrameHeader header;
    std::span<const std::uint8_t> payload;
};
std::optional<DecodedFrame> decodeFrame(std::span<const std::uint8_t> bytes);

std::vector<std::uint8_t> encodeTelemetryBatch(std::span<const TelemetryBlock> blocks);
std::optional<std::vector<TelemetryBlock>> decodeTelemetryBatch(std::span<const std::uint8_t> payload);

std::vector<std::uint8_t> encodeCanMonitorBatch(std::span<const CanMonitorItem> items);
std::optional<std::vector<CanMonitorItem>> decodeCanMonitorBatch(std::span<const std::uint8_t> payload);

std::vector<std::uint8_t> encodeTimelineStatus(const TimelineStatus& status);
std::optional<TimelineStatus> decodeTimelineStatus(std::span<const std::uint8_t> payload);

struct VideoFrame {
    std::uint64_t timestampMs = 0;
    std::uint8_t flags = 0;
    std::uint8_t reserved[3]{};
    std::vector<std::uint8_t> nal;
};

inline bool videoKeyframe(std::uint8_t flags) { return (flags & kVideoFlagKeyframe) != 0; }
inline bool videoConfig(std::uint8_t flags) { return (flags & kVideoFlagConfig) != 0; }

std::vector<std::uint8_t> encodeVideoFrame(const VideoFrame& frame);
std::optional<VideoFrame> decodeVideoFrame(std::span<const std::uint8_t> payload);

}  // namespace photon::proto
