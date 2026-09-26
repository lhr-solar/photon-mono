#pragma once

#include "photon_ui/tile_cache.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace photon::ui {

inline void capPlotArrays(PlotArrays& a, std::uint32_t maxPoints) {
    if (maxPoints == 0 || a.t.size() <= maxPoints) {
        return;
    }
    if (a.y.size() != a.t.size()) {
        const auto n = std::min(a.t.size(), a.y.size());
        a.t.resize(n);
        a.y.resize(n);
        if (!a.yMin.empty()) {
            a.yMin.resize(std::min(a.yMin.size(), n));
        }
        if (!a.yMax.empty()) {
            a.yMax.resize(std::min(a.yMax.size(), n));
        }
        if (a.t.size() <= maxPoints) {
            return;
        }
    }
    if (!a.envelope) {
        a.yMin = a.y;
        a.yMax = a.y;
        a.envelope = true;
    }
    while (a.t.size() > maxPoints && a.t.size() >= 2) {
        PlotArrays next;
        next.envelope = true;
        next.incomplete = a.incomplete;
        next.t.reserve((a.t.size() + 1) / 2);
        next.y.reserve((a.t.size() + 1) / 2);
        next.yMin.reserve((a.t.size() + 1) / 2);
        next.yMax.reserve((a.t.size() + 1) / 2);
        for (std::size_t i = 0; i < a.t.size(); i += 2) {
            if (i + 1 >= a.t.size()) {
                next.t.push_back(a.t[i]);
                next.y.push_back(a.y[i]);
                next.yMin.push_back(i < a.yMin.size() ? a.yMin[i] : a.y[i]);
                next.yMax.push_back(i < a.yMax.size() ? a.yMax[i] : a.y[i]);
                break;
            }
            const double y0 = a.y[i];
            const double y1 = a.y[i + 1];
            const double mn0 = i < a.yMin.size() ? a.yMin[i] : y0;
            const double mn1 = i + 1 < a.yMin.size() ? a.yMin[i + 1] : y1;
            const double mx0 = i < a.yMax.size() ? a.yMax[i] : y0;
            const double mx1 = i + 1 < a.yMax.size() ? a.yMax[i + 1] : y1;
            next.t.push_back(a.t[i]);
            next.y.push_back((y0 + y1) * 0.5);
            next.yMin.push_back(std::min(mn0, mn1));
            next.yMax.push_back(std::max(mx0, mx1));
        }
        a = std::move(next);
    }
}

inline bool plotArraysAllFinite(const PlotArrays& hist, int n) {
    if (n <= 0 || static_cast<std::size_t>(n) > hist.t.size() || static_cast<std::size_t>(n) > hist.y.size()) {
        return false;
    }
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(hist.t[static_cast<std::size_t>(i)]) ||
            !std::isfinite(hist.y[static_cast<std::size_t>(i)])) {
            return false;
        }
        if (hist.envelope && static_cast<std::size_t>(i) < hist.yMin.size() &&
            static_cast<std::size_t>(i) < hist.yMax.size() &&
            (!std::isfinite(hist.yMin[static_cast<std::size_t>(i)]) ||
             !std::isfinite(hist.yMax[static_cast<std::size_t>(i)]))) {
            return false;
        }
    }
    return true;
}

inline void padPlotAxis(double& lo, double& hi) {
    const double pad = std::max(1.0e-3, (hi - lo) * 0.12);
    lo -= pad;
    hi += pad;
    if (hi <= lo) {
        hi = lo + 1.0;
    }
}

// ImPlotFlags_Equal + a non-square limit box shrinks the drawable plot
// to a sliver. Use a square window around the data.
inline void squareEqualLimits(double& x0, double& x1, double& y0, double& y1) {
    const double cx = 0.5 * (x0 + x1);
    const double cy = 0.5 * (y0 + y1);
    double hx = std::max(std::abs(x1 - cx), 0.5);
    double hy = std::max(std::abs(y1 - cy), 0.5);
    const double h = std::max(hx, hy);
    x0 = cx - h;
    x1 = cx + h;
    y0 = cy - h;
    y1 = cy + h;
}

inline void extendYFromPlot(const PlotArrays& hist, double& lo, double& hi, bool& have) {
    auto consider = [&](double v) {
        if (!std::isfinite(v)) {
            return;
        }
        if (!have) {
            lo = v;
            hi = v;
            have = true;
            return;
        }
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    };
    if (hist.envelope && hist.yMin.size() == hist.t.size() && hist.yMax.size() == hist.t.size()) {
        for (double v : hist.yMin) {
            consider(v);
        }
        for (double v : hist.yMax) {
            consider(v);
        }
        return;
    }
    for (double v : hist.y) {
        consider(v);
    }
}

