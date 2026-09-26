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

inline int startCodeLen(std::span<const std::uint8_t> p) {
    if (p.size() >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
        return 4;
    }
    if (p.size() >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) {
        return 3;
    }
    return 0;
}

inline int annexBNalType(std::span<const std::uint8_t> nal) {
    const int i = startCodeLen(nal);
    return i < static_cast<int>(nal.size()) ? (nal[static_cast<std::size_t>(i)] & 0x1f) : 0;
}

inline std::uint8_t flagsForNalType(int type) {
    if (type == 7 || type == 8) {
        return kFlagConfig;
    }
    if (type == 5) {
        return kFlagKeyframe;
    }
    return 0;
}

// Today's catalog GOP starts with a 192x144 still plus 9-byte P stubs.
// WebCodecs treats those stubs as delta frames and dies; skip them.
inline constexpr std::size_t kMinPsliceBytes = 24;

inline bool nalIsTinyPslice(std::span<const std::uint8_t> nal) {
    return annexBNalType(nal) == 1 && nal.size() < kMinPsliceBytes;
}

// SPS coded size (same walk as native_video). Used to drop the thumb GOP
// when a 720p GOP is in the same locate.
inline bool parseSpsSize(std::span<const std::uint8_t> nal, int& w, int& h) {
    const int sc = startCodeLen(nal);
    std::size_t i = static_cast<std::size_t>(sc) + 1;
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(nal.size());
    while (i < nal.size()) {
        if (i + 2 < nal.size() && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3) {
            rbsp.push_back(0);
            rbsp.push_back(0);
            i += 3;
            continue;
        }
        rbsp.push_back(nal[i]);
        ++i;
    }
    if (rbsp.size() < 4) {
        return false;
    }
    std::size_t bit = 0;
    auto getBit = [&]() -> int {
        const std::size_t idx = bit >> 3;
        if (idx >= rbsp.size()) {
            return 0;
        }
        const int v = (rbsp[idx] >> (7 - static_cast<int>(bit & 7))) & 1;
        ++bit;
        return v;
    };
    auto u = [&](int n) -> std::uint32_t {
        std::uint32_t v = 0;
        for (int k = 0; k < n; ++k) {
            v = (v << 1) | static_cast<std::uint32_t>(getBit());
        }
        return v;
    };
    auto ue = [&]() -> std::uint32_t {
        int z = 0;
        while (getBit() == 0) {
            ++z;
            if (z > 31) {
                return 0;
            }
        }
        return ((1u << z) - 1u) + u(z);
    };
    const std::uint32_t profile = u(8);
    u(8);
    u(8);
    ue();
    const bool high = profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 ||
                      profile == 83 || profile == 86 || profile == 118 || profile == 128 || profile == 138;
    if (high) {
        const std::uint32_t chroma = ue();
        if (chroma == 3) {
            u(1);
        }
        ue();
        ue();
        u(1);
        if (u(1)) {
            return false;
        }
    }
    ue();
    const std::uint32_t poc = ue();
    if (poc == 0) {
        ue();
    } else if (poc == 1) {
        u(1);
        ue();
        ue();
        const std::uint32_t n = ue();
        for (std::uint32_t k = 0; k < n; ++k) {
            ue();
        }
    }
    ue();
    u(1);
    const std::uint32_t wMbs = ue();
    const std::uint32_t hMap = ue();
    const std::uint32_t frameOnly = u(1);
    w = static_cast<int>((wMbs + 1) * 16);
    h = static_cast<int>((hMap + 1) * 16 * (2 - static_cast<int>(frameOnly)));
    return w > 0 && h > 0 && w <= 4096 && h <= 4096;
}

