#include "photon/tracksim.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace photon::tracksim {
namespace {

constexpr double kEpsV = 1e-6;
constexpr double kTimeTolS = 1e-4;
constexpr int kLambdaIters = 56;
constexpr int kSpeedGrid = 80;

double clamp(double v, double lo, double hi) {
    return std::min(hi, std::max(lo, v));
}

double clampEff(double e) {
    if (!std::isfinite(e) || e <= 0.0) {
        return 0.05;
    }
    return std::min(1.0, e);
}

double keBatteryJ(const Car& car, double v0, double v1) {
    const double dke = 0.5 * car.massKg * (v1 * v1 - v0 * v0);
    if (dke >= 0.0) {
        return dke / car.driveEff;
    }
    return dke * car.regenEff;
}

}  // namespace

Car defaultCar() {
    return Car{};
}

Car sanitizeCar(Car car) {
    if (!(car.massKg > 1.0) || !std::isfinite(car.massKg)) {
        car.massKg = 280.0;
    }
    if (!(car.cdaM2 > 0.0) || !std::isfinite(car.cdaM2)) {
        car.cdaM2 = 0.12;
    }
    if (!(car.rhoKgpm3 > 0.0) || !std::isfinite(car.rhoKgpm3)) {
        car.rhoKgpm3 = 1.225;
    }
    if (!(car.crr >= 0.0) || !std::isfinite(car.crr)) {
        car.crr = 0.0045;
    }
    car.driveEff = clampEff(car.driveEff);
    car.regenEff = clampEff(car.regenEff);
    if (!(car.motorPowerW > 0.0) || !std::isfinite(car.motorPowerW)) {
        car.motorPowerW = 3500.0;
    }
    if (!(car.regenPowerW >= 0.0) || !std::isfinite(car.regenPowerW)) {
        car.regenPowerW = 2000.0;
    }
    if (!(car.batteryWh > 0.0) || !std::isfinite(car.batteryWh)) {
        car.batteryWh = 5000.0;
    }
    if (!std::isfinite(car.socMin)) {
        car.socMin = 0.10;
    }
    if (!std::isfinite(car.socMax) || car.socMax <= car.socMin) {
        car.socMax = 0.95;
    }
    if (!std::isfinite(car.solarW) || car.solarW < 0.0) {
        car.solarW = 0.0;
    }
    if (!(car.vMinMps > 0.0) || !std::isfinite(car.vMinMps)) {
        car.vMinMps = 1.0;
    }
    if (!(car.latAccelMaxMps2 > 0.0) || !std::isfinite(car.latAccelMaxMps2)) {
        car.latAccelMaxMps2 = 3.0;
    }
    return car;
}

Car applyCarOverride(Car base, const CarOverride& o) {
    if (o.massKg) {
        base.massKg = *o.massKg;
    }
    if (o.cdaM2) {
        base.cdaM2 = *o.cdaM2;
    }
    if (o.rhoKgpm3) {
        base.rhoKgpm3 = *o.rhoKgpm3;
    }
    if (o.crr) {
        base.crr = *o.crr;
    }
    if (o.driveEff) {
        base.driveEff = *o.driveEff;
    }
    if (o.motorPowerW) {
        base.motorPowerW = *o.motorPowerW;
    }
    if (o.regenEff) {
        base.regenEff = *o.regenEff;
    }
    if (o.regenPowerW) {
        base.regenPowerW = *o.regenPowerW;
    }
    if (o.batteryWh) {
        base.batteryWh = *o.batteryWh;
    }
    if (o.socMin) {
        base.socMin = *o.socMin;
    }
    if (o.socMax) {
        base.socMax = *o.socMax;
    }
    if (o.solarW) {
        base.solarW = *o.solarW;
    }
    if (o.vMinMps) {
        base.vMinMps = *o.vMinMps;
    }
    if (o.latAccelMaxMps2) {
        base.latAccelMaxMps2 = *o.latAccelMaxMps2;
    }
    return sanitizeCar(std::move(base));
}

