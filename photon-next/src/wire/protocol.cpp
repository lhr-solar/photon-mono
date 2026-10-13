#include "wire/protocol.hpp"

#include "base/endian.hpp"

#include <algorithm>
#include <initializer_list>
#include <unordered_map>

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

std::vector<CanMonitorItem> latestCanPerId(std::span<const CanMonitorItem> items, std::size_t cap) {
    if (items.empty() || cap == 0) {
        return {};
    }
    std::vector<CanMonitorItem> latest;
    latest.reserve(std::min(items.size(), cap));
    std::unordered_map<std::uint32_t, std::size_t> at;
    at.reserve(std::min(items.size(), cap));
    for (const auto& item : items) {
        const auto found = at.find(item.canId);
        if (found == at.end()) {
            at.emplace(item.canId, latest.size());
            latest.push_back(item);
        } else {
            latest[found->second] = item;
        }
    }
    if (latest.size() > cap) {
        latest.erase(latest.begin(),
                     latest.begin() + static_cast<std::ptrdiff_t>(latest.size() - cap));
    }
    return latest;
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

namespace {

std::size_t fieldCount(const HistoryChunk& chunk) {
    return std::max({chunk.minV.size(), chunk.maxV.size(), chunk.meanV.size(), chunk.firstV.size(),
                     chunk.lastV.size(), chunk.countV.size(), chunk.rawV.size(), chunk.timestamps.size()});
}

void writeF32Array(std::vector<std::uint8_t>& out, std::span<const float> values, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        writeF32Le(out, i < values.size() ? values[i] : 0.0f);
    }
}

bool readF32Array(std::span<const std::uint8_t> in, std::size_t& off, std::size_t n, std::vector<float>& out) {
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!readF32Le(in, off, out[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<std::uint8_t> encodeHistoryChunk(const HistoryChunk& chunk) {
    HistoryChunk copy = chunk;
    if (copy.valueCount == 0) {
        copy.valueCount = static_cast<std::uint32_t>(fieldCount(copy));
    }
    if (copy.slotCount == 0) {
        copy.slotCount = copy.valueCount;
    }
    if (copy.bucketMs == 0) {
        copy.flags = static_cast<std::uint16_t>(copy.flags | kHistDense);
        copy.slotCount = copy.valueCount;
    }
    const bool dense = (copy.flags & kHistDense) != 0;
    std::vector<std::uint8_t> out;
    out.reserve(kHistoryChunkHeaderSize + copy.valueCount * 16 + (copy.slotCount + 7) / 8);
    writeU32Le(out, copy.queryId);
    writeU32Le(out, copy.signalId);
    writeU8(out, copy.level);
    writeU8(out, copy.fields);
    writeU16Le(out, copy.flags);
    writeU32Le(out, copy.chunkSeq);
    writeU64Le(out, copy.baseMs);
    writeU32Le(out, copy.bucketMs);
    writeU32Le(out, copy.slotCount);
    writeU32Le(out, copy.valueCount);
    if (copy.bucketMs == 0) {
        for (std::size_t i = 0; i < copy.valueCount; ++i) {
            writeF64Le(out, i < copy.timestamps.size() ? copy.timestamps[i] : 0.0);
        }
    } else if (!dense) {
        const std::size_t bytes = (static_cast<std::size_t>(copy.slotCount) + 7) / 8;
        if (copy.presence.size() >= bytes) {
            writeBytes(out, std::span<const std::uint8_t>(copy.presence.data(), bytes));
        } else {
            std::vector<std::uint8_t> bits(bytes, 0);
            writeBytes(out, bits);
        }
    }
    if ((copy.fields & kHistMin) != 0) {
        writeF32Array(out, copy.minV, copy.valueCount);
    }
    if ((copy.fields & kHistMax) != 0) {
        writeF32Array(out, copy.maxV, copy.valueCount);
    }
    if ((copy.fields & kHistMean) != 0) {
        writeF32Array(out, copy.meanV, copy.valueCount);
    }
    if ((copy.fields & kHistFirst) != 0) {
        writeF32Array(out, copy.firstV, copy.valueCount);
    }
    if ((copy.fields & kHistLast) != 0) {
        writeF32Array(out, copy.lastV, copy.valueCount);
    }
    if ((copy.fields & kHistCount) != 0) {
        writeF32Array(out, copy.countV, copy.valueCount);
    }
    if ((copy.fields & kHistRaw) != 0) {
        writeF32Array(out, copy.rawV, copy.valueCount);
    }
    return out;
}

std::optional<HistoryChunk> decodeHistoryChunk(std::span<const std::uint8_t> payload) {
    if (payload.size() < kHistoryChunkHeaderSize) {
        return std::nullopt;
    }
    HistoryChunk chunk;
    std::size_t off = 0;
    if (!readU32Le(payload, off, chunk.queryId) || !readU32Le(payload, off, chunk.signalId) ||
        !readU8(payload, off, chunk.level) || !readU8(payload, off, chunk.fields) ||
        !readU16Le(payload, off, chunk.flags) || !readU32Le(payload, off, chunk.chunkSeq) ||
        !readU64Le(payload, off, chunk.baseMs) || !readU32Le(payload, off, chunk.bucketMs) ||
        !readU32Le(payload, off, chunk.slotCount) || !readU32Le(payload, off, chunk.valueCount)) {
        return std::nullopt;
    }
    const bool dense = (chunk.flags & kHistDense) != 0;
    if (chunk.bucketMs == 0) {
        chunk.timestamps.resize(chunk.valueCount);
        for (std::size_t i = 0; i < chunk.valueCount; ++i) {
            if (!readF64Le(payload, off, chunk.timestamps[i])) {
                return std::nullopt;
            }
        }
    } else if (!dense) {
        const std::size_t bytes = (static_cast<std::size_t>(chunk.slotCount) + 7) / 8;
        if (off + bytes > payload.size()) {
            return std::nullopt;
        }
        chunk.presence.assign(payload.begin() + static_cast<std::ptrdiff_t>(off),
                              payload.begin() + static_cast<std::ptrdiff_t>(off + bytes));
        off += bytes;
    }
    if ((chunk.fields & kHistMin) != 0 && !readF32Array(payload, off, chunk.valueCount, chunk.minV)) {
        return std::nullopt;
    }
    if ((chunk.fields & kHistMax) != 0 && !readF32Array(payload, off, chunk.valueCount, chunk.maxV)) {
        return std::nullopt;
    }
    if ((chunk.fields & kHistMean) != 0 && !readF32Array(payload, off, chunk.valueCount, chunk.meanV)) {
        return std::nullopt;
    }
    if ((chunk.fields & kHistFirst) != 0 && !readF32Array(payload, off, chunk.valueCount, chunk.firstV)) {
        return std::nullopt;
    }
    if ((chunk.fields & kHistLast) != 0 && !readF32Array(payload, off, chunk.valueCount, chunk.lastV)) {
        return std::nullopt;
    }
    if ((chunk.fields & kHistCount) != 0 && !readF32Array(payload, off, chunk.valueCount, chunk.countV)) {
        return std::nullopt;
    }
    if ((chunk.fields & kHistRaw) != 0 && !readF32Array(payload, off, chunk.valueCount, chunk.rawV)) {
        return std::nullopt;
    }
    return chunk;
}

}  // namespace photon::proto