inline void extendRange(const std::vector<double>& values, double& lo, double& hi, bool& have) {
    for (double v : values) {
        if (!std::isfinite(v)) {
            continue;
        }
        if (!have) {
            lo = v;
            hi = v;
            have = true;
        } else {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
}

inline double clampPlotTimeMs(double t) {
    if (!std::isfinite(t) || t < 0.0) {
        return 0.0;
    }
    return t;
}

inline void containPlotTime(double t, double& lo, double& hi, double minBound = 0.0, double maxBound = 0.0) {
    t = clampPlotTimeMs(t);
    if (maxBound > minBound) {
        t = std::min(maxBound, std::max(minBound, t));
    }
    double span = hi - lo;
    if (!(span > 1.0)) {
        span = 1000.0;
    }
    if (t < lo) {
        lo = t;
        hi = t + span;
    } else if (t > hi) {
        hi = t;
        lo = t - span;
    }
    if (lo < minBound) {
        hi += minBound - lo;
        lo = minBound;
    }
    if (maxBound > minBound && hi > maxBound) {
        lo -= hi - maxBound;
        hi = maxBound;
        if (lo < minBound) {
            lo = minBound;
        }
    }
}

inline bool sanitizePlotWindow(double& lo, double& hi, double minSpanMs = 2000.0,
                               double maxSpanMs = 7200000.0) {
    if (!std::isfinite(lo) || !std::isfinite(hi)) {
        return false;
    }
    if (hi < lo) {
        std::swap(lo, hi);
    }
    double span = hi - lo;
    if (!(span > 1.0)) {
        hi = lo + minSpanMs;
        span = minSpanMs;
    }
    if (span > maxSpanMs) {
        hi = lo + maxSpanMs;
    }
    if (lo < 0.0) {
        hi -= lo;
        lo = 0.0;
    }
    return hi > lo && std::isfinite(lo) && std::isfinite(hi);
}

// Zoom the [lo, hi] window by `factor` (<1 in, >1 out) keeping `focusMs` at the
// same fractional position inside the window (playhead-centered zoom).
inline void zoomTimeWindowAround(double focusMs, double& lo, double& hi, double factor,
                                 double minSpanMs = 2000.0, double maxSpanMs = 7200000.0) {
    focusMs = clampPlotTimeMs(focusMs);
    if (!sanitizePlotWindow(lo, hi, minSpanMs, maxSpanMs)) {
        lo = std::max(0.0, focusMs - minSpanMs * 0.5);
        hi = lo + minSpanMs;
    }
    double span = hi - lo;
    if (!(span > 1.0)) {
        span = minSpanMs;
        lo = std::max(0.0, focusMs - span * 0.5);
        hi = lo + span;
    }
    double u = (focusMs - lo) / span;
    if (!std::isfinite(u)) {
        u = 0.5;
    }
    u = std::max(0.0, std::min(1.0, u));
    double next = span * factor;
    if (!std::isfinite(next)) {
        next = span;
    }
    next = std::max(minSpanMs, std::min(maxSpanMs, next));
    lo = focusMs - next * u;
    hi = focusMs + next * (1.0 - u);
    if (!sanitizePlotWindow(lo, hi, minSpanMs, maxSpanMs)) {
        lo = std::max(0.0, focusMs - minSpanMs * 0.5);
        hi = lo + minSpanMs;
    }
}

inline bool samplePlotArraysAt(const PlotArrays& hist, double tMs, double& y) {
    const std::size_t n = hist.t.size();
    if (n == 0 || hist.y.size() != n || !std::isfinite(tMs)) {
        return false;
    }
    std::size_t first = n;
    std::size_t last = n;
    bool decreasing = false;
    double prevT = 0.0;
    bool havePrev = false;
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(hist.t[i]) || !std::isfinite(hist.y[i])) {
            continue;
        }
        if (first == n) {
            first = i;
        }
        if (havePrev && hist.t[i] < prevT) {
            decreasing = true;
        }
        prevT = hist.t[i];
        havePrev = true;
        last = i;
    }
    if (first == n) {
        return false;
    }
    if (tMs <= hist.t[first]) {
        y = hist.y[first];
        return true;
    }
    if (tMs >= hist.t[last]) {
        y = hist.y[last];
        return true;
    }
    auto lerp = [&](std::size_t a, std::size_t b) -> bool {
        const double t0 = hist.t[a];
        const double t1 = hist.t[b];
        const double y0 = hist.y[a];
        const double y1 = hist.y[b];
        if (!(t1 > t0)) {
            y = y0;
            return std::isfinite(y);
        }
        const double u = (tMs - t0) / (t1 - t0);
        y = y0 + (y1 - y0) * u;
        return std::isfinite(y);
    };
    if (decreasing) {
        std::size_t prev = first;
        std::size_t matchA = n;
        std::size_t matchB = n;
        for (std::size_t i = first + 1; i <= last; ++i) {
            if (!std::isfinite(hist.t[i]) || !std::isfinite(hist.y[i])) {
                continue;
            }
            if (hist.t[i] < hist.t[prev]) {
                prev = i;
                continue;
            }
            if (tMs >= hist.t[prev] && tMs <= hist.t[i]) {
                matchA = prev;
                matchB = i;
            }
            prev = i;
        }
        if (matchA != n) {
            return lerp(matchA, matchB);
        }
        y = hist.y[last];
        return true;
    }
    std::size_t lo = first;
    std::size_t hi = last;
    while (hi - lo > 1) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (!std::isfinite(hist.t[mid])) {
            std::size_t m = mid;
            while (m > lo && !std::isfinite(hist.t[m])) {
                --m;
            }
            if (m == lo || !std::isfinite(hist.t[m])) {
                hi = mid;
                continue;
            }
            if (hist.t[m] <= tMs) {
                lo = m;
            } else {
                hi = m;
            }
            continue;
        }
        if (hist.t[mid] <= tMs) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lerp(lo, hi);
}

}  // namespace photon::ui