double sinTheta(double grade) {
    return clamp(grade, -1.0, 1.0);
}

double cosTheta(double grade) {
    const double s = sinTheta(grade);
    const double c2 = 1.0 - s * s;
    return c2 <= 0.0 ? 0.0 : std::sqrt(c2);
}

double aeroForce(const Car& car, double speedMps) {
    const double v = std::max(0.0, speedMps);
    return 0.5 * car.rhoKgpm3 * car.cdaM2 * v * v;
}

double rollingForce(const Car& car, double grade) {
    return car.crr * car.massKg * kG * cosTheta(grade);
}

double gravityForce(const Car& car, double grade) {
    return car.massKg * kG * sinTheta(grade);
}

double wheelForce(const Car& car, double speedMps, double grade, double accelMps2) {
    return aeroForce(car, speedMps) + rollingForce(car, grade) + gravityForce(car, grade) +
           car.massKg * accelMps2;
}

double wheelPower(const Car& car, double speedMps, double grade, double accelMps2) {
    return wheelForce(car, speedMps, grade, accelMps2) * std::max(0.0, speedMps);
}

double tractionBatteryPower(const Car& car, double wheelPowerW, bool* regenCapped) {
    if (regenCapped) {
        *regenCapped = false;
    }
    if (wheelPowerW >= 0.0) {
        return wheelPowerW / car.driveEff;
    }
    const double uncapped = -wheelPowerW * car.regenEff;
    if (uncapped > car.regenPowerW) {
        if (regenCapped) {
            *regenCapped = true;
        }
        return -car.regenPowerW;
    }
    return -uncapped;
}

InstantPower instantPower(const Car& car, double speedMps, double grade, double accelMps2) {
    InstantPower out;
    out.wheelN = wheelForce(car, speedMps, grade, accelMps2);
    out.wheelW = out.wheelN * std::max(0.0, speedMps);
    out.tractionBattW = tractionBatteryPower(car, out.wheelW, &out.regenCapped);
    out.solarW = car.solarW;
    out.batteryW = out.tractionBattW - car.solarW;
    return out;
}

