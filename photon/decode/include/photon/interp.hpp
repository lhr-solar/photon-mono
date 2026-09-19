#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace photon::interp {

// How far the playhead sits behind the newest GPS sample. That lookahead is the
// client buffer: as long as another point is already in the ring we interpolate
// instead of waiting for the next WebSocket packet.
inline constexpr double kGpsDelayMs = 200.0;
inline constexpr double kGpsCatchupMs = 500.0;
inline constexpr double kGpsSeekJumpMs = 2500.0;
inline constexpr double kGpsHoldEpsMs = 0.5;

inline std::size_t ringIndex(std::size_t logical, std::size_t count, std::size_t cap, std::size_t write) {
    if (count == 0) {
        return 0;
    }
    if (logical >= count) {
        logical = count - 1;
    }
    if (count < cap) {
        return logical;
    }
    return (write + logical) % cap;
}

inline bool interpolate(const double* t, const double* v, std::size_t count, std::size_t cap, std::size_t write,
                        double tQuery, double& out) {
    if (t == nullptr || v == nullptr || count == 0) {
        return false;
    }
    auto at = [&](std::size_t i) { return ringIndex(i, count, cap, write); };
    if (count == 1) {
        out = v[at(0)];
        return true;
    }
    const double t0 = t[at(0)];
    const double t1 = t[at(count - 1)];
    if (!(t1 >= t0)) {
        out = v[at(count - 1)];
        return true;
    }
    if (tQuery <= t0) {
        out = v[at(0)];
        return true;
    }
    if (tQuery >= t1) {
        out = v[at(count - 1)];
        return true;
    }
    std::size_t lo = 0;
    std::size_t hi = count - 1;
    while (lo + 1 < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (t[at(mid)] <= tQuery) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const double ta = t[at(lo)];
    const double tb = t[at(hi)];
    const double ua = v[at(lo)];
    const double ub = v[at(hi)];
    if (!(tb > ta)) {
        out = ub;
        return true;
    }
    const double u = (tQuery - ta) / (tb - ta);
    out = ua + (ub - ua) * u;
    return true;
}

inline bool interpolateLinear(const double* t, const double* v, std::size_t count, double tQuery, double& out) {
    return interpolate(t, v, count, count, 0, tQuery, out);
}

inline double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double p1 = lat1 * 0.017453292519943295;
    const double p2 = lat2 * 0.017453292519943295;
    const double dL = (lon2 - lon1) * 0.017453292519943295;
    const double y = std::sin(dL) * std::cos(p2);
    const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dL);
    double deg = std::atan2(y, x) * 57.29577951308232;
    if (deg < 0.0) {
        deg += 360.0;
    }
    return deg;
}

struct GpsFix {
    double lat = 0.0;
    double lon = 0.0;
    double headingDeg = 0.0;
    double playMs = 0.0;
    bool ok = false;
};

struct GpsPlayback {
    double playMs = 0.0;
    bool havePlay = false;
    double lastLat = 0.0;
    double lastLon = 0.0;
    double lastHdg = 0.0;
    bool havePose = false;

    void reset() { *this = GpsPlayback{}; }

    // Advance the playhead. `playing` follows Play/Live; Pause/seek snaps to `cursorMs`.
    // Starts as soon as two samples exist — no fill wait. Stays ~kGpsDelayMs behind the
    // newest sample so the ring always has a future point to lerp toward.
    void advance(double newestMs, double oldestMs, double dtMs, bool playing, double cursorMs,
                 bool haveCursor) {
        if (newestMs < oldestMs) {
            newestMs = oldestMs;
        }
        const double span = newestMs - oldestMs;
        const double delay = std::min(kGpsDelayMs, std::max(0.0, span));
        const double target = newestMs - delay;

        if (haveCursor && !playing) {
            playMs = cursorMs;
            havePlay = true;
            if (playMs < oldestMs) {
                playMs = oldestMs;
            }
            if (playMs > newestMs) {
                playMs = newestMs;
            }
            return;
        }

        if (!havePlay) {
            playMs = target;
            havePlay = true;
            return;
        }

        if (haveCursor && std::fabs(cursorMs - playMs) > kGpsSeekJumpMs) {
            playMs = target;
            return;
        }

        if (playing && dtMs > 0.0) {
            playMs += dtMs;
        }
        if (playMs > newestMs) {
            playMs = newestMs;
        }
        if (playMs < oldestMs) {
            playMs = oldestMs;
        }
        if (playMs < newestMs - kGpsCatchupMs) {
            playMs = target;
        }
    }

    GpsFix sample(const double* latT, const double* latV, std::size_t latN, std::size_t latCap,
                  std::size_t latWrite, const double* lonT, const double* lonV, std::size_t lonN,
                  std::size_t lonCap, std::size_t lonWrite) {
        GpsFix fix{};
        double lat = 0.0;
        double lon = 0.0;
        if (!interpolate(latT, latV, latN, latCap, latWrite, playMs, lat) ||
            !interpolate(lonT, lonV, lonN, lonCap, lonWrite, playMs, lon)) {
            if (havePose) {
                fix.lat = lastLat;
                fix.lon = lastLon;
                fix.headingDeg = lastHdg;
                fix.playMs = playMs;
                fix.ok = true;
            }
            return fix;
        }
        if (std::fabs(lat) > 90.0 || std::fabs(lon) > 180.0 || (lat == 0.0 && lon == 0.0)) {
            return fix;
        }
        double heading = havePose ? lastHdg : 0.0;
        double latB = lat;
        double lonB = lon;
        if (interpolate(latT, latV, latN, latCap, latWrite, playMs + 20.0, latB) &&
            interpolate(lonT, lonV, lonN, lonCap, lonWrite, playMs + 20.0, lonB)) {
            const double dlat = latB - lat;
            const double dlon = lonB - lon;
            if (dlat * dlat + dlon * dlon > 1.0e-16) {
                heading = bearingDeg(lat, lon, latB, lonB);
            }
        } else if (havePose) {
            const double dlat = lat - lastLat;
            const double dlon = lon - lastLon;
            if (dlat * dlat + dlon * dlon > 1.0e-16) {
                heading = bearingDeg(lastLat, lastLon, lat, lon);
            }
        }
        lastLat = lat;
        lastLon = lon;
        lastHdg = heading;
        havePose = true;
        fix.lat = lat;
        fix.lon = lon;
        fix.headingDeg = heading;
        fix.playMs = playMs;
        fix.ok = true;
        return fix;
    }
};

}  // namespace photon::interp
