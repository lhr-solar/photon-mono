#include "photon/base64.hpp"

namespace photon::crypto {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeValue(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

}  // namespace

std::string base64Encode(std::span<const std::uint8_t> data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const unsigned n = (static_cast<unsigned>(data[i]) << 16) | (static_cast<unsigned>(data[i + 1]) << 8) |
                           static_cast<unsigned>(data[i + 2]);
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        out.push_back(kAlphabet[(n >> 6) & 63]);
        out.push_back(kAlphabet[n & 63]);
        i += 3;
    }
    if (i < data.size()) {
        unsigned n = static_cast<unsigned>(data[i]) << 16;
        if (i + 1 < data.size()) {
            n |= static_cast<unsigned>(data[i + 1]) << 8;
        }
        out.push_back(kAlphabet[(n >> 18) & 63]);
        out.push_back(kAlphabet[(n >> 12) & 63]);
        if (i + 1 < data.size()) {
            out.push_back(kAlphabet[(n >> 6) & 63]);
        } else {
            out.push_back('=');
        }
        out.push_back('=');
    }
    return out;
}

std::vector<std::uint8_t> base64Decode(std::string_view text) {
    std::vector<std::uint8_t> out;
    int values[4] = {0, 0, 0, 0};
    int filled = 0;
    for (char c : text) {
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') {
            if (c == '=') {
                break;
            }
            continue;
        }
        const int v = decodeValue(c);
        if (v < 0) {
            return {};
        }
        values[filled++] = v;
        if (filled == 4) {
            const unsigned n = (static_cast<unsigned>(values[0]) << 18) | (static_cast<unsigned>(values[1]) << 12) |
                               (static_cast<unsigned>(values[2]) << 6) | static_cast<unsigned>(values[3]);
            out.push_back(static_cast<std::uint8_t>(n >> 16));
            out.push_back(static_cast<std::uint8_t>(n >> 8));
            out.push_back(static_cast<std::uint8_t>(n));
            filled = 0;
        }
    }
    if (filled == 2) {
        const unsigned n = (static_cast<unsigned>(values[0]) << 18) | (static_cast<unsigned>(values[1]) << 12);
        out.push_back(static_cast<std::uint8_t>(n >> 16));
    } else if (filled == 3) {
        const unsigned n = (static_cast<unsigned>(values[0]) << 18) | (static_cast<unsigned>(values[1]) << 12) |
                           (static_cast<unsigned>(values[2]) << 6);
        out.push_back(static_cast<std::uint8_t>(n >> 16));
        out.push_back(static_cast<std::uint8_t>(n >> 8));
    }
    return out;
}

}  // namespace photon::crypto
