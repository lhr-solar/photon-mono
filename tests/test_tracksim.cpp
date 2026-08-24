#include "photon/control.hpp"
#include "photon/tracksim.hpp"
#include "test_harness.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef PHOTON_ASSETS_DIR
#define PHOTON_ASSETS_DIR ""
#endif

namespace {

using photon::tracksim::Car;
using photon::tracksim::Segment;
using photon::tracksim::SolveInput;
using photon::tracksim::Track;

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string hillLoopPath() {
    return std::string(PHOTON_ASSETS_DIR) + "/tracks/hill_loop.json";
}

Track loadHillLoop() {
    std::string error;
    auto track = photon::tracksim::parseTrack(readFile(hillLoopPath()), &error);
    CHECK(track.has_value());
    if (!track) {
        Track empty;
        return empty;
    }
    return *track;
}

void testFlatPowerMatchesHandCalc() {
    const Car car = photon::tracksim::defaultCar();
    const double v = 10.0;
    const double fAero = 0.5 * car.rhoKgpm3 * car.cdaM2 * v * v;
    const double fRoll = car.crr * car.massKg * photon::tracksim::kG;
    const double pWheel = (fAero + fRoll) * v;
    const double pTrac = pWheel / car.driveEff;

    CHECK(near(photon::tracksim::aeroForce(car, v), fAero, 1e-9, 1e-12));
    CHECK(near(photon::tracksim::rollingForce(car, 0.0), fRoll, 1e-9, 1e-12));
    CHECK(near(photon::tracksim::gravityForce(car, 0.0), 0.0, 1e-12, 0.0));
    CHECK(near(photon::tracksim::wheelPower(car, v, 0.0, 0.0), pWheel, 1e-8, 1e-10));

    const auto inst = photon::tracksim::instantPower(car, v, 0.0, 0.0);
    CHECK(near(inst.tractionBattW, pTrac, 1e-6, 1e-8));
    CHECK(near(inst.batteryW, pTrac - car.solarW, 1e-6, 1e-8));
    CHECK(!inst.regenCapped);
}

void testUphillExceedsFlat() {
    const Car car = photon::tracksim::defaultCar();
    const double v = 10.0;
    const double pFlat = photon::tracksim::wheelPower(car, v, 0.0, 0.0);
    const double pHill = photon::tracksim::wheelPower(car, v, 0.05, 0.0);
    CHECK(pHill > pFlat + 100.0);
    CHECK(photon::tracksim::instantPower(car, v, 0.05, 0.0).tractionBattW >
          photon::tracksim::instantPower(car, v, 0.0, 0.0).tractionBattW);
}

void testRegenCapRespected() {
    const Car car = photon::tracksim::defaultCar();
    const double v = 20.0;
    const double grade = -0.15;
    const auto inst = photon::tracksim::instantPower(car, v, grade, 0.0);
    CHECK(inst.wheelW < 0.0);
    CHECK(inst.regenCapped);
    CHECK(near(inst.tractionBattW, -car.regenPowerW, 1e-9, 1e-12));
    CHECK(inst.tractionBattW >= -car.regenPowerW - 1e-9);

    const double mild = photon::tracksim::tractionBatteryPower(car, -100.0);
    CHECK(near(mild, -100.0 * car.regenEff, 1e-9, 1e-12));
}

void testTrackJsonRoundTrip() {
    const Track track = loadHillLoop();
    CHECK(track.id == "hill_loop");
    CHECK(track.closed);
    CHECK(track.segments.size() == 6);
    CHECK(near(photon::tracksim::trackLengthM(track), 3400.0, 1e-9, 0.0));

    std::string error;
    auto again = photon::tracksim::parseTrack(photon::tracksim::stringifyTrack(track), &error);
    CHECK(again.has_value());
    CHECK(again->segments.size() == track.segments.size());
    CHECK(near(again->segments[1].grade, 0.055, 1e-12, 0.0));
    CHECK(near(again->segments[4].curvature1pm, 0.025, 1e-12, 0.0));
}

void testControlRequestResultRoundTrip() {
    auto req = photon::ctrl::trackSimRequest("hill_loop", 400.0, 0.88);
    req.carOverride.solarW = 900.0;
    req.carOverride.massKg = 275.0;
    const auto reqBack = photon::ctrl::parse(photon::ctrl::serialize(req));
    CHECK(reqBack.has_value());
    CHECK(reqBack->type == photon::ctrl::Type::TrackSimRequest);
    CHECK(reqBack->trackId == "hill_loop");
    CHECK(near(reqBack->timeBudgetS, 400.0, 1e-12, 0.0));
    CHECK(near(reqBack->initialSoc, 0.88, 1e-12, 0.0));
    CHECK(reqBack->carOverride.solarW.has_value());
    CHECK(near(*reqBack->carOverride.solarW, 900.0, 1e-12, 0.0));
    CHECK(reqBack->carOverride.massKg.has_value());
    CHECK(near(*reqBack->carOverride.massKg, 275.0, 1e-12, 0.0));

    Track inlineTrack;
    inlineTrack.id = "tiny";
    inlineTrack.segments.push_back(Segment{200.0, 0.0, 0.0, 15.0});
    req.track = inlineTrack;
    const auto inlineBack = photon::ctrl::parse(photon::ctrl::serialize(req));
    CHECK(inlineBack.has_value());
    CHECK(inlineBack->track.has_value());
    CHECK(inlineBack->track->id == "tiny");
    CHECK(inlineBack->track->segments.size() == 1);

    photon::tracksim::SolveOutput solved;
    solved.feasible = true;
    solved.totalTimeS = 399.2;
    solved.energyWh = 42.5;
    solved.endSoc = 0.87;
    solved.dEdT = -0.01;
    photon::tracksim::SegmentResult row;
    row.index = 0;
    row.lengthM = 200.0;
    row.grade = 0.0;
    row.speedMps = 8.5;
    row.tCumS = 23.5;
    row.whCum = 1.2;
    solved.segments.push_back(row);
    const auto resultMsg = photon::ctrl::trackSimResult(solved, "tiny");
    const auto resultBack = photon::ctrl::parse(photon::ctrl::serialize(resultMsg));
    CHECK(resultBack.has_value());
    CHECK(resultBack->type == photon::ctrl::Type::TrackSimResult);
    CHECK(resultBack->feasible);
    CHECK(resultBack->trackId == "tiny");
    CHECK(near(resultBack->totalTimeS, 399.2, 1e-9, 0.0));
    CHECK(near(resultBack->energyWh, 42.5, 1e-9, 0.0));
    CHECK(near(resultBack->endSoc, 0.87, 1e-9, 0.0));
    CHECK(near(resultBack->dEdT, -0.01, 1e-9, 0.0));
    CHECK(resultBack->trackSegments.size() == 1);
    CHECK(near(resultBack->trackSegments[0].speedMps, 8.5, 1e-9, 0.0));

    auto list = photon::ctrl::trackSimListResult({photon::tracksim::infoOf(inlineTrack)});
    const auto listBack = photon::ctrl::parse(photon::ctrl::serialize(list));
    CHECK(listBack.has_value());
    CHECK(listBack->type == photon::ctrl::Type::TrackSimList);
    CHECK(listBack->tracks.size() == 1);
    CHECK(listBack->tracks[0].id == "tiny");
}

void testSolverPropertiesAndExampleTrack() {
    const Track track = loadHillLoop();
    CHECK(!track.segments.empty());

    SolveInput input;
    input.track = track;
    input.car = photon::tracksim::defaultCar();
    input.initialSoc = 0.90;

    std::vector<double> vmax;
    for (const auto& seg : track.segments) {
        double lo = 0.0;
        double hi = 0.0;
        photon::tracksim::segmentSpeedBounds(input.car, seg, lo, hi);
        vmax.push_back(hi);
    }
    const auto fastest = photon::tracksim::evaluateProfile(input, vmax);
    CHECK(fastest.totalTimeS > 0.0);
    const double tMin = fastest.totalTimeS;

    input.timeBudgetS = tMin * 0.5;
    const auto infeas = photon::tracksim::solve(input);
    CHECK(!infeas.feasible);
    CHECK(infeas.segments.size() == track.segments.size());

    input.timeBudgetS = tMin * 1.05;
    const auto tight = photon::tracksim::solve(input);
    CHECK(tight.feasible);
    CHECK(tight.totalTimeS <= input.timeBudgetS + 0.05);
    for (std::size_t i = 0; i < tight.segments.size(); ++i) {
        CHECK(tight.segments[i].speedMps <= vmax[i] + 1e-6);
        CHECK(tight.segments[i].speedMps >= vmax[i] * 0.80);
    }

    input.timeBudgetS = 250.0;
    const auto mid = photon::tracksim::solve(input);
    input.timeBudgetS = 400.0;
    const auto loose = photon::tracksim::solve(input);
    CHECK(mid.feasible);
    CHECK(loose.feasible);
    CHECK(loose.energyWh <= mid.energyWh + 1e-6);
    CHECK(loose.totalTimeS + 1e-6 >= mid.totalTimeS);

    CHECK(loose.segments.size() == 6);
    CHECK(loose.totalTimeS <= 400.0 + 0.05);
    CHECK(loose.totalTimeS >= 100.0);
    CHECK(std::isfinite(loose.energyWh));
    CHECK(std::isfinite(loose.endSoc));
    CHECK(loose.endSoc > 0.0);
    CHECK(loose.endSoc <= 1.0);
    double prevT = 0.0;
    for (std::size_t i = 0; i < loose.segments.size(); ++i) {
        const auto& row = loose.segments[i];
        CHECK(row.index == static_cast<int>(i));
        CHECK(row.speedMps > 0.0);
        CHECK(row.tCumS >= prevT);
        prevT = row.tCumS;
        double lo = 0.0;
        double hi = 0.0;
        photon::tracksim::segmentSpeedBounds(input.car, track.segments[i], lo, hi);
        CHECK(row.speedMps + 1e-6 >= lo);
        CHECK(row.speedMps <= hi + 1e-6);
    }
    CHECK(near(loose.segments.back().tCumS, loose.totalTimeS, 1e-6, 1e-8));
}

void testSocSaturatesWhenSolarExceedsLoad() {
    Car car = photon::tracksim::defaultCar();
    car.solarW = 4000.0;
    car.socMin = 0.0;
    car.socMax = 1.0;
    car.batteryWh = 2000.0;

    Track track;
    track.id = "solar_flat";
    for (int i = 0; i < 4; ++i) {
        track.segments.push_back(Segment{500.0, 0.0, 0.0, 12.0});
    }

    SolveInput input;
    input.track = track;
    input.car = car;
    input.initialSoc = 0.92;
    input.timeBudgetS = 400.0;

    const auto out = photon::tracksim::solve(input);
    CHECK(out.feasible);
    CHECK(out.endSoc == 1.0);
    CHECK(near(out.energyWh, (0.92 - 1.0) * car.batteryWh, 1e-9, 1e-12));
    CHECK(out.segments.size() == 4);

    double prevHeadroom = 1.0 - 0.92;
    for (const auto& row : out.segments) {
        const double soc = 0.92 - row.whCum / car.batteryWh;
        CHECK(soc <= 1.0);
        CHECK(soc >= 0.0);
        const double headroom = 1.0 - soc;
        CHECK(headroom <= prevHeadroom + 1e-12);
        prevHeadroom = headroom;
    }
    CHECK(prevHeadroom == 0.0);
    CHECK(near(out.segments.back().whCum, out.energyWh, 1e-12, 0.0));

    input.initialSoc = 1.4;
    const auto fromOverfull = photon::tracksim::solve(input);
    CHECK(fromOverfull.endSoc == 1.0);
    CHECK(near(fromOverfull.energyWh, 0.0, 1e-9, 1e-12));
}

}  // namespace

void runTrackSimTests() {
    testFlatPowerMatchesHandCalc();
    testUphillExceedsFlat();
    testRegenCapRespected();
    testTrackJsonRoundTrip();
    testControlRequestResultRoundTrip();
    testSolverPropertiesAndExampleTrack();
    testSocSaturatesWhenSolarExceedsLoad();
}
