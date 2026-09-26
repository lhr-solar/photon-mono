#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace photon::ui {

inline bool unixMsLooksLikeWall(std::uint64_t ms) {
    return ms > 100000000000ull;
}

inline std::uint64_t unixWallMs() {
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    return ms > 0 ? static_cast<std::uint64_t>(ms) : 0;
}

// Live chrome: leftover catalog/replay cursors must not freeze the titlebar.
// If the playhead is missing or more than 2s behind wall, show now.
inline std::uint64_t liveTitlebarClockMs(std::uint64_t cursorMs) {
    const std::uint64_t wall = unixWallMs();
    if (cursorMs == 0 || !unixMsLooksLikeWall(cursorMs)) {
        return wall;
    }
    if (wall > cursorMs && wall - cursorMs > 2000ull) {
        return wall;
    }
    return cursorMs;
}

inline bool localTmFromUnixMs(std::uint64_t ms, std::tm& out) {
    const auto sec = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    if (localtime_s(&tm, &sec) != 0) {
        return false;
    }
#else
    if (localtime_r(&sec, &tm) == nullptr) {
        return false;
    }
#endif
    out = tm;
    return true;
}

inline std::string localYmdFromUnixMs(std::uint64_t ms) {
    std::tm tm{};
    if (!localTmFromUnixMs(ms, tm)) {
        return {};
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

inline bool localDayRangeMs(const std::string& ymd, std::uint64_t& startMs, std::uint64_t& endMs) {
    int y = 0;
    int m = 0;
    int d = 0;
    if (std::sscanf(ymd.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || y < 1970 || m < 1 || m > 12 || d < 1 ||
        d > 31) {
        return false;
    }
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    const auto t0 = std::mktime(&tm);
    if (t0 == static_cast<std::time_t>(-1)) {
        return false;
    }
    tm.tm_mday += 1;
    const auto t1 = std::mktime(&tm);
    if (t1 == static_cast<std::time_t>(-1) || t1 <= t0) {
        return false;
    }
    startMs = static_cast<std::uint64_t>(t0) * 1000ull;
    endMs = static_cast<std::uint64_t>(t1) * 1000ull;
    return true;
}

inline bool sessionOverlapsLocalYmd(std::uint64_t minMs, std::uint64_t maxMs, const std::string& ymd) {
    if (ymd.empty() || maxMs < minMs) {
        return false;
    }
    std::uint64_t day0 = 0;
    std::uint64_t day1 = 0;
    if (!localDayRangeMs(ymd, day0, day1)) {
        return localYmdFromUnixMs(minMs) == ymd || localYmdFromUnixMs(maxMs) == ymd;
    }
    return maxMs > day0 && minMs < day1;
}

inline int displayHour12(int hour) {
    const int h = hour % 12;
    return h == 0 ? 12 : h;
}

enum class ClockParts : std::uint8_t {
    Time = 0,
    TimeMs = 1,
    DateTime = 2,
};

// Local wall clock when `ms` looks like unix milliseconds; otherwise elapsed
// session time. Browser/WASM uses the viewer's timezone.
inline int formatLocalClock(std::uint64_t ms, char* buf, std::size_t n,
                            ClockParts parts = ClockParts::Time) {
    if (buf == nullptr || n == 0) {
        return 0;
    }
    if (ms == 0) {
        return std::snprintf(buf, n, "--:--:--");
    }
    if (!unixMsLooksLikeWall(ms)) {
        const auto sec = ms / 1000;
        const auto msec = static_cast<unsigned>(ms % 1000);
        const auto s = static_cast<unsigned>(sec % 60);
        const auto m = static_cast<unsigned>((sec / 60) % 60);
        const auto h = static_cast<unsigned>(sec / 3600);
        if (parts == ClockParts::TimeMs) {
            return std::snprintf(buf, n, "%u:%02u:%02u.%03u", h, m, s, msec);
        }
        return std::snprintf(buf, n, "%u:%02u:%02u", h, m, s);
    }
    std::tm tm{};
    if (!localTmFromUnixMs(ms, tm)) {
        return std::snprintf(buf, n, "--:--:--");
    }
    const int hour = displayHour12(tm.tm_hour);
    const char* ampm = tm.tm_hour < 12 ? "AM" : "PM";
    const auto msec = static_cast<unsigned>(ms % 1000);
    static constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const int month = tm.tm_mon >= 0 && tm.tm_mon < 12 ? tm.tm_mon : 0;
    if (parts == ClockParts::DateTime) {
        return std::snprintf(buf, n, "%s %d, %d:%02d:%02d %s", kMonths[month], tm.tm_mday, hour,
                             tm.tm_min, tm.tm_sec, ampm);
    }
    if (parts == ClockParts::TimeMs) {
        return std::snprintf(buf, n, "%d:%02d:%02d.%03u %s", hour, tm.tm_min, tm.tm_sec, msec, ampm);
    }
    return std::snprintf(buf, n, "%d:%02d:%02d %s", hour, tm.tm_min, tm.tm_sec, ampm);
}

struct TimelinePartRange {
    int lo = 0;
    int hi = 0;
};

inline TimelinePartRange timelineHourRange(std::uint64_t minMs, std::uint64_t maxMs) {
    TimelinePartRange r{0, 23};
    if (minMs == 0 || maxMs < minMs) {
        return r;
    }
    std::tm a{};
    std::tm b{};
    if (!localTmFromUnixMs(minMs, a) || !localTmFromUnixMs(maxMs, b)) {
        return r;
    }
    if (localYmdFromUnixMs(minMs) != localYmdFromUnixMs(maxMs)) {
        return r;
    }
    r.lo = a.tm_hour;
    r.hi = b.tm_hour;
    return r;
}

inline TimelinePartRange timelineMinuteRange(std::uint64_t minMs, std::uint64_t maxMs, int hour) {
    TimelinePartRange r{0, 59};
    if (minMs == 0 || maxMs < minMs) {
        return r;
    }
    std::tm a{};
    std::tm b{};
    if (!localTmFromUnixMs(minMs, a) || !localTmFromUnixMs(maxMs, b)) {
        return r;
    }
    if (localYmdFromUnixMs(minMs) != localYmdFromUnixMs(maxMs)) {
        return r;
    }
    if (hour < a.tm_hour || hour > b.tm_hour) {
        r.lo = 0;
        r.hi = 0;
        return r;
    }
    if (hour == a.tm_hour) {
        r.lo = a.tm_min;
    }
    if (hour == b.tm_hour) {
        r.hi = b.tm_min;
    }
    if (r.lo > r.hi) {
        r.hi = r.lo;
    }
    return r;
}

inline TimelinePartRange timelineSecondRange(std::uint64_t minMs, std::uint64_t maxMs, int hour, int minute) {
    TimelinePartRange r{0, 59};
    if (minMs == 0 || maxMs < minMs) {
        return r;
    }
    std::tm a{};
    std::tm b{};
    if (!localTmFromUnixMs(minMs, a) || !localTmFromUnixMs(maxMs, b)) {
        return r;
    }
    if (localYmdFromUnixMs(minMs) != localYmdFromUnixMs(maxMs)) {
        return r;
    }
    if (hour < a.tm_hour || hour > b.tm_hour) {
        r.lo = 0;
        r.hi = 0;
        return r;
    }
    if (hour == a.tm_hour && minute < a.tm_min) {
        r.lo = 0;
        r.hi = 0;
        return r;
    }
    if (hour == b.tm_hour && minute > b.tm_min) {
        r.lo = 0;
        r.hi = 0;
        return r;
    }
    if (hour == a.tm_hour && minute == a.tm_min) {
        r.lo = a.tm_sec;
    }
    if (hour == b.tm_hour && minute == b.tm_min) {
        r.hi = b.tm_sec;
    }
    if (r.lo > r.hi) {
        r.hi = r.lo;
    }
    return r;
}

inline int formatLocalClock(double ms, char* buf, std::size_t n, ClockParts parts = ClockParts::Time) {
    if (buf == nullptr || n == 0) {
        return 0;
    }
    if (!(ms >= 0.0)) {
        return std::snprintf(buf, n, "--:--:--");
    }
    return formatLocalClock(static_cast<std::uint64_t>(ms), buf, n, parts);
}

}  // namespace photon::ui
