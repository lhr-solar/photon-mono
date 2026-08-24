#include "photon/protocol.hpp"

#include "photon/endian.hpp"

#include <algorithm>

namespace photon::proto {
namespace {

FrameHeader headerFromFields(MsgType type, std::uint32_t sequence, std::uint32_t payloadLen) {
    FrameHeader header{};
    header.magic = kMagic;
    header.version = kVersion;
    header.msgType = static_cast<std::uint8_t>(type);
    header.sequence = sequence;
    header.payloadLen = payloadLen;
    return header;
}

}  // namespace

std::vector<std::uint8_t> encodeHeader(const FrameHeader& header) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize);
    writeU16Le(out, header.magic);
    writeU8(out, header.version);
    writeU8(out, header.msgType);
    writeU32Le(out, header.sequence);
    writeU32Le(out, header.payloadLen);
    return out;
}

std::optional<FrameHeader> decodeHeader(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize) {
        return std::nullopt;
    }
    FrameHeader header{};
    std::size_t off = 0;
    if (!readU16Le(bytes, off, header.magic) || !readU8(bytes, off, header.version) ||
        !readU8(bytes, off, header.msgType) || !readU32Le(bytes, off, header.sequence) ||
        !readU32Le(bytes, off, header.payloadLen)) {
        return std::nullopt;
    }
    if (header.magic != kMagic || header.version != kVersion) {
        return std::nullopt;
    }
    return header;
}

std::vector<std::uint8_t> encodeFrame(MsgType type, std::uint32_t sequence,
                                      std::span<const std::uint8_t> payload) {
    const auto header = headerFromFields(type, sequence, static_cast<std::uint32_t>(payload.size()));
    auto out = encodeHeader(header);
    writeBytes(out, payload);
    return out;
}

std::optional<DecodedFrame> decodeFrame(std::span<const std::uint8_t> bytes) {
    auto header = decodeHeader(bytes);
    if (!header) {
        return std::nullopt;
    }
    if (bytes.size() < kHeaderSize + header->payloadLen) {
        return std::nullopt;
    }
    return DecodedFrame{*header, bytes.subspan(kHeaderSize, header->payloadLen)};
}

std::vector<std::uint8_t> encodeTelemetryBatch(std::span<const TelemetryBlock> blocks) {
    std::vector<std::uint8_t> out;
    for (const auto& block : blocks) {
        const auto count = static_cast<std::uint16_t>(block.timestamps.size());
        writeU32Le(out, block.signalId);
        writeU16Le(out, count);
        writeU16Le(out, block.reserved);
        const std::size_t n = std::min(block.timestamps.size(), block.values.size());
        for (std::size_t i = 0; i < n; ++i) {
            writeF64Le(out, block.timestamps[i]);
        }
        for (std::size_t i = 0; i < n; ++i) {
            writeF64Le(out, block.values[i]);
        }
    }
    return out;
}

std::optional<std::vector<TelemetryBlock>> decodeTelemetryBatch(std::span<const std::uint8_t> payload) {
    std::vector<TelemetryBlock> blocks;
    std::size_t off = 0;
    while (off < payload.size()) {
        TelemetryBlock block;
        std::uint16_t count = 0;
        if (!readU32Le(payload, off, block.signalId) || !readU16Le(payload, off, count) ||
            !readU16Le(payload, off, block.reserved)) {
            return std::nullopt;
        }
        const std::size_t need = static_cast<std::size_t>(count) * 16;
        if (off + need > payload.size()) {
            return std::nullopt;
        }
        block.timestamps.resize(count);
        block.values.resize(count);
        for (std::uint16_t i = 0; i < count; ++i) {
            if (!readF64Le(payload, off, block.timestamps[i])) {
                return std::nullopt;
            }
        }
        for (std::uint16_t i = 0; i < count; ++i) {
            if (!readF64Le(payload, off, block.values[i])) {
                return std::nullopt;
            }
        }
        blocks.push_back(std::move(block));
    }
    return blocks;
}