// first_mb_in_slice. A new picture starts at 0; later slices of the same
// picture are > 0. Same bit reader as the WebCodecs path.
inline int firstMbInSlice(std::span<const std::uint8_t> nal) {
    const int sc = startCodeLen(nal);
    const std::size_t i = static_cast<std::size_t>(sc) + 1;
    if (i >= nal.size()) {
        return 0;
    }
    int bit = 0;
    auto getBit = [&]() -> int {
        const std::size_t idx = i + static_cast<std::size_t>(bit >> 3);
        if (idx >= nal.size()) {
            return 0;
        }
        const int v = (nal[idx] >> (7 - (bit & 7))) & 1;
        ++bit;
        return v;
    };
    int zeros = 0;
    while (zeros < 24 && getBit() == 0) {
        ++zeros;
    }
    if (zeros >= 24) {
        return 0;
    }
    int val = 1;
    for (int n = 0; n < zeros; ++n) {
        val = (val << 1) | getBit();
    }
    return val - 1;
}

inline bool nalStartsPicture(std::span<const std::uint8_t> nal) {
    const int t = annexBNalType(nal);
    if (t != 1 && t != 5) {
        return false;
    }
    return firstMbInSlice(nal) == 0;
}

// Spread pictures across [startMs, endMs]. A GOP with start==end (or one
// timestamp on every NAL) used to decode as a single still.
inline std::uint64_t gopPictureMs(std::uint64_t startMs, std::uint64_t endMs, std::size_t index,
                                 std::size_t count) {
    if (count <= 1 || index == 0) {
        return startMs;
    }
    const std::uint64_t span = endMs > startMs ? (endMs - startMs) : (count - 1) * 33ull;
    if (index >= count) {
        index = count - 1;
    }
    return startMs + (span * index) / (count - 1);
}

template <typename Fn>
void forEachStampedGopNal(std::span<const std::uint8_t> gop, std::uint64_t startMs, std::uint64_t endMs,
                          Fn&& fn) {
    struct Item {
        int type = 0;
        std::span<const std::uint8_t> nal;
    };
    std::vector<Item> items;
    forEachAnnexB(gop, [&](std::span<const std::uint8_t> nal) {
        const int t = annexBNalType(nal);
        if (t != 1 && t != 5 && t != 7 && t != 8) {
            return;
        }
        items.push_back(Item{t, nal});
    });
    std::size_t pictures = 0;
    for (const Item& it : items) {
        if ((it.type == 1 || it.type == 5) && firstMbInSlice(it.nal) == 0) {
            ++pictures;
        }
    }
    if (pictures == 0) {
        for (const Item& it : items) {
            if (it.type == 1 || it.type == 5) {
                ++pictures;
            }
        }
    }
    std::size_t pic = 0;
    bool havePic = false;
    std::uint64_t cur = startMs;
    for (const Item& it : items) {
        if (it.type == 1 || it.type == 5) {
            const bool newPic = !havePic || firstMbInSlice(it.nal) == 0;
            if (newPic) {
                cur = gopPictureMs(startMs, endMs, pic, pictures == 0 ? 1 : pictures);
                ++pic;
                havePic = true;
            }
        }
        fn(cur, flagsForNalType(it.type), it.nal);
    }
}

// Walk Annex-B start codes. A buffer with no start code is one NAL.
template <typename Fn>
void forEachAnnexB(std::span<const std::uint8_t> bytes, Fn&& fn) {
    if (bytes.empty()) {
        return;
    }
    if (startCodeLen(bytes) == 0) {
        fn(bytes);
        return;
    }
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        const int sc = startCodeLen(bytes.subspan(pos));
        if (sc == 0) {
            break;
        }
        std::size_t next = bytes.size();
        for (std::size_t i = pos + static_cast<std::size_t>(sc); i < bytes.size(); ++i) {
            if (startCodeLen(bytes.subspan(i)) != 0) {
                next = i;
                break;
            }
        }
        fn(bytes.subspan(pos, next - pos));
        pos = next;
    }
}

inline int gopSpsWidth(std::span<const std::uint8_t> gop) {
    int w = 0;
    int h = 0;
    forEachAnnexB(gop, [&](std::span<const std::uint8_t> nal) {
        if (w > 0 || annexBNalType(nal) != 7) {
            return;
        }
        parseSpsSize(nal, w, h);
    });
    return w;
}

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
