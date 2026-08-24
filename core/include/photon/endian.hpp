#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace photon {

inline void writeU8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }

inline void writeU16Le(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

inline void writeU32Le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 24));
}

inline void writeU64Le(std::vector<std::uint8_t>& out, std::uint64_t v) {
    writeU32Le(out, static_cast<std::uint32_t>(v));
    writeU32Le(out, static_cast<std::uint32_t>(v >> 32));
}

inline void writeF64Le(std::vector<std::uint8_t>& out, double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    writeU64Le(out, bits);
}

inline void writeU16Be(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

inline void writeU32Be(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

inline void writeU64Be(std::vector<std::uint8_t>& out, std::uint64_t v) {
    writeU32Be(out, static_cast<std::uint32_t>(v >> 32));
    writeU32Be(out, static_cast<std::uint32_t>(v));
}

inline bool readU8(std::span<const std::uint8_t> in, std::size_t& off, std::uint8_t& v) {
    if (off >= in.size()) {
        return false;
    }
    v = in[off++];
    return true;
}

inline bool readU16Le(std::span<const std::uint8_t> in, std::size_t& off, std::uint16_t& v) {
    if (off + 2 > in.size()) {
        return false;
    }
    v = static_cast<std::uint16_t>(in[off] | (static_cast<std::uint16_t>(in[off + 1]) << 8));
    off += 2;
    return true;
}

inline bool readU32Le(std::span<const std::uint8_t> in, std::size_t& off, std::uint32_t& v) {
    if (off + 4 > in.size()) {
        return false;
    }
    v = static_cast<std::uint32_t>(in[off]) | (static_cast<std::uint32_t>(in[off + 1]) << 8) |
        (static_cast<std::uint32_t>(in[off + 2]) << 16) | (static_cast<std::uint32_t>(in[off + 3]) << 24);
    off += 4;
    return true;
}

inline bool readU64Le(std::span<const std::uint8_t> in, std::size_t& off, std::uint64_t& v) {
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    if (!readU32Le(in, off, lo) || !readU32Le(in, off, hi)) {
        return false;
    }
    v = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
    return true;
}

inline bool readF64Le(std::span<const std::uint8_t> in, std::size_t& off, double& v) {
    std::uint64_t bits = 0;
    if (!readU64Le(in, off, bits)) {
        return false;
    }
    std::memcpy(&v, &bits, sizeof(v));
    return true;
}

inline bool readU16Be(std::span<const std::uint8_t> in, std::size_t& off, std::uint16_t& v) {
    if (off + 2 > in.size()) {
        return false;
    }
    v = static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[off]) << 8) | in[off + 1]);
    off += 2;
    return true;
}

inline bool readU32Be(std::span<const std::uint8_t> in, std::size_t& off, std::uint32_t& v) {
    if (off + 4 > in.size()) {
        return false;
    }
    v = (static_cast<std::uint32_t>(in[off]) << 24) | (static_cast<std::uint32_t>(in[off + 1]) << 16) |
        (static_cast<std::uint32_t>(in[off + 2]) << 8) | static_cast<std::uint32_t>(in[off + 3]);
    off += 4;
    return true;
}

inline bool readU64Be(std::span<const std::uint8_t> in, std::size_t& off, std::uint64_t& v) {
    std::uint32_t hi = 0;
    std::uint32_t lo = 0;
    if (!readU32Be(in, off, hi) || !readU32Be(in, off, lo)) {
        return false;
    }
    v = (static_cast<std::uint64_t>(hi) << 32) | lo;
    return true;
}

inline void writeBytes(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

}  // namespace photon
