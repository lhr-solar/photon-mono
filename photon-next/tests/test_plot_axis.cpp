#include "ui/clock_format.hpp"
#include "ui/plot_color.hpp"
#include "ui/plot_time.hpp"
#include "test_harness.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

void runPlotAxisTests() {
    using photon::ui::PlotArrays;
    using photon::ui::capPlotArrays;
    using photon::ui::samplePlotArraysAt;
    using photon::ui::containPlotTime;
    using photon::ui::zoomTimeWindowAround;

    {
        PlotArrays hist;
        hist.t = {1000.0, 2000.0, 3000.0};
        hist.y = {10.0, 20.0, 40.0};
        double y = 0.0;
        CHECK(samplePlotArraysAt(hist, 1000.0, y));
        CHECK(near(y, 10.0));
        CHECK(samplePlotArraysAt(hist, 1500.0, y));
        CHECK(near(y, 15.0));
        CHECK(samplePlotArraysAt(hist, 2500.0, y));
        CHECK(near(y, 30.0));
        CHECK(samplePlotArraysAt(hist, 3000.0, y));
        CHECK(near(y, 40.0));
        CHECK(!samplePlotArraysAt(PlotArrays{}, 1500.0, y));
    }

    {
        PlotArrays hist;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        hist.t = {1000.0, 1500.0, 2000.0, 2500.0};
        hist.y = {nan, 10.0, nan, 30.0};
        double y = 0.0;
        CHECK(samplePlotArraysAt(hist, 1500.0, y));
        CHECK(near(y, 10.0));
        CHECK(samplePlotArraysAt(hist, 4000.0, y));
        CHECK(near(y, 30.0));
        PlotArrays allNan;
        allNan.t = {nan};
        allNan.y = {nan};
        CHECK(!samplePlotArraysAt(allNan, 1500.0, y));
        CHECK(!photon::ui::plotArraysAllFinite(hist, 4));
        PlotArrays clean;
        clean.t = {1000.0, 2000.0};
        clean.y = {1.0, 2.0};
        CHECK(photon::ui::plotArraysAllFinite(clean, 2));
        CHECK(!photon::ui::plotArraysAllFinite(clean, 3));
    }

    {
        PlotArrays hist;
        hist.t = {0.0, 100.0, 50.0, 60.0};
        hist.y = {0.0, 100.0, 5.0, 15.0};
        double y = 0.0;
        CHECK(samplePlotArraysAt(hist, 55.0, y));
        CHECK(near(y, 10.0));
        CHECK(samplePlotArraysAt(hist, 0.0, y));
        CHECK(near(y, 0.0));
    }

    {
        PlotArrays hist;
        for (int i = 0; i < 64; ++i) {
            hist.t.push_back(static_cast<double>(i));
            hist.y.push_back(static_cast<double>(i));
        }
        capPlotArrays(hist, 8);
        CHECK(hist.t.size() <= 8);
        CHECK(hist.y.size() == hist.t.size());
        CHECK(hist.t.front() == 0.0);
    }

    {
        double lo = 0.0;
        double hi = 10000.0;
        zoomTimeWindowAround(2500.0, lo, hi, 0.5);
        CHECK(near(lo, 1250.0));
        CHECK(near(hi, 6250.0));
        // Playhead at 2500 stays at the same fraction of the window.
        const double u = (2500.0 - lo) / (hi - lo);
        CHECK(near(u, 0.25));
    }

    {
        // Cursor at the right edge (live / follow): zoom in keeps it on the right.
        double lo = 0.0;
        double hi = 15000.0;
        zoomTimeWindowAround(15000.0, lo, hi, 0.5);
        CHECK(near(hi, 15000.0));
        CHECK(near(lo, 7500.0));
    }

    {
        using photon::ui::sanitizePlotWindow;
        double lo = std::numeric_limits<double>::quiet_NaN();
        double hi = 1000.0;
        CHECK(!sanitizePlotWindow(lo, hi));
        lo = 8000.0;
        hi = 2000.0;
        CHECK(sanitizePlotWindow(lo, hi));
        CHECK(lo < hi);
        lo = std::numeric_limits<double>::quiet_NaN();
        hi = std::numeric_limits<double>::quiet_NaN();
        zoomTimeWindowAround(5000.0, lo, hi, 0.5);
        CHECK(std::isfinite(lo) && std::isfinite(hi));
        CHECK(hi > lo);
    }

    {
        double lo = 8000.0;
        double hi = 10000.0;
        zoomTimeWindowAround(9000.0, lo, hi, 2.0);
        CHECK(lo >= 0.0);
        CHECK(near((9000.0 - lo) / (hi - lo), 0.5, 1e-6, 1e-6));
    }

    {
        using photon::ui::zoomValueWindowAround;
        double lo = 0.0;
        double hi = 10.0;
        zoomValueWindowAround(2.0, lo, hi, 0.5);
        CHECK(near(lo, 1.0));
        CHECK(near(hi, 6.0));
        const double u = (2.0 - lo) / (hi - lo);
        CHECK(near(u, 0.2));
    }

    {
        using photon::ui::fillHistogram;
        std::vector<double> xs;
        std::vector<double> counts;
        fillHistogram({1.0, 1.0, 1.0, 3.0}, 4, xs, counts);
        CHECK(xs.size() == 4);
        CHECK(counts.size() == 4);
        CHECK(near(counts[0], 3.0));
        CHECK(near(counts[3], 1.0));
        fillHistogram({}, 8, xs, counts);
        CHECK(xs.empty());
    }

    {
        double lo = 10000.0;
        double hi = 20000.0;
        containPlotTime(8000.0, lo, hi);
        CHECK(near(lo, 8000.0));
        CHECK(near(hi, 18000.0));
        containPlotTime(25000.0, lo, hi);
        CHECK(near(hi, 25000.0));
        CHECK(near(lo, 15000.0));
    }

    {
        double lo = 5000.0;
        double hi = 15000.0;
        containPlotTime(1000.0, lo, hi, 4000.0, 20000.0);
        CHECK(near(lo, 4000.0));
        CHECK(near(hi, 14000.0));
    }

    {
        using photon::ui::centerPlotWindow;
        using photon::ui::shiftPlotWindow;
        double lo = 0.0;
        double hi = 0.0;
        centerPlotWindow(15000.0, 10000.0, lo, hi);
        CHECK(near(lo, 10000.0));
        CHECK(near(hi, 20000.0));
        centerPlotWindow(1000.0, 10000.0, lo, hi, 0.0, 50000.0);
        CHECK(near(lo, 0.0));
        CHECK(near(hi, 10000.0));
        centerPlotWindow(48000.0, 10000.0, lo, hi, 0.0, 50000.0);
        CHECK(near(lo, 40000.0));
        CHECK(near(hi, 50000.0));
        lo = 10000.0;
        hi = 20000.0;
        shiftPlotWindow(lo, hi, -3000.0);
        CHECK(near(lo, 7000.0));
        CHECK(near(hi, 17000.0));
        shiftPlotWindow(lo, hi, -20000.0, 0.0, 50000.0);
        CHECK(near(lo, 0.0));
        CHECK(near(hi, 10000.0));
    }

    {
        using photon::ui::formatHexColor;
        using photon::ui::parseHexColor;
        float rgba[4]{};
        CHECK(parseHexColor("#f80", rgba));
        CHECK(near(rgba[0], 1.0));
        CHECK(near(rgba[1], 0x88 / 255.0));
        CHECK(near(rgba[2], 0.0));
        CHECK(near(rgba[3], 1.0));
        CHECK(parseHexColor("00ff88", rgba));
        CHECK(near(rgba[0], 0.0));
        CHECK(near(rgba[1], 1.0));
        CHECK(near(rgba[2], 0x88 / 255.0));
        CHECK(parseHexColor("#11223344", rgba));
        CHECK(near(rgba[3], 0x44 / 255.0));
        CHECK(!parseHexColor("", rgba));
        CHECK(!parseHexColor("zz", rgba));
        CHECK(formatHexColor(1.0f, 0.0f, 0.5f) == "#FF0080");
        CHECK(formatHexColor(1.0f, 1.0f, 1.0f, 0.5f) == "#FFFFFF80");
    }

    {
        using photon::ui::ClockParts;
        using photon::ui::formatLocalClock;
        char buf[48]{};
        CHECK(formatLocalClock(0ull, buf, sizeof(buf)) > 0);
        CHECK(std::strcmp(buf, "--:--:--") == 0);
        formatLocalClock(3661000ull, buf, sizeof(buf));
        CHECK(std::strcmp(buf, "1:01:01") == 0);
        formatLocalClock(3661123ull, buf, sizeof(buf), ClockParts::TimeMs);
        CHECK(std::strcmp(buf, "1:01:01.123") == 0);
        formatLocalClock(1757088000000ull, buf, sizeof(buf), ClockParts::DateTime);
        const bool ampm = std::strstr(buf, " AM") != nullptr || std::strstr(buf, " PM") != nullptr;
        CHECK(ampm);
        CHECK(std::strstr(buf, ",") != nullptr);
        char nextTick[48]{};
        photon::ui::formatPlotClock(1789250000000.0, buf, sizeof(buf), nullptr);
        photon::ui::formatPlotClock(1789250000033.0, nextTick, sizeof(nextTick), nullptr);
        CHECK(std::strcmp(buf, nextTick) != 0);
        CHECK(std::strlen(buf) == 8);
        CHECK(std::strstr(buf, "AM") == nullptr && std::strstr(buf, "PM") == nullptr);
        CHECK(std::strstr(nextTick, ".033") != nullptr);
    }

    {
        using photon::ui::timelineHourRange;
        using photon::ui::timelineMinuteRange;
        using photon::ui::timelineSecondRange;
        using photon::ui::localTmFromUnixMs;
        const std::uint64_t t0 = 1757088000000ull;
        const std::uint64_t t1 = t0 + 90ull * 1000ull;
        std::tm a{};
        std::tm b{};
        CHECK(localTmFromUnixMs(t0, a));
        CHECK(localTmFromUnixMs(t1, b));
        const auto hours = timelineHourRange(t0, t1);
        CHECK(hours.lo == a.tm_hour);
        CHECK(hours.hi == b.tm_hour);
        const auto mins = timelineMinuteRange(t0, t1, a.tm_hour);
        CHECK(mins.lo == a.tm_min);
        CHECK(mins.hi >= mins.lo);
        if (a.tm_hour == b.tm_hour) {
            CHECK(mins.hi == b.tm_min);
        }
        const auto secs = timelineSecondRange(t0, t1, a.tm_hour, a.tm_min);
        CHECK(secs.lo == a.tm_sec);
        if (a.tm_hour == b.tm_hour && a.tm_min == b.tm_min) {
            CHECK(secs.hi == b.tm_sec);
        } else {
            CHECK(secs.hi == 59);
        }
        const auto empty = timelineHourRange(0, 0);
        CHECK(empty.lo == 0);
        CHECK(empty.hi == 23);
    }

    {
        using photon::ui::squareEqualLimits;
        double x0 = -2.0;
        double x1 = 2.0;
        double y0 = -0.1;
        double y1 = 0.1;
        squareEqualLimits(x0, x1, y0, y1);
        CHECK(near(x1 - x0, y1 - y0));
        CHECK(x1 - x0 >= 4.0 - 1e-9);
        double a0 = 10.0;
        double a1 = 12.0;
        double b0 = -1.0;
        double b1 = 1.0;
        squareEqualLimits(a0, a1, b0, b1);
        CHECK(near(a1 - a0, b1 - b0));
        CHECK(near(0.5 * (a0 + a1), 11.0));
        CHECK(near(0.5 * (b0 + b1), 0.0));
    }

    {
        using photon::ui::clampPlotWindowSec;
        using photon::ui::formatPlotWindowLabel;
        using photon::ui::kPlotTimePresets;
        using photon::ui::matchPlotTimePreset;
        CHECK(near(clampPlotWindowSec(0.0f), 2.0f));
        CHECK(near(clampPlotWindowSec(300.0f), 300.0f));
        CHECK(near(clampPlotWindowSec(200000.0f), 86400.0f));
        const int fiveMin = matchPlotTimePreset(300.0f);
        CHECK(fiveMin >= 0);
        if (fiveMin >= 0) {
            CHECK(std::strcmp(kPlotTimePresets[fiveMin].label, "5m") == 0);
        }
        const int day = matchPlotTimePreset(86400.0f);
        CHECK(day >= 0);
        if (day >= 0) {
            CHECK(std::strcmp(kPlotTimePresets[day].label, "24h") == 0);
        }
        CHECK(matchPlotTimePreset(1234.0f) < 0);
        char label[16]{};
        formatPlotWindowLabel(15.0f, label, static_cast<int>(sizeof(label)));
        CHECK(std::strcmp(label, "15s") == 0);
        formatPlotWindowLabel(90.0f, label, static_cast<int>(sizeof(label)));
        CHECK(std::strcmp(label, "1.5m") == 0);
        formatPlotWindowLabel(3600.0f, label, static_cast<int>(sizeof(label)));
        CHECK(std::strcmp(label, "1h") == 0);
        formatPlotWindowLabel(5400.0f, label, static_cast<int>(sizeof(label)));
        CHECK(std::strcmp(label, "1.5h") == 0);
        double lo = 0.0;
        double hi = 10.0 * 3600.0 * 1000.0;
        CHECK(photon::ui::sanitizePlotWindow(lo, hi));
        CHECK(near(hi - lo, 10.0 * 3600.0 * 1000.0));
    }

    {
        using photon::ui::extendYFromLine;
        std::vector<double> y(40, 4.0);
        y[3] = 0.0;
        y[30] = 80.0;
        double lo = 0.0;
        double hi = 1.0;
        bool have = false;
        extendYFromLine(y, lo, hi, have);
        CHECK(have);
        CHECK(lo > -1.0);
        CHECK(hi < 20.0);
        CHECK(lo <= 4.0);
        CHECK(hi >= 4.0);
    }
    {
        // Zooming out beyond a recording must not collapse the span on pan.
        double lo = 100000, hi = 130000;
        for (int i = 0; i < 4; ++i)
            zoomTimeWindowAround((lo + hi) * 0.5, lo, hi, 2.0);
        const double span = hi - lo;
        const double start = lo;
        photon::ui::panTimeWindow(lo, hi, 5000, 100000, 130000);
        CHECK(near(hi - lo, span));
        CHECK(near(lo, start + 5000));
        photon::ui::panTimeWindow(lo, hi, -5000, 100000, 130000);
        CHECK(near(lo, start));
        photon::ui::panTimeWindow(lo, hi, 1e9, 100000, 130000);
        CHECK(near(hi - lo, span));
        CHECK(lo < 130000);
    }
    {
        double lo = -10, hi = 30;
        photon::ui::panValueWindow(lo, hi, 100, 400);
        CHECK(near(lo, 0));
        CHECK(near(hi, 40));
        photon::ui::panValueWindow(lo, hi, -100, 400);
        CHECK(near(lo, -10));
        CHECK(near(hi, 30));
        double rightLo = 1000, rightHi = 3000;
        photon::ui::panValueWindow(rightLo, rightHi, 100, 400);
        CHECK(near(rightLo, 1500));
        CHECK(near(rightHi, 3500));
        photon::ui::panValueWindow(lo, hi, 100, 0);
        CHECK(near(lo, -10));
        CHECK(near(hi, 30));
    }

}
