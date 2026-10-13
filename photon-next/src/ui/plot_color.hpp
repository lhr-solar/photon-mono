#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace photon::ui {

inline int plotHexNibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Parses #RGB, #RGBA, #RRGGBB, #RRGGBBAA (leading # optional). out is r,g,b,a in 0..1.
inline bool parseHexColor(std::string_view text, float out[4]) {
    if (out == nullptr || text.empty()) {
        return false;
    }
    if (text.front() == '#') {
        text.remove_prefix(1);
    }
    auto byteAt = [&](std::size_t i) -> int {
        if (i + 1 >= text.size()) {
            return -1;
        }
        const int hi = plotHexNibble(text[i]);
        const int lo = plotHexNibble(text[i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        return (hi << 4) | lo;
    };
    if (text.size() == 3 || text.size() == 4) {
        const int r = plotHexNibble(text[0]);
        const int g = plotHexNibble(text[1]);
        const int b = plotHexNibble(text[2]);
        const int a = text.size() == 4 ? plotHexNibble(text[3]) : 15;
        if (r < 0 || g < 0 || b < 0 || a < 0) {
            return false;
        }
        out[0] = static_cast<float>((r << 4) | r) / 255.0f;
        out[1] = static_cast<float>((g << 4) | g) / 255.0f;
        out[2] = static_cast<float>((b << 4) | b) / 255.0f;
        out[3] = static_cast<float>((a << 4) | a) / 255.0f;
        return true;
    }
    if (text.size() == 6 || text.size() == 8) {
        const int r = byteAt(0);
        const int g = byteAt(2);
        const int b = byteAt(4);
        const int a = text.size() == 8 ? byteAt(6) : 255;
        if (r < 0 || g < 0 || b < 0 || a < 0) {
            return false;
        }
        out[0] = static_cast<float>(r) / 255.0f;
        out[1] = static_cast<float>(g) / 255.0f;
        out[2] = static_cast<float>(b) / 255.0f;
        out[3] = static_cast<float>(a) / 255.0f;
        return true;
    }
    return false;
}

inline int plotClampByte(float v) {
    const int n = static_cast<int>(v * 255.0f + 0.5f);
    return n < 0 ? 0 : (n > 255 ? 255 : n);
}

inline std::string formatHexColor(float r, float g, float b, float a = 1.0f) {
    char buf[16];
    const int ri = plotClampByte(r);
    const int gi = plotClampByte(g);
    const int bi = plotClampByte(b);
    const int ai = plotClampByte(a);
    if (ai < 255) {
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", ri, gi, bi, ai);
    } else {
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", ri, gi, bi);
    }
    return buf;
}

}  // namespace photon::ui
