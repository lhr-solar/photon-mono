#pragma once

#include "photon/json.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace photon::tracksim {

// ---------------------------------------------------------------------------
// Longitudinal QSS model (documented for the race-strategy UI / reviewers)
//
// A segment is driven at a single speed v (quasi-steady-state). Wheel-side
// force and power at speed v, grade g = sin(theta) = dh/ds, and accel a:
//
//   F_aero =  1/2 * rho * CdA * v^2          (v >= 0, drag opposes motion)
//   F_roll =  Crr * m * g * cos(theta)
//   F_grav =  m * g * sin(theta)             (positive grade = uphill)
//   F_acc  =  m * a
//   F_whl  =  F_aero + F_roll + F_grav + F_acc
//   P_whl  =  F_whl * v
//
// Battery traction power (solar not yet applied):
//   P_whl >= 0:  P_trac = P_whl / eta_drive          (limited by motor_power_w)
//   P_whl <  0:  P_trac = -min(-P_whl * eta_regen,
//                               regen_power_w)       (regen cap)
//
// Net battery power (positive = SOC falling):
//   P_batt = P_trac - solar_w
//
// Segment energy at constant v:  E = P_batt * (L / v)
// Discrete speed changes add kinetic energy converted through the same
// drive / regen efficiencies (start-from-rest on the first segment).
//
// SOC is stepped segment-by-segment and clamped to the usable window
// [max(0, soc_min), min(1, soc_max)]. Surplus solar/regen at the top of
// the window is curtailed (not banked); demand below the floor cannot
// draw more than remains. energy_wh and per-segment wh_cum are the
// actual battery energy along that clamped path:
//   end_soc = start_soc - energy_wh / battery_wh
// initial_soc is clamped to [0, 1] before integration.
//
// Solver: given a time budget T, minimize net battery energy subject to
//   sum_i L_i / v_i  <=  T
//   v_min <= v_i <= v_max,i
//
// v_max,i is the tightest of: posted speed limit, curvature limit
//   v_curve = sqrt(a_lat_max / kappa), and the QSS motor-power limit
//   (largest v with P_whl(v) <= motor_power_w * eta_drive).
//
// The time constraint is dualized with a constant "cost of time" lambda (W).
// For a fixed lambda each segment independently minimizes
//   J_i(v) = E_i(v) + lambda * t_i(v) = (P_batt(v) + lambda) * L / v
// i.e. argmin_v (P_batt(v) + lambda) / v.  Bisection on lambda raises speeds
// (lowers total time) until the route finishes in T, or reports infeasible
// if even all-v_max exceeds T.  lambda = 0 recovers the unconstrained
// energy-optimal (slow) profile.  dE/dT at a binding solution is -lambda
// (J/s); we report it in Wh/s.  A uniform-speed baseline (v = L_tot / T,
// then clamped) is always evaluated for comparison and used if the
// lambda search fails numerically.
// ---------------------------------------------------------------------------

inline constexpr double kG = 9.80665;
inline constexpr double kAbsVMaxMps = 50.0;

struct Car {
    double massKg = 280.0;
    double cdaM2 = 0.12;
    double rhoKgpm3 = 1.225;
    double crr = 0.0045;
    double driveEff = 0.95;
    double motorPowerW = 3500.0;
    double regenEff = 0.80;
    double regenPowerW = 2000.0;
    double batteryWh = 5000.0;
    double socMin = 0.10;
    double socMax = 0.95;
    double solarW = 850.0;
    double vMinMps = 1.0;
    double latAccelMaxMps2 = 3.0;
};

struct CarOverride {
    std::optional<double> massKg;
    std::optional<double> cdaM2;
    std::optional<double> rhoKgpm3;
    std::optional<double> crr;
    std::optional<double> driveEff;
    std::optional<double> motorPowerW;
    std::optional<double> regenEff;
    std::optional<double> regenPowerW;
    std::optional<double> batteryWh;
    std::optional<double> socMin;
    std::optional<double> socMax;
    std::optional<double> solarW;
    std::optional<double> vMinMps;
    std::optional<double> latAccelMaxMps2;
};

struct Segment {
    double lengthM = 0.0;
    double grade = 0.0;          // dh / path length = sin(theta)
    double curvature1pm = 0.0;   // 1/m, 0 = unused
    double speedLimitMps = 0.0;  // 0 = unused
};

struct Track {
    std::string id;
    std::string name;
    bool closed = false;
    std::vector<Segment> segments;
};

struct TrackInfo {
    std::string id;
    std::string name;
    double lengthM = 0.0;
    int segmentCount = 0;
};

struct InstantPower {
    double wheelN = 0.0;
    double wheelW = 0.0;
    double tractionBattW = 0.0;
    double solarW = 0.0;
    double batteryW = 0.0;
    bool regenCapped = false;
};

struct SegmentResult {
    int index = 0;
    double lengthM = 0.0;
    double grade = 0.0;
    double speedMps = 0.0;
    double tCumS = 0.0;
    double whCum = 0.0;
};

struct SolveInput {
    Track track;
    Car car{};
    double timeBudgetS = 0.0;
    double initialSoc = 0.90;
};

struct SolveOutput {
    bool feasible = false;
    double totalTimeS = 0.0;
    double energyWh = 0.0;
    double baselineEnergyWh = 0.0;
    double endSoc = 0.0;
    double dEdT = 0.0;  // Wh/s, d(energy_wh)/d(time_budget_s)
    double lambdaW = 0.0;
    bool usedUniformBaseline = false;
    std::vector<SegmentResult> segments;
};

Car defaultCar();
Car applyCarOverride(Car base, const CarOverride& o);
Car sanitizeCar(Car car);

double sinTheta(double grade);
double cosTheta(double grade);
double aeroForce(const Car& car, double speedMps);
double rollingForce(const Car& car, double grade);
double gravityForce(const Car& car, double grade);
double wheelForce(const Car& car, double speedMps, double grade, double accelMps2);
double wheelPower(const Car& car, double speedMps, double grade, double accelMps2);
double tractionBatteryPower(const Car& car, double wheelPowerW, bool* regenCapped = nullptr);
InstantPower instantPower(const Car& car, double speedMps, double grade, double accelMps2);

double powerLimitedSpeed(const Car& car, double grade);
double curveLimitedSpeed(const Car& car, double curvature1pm);
void segmentSpeedBounds(const Car& car, const Segment& seg, double& vMin, double& vMax);

double trackLengthM(const Track& track);
TrackInfo infoOf(const Track& track);

SolveOutput evaluateProfile(const SolveInput& in, const std::vector<double>& speeds);
SolveOutput solve(const SolveInput& in);

std::optional<Track> parseTrack(std::string_view jsonText, std::string* error = nullptr);
std::optional<Track> trackFromValue(const json::Value& value, std::string* error = nullptr);
json::Value trackToValue(const Track& track);
std::string stringifyTrack(const Track& track);

CarOverride carOverrideFromValue(const json::Value& value);
json::Value carOverrideToValue(const CarOverride& o);
bool carOverrideEmpty(const CarOverride& o);

}  // namespace photon::tracksim
