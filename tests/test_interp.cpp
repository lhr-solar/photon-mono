#include "photon/interp.hpp"
#include "test_harness.hpp"

#include <cmath>

void runInterpTests() {
    using photon::interp::GpsPlayback;
    using photon::interp::interpolateLinear;
    using photon::interp::kGpsDelayMs;
    using photon::interp::ringIndex;

    {
        CHECK(ringIndex(0, 3, 8, 0) == 0);
        CHECK(ringIndex(2, 3, 8, 0) == 2);
        CHECK(ringIndex(0, 4, 4, 6) == 2);
        CHECK(ringIndex(1, 4, 4, 6) == 3);
        CHECK(ringIndex(3, 4, 4, 6) == 1);
    }

    {
        const double t[] = {1000.0, 1100.0, 1300.0};
        const double v[] = {10.0, 20.0, 40.0};
        double out = 0.0;
        CHECK(interpolateLinear(t, v, 3, 1000.0, out));
        CHECK(out == 10.0);
        CHECK(interpolateLinear(t, v, 3, 1050.0, out));
        CHECK(std::fabs(out - 15.0) < 1e-9);
        CHECK(interpolateLinear(t, v, 3, 1200.0, out));
        CHECK(std::fabs(out - 30.0) < 1e-9);
        CHECK(interpolateLinear(t, v, 3, 1400.0, out));
        CHECK(out == 40.0);
        CHECK(interpolateLinear(t, v, 3, 900.0, out));
        CHECK(out == 10.0);
    }

    {
        // Circular: cap 4, write=6 → physical [2,3,0,1] = logical 0..3
        const double t[] = {1300.0, 1400.0, 1100.0, 1200.0};
        const double v[] = {40.0, 50.0, 20.0, 30.0};
        double out = 0.0;
        CHECK(photon::interp::interpolate(t, v, 4, 4, 6, 1150.0, out));
        CHECK(std::fabs(out - 25.0) < 1e-9);
        CHECK(photon::interp::interpolate(t, v, 4, 4, 6, 1350.0, out));
        CHECK(std::fabs(out - 45.0) < 1e-9);
    }

    {
        const double latT[] = {0.0, 200.0, 400.0};
        const double latV[] = {30.0, 30.001, 30.002};
        const double lonT[] = {0.0, 200.0, 400.0};
        const double lonV[] = {-97.0, -97.001, -97.002};
        GpsPlayback play;
        play.advance(400.0, 0.0, 0.0, true, 0.0, false);
        CHECK(play.havePlay);
        CHECK(std::fabs(play.playMs - (400.0 - kGpsDelayMs)) < 1e-9);
        const auto fix = play.sample(latT, latV, 3, 3, 0, lonT, lonV, 3, 3, 0);
        CHECK(fix.ok);
        CHECK(fix.lat > 30.0 && fix.lat < 30.002);
        CHECK(fix.lon < -97.0 && fix.lon > -97.002);
        CHECK(fix.headingDeg > 0.0);
    }

    {
        GpsPlayback play;
        play.advance(100.0, 0.0, 0.0, true, 0.0, false);
        const double first = play.playMs;
        play.advance(100.0, 0.0, 16.0, true, 0.0, false);
        CHECK(play.playMs >= first);
        play.advance(100.0, 0.0, 16.0, true, 0.0, false);
        CHECK(play.playMs <= 100.0);
    }

    {
        GpsPlayback play;
        play.advance(5000.0, 0.0, 0.0, false, 2500.0, true);
        CHECK(std::fabs(play.playMs - 2500.0) < 1e-9);
    }
}