double powerLimitedSpeed(const Car& car, double grade) {
    const double pMaxWheel = car.motorPowerW * car.driveEff;
    if (pMaxWheel <= 0.0) {
        return car.vMinMps;
    }
    // Largest v with P_whl(v, a=0) <= P_motor * eta. Binary search on [0, kAbsVMaxMps].
    if (wheelPower(car, kAbsVMaxMps, grade, 0.0) <= pMaxWheel) {
        return kAbsVMaxMps;
    }
    double lo = 0.0;
    double hi = kAbsVMaxMps;
    for (int i = 0; i < 48; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (wheelPower(car, mid, grade, 0.0) <= pMaxWheel) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

double curveLimitedSpeed(const Car& car, double curvature1pm) {
    if (!(curvature1pm > 0.0) || !std::isfinite(curvature1pm)) {
        return kAbsVMaxMps;
    }
    return std::sqrt(car.latAccelMaxMps2 / curvature1pm);
}

void segmentSpeedBounds(const Car& car, const Segment& seg, double& vMin, double& vMax) {
    vMax = powerLimitedSpeed(car, seg.grade);
    vMax = std::min(vMax, curveLimitedSpeed(car, seg.curvature1pm));
    if (seg.speedLimitMps > 0.0) {
        vMax = std::min(vMax, seg.speedLimitMps);
    }
    vMin = car.vMinMps;
    if (vMax < vMin) {
        vMin = vMax;
    }
    vMin = std::max(vMin, kEpsV);
    vMax = std::max(vMax, vMin);
}

double trackLengthM(const Track& track) {
    double length = 0.0;
    for (const auto& seg : track.segments) {
        length += std::max(0.0, seg.lengthM);
    }
    return length;
}

TrackInfo infoOf(const Track& track) {
    TrackInfo info;
    info.id = track.id;
    info.name = track.name;
    info.lengthM = trackLengthM(track);
    info.segmentCount = static_cast<int>(track.segments.size());
    return info;
}

SolveOutput evaluateProfile(const SolveInput& in, const std::vector<double>& speeds) {
    SolveOutput out;
    const Car car = sanitizeCar(in.car);
    const std::size_t n = in.track.segments.size();
    if (n == 0 || speeds.size() != n) {
        return out;
    }

    const double socLo = std::max(0.0, std::min(car.socMin, car.socMax));
    const double socHi = std::min(1.0, std::max(car.socMin, car.socMax));
    double initialSoc = in.initialSoc;
    if (!std::isfinite(initialSoc)) {
        initialSoc = 0.90;
    }
    initialSoc = clamp(initialSoc, 0.0, 1.0);
    double soc = clamp(initialSoc, socLo, socHi);

    double tCum = 0.0;
    double eWh = 0.0;
    double vPrev = 0.0;
    out.segments.resize(n);

    for (std::size_t i = 0; i < n; ++i) {
        const auto& seg = in.track.segments[i];
        const double v = std::max(kEpsV, speeds[i]);
        const double length = std::max(0.0, seg.lengthM);
        const double dt = length / v;
        const auto p = instantPower(car, v, seg.grade, 0.0);
        const double rawWh = (p.batteryW * dt + keBatteryJ(car, vPrev, v)) / 3600.0;
        // Positive rawWh discharges. Clamp SOC to the usable window; surplus
        // charge at socHi is curtailed and does not bank above full.
        const double socNext = clamp(soc - rawWh / car.batteryWh, socLo, socHi);
        eWh += (soc - socNext) * car.batteryWh;
        soc = socNext;
        tCum += dt;
        vPrev = v;

        SegmentResult row;
        row.index = static_cast<int>(i);
        row.lengthM = length;
        row.grade = seg.grade;
        row.speedMps = v;
        row.tCumS = tCum;
        row.whCum = eWh;
        out.segments[i] = row;
    }

    out.totalTimeS = tCum;
    out.energyWh = eWh;
    out.endSoc = soc;
    out.feasible = true;
    return out;
}

namespace {

double segmentCost(const Car& car, const Segment& seg, double v, double lambdaW) {
    const auto p = instantPower(car, v, seg.grade, 0.0);
    return (p.batteryW + lambdaW) / std::max(kEpsV, v);
}

double bestSpeedForLambda(const Car& car, const Segment& seg, double lambdaW) {
    double vMin = 0.0;
    double vMax = 0.0;
    segmentSpeedBounds(car, seg, vMin, vMax);
    if (vMax <= vMin + kEpsV) {
        return vMax;
    }

    double bestV = vMin;
    double bestJ = segmentCost(car, seg, vMin, lambdaW);
    const double jMax = segmentCost(car, seg, vMax, lambdaW);
    if (jMax < bestJ) {
        bestJ = jMax;
        bestV = vMax;
    }

    // Dense grid: J(v) = (P_batt(v) + lambda) / v is smooth except at the
    // regen-cap kink, so a uniform sample plus endpoints is robust.
    for (int i = 1; i < kSpeedGrid; ++i) {
        const double u = static_cast<double>(i) / static_cast<double>(kSpeedGrid);
        const double v = vMin + (vMax - vMin) * u;
        const double j = segmentCost(car, seg, v, lambdaW);
        if (j < bestJ) {
            bestJ = j;
            bestV = v;
        }
    }
    return bestV;
}

std::vector<double> speedsForLambda(const Car& car, const Track& track, double lambdaW) {
    std::vector<double> speeds;
    speeds.reserve(track.segments.size());
    for (const auto& seg : track.segments) {
        speeds.push_back(bestSpeedForLambda(car, seg, lambdaW));
    }
    return speeds;
}

std::vector<double> maxSpeeds(const Car& car, const Track& track) {
    std::vector<double> speeds;
    speeds.reserve(track.segments.size());
    for (const auto& seg : track.segments) {
        double vMin = 0.0;
        double vMax = 0.0;
        segmentSpeedBounds(car, seg, vMin, vMax);
        speeds.push_back(vMax);
    }
    return speeds;
}

std::vector<double> minSpeeds(const Car& car, const Track& track) {
    std::vector<double> speeds;
    speeds.reserve(track.segments.size());
    for (const auto& seg : track.segments) {
        double vMin = 0.0;
        double vMax = 0.0;
        segmentSpeedBounds(car, seg, vMin, vMax);
        speeds.push_back(vMin);
    }
    return speeds;
}

std::vector<double> uniformSpeeds(const Car& car, const Track& track, double timeBudgetS) {
    const double length = trackLengthM(track);
    const double vTarget = (timeBudgetS > 0.0 && length > 0.0) ? (length / timeBudgetS) : car.vMinMps;
    std::vector<double> speeds;
    speeds.reserve(track.segments.size());
    for (const auto& seg : track.segments) {
        double vMin = 0.0;
        double vMax = 0.0;
        segmentSpeedBounds(car, seg, vMin, vMax);
        speeds.push_back(clamp(vTarget, vMin, vMax));
    }
    return speeds;
}

double profileTime(const Track& track, const std::vector<double>& speeds) {
    double t = 0.0;
    const std::size_t n = std::min(track.segments.size(), speeds.size());
    for (std::size_t i = 0; i < n; ++i) {
        t += std::max(0.0, track.segments[i].lengthM) / std::max(kEpsV, speeds[i]);
    }
    return t;
}

}  // namespace

SolveOutput solve(const SolveInput& raw) {
    SolveInput in = raw;
    in.car = sanitizeCar(in.car);
    if (!std::isfinite(in.initialSoc)) {
        in.initialSoc = 0.90;
    }
    in.initialSoc = clamp(in.initialSoc, 0.0, 1.0);

    SolveOutput out;
    if (in.track.segments.empty() || !(in.timeBudgetS > 0.0) || !std::isfinite(in.timeBudgetS)) {
        out.feasible = false;
        return out;
    }
    for (const auto& seg : in.track.segments) {
        if (!(seg.lengthM > 0.0) || !std::isfinite(seg.lengthM)) {
            out.feasible = false;
            return out;
        }
    }

    const auto vmax = maxSpeeds(in.car, in.track);
    auto fastest = evaluateProfile(in, vmax);
    const double tMin = fastest.totalTimeS;

    const auto vmin = minSpeeds(in.car, in.track);
    auto slowest = evaluateProfile(in, vmin);

    const auto vUni = uniformSpeeds(in.car, in.track, in.timeBudgetS);
    auto baseline = evaluateProfile(in, vUni);
    out.baselineEnergyWh = baseline.energyWh;

    if (tMin > in.timeBudgetS + kTimeTolS) {
        // Infeasible: even the limit profile misses T. Return that profile.
        out = fastest;
        out.feasible = false;
        out.baselineEnergyWh = baseline.energyWh;
        out.lambdaW = std::numeric_limits<double>::infinity();
        // Sensitivity at the min-time corner: extra time lets us back off limits.
        if (in.timeBudgetS > 0.0) {
            const auto relaxed = solve(SolveInput{in.track, in.car, tMin * 1.02, in.initialSoc});
            if (relaxed.feasible && relaxed.totalTimeS > tMin + kTimeTolS) {
                out.dEdT = (relaxed.energyWh - fastest.energyWh) / (relaxed.totalTimeS - tMin);
            }
        }
        return out;
    }

    if (slowest.totalTimeS <= in.timeBudgetS + kTimeTolS) {
        // Slack: energy-optimal (slow) profile already finishes inside T.
        out = slowest;
        out.feasible = true;
        out.baselineEnergyWh = baseline.energyWh;
        out.lambdaW = 0.0;
        out.dEdT = 0.0;
        return out;
    }

    // Bisection on lambda: time(lambda) is monotonically decreasing.
    // lambda = 0 -> slow / min-energy; lambda -> inf -> v_max.
    double lo = 0.0;
    double hi = 1.0;
    auto speedsHi = speedsForLambda(in.car, in.track, hi);
    int grow = 0;
    while (profileTime(in.track, speedsHi) > in.timeBudgetS && grow < 48) {
        hi *= 2.0;
        speedsHi = speedsForLambda(in.car, in.track, hi);
        ++grow;
    }

    std::vector<double> speeds = speedsHi;
    double lambda = hi;
    for (int i = 0; i < kLambdaIters; ++i) {
        const double mid = 0.5 * (lo + hi);
        const auto trial = speedsForLambda(in.car, in.track, mid);
        const double t = profileTime(in.track, trial);
        if (t > in.timeBudgetS) {
            lo = mid;
        } else {
            hi = mid;
            speeds = trial;
            lambda = mid;
        }
    }

    auto solved = evaluateProfile(in, speeds);
    if (!solved.feasible || solved.totalTimeS > in.timeBudgetS + 1e-2) {
        // Numerical fallback: clamped uniform speed.
        out = baseline;
        out.feasible = baseline.totalTimeS <= in.timeBudgetS + 1e-2;
        out.usedUniformBaseline = true;
        out.baselineEnergyWh = baseline.energyWh;
        out.lambdaW = 0.0;
        out.dEdT = 0.0;
        return out;
    }

    out = solved;
    out.feasible = true;
    out.baselineEnergyWh = baseline.energyWh;
    out.lambdaW = lambda;
    // dE/dT = -lambda  (J/s) when the time constraint is binding; report Wh/s.
    out.dEdT = -lambda / 3600.0;
    return out;
}

namespace {

std::optional<double> readNumber(const json::Value& obj, const char* key) {
    if (!obj.contains(key)) {
        return std::nullopt;
    }
    const auto& v = obj[key];
    if (!v.isNumber()) {
        return std::nullopt;
    }
    return v.asNumber();
}

void putNumber(json::Object& obj, const char* key, double value) {
    obj.emplace(key, json::Value(value));
}

}  // namespace

std::optional<Track> trackFromValue(const json::Value& value, std::string* error) {
    if (!value.isObject()) {
        if (error) {
            *error = "track must be a JSON object";
        }
        return std::nullopt;
    }
    Track track;
    track.id = value["id"].asString();
    track.name = value["name"].asString();
    track.closed = value["closed"].asBool();
    if (!value["segments"].isArray() || value["segments"].asArray().empty()) {
        if (error) {
            *error = "track.segments must be a non-empty array";
        }
        return std::nullopt;
    }
    for (const auto& item : value["segments"].asArray()) {
        if (!item.isObject()) {
            if (error) {
                *error = "track segment must be an object";
            }
            return std::nullopt;
        }
        Segment seg;
        if (const auto length = readNumber(item, "length_m")) {
            seg.lengthM = *length;
        }
        if (!(seg.lengthM > 0.0) || !std::isfinite(seg.lengthM)) {
            if (error) {
                *error = "track segment length_m must be positive";
            }
            return std::nullopt;
        }
        if (const auto grade = readNumber(item, "grade")) {
            seg.grade = *grade;
        } else if (const auto dh = readNumber(item, "elev_change_m")) {
            seg.grade = *dh / seg.lengthM;
        }
        if (const auto k = readNumber(item, "curvature_1pm")) {
            seg.curvature1pm = *k;
        }
        if (const auto lim = readNumber(item, "speed_limit_mps")) {
            seg.speedLimitMps = *lim;
        }
        track.segments.push_back(seg);
    }
    if (track.id.empty()) {
        track.id = "inline";
    }
    if (track.name.empty()) {
        track.name = track.id;
    }
    return track;
}

std::optional<Track> parseTrack(std::string_view jsonText, std::string* error) {
    json::ParseError parseError;
    const auto root = json::parse(jsonText, &parseError);
    if (!root) {
        if (error) {
            *error = parseError.message.empty() ? "invalid track JSON" : parseError.message;
        }
        return std::nullopt;
    }
    return trackFromValue(*root, error);
}

json::Value trackToValue(const Track& track) {
    json::Object obj;
    obj.emplace("id", json::Value(track.id));
    obj.emplace("name", json::Value(track.name));
    obj.emplace("closed", json::Value(track.closed));
    json::Array segs;
    segs.reserve(track.segments.size());
    for (const auto& seg : track.segments) {
        json::Object s;
        putNumber(s, "length_m", seg.lengthM);
        putNumber(s, "grade", seg.grade);
        if (seg.curvature1pm > 0.0) {
            putNumber(s, "curvature_1pm", seg.curvature1pm);
        }
        if (seg.speedLimitMps > 0.0) {
            putNumber(s, "speed_limit_mps", seg.speedLimitMps);
        }
        segs.emplace_back(std::move(s));
    }
    obj.emplace("segments", json::Value(std::move(segs)));
    return json::Value(std::move(obj));
}

std::string stringifyTrack(const Track& track) {
    return json::stringify(trackToValue(track));
}

CarOverride carOverrideFromValue(const json::Value& value) {
    CarOverride o;
    if (!value.isObject()) {
        return o;
    }
    o.massKg = readNumber(value, "mass_kg");
    o.cdaM2 = readNumber(value, "cda_m2");
    o.rhoKgpm3 = readNumber(value, "rho_kgpm3");
    o.crr = readNumber(value, "crr");
    o.driveEff = readNumber(value, "drive_eff");
    o.motorPowerW = readNumber(value, "motor_power_w");
    o.regenEff = readNumber(value, "regen_eff");
    o.regenPowerW = readNumber(value, "regen_power_w");
    o.batteryWh = readNumber(value, "battery_wh");
    o.socMin = readNumber(value, "soc_min");
    o.socMax = readNumber(value, "soc_max");
    o.solarW = readNumber(value, "solar_w");
    o.vMinMps = readNumber(value, "v_min_mps");
    o.latAccelMaxMps2 = readNumber(value, "lat_accel_max_mps2");
    return o;
}

json::Value carOverrideToValue(const CarOverride& o) {
    json::Object obj;
    if (o.massKg) {
        putNumber(obj, "mass_kg", *o.massKg);
    }
    if (o.cdaM2) {
        putNumber(obj, "cda_m2", *o.cdaM2);
    }
    if (o.rhoKgpm3) {
        putNumber(obj, "rho_kgpm3", *o.rhoKgpm3);
    }
    if (o.crr) {
        putNumber(obj, "crr", *o.crr);
    }
    if (o.driveEff) {
        putNumber(obj, "drive_eff", *o.driveEff);
    }
    if (o.motorPowerW) {
        putNumber(obj, "motor_power_w", *o.motorPowerW);
    }
    if (o.regenEff) {
        putNumber(obj, "regen_eff", *o.regenEff);
    }
    if (o.regenPowerW) {
        putNumber(obj, "regen_power_w", *o.regenPowerW);
    }
    if (o.batteryWh) {
        putNumber(obj, "battery_wh", *o.batteryWh);
    }
    if (o.socMin) {
        putNumber(obj, "soc_min", *o.socMin);
    }
    if (o.socMax) {
        putNumber(obj, "soc_max", *o.socMax);
    }
    if (o.solarW) {
        putNumber(obj, "solar_w", *o.solarW);
    }
    if (o.vMinMps) {
        putNumber(obj, "v_min_mps", *o.vMinMps);
    }
    if (o.latAccelMaxMps2) {
        putNumber(obj, "lat_accel_max_mps2", *o.latAccelMaxMps2);
    }
    return json::Value(std::move(obj));
}

bool carOverrideEmpty(const CarOverride& o) {
    return !o.massKg && !o.cdaM2 && !o.rhoKgpm3 && !o.crr && !o.driveEff && !o.motorPowerW && !o.regenEff &&
           !o.regenPowerW && !o.batteryWh && !o.socMin && !o.socMax && !o.solarW && !o.vMinMps &&
           !o.latAccelMaxMps2;
}

}  // namespace photon::tracksim
