#pragma once

#include <cstddef>
#include <cstdint>
#include <stddef.h>
#include <optional>
#include <span>
#include <vector>

#if defined(_MSC_VER)
#define PHOTON_CANP_PACKED_BEGIN __pragma(pack(push, 1))
#define PHOTON_CANP_PACKED_END __pragma(pack(pop))
#define PHOTON_CANP_PACKED
#else
#define PHOTON_CANP_PACKED_BEGIN
#define PHOTON_CANP_PACKED_END
#define PHOTON_CANP_PACKED __attribute__((packed))
#endif

namespace photon::canp {

// Native CANP v3 framing, compatible with photon-server/wire/canp.go.
// Wire is network byte order (big-endian), including the 64-bit timestamp.
inline constexpr std::uint32_t kMagic = 0x43414E31u;  // "CAN1"
inline constexpr std::uint16_t kVersion = 3;
inline constexpr std::uint16_t kMaxBatch = 64;
inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::size_t kPacketSize = 29;

PHOTON_CANP_PACKED_BEGIN
struct PHOTON_CANP_PACKED WireHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t count;
    std::uint32_t seq;
    std::uint64_t timestamp;
};

struct PHOTON_CANP_PACKED WirePacket {
    std::uint32_t canId;
    std::uint8_t dlc;
    std::uint8_t data[8];
    std::uint16_t dt[8];
};
PHOTON_CANP_PACKED_END

static_assert(sizeof(WireHeader) == kHeaderSize);
static_assert(offsetof(WireHeader, magic) == 0);
static_assert(offsetof(WireHeader, version) == 4);
static_assert(offsetof(WireHeader, count) == 6);
static_assert(offsetof(WireHeader, seq) == 8);
static_assert(offsetof(WireHeader, timestamp) == 12);

static_assert(sizeof(WirePacket) == kPacketSize);
static_assert(offsetof(WirePacket, canId) == 0);
static_assert(offsetof(WirePacket, dlc) == 4);
static_assert(offsetof(WirePacket, data) == 5);
static_assert(offsetof(WirePacket, dt) == 13);

enum class ParseStatus {
    Ok,
    Incomplete,
    BadMagic,
    BadVersion,
    BadCount,
};

struct Packet {
    std::uint32_t canId = 0;  // host order
    std::uint8_t dlc = 0;
    std::uint8_t data[8]{};
    std::uint16_t dt[8]{};  // host order; per-byte δt from CANP v3
};

struct Batch {
    std::uint32_t seq = 0;
    std::uint64_t timestamp = 0;  // unix milliseconds
    std::uint16_t count = 0;
    Packet packets[kMaxBatch]{};
};

std::vector<std::uint8_t> encodeBatch(const Batch& batch);
ParseStatus parseBatch(std::span<const std::uint8_t> bytes, Batch& out, std::size_t& consumed);

// Incremental TCP stream parser for concatenated CANP batches.
class StreamParser {
public:
    void feed(std::span<const std::uint8_t> bytes);
    std::optional<Batch> take();
    ParseStatus lastError() const { return lastError_; }
    void clear();

private:
    std::vector<std::uint8_t> buf_;
    ParseStatus lastError_ = ParseStatus::Incomplete;
};

}  // namespace photon::canp
