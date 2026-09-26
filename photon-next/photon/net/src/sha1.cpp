#include "photon/sha1.hpp"

#include <cstring>

namespace photon::crypto {
namespace {

std::uint32_t rol(std::uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32u - bits));
}

void processBlock(std::uint32_t h[5], const std::uint8_t block[64]) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    std::uint32_t a = h[0];
    std::uint32_t b = h[1];
    std::uint32_t c = h[2];
    std::uint32_t d = h[3];
    std::uint32_t e = h[4];

    for (int i = 0; i < 80; ++i) {
        std::uint32_t f = 0;
        std::uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const std::uint32_t temp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = temp;
    }

    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

}  // namespace

std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> data) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};

    std::uint8_t block[64];
    std::size_t remaining = data.size();
    std::size_t offset = 0;
    while (remaining >= 64) {
        std::memcpy(block, data.data() + offset, 64);
        processBlock(h, block);
        offset += 64;
        remaining -= 64;
    }

    std::memset(block, 0, sizeof(block));
    if (remaining > 0) {
        std::memcpy(block, data.data() + offset, remaining);
    }
    block[remaining] = 0x80;

    const std::uint64_t bitLen = static_cast<std::uint64_t>(data.size()) * 8u;
    if (remaining >= 56) {
        processBlock(h, block);
        std::memset(block, 0, sizeof(block));
    }
    for (int i = 0; i < 8; ++i) {
        block[63 - i] = static_cast<std::uint8_t>(bitLen >> (8 * i));
    }
    processBlock(h, block);

    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i) {
        out[static_cast<std::size_t>(i) * 4] = static_cast<std::uint8_t>(h[i] >> 24);
        out[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
        out[static_cast<std::size_t>(i) * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
        out[static_cast<std::size_t>(i) * 4 + 3] = static_cast<std::uint8_t>(h[i]);
    }
    return out;
}

std::array<std::uint8_t, 20> sha1(std::string_view data) {
    return sha1(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
}

}  // namespace photon::crypto
