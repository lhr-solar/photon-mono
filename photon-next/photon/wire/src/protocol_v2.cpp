#include "photon/protocol_v2.hpp"

#include "photon/endian.hpp"

namespace photon::proto2 {
namespace {

// Payload sizes the spec fixes. Sums are 64-bit so a huge count cannot wrap.
constexpr std::uint64_t kLiveFixed = 12;    // base_ns, count, reserved
constexpr std::uint64_t kLiveRecord = 16;   // dt_us, signal, value
constexpr std::uint64_t kGapPayload = 16;   // from_ns, to_ns
constexpr std::uint64_t kTileFixed = 28;    // qid through n
constexpr std::uint64_t kVideoFixed = 16;   // t_ms, frame, flags, stream, reserved

void writeHeader(std::vector<std::uint8_t>& out, MsgType type, std::uint32_t seq, std::size_t payloadLen) {
    writeU16Le(out, kMagic);
    writeU8(out, kVersion);
    writeU8(out, static_cast<std::uint8_t>(type));
    writeU32Le(out, seq);
    writeU32Le(out, static_cast<std::uint32_t>(payloadLen));
}

void writeI64Le(std::vector<std::uint8_t>& out, std::int64_t v) {
    writeU64Le(out, static_cast<std::uint64_t>(v));
}

bool readI64Le(std::span<const std::uint8_t> in, std::size_t& off, std::int64_t& v) {
    std::uint64_t bits = 0;
    if (!readU64Le(in, off, bits)) {
        return false;
    }
    v = static_cast<std::int64_t>(bits);
    return true;
}

// Reads n f64 values into out. The caller has checked that n values fit in in.
bool readDoubles(std::span<const std::uint8_t> in, std::size_t& off, std::size_t n, std::vector<double>& out) {
    out.resize(n);
    for (double& v : out) {
        if (!readF64Le(in, off, v)) {
            return false;
        }
    }
    return true;
}

// The payload of a frame that decodes and has the given type.
std::optional<std::span<const std::uint8_t>> payloadOf(std::span<const std::uint8_t> bytes, MsgType type) {
    const auto frame = decodeFrame(bytes);
    if (!frame || frame->header.type != static_cast<std::uint8_t>(type)) {
        return std::nullopt;
    }
    return frame->payload;
}

}  // namespace

std::optional<Frame> decodeFrame(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize) {
        return std::nullopt;
    }
    std::size_t off = 0;
    std::uint16_t magic = 0;
    std::uint8_t version = 0;
    Frame frame;
    if (!readU16Le(bytes, off, magic) || !readU8(bytes, off, version) ||
        !readU8(bytes, off, frame.header.type) || !readU32Le(bytes, off, frame.header.seq) ||
        !readU32Le(bytes, off, frame.header.len)) {
        return std::nullopt;
    }
    if (magic != kMagic || version != kVersion) {
        return std::nullopt;
    }
    if (static_cast<std::uint64_t>(bytes.size()) != kHeaderSize + static_cast<std::uint64_t>(frame.header.len)) {
        return std::nullopt;
    }
    frame.payload = bytes.subspan(kHeaderSize);
    return frame;
}

// The count is a u16, so a message holds at most 65535 records. A larger one is
// written with a wrapped count, which decodeLiveSamples refuses.
std::vector<std::uint8_t> encodeLiveSamples(const LiveSamples& msg, std::uint32_t seq) {
    const std::size_t payload = static_cast<std::size_t>(kLiveFixed + kLiveRecord * msg.records.size());
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + payload);
    writeHeader(out, MsgType::LiveSamples, seq, payload);
    writeI64Le(out, msg.baseNs);
    writeU16Le(out, static_cast<std::uint16_t>(msg.records.size()));
    writeU16Le(out, 0);
    for (const LiveRecord& r : msg.records) {
        writeU32Le(out, r.dtUs);
        writeU32Le(out, r.signal);
        writeF64Le(out, r.value);
    }
    return out;
}

std::optional<LiveSamples> decodeLiveSamples(std::span<const std::uint8_t> bytes) {
    const auto payload = payloadOf(bytes, MsgType::LiveSamples);
    if (!payload || static_cast<std::uint64_t>(payload->size()) < kLiveFixed) {
        return std::nullopt;
    }
    std::size_t off = 0;
    LiveSamples msg;
    std::uint16_t count = 0;
    std::uint16_t reserved = 0;
    if (!readI64Le(*payload, off, msg.baseNs) || !readU16Le(*payload, off, count) ||
        !readU16Le(*payload, off, reserved)) {
        return std::nullopt;
    }
    if (static_cast<std::uint64_t>(payload->size()) != kLiveFixed + kLiveRecord * count) {
        return std::nullopt;
    }
    msg.records.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        LiveRecord r;
        if (!readU32Le(*payload, off, r.dtUs) || !readU32Le(*payload, off, r.signal) ||
            !readF64Le(*payload, off, r.value)) {
            return std::nullopt;
        }
        msg.records.push_back(r);
    }
    return msg;
}

std::vector<std::uint8_t> encodeGap(const Gap& msg, std::uint32_t seq) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + kGapPayload);
    writeHeader(out, MsgType::Gap, seq, kGapPayload);
    writeI64Le(out, msg.fromNs);
    writeI64Le(out, msg.toNs);
    return out;
}

