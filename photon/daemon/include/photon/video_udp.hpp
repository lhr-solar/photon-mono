#pragma once

#include "photon/client_auth.hpp"
#include "photon/endian.hpp"
#include "photon/video_framing.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace photon::video {

inline constexpr std::uint32_t kUdpMagic = 0x50565531u;  // PVU1
inline constexpr std::size_t kUdpHeaderBytes = 22;
inline constexpr std::size_t kUdpMaxPayload = 1200;
inline constexpr std::uint8_t kUdpSubscribe = 1;
inline constexpr std::uint8_t kUdpUnsubscribe = 0;

struct UdpPacket {
    std::uint32_t frameSeq = 0;
    std::uint16_t fragIndex = 0;
    std::uint16_t fragCount = 0;
    std::uint64_t timestampMs = 0;
    std::uint8_t flags = 0;
    std::vector<std::uint8_t> payload;
};

inline std::vector<std::uint8_t> encodeUdpSubscribe(std::string_view password, bool subscribe) {
    std::vector<std::uint8_t> out;
    if (!auth::encode(password, out)) {
        return {};
    }
    out.push_back(subscribe ? kUdpSubscribe : kUdpUnsubscribe);
    return out;
}

inline bool decodeUdpPacket(std::span<const std::uint8_t> bytes, UdpPacket& out) {
    if (bytes.size() < kUdpHeaderBytes) {
        return false;
    }
    std::size_t off = 0;
    std::uint32_t magic = 0;
    if (!readU32Be(bytes, off, magic) || magic != kUdpMagic) {
        return false;
    }
    if (!readU32Be(bytes, off, out.frameSeq) || !readU16Be(bytes, off, out.fragIndex) ||
        !readU16Be(bytes, off, out.fragCount) || !readU64Be(bytes, off, out.timestampMs) ||
        !readU8(bytes, off, out.flags)) {
        return false;
    }
    std::uint8_t reserved = 0;
    if (!readU8(bytes, off, reserved)) {
        return false;
    }
    if (out.fragCount == 0 || out.fragIndex >= out.fragCount) {
        return false;
    }
    out.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kUdpHeaderBytes), bytes.end());
    return true;
}

class UdpAssembler {
public:
    std::optional<ServerFrame> add(const UdpPacket& p) {
        if (parts_.empty() || seq_ != p.frameSeq || count_ != p.fragCount) {
            seq_ = p.frameSeq;
            count_ = p.fragCount;
            ts_ = p.timestampMs;
            flags_ = p.flags;
            parts_.assign(p.fragCount, {});
            have_.assign(p.fragCount, 0);
            got_ = 0;
        }
        if (p.fragIndex >= have_.size() || have_[p.fragIndex] != 0) {
            return std::nullopt;
        }
        have_[p.fragIndex] = 1;
        parts_[p.fragIndex] = p.payload;
        ++got_;
        if (got_ < count_) {
            return std::nullopt;
        }
        ServerFrame frame;
        frame.timestampMs = ts_;
        frame.flags = flags_;
        for (const auto& part : parts_) {
            frame.payload.insert(frame.payload.end(), part.begin(), part.end());
        }
        parts_.clear();
        have_.clear();
        return frame;
    }

    void clear() {
        parts_.clear();
        have_.clear();
        got_ = 0;
    }

private:
    std::uint32_t seq_ = 0;
    std::uint16_t count_ = 0;
    std::uint16_t got_ = 0;
    std::uint64_t ts_ = 0;
    std::uint8_t flags_ = 0;
    std::vector<std::vector<std::uint8_t>> parts_;
    std::vector<std::uint8_t> have_;
};

}  // namespace photon::video
