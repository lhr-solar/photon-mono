#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Protocol v2 binary frames: photon-next/docs/protocol-v2.md. Every integer is
// little-endian. Each encode returns a whole frame (12-byte header, then the
// payload). Each decode takes a whole frame and returns nullopt for anything
// malformed: bad magic, bad version, a length that is not the frame's size, a
// wrong type, or a payload that does not match its own counts.
namespace photon::proto2 {

inline constexpr std::uint16_t kMagic = 0x3250;  // "P2" on the wire
inline constexpr std::uint8_t kVersion = 2;
inline constexpr std::size_t kHeaderSize = 12;
inline constexpr std::size_t kVideoChunkMax = 16 << 10;  // largest video payload piece
inline constexpr std::size_t kTilePieceMax = 64 << 10;   // largest tile piece on the wire

enum class MsgType : std::uint8_t {
    LiveSamples = 0x20,
    Gap = 0x21,
    Tile = 0x22,
    VideoChunk = 0x23,
};

// Tile flags.
inline constexpr std::uint8_t kTileSealed = 1 << 0;  // the tile never changes
inline constexpr std::uint8_t kTileLast = 1 << 1;    // the last piece of the tile

// VideoChunk flags.
inline constexpr std::uint8_t kVideoKeyframe = 1 << 0;
inline constexpr std::uint8_t kVideoConfig = 1 << 1;  // SPS/PPS
inline constexpr std::uint8_t kVideoFirst = 1 << 2;   // first piece of a frame
inline constexpr std::uint8_t kVideoLast = 1 << 3;    // last piece of a frame
inline constexpr std::uint8_t kVideoReplay = 1 << 4;

struct Header {
    std::uint8_t type = 0;
    std::uint32_t seq = 0;  // the server's per-socket counter; 0 when encoding
    std::uint32_t len = 0;  // payload bytes after the header
};

// A checked frame: its header and the payload view into the bytes it was read from.
struct Frame {
    Header header;
    std::span<const std::uint8_t> payload;
};

// decodeFrame checks the magic, the version and that the bytes hold exactly the
// payload the header announces.
std::optional<Frame> decodeFrame(std::span<const std::uint8_t> bytes);

// LiveSamples (0x20): decoded values of subscribed signals. Record times are
// baseNs + dtUs * 1000.
struct LiveRecord {
    std::uint32_t dtUs = 0;
    std::uint32_t signal = 0;  // catalog signal index
    double value = 0.0;        // in the signal's unit
};
struct LiveSamples {
    std::int64_t baseNs = 0;
    std::vector<LiveRecord> records;
};
std::vector<std::uint8_t> encodeLiveSamples(const LiveSamples& msg, std::uint32_t seq = 0);
std::optional<LiveSamples> decodeLiveSamples(std::span<const std::uint8_t> bytes);

// Gap (0x21): the client fell behind the live ring between fromNs and toNs.
struct Gap {
    std::int64_t fromNs = 0;
    std::int64_t toNs = 0;
};
std::vector<std::uint8_t> encodeGap(const Gap& msg, std::uint32_t seq = 0);
std::optional<Gap> decodeGap(std::span<const std::uint8_t> bytes);

// Tile (0x22): one piece of a tile. Level 0 carries value; level above 0 carries
// min, max and mean. tMs holds the sample times in Unix ms, and n == tMs.size().
struct TilePiece {
    std::uint32_t qid = 0;
    std::uint32_t signal = 0;
    std::uint8_t level = 0;
    std::uint8_t flags = 0;
    std::int64_t index = 0;
    std::uint32_t offset = 0;
    std::uint32_t n = 0;
    std::vector<double> tMs;
    std::vector<double> value;
    std::vector<double> min;
    std::vector<double> max;
    std::vector<double> mean;
};
std::vector<std::uint8_t> encodeTile(const TilePiece& piece, std::uint32_t seq = 0);
std::optional<TilePiece> decodeTile(std::span<const std::uint8_t> bytes);

// VideoChunk (0x23): one piece of an Annex-B H.264 frame, at most kVideoChunkMax.
struct VideoChunk {
    std::uint64_t tMs = 0;
    std::uint32_t frame = 0;
    std::uint8_t flags = 0;
    std::uint8_t stream = 0;
    std::vector<std::uint8_t> payload;
};
std::vector<std::uint8_t> encodeVideoChunk(const VideoChunk& chunk, std::uint32_t seq = 0);
std::optional<VideoChunk> decodeVideoChunk(std::span<const std::uint8_t> bytes);

}  // namespace photon::proto2