std::optional<Gap> decodeGap(std::span<const std::uint8_t> bytes) {
    const auto payload = payloadOf(bytes, MsgType::Gap);
    if (!payload || static_cast<std::uint64_t>(payload->size()) != kGapPayload) {
        return std::nullopt;
    }
    std::size_t off = 0;
    Gap msg;
    if (!readI64Le(*payload, off, msg.fromNs) || !readI64Le(*payload, off, msg.toNs)) {
        return std::nullopt;
    }
    return msg;
}

// n is tMs.size(); piece.n is not read. Each array used by the level is written
// as exactly n values, a short one padded with zeros and a long one cut, so the
// frame always has the layout decodeTile expects. Keep the arrays at n to avoid
// the padding.
std::vector<std::uint8_t> encodeTile(const TilePiece& piece, std::uint32_t seq) {
    std::vector<const std::vector<double>*> arrays{&piece.tMs};
    if (piece.level == 0) {
        arrays.push_back(&piece.value);
    } else {
        arrays.push_back(&piece.min);
        arrays.push_back(&piece.max);
        arrays.push_back(&piece.mean);
    }
    const std::size_t n = piece.tMs.size();
    const std::size_t payload = static_cast<std::size_t>(kTileFixed + 8 * n * (arrays.size()));
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + payload);
    writeHeader(out, MsgType::Tile, seq, payload);
    writeU32Le(out, piece.qid);
    writeU32Le(out, piece.signal);
    writeU8(out, piece.level);
    writeU8(out, piece.flags);
    writeU16Le(out, 0);
    writeI64Le(out, piece.index);
    writeU32Le(out, piece.offset);
    writeU32Le(out, static_cast<std::uint32_t>(n));
    for (const std::vector<double>* array : arrays) {
        for (std::size_t i = 0; i < n; ++i) {
            writeF64Le(out, i < array->size() ? (*array)[i] : 0.0);
        }
    }
    return out;
}

// A tile piece is at most kTilePieceMax bytes on the wire, as the server cuts them.
std::optional<TilePiece> decodeTile(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > kTilePieceMax) {
        return std::nullopt;
    }
    const auto payload = payloadOf(bytes, MsgType::Tile);
    if (!payload || static_cast<std::uint64_t>(payload->size()) < kTileFixed) {
        return std::nullopt;
    }
    std::size_t off = 0;
    TilePiece piece;
    std::uint16_t reserved = 0;
    if (!readU32Le(*payload, off, piece.qid) || !readU32Le(*payload, off, piece.signal) ||
        !readU8(*payload, off, piece.level) || !readU8(*payload, off, piece.flags) ||
        !readU16Le(*payload, off, reserved) || !readI64Le(*payload, off, piece.index) ||
        !readU32Le(*payload, off, piece.offset) || !readU32Le(*payload, off, piece.n)) {
        return std::nullopt;
    }
    const std::uint64_t arrays = piece.level == 0 ? 2 : 4;
    if (static_cast<std::uint64_t>(payload->size()) != kTileFixed + 8 * arrays * std::uint64_t{piece.n}) {
        return std::nullopt;
    }
    const std::size_t n = piece.n;
    if (!readDoubles(*payload, off, n, piece.tMs)) {
        return std::nullopt;
    }
    if (piece.level == 0) {
        if (!readDoubles(*payload, off, n, piece.value)) {
            return std::nullopt;
        }
    } else if (!readDoubles(*payload, off, n, piece.min) || !readDoubles(*payload, off, n, piece.max) ||
               !readDoubles(*payload, off, n, piece.mean)) {
        return std::nullopt;
    }
    return piece;
}

// decodeVideoChunk refuses a payload over kVideoChunkMax; encode does not check.
std::vector<std::uint8_t> encodeVideoChunk(const VideoChunk& chunk, std::uint32_t seq) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + kVideoFixed + chunk.payload.size());
    writeHeader(out, MsgType::VideoChunk, seq, static_cast<std::size_t>(kVideoFixed) + chunk.payload.size());
    writeU64Le(out, chunk.tMs);
    writeU32Le(out, chunk.frame);
    writeU8(out, chunk.flags);
    writeU8(out, chunk.stream);
    writeU16Le(out, 0);
    writeBytes(out, chunk.payload);
    return out;
}

std::optional<VideoChunk> decodeVideoChunk(std::span<const std::uint8_t> bytes) {
    const auto payload = payloadOf(bytes, MsgType::VideoChunk);
    if (!payload || static_cast<std::uint64_t>(payload->size()) < kVideoFixed) {
        return std::nullopt;
    }
    if (payload->size() - kVideoFixed > kVideoChunkMax) {
        return std::nullopt;
    }
    std::size_t off = 0;
    VideoChunk chunk;
    std::uint16_t reserved = 0;
    if (!readU64Le(*payload, off, chunk.tMs) || !readU32Le(*payload, off, chunk.frame) ||
        !readU8(*payload, off, chunk.flags) || !readU8(*payload, off, chunk.stream) ||
        !readU16Le(*payload, off, reserved)) {
        return std::nullopt;
    }
    const auto piece = payload->subspan(off);
    chunk.payload.assign(piece.begin(), piece.end());
    return chunk;
}

}  // namespace photon::proto2
