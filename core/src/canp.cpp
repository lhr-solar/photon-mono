#include "photon/canp.hpp"

#include "photon/endian.hpp"

namespace photon::canp {

std::vector<std::uint8_t> encodeBatch(const Batch& batch) {
    std::vector<std::uint8_t> out;
    if (batch.count == 0 || batch.count > kMaxBatch) {
        return out;
    }
    out.reserve(kHeaderSize + static_cast<std::size_t>(batch.count) * kPacketSize);
    writeU32Be(out, kMagic);
    writeU16Be(out, kVersion);
    writeU16Be(out, batch.count);
    writeU32Be(out, batch.seq);
    writeU64Be(out, batch.timestamp);
    for (std::uint16_t i = 0; i < batch.count; ++i) {
        const Packet& p = batch.packets[i];
        writeU32Be(out, p.canId);
        writeU8(out, p.dlc);
        for (std::uint8_t b : p.data) {
            writeU8(out, b);
        }
        for (std::uint16_t d : p.dt) {
            writeU16Be(out, d);
        }
    }
    return out;
}

ParseStatus parseBatch(std::span<const std::uint8_t> bytes, Batch& out, std::size_t& consumed) {
    consumed = 0;
    if (bytes.size() < kHeaderSize) {
        return ParseStatus::Incomplete;
    }
    std::size_t off = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t count = 0;
    std::uint32_t seq = 0;
    std::uint64_t timestamp = 0;
    if (!readU32Be(bytes, off, magic) || !readU16Be(bytes, off, version) || !readU16Be(bytes, off, count) ||
        !readU32Be(bytes, off, seq) || !readU64Be(bytes, off, timestamp)) {
        return ParseStatus::Incomplete;
    }
    if (magic != kMagic) {
        return ParseStatus::BadMagic;
    }
    if (version != kVersion) {
        return ParseStatus::BadVersion;
    }
    if (count == 0 || count > kMaxBatch) {
        return ParseStatus::BadCount;
    }
    const std::size_t need = kHeaderSize + static_cast<std::size_t>(count) * kPacketSize;
    if (bytes.size() < need) {
        return ParseStatus::Incomplete;
    }

    out = Batch{};
    out.seq = seq;
    out.timestamp = timestamp;
    out.count = count;
    for (std::uint16_t i = 0; i < count; ++i) {
        Packet& p = out.packets[i];
        if (!readU32Be(bytes, off, p.canId) || !readU8(bytes, off, p.dlc)) {
            return ParseStatus::Incomplete;
        }
        for (std::uint8_t& b : p.data) {
            if (!readU8(bytes, off, b)) {
                return ParseStatus::Incomplete;
            }
        }
        for (std::uint16_t& d : p.dt) {
            if (!readU16Be(bytes, off, d)) {
                return ParseStatus::Incomplete;
            }
        }
    }
    consumed = need;
    return ParseStatus::Ok;
}

void StreamParser::feed(std::span<const std::uint8_t> bytes) {
    buf_.insert(buf_.end(), bytes.begin(), bytes.end());
}

std::optional<Batch> StreamParser::take() {
    lastError_ = ParseStatus::Incomplete;
    while (buf_.size() >= kHeaderSize) {
        Batch batch;
        std::size_t consumed = 0;
        lastError_ = parseBatch(buf_, batch, consumed);
        if (lastError_ == ParseStatus::Ok) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(consumed));
            return batch;
        }
        if (lastError_ == ParseStatus::Incomplete) {
            return std::nullopt;
        }
        // Resync: drop one byte and look for the next "CAN1" magic.
        buf_.erase(buf_.begin());
    }
    lastError_ = ParseStatus::Incomplete;
    return std::nullopt;
}

void StreamParser::clear() {
    buf_.clear();
    lastError_ = ParseStatus::Incomplete;
}

}  // namespace photon::canp