std::vector<std::uint8_t> encodeCanMonitorBatch(std::span<const CanMonitorItem> items) {
    std::vector<std::uint8_t> out;
    out.reserve(items.size() * sizeof(CanMonitorItem));
    for (const auto& item : items) {
        writeU32Le(out, item.canId);
        writeU8(out, item.dlc);
        writeU8(out, item.flags);
        writeU16Le(out, item.reserved);
        writeU64Le(out, item.timestampMs);
        for (std::uint8_t b : item.data) {
            writeU8(out, b);
        }
    }
    return out;
}

std::optional<std::vector<CanMonitorItem>> decodeCanMonitorBatch(std::span<const std::uint8_t> payload) {
    if (payload.size() % sizeof(CanMonitorItem) != 0) {
        return std::nullopt;
    }
    std::vector<CanMonitorItem> items;
    items.reserve(payload.size() / sizeof(CanMonitorItem));
    std::size_t off = 0;
    while (off < payload.size()) {
        CanMonitorItem item{};
        if (!readU32Le(payload, off, item.canId) || !readU8(payload, off, item.dlc) ||
            !readU8(payload, off, item.flags) || !readU16Le(payload, off, item.reserved) ||
            !readU64Le(payload, off, item.timestampMs)) {
            return std::nullopt;
        }
        for (std::uint8_t& b : item.data) {
            if (!readU8(payload, off, b)) {
                return std::nullopt;
            }
        }
        items.push_back(item);
    }
    return items;
}

std::vector<std::uint8_t> encodeTimelineStatus(const TimelineStatus& status) {
    std::vector<std::uint8_t> out;
    out.reserve(sizeof(TimelineStatus));
    writeU8(out, status.mode);
    writeU64Le(out, status.cursorMs);
    writeU64Le(out, status.minTimeMs);
    writeU64Le(out, status.maxTimeMs);
    writeU64Le(out, status.totalBatches);
    return out;
}

std::optional<TimelineStatus> decodeTimelineStatus(std::span<const std::uint8_t> payload) {
    if (payload.size() != sizeof(TimelineStatus)) {
        return std::nullopt;
    }
    TimelineStatus status{};
    std::size_t off = 0;
    if (!readU8(payload, off, status.mode) || !readU64Le(payload, off, status.cursorMs) ||
        !readU64Le(payload, off, status.minTimeMs) || !readU64Le(payload, off, status.maxTimeMs) ||
        !readU64Le(payload, off, status.totalBatches)) {
        return std::nullopt;
    }
    return status;
}

std::vector<std::uint8_t> encodeVideoFrame(const VideoFrame& frame) {
    std::vector<std::uint8_t> out;
    out.reserve(kVideoFrameHeaderSize + frame.nal.size());
    writeU64Le(out, frame.timestampMs);
    writeU8(out, frame.flags);
    writeU8(out, frame.reserved[0]);
    writeU8(out, frame.reserved[1]);
    writeU8(out, frame.reserved[2]);
    writeBytes(out, frame.nal);
    return out;
}

std::optional<VideoFrame> decodeVideoFrame(std::span<const std::uint8_t> payload) {
    if (payload.size() < kVideoFrameHeaderSize) {
        return std::nullopt;
    }
    VideoFrame frame;
    std::size_t off = 0;
    if (!readU64Le(payload, off, frame.timestampMs) || !readU8(payload, off, frame.flags) ||
        !readU8(payload, off, frame.reserved[0]) || !readU8(payload, off, frame.reserved[1]) ||
        !readU8(payload, off, frame.reserved[2])) {
        return std::nullopt;
    }
    if (off < payload.size()) {
        frame.nal.assign(payload.begin() + static_cast<std::ptrdiff_t>(off), payload.end());
    }
    return frame;
}

}  // namespace photon::proto
