#pragma once

#include "photon/endian.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace photon::video {

// photon-server TCP video client protocol (big-endian). See photon-server readme.md.

inline constexpr std::uint8_t kModeLive = 0;
inline constexpr std::uint8_t kModeReplay = 1;
inline constexpr std::uint8_t kFlagKeyframe = 1 << 0;
inline constexpr std::uint8_t kFlagConfig = 1 << 1;
inline constexpr std::size_t kFrameHeaderBytes = 13;
inline constexpr std::size_t kRequestBytes = 9;
inline constexpr std::uint32_t kMaxPayloadBytes = 16u * 1024u * 1024u;

struct ServerFrame {
    std::uint64_t timestampMs = 0;
    std::uint8_t flags = 0;
    std::vector<std::uint8_t> payload;
};

inline bool isKeyframe(std::uint8_t flags) { return (flags & kFlagKeyframe) != 0; }
inline bool isConfig(std::uint8_t flags) { return (flags & kFlagConfig) != 0; }

inline std::vector<std::uint8_t> encodeRequest(std::uint8_t mode, std::uint64_t startTimeMs) {
    std::vector<std::uint8_t> out;
    out.reserve(kRequestBytes);
    writeU8(out, mode);
    writeU64Be(out, startTimeMs);
    return out;
}

inline std::vector<std::uint8_t> encodeServerFrame(const ServerFrame& frame) {
    std::vector<std::uint8_t> out;
    out.reserve(kFrameHeaderBytes + frame.payload.size());
    writeU32Be(out, static_cast<std::uint32_t>(frame.payload.size()));
    writeU64Be(out, frame.timestampMs);
    writeU8(out, frame.flags);
    writeBytes(out, frame.payload);
    return out;
}

// Incremental parser for the 13-byte header + Annex-B NAL payload. Survives split reads.
class StreamParser {
public:
    void feed(std::span<const std::uint8_t> bytes) {
        if (failed_) {
            return;
        }
        buf_.insert(buf_.end(), bytes.begin(), bytes.end());
        pump();
    }

    std::optional<ServerFrame> take() {
        if (ready_.empty()) {
            return std::nullopt;
        }
        auto frame = std::move(ready_.front());
        ready_.erase(ready_.begin());
        return frame;
    }

    void clear() {
        buf_.clear();
        ready_.clear();
        failed_ = false;
    }

    bool failed() const { return failed_; }
    std::size_t buffered() const { return buf_.size(); }

private:
    std::vector<std::uint8_t> buf_;
    std::vector<ServerFrame> ready_;
    bool failed_ = false;

    void pump() {
        while (!failed_) {
            if (buf_.size() < kFrameHeaderBytes) {
                return;
            }
            const std::span<const std::uint8_t> view(buf_);
            std::size_t off = 0;
            std::uint32_t payloadLen = 0;
            std::uint64_t timestampMs = 0;
            std::uint8_t flags = 0;
            if (!readU32Be(view, off, payloadLen) || !readU64Be(view, off, timestampMs) ||
                !readU8(view, off, flags)) {
                failed_ = true;
                buf_.clear();
                return;
            }
            if (payloadLen > kMaxPayloadBytes) {
                failed_ = true;
                buf_.clear();
                return;
            }
            const std::size_t need = kFrameHeaderBytes + static_cast<std::size_t>(payloadLen);
            if (buf_.size() < need) {
                return;
            }
            ServerFrame frame;
            frame.timestampMs = timestampMs;
            frame.flags = flags;
            if (payloadLen > 0) {
                frame.payload.assign(buf_.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes),
                                     buf_.begin() + static_cast<std::ptrdiff_t>(need));
            }
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(need));
            ready_.push_back(std::move(frame));
        }
    }
};

}  // namespace photon::video
