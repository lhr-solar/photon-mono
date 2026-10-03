#pragma once

#include "photon_ui/tile_cache.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

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

// Axis range from the line that is drawn. A few spikes, and the bucket
// min/max around a one-second point, must not stretch the scale.
inline void extendYFromLine(const std::vector<double>& y, double& lo, double& hi, bool& have) {
    std::vector<double> values;
    values.reserve(y.size());
    for (double v : y) {
        if (std::isfinite(v)) {
            values.push_back(v);
        }
    }
    if (values.empty()) {
        return;
    }
    auto consider = [&](double v) {
        if (!have) {
            lo = v;
            hi = v;
            have = true;
            return;
        }
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    };
    if (values.size() < 8) {
        for (double v : values) {
            consider(v);
        }
        return;
    }
    std::sort(values.begin(), values.end());
    const std::size_t skip = std::max<std::size_t>(1, values.size() / 20);
    consider(values[skip]);
    consider(values[values.size() - 1 - skip]);
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

// Keep `span` and put `t` in the middle. Bounds only slide the window, they do not shrink it
// unless the data range itself is shorter than the span.
inline void centerPlotWindow(double t, double span, double& lo, double& hi, double minBound = 0.0,
                             double maxBound = 0.0) {
    t = clampPlotTimeMs(t);
    if (!(span > 1.0)) {
        span = 1000.0;
    }
    if (maxBound > minBound) {
        t = std::min(maxBound, std::max(minBound, t));
        const double boundSpan = maxBound - minBound;
        if (span > boundSpan) {
            span = boundSpan;
        }
    }
    lo = t - span * 0.5;
    hi = lo + span;
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

// Slide a window by `deltaMs` without changing its span.
inline void shiftPlotWindow(double& lo, double& hi, double deltaMs, double minBound = 0.0,
                            double maxBound = 0.0) {
    double span = hi - lo;
    if (!(span > 1.0)) {
        span = 1000.0;
    }
    if (maxBound > minBound && span > maxBound - minBound) {
        span = maxBound - minBound;
    }
    lo += deltaMs;
    hi = lo + span;
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

inline constexpr float kPlotWindowMinSec = 2.0f;
inline constexpr float kPlotWindowMaxSec = 86400.0f;
inline constexpr double kPlotWindowMinMs = 2000.0;
inline constexpr double kPlotWindowMaxMs = 86400000.0;

struct PlotTimePreset {
    const char* label;
    float sec;
};

// Grafana-style relative ranges. The live edge keeps moving; these only set the span.
inline constexpr PlotTimePreset kPlotTimePresets[] = {
    {"15s", 15.0f}, {"1m", 60.0f},   {"5m", 300.0f},   {"15m", 900.0f},
    {"1h", 3600.0f}, {"3h", 10800.0f}, {"6h", 21600.0f}, {"24h", 86400.0f},
};

inline float clampPlotWindowSec(float sec) {
    if (!std::isfinite(sec) || sec < kPlotWindowMinSec) {
        return kPlotWindowMinSec;
    }
    if (sec > kPlotWindowMaxSec) {
        return kPlotWindowMaxSec;
    }
    return sec;
}

inline int matchPlotTimePreset(float sec) {
    if (!std::isfinite(sec)) {
        return -1;
    }
    const int n = static_cast<int>(sizeof(kPlotTimePresets) / sizeof(kPlotTimePresets[0]));
    for (int i = 0; i < n; ++i) {
        const float tol = std::max(0.5f, kPlotTimePresets[i].sec * 0.02f);
        if (std::fabs(sec - kPlotTimePresets[i].sec) <= tol) {
            return i;
        }
    }
    return -1;
}

inline void formatPlotWindowLabel(float sec, char* buf, int size) {
    if (buf == nullptr || size <= 0) {
        return;
    }
    sec = clampPlotWindowSec(sec);
    if (sec >= 3600.0f - 0.5f) {
        const float hours = sec / 3600.0f;
        if (std::fabs(hours - std::round(hours)) < 0.05f) {
            std::snprintf(buf, static_cast<std::size_t>(size), "%.0fh", hours);
        } else {
            std::snprintf(buf, static_cast<std::size_t>(size), "%.1fh", hours);
        }
        return;
    }
    if (sec >= 60.0f - 0.5f) {
        const float mins = sec / 60.0f;
        if (std::fabs(mins - std::round(mins)) < 0.05f) {
            std::snprintf(buf, static_cast<std::size_t>(size), "%.0fm", mins);
        } else {
            std::snprintf(buf, static_cast<std::size_t>(size), "%.1fm", mins);
        }
        return;
    }
    std::snprintf(buf, static_cast<std::size_t>(size), "%.0fs", sec);
}

inline bool sanitizePlotWindow(double& lo, double& hi, double minSpanMs = kPlotWindowMinMs,
                               double maxSpanMs = kPlotWindowMaxMs) {
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
                                 double minSpanMs = kPlotWindowMinMs, double maxSpanMs = kPlotWindowMaxMs) {
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

// factor < 1 zooms in. The anchor stays at the same fraction of the range.
inline void zoomValueWindowAround(double anchor, double& lo, double& hi, double factor) {
    if (!std::isfinite(anchor) || !std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo)) {
        return;
    }
    if (!std::isfinite(factor) || factor <= 0.0) {
        return;
    }
    factor = std::max(0.25, std::min(4.0, factor));
    const double span = hi - lo;
    double u = (anchor - lo) / span;
    if (!std::isfinite(u)) {
        u = 0.5;
    }
    u = std::max(0.0, std::min(1.0, u));
    const double next = std::max(span * factor, span * 1.0e-6);
    lo = anchor - next * u;
    hi = anchor + next * (1.0 - u);
    if (!(hi > lo)) {
        hi = lo + std::max(std::abs(anchor) * 1.0e-6, 1.0e-6);
    }
}

// Equal-width bins over the finite samples. xs are bin centers.
inline void fillHistogram(const std::vector<double>& values, int bins, std::vector<double>& xs,
                          std::vector<double>& counts) {
    xs.clear();
    counts.clear();
    if (bins < 4) {
        bins = 4;
    }
    if (bins > 80) {
        bins = 80;
    }
    double lo = 0.0;
    double hi = 0.0;
    bool have = false;
    for (double v : values) {
        if (!std::isfinite(v)) {
            continue;
        }
        if (!have) {
            lo = hi = v;
            have = true;
            continue;
        }
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (!have) {
        return;
    }
    if (!(hi > lo)) {
        hi = lo + 1.0;
    }
    const double width = (hi - lo) / static_cast<double>(bins);
    xs.resize(static_cast<std::size_t>(bins));
    counts.assign(static_cast<std::size_t>(bins), 0.0);
    for (int i = 0; i < bins; ++i) {
        xs[static_cast<std::size_t>(i)] = lo + (static_cast<double>(i) + 0.5) * width;
    }
    for (double v : values) {
        if (!std::isfinite(v)) {
            continue;
        }
        int bin = static_cast<int>(std::floor((v - lo) / width));
        if (bin < 0) {
            bin = 0;
        }
        if (bin >= bins) {
            bin = bins - 1;
        }
        counts[static_cast<std::size_t>(bin)] += 1.0;
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
