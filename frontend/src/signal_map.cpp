#include "photon_ui/signal_map.hpp"

#include "photon_ui/arena.hpp"
#include "photon_ui/client.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace photon::ui {
namespace {

// DBC names from assets/dbc/carcan_telemcan.dbc (Message.Signal).
// pack_soc: no pack SOC in the DBC; derived from Main_Battery_Voltage (legacy
// 32S Li-ion estimate) or synthetic id 3.
// pack_current: no Main_Battery_Current in the DBC; closest live current is
// MC_BusMeasurement.MC_BusCurrent (legacy dashboard "Bus Current").
// cell_* : muxed BPS_*_Aggregate_Arr + BPS_Tap_idx (32 taps), not scalars.
// array_power: no precomputed power; sum of MPPT A/B/C Vin*Iin.
// wheel_rpm_rear / dedicated linpot names: not in this DBC (SUS_Data1 per corner).

constexpr ScalarDef kScalarDefs[] = {
    {"pack_soc", "Pack SOC", "", "%", kSignalSoc, MapScreen::Pit, true, false, true, false, 30.0, 0.0},
    {"pack_voltage", "Pack V", "BPS_Status.Main_Battery_Voltage", "V", 0, MapScreen::Pit, true, false, true,
     true, 90.0, 134.4},
    {"pack_current", "Pack I", "MC_BusMeasurement.MC_BusCurrent", "A", 0, MapScreen::Pit, true, false, false,
     false, 0.0, 0.0},
    {"pack_avg_temp", "Pack T", "BPS_Status.Main_Battery_Avg_Temperature", "C", 0, MapScreen::Pit, false, false,
     false, true, 0.0, 45.0},
    {"bps_state", "BPS", "BPS_Status.BPS_Fault", "", 0, MapScreen::Pit, true, false, false, true, 0.0, 0.0},
    {"contactor_hv_plus", "HV+", "BPS_Status.HV_Plus_Contactor_State", "", 0, MapScreen::Pit, false, false,
     false, false, 0.0, 0.0},
    {"contactor_hv_minus", "HV-", "BPS_Status.HV_Minus_Contactor_State", "", 0, MapScreen::Pit, false, false,
     false, false, 0.0, 0.0},
    {"contactor_array", "Array", "BPS_Status.Array_Contactor_State", "", 0, MapScreen::Pit, false, false, false,
     false, 0.0, 0.0},
    {"contactor_array_precharge", "Arr Pchg", "BPS_Status.Array_Precharge_Contactor_State", "", 0,
     MapScreen::Pit, false, false, false, false, 0.0, 0.0},
    {"contactor_motor", "Motor", "VCU_Status.Motor_Contactor_State", "", 0, MapScreen::Pit, false, false, false,
     false, 0.0, 0.0},
    {"contactor_motor_precharge", "Mtr Pchg", "VCU_Status.Motor_Precharge_Contactor_State", "", 0,
     MapScreen::Pit, false, false, false, false, 0.0, 0.0},
    {"motor_temp", "Motor T", "MC_Motor_TempMeasurement.MC_MotorTemp", "C", kSignalTemp, MapScreen::Pit, true,
     false, false, true, 0.0, 75.0},
    {"inverter_temp", "Inverter T", "MC_Motor_TempMeasurement.MC_HeatsinkTemp", "C", 0, MapScreen::Pit, true,
     false, false, true, 0.0, 80.0},
    {"motor_rpm", "Motor RPM", "MC_VelocityMeasurement.MC_MotorVelocity", "rpm", kSignalRpm,
     MapScreen::Both | MapScreen::Scene3d, true, true, false, true, 0.0, 7500.0},
    {"vehicle_speed", "Speed", "MC_VelocityMeasurement.MC_VehicleVelocity", "m/s", kSignalSpeed,
     MapScreen::Both | MapScreen::Scene3d, true, false, false, true, 0.0, 15.0},
    {"throttle", "Throttle", "Pedal_Status.AccelPedal_Main_Pos", "%", 0, MapScreen::Both, true, false, false,
     true, 0.0, 100.0},
    {"brake", "Brake", "Pedal_Status.BrakePedal_Main_Pos", "%", 0, MapScreen::Pit, true, false, false, true,
     0.0, 100.0},
    {"mppt_a_vin", "MPPT A Vin", "MPPT_A_PowerMeasurements.MPPT_Input_Voltage", "V", 0, MapScreen::Pit, false,
     false, false, false, 0.0, 0.0},
    {"mppt_a_iin", "MPPT A Iin", "MPPT_A_PowerMeasurements.MPPT_Input_Current", "A", 0, MapScreen::Pit, false,
     false, false, false, 0.0, 0.0},
    {"mppt_b_vin", "MPPT B Vin", "MPPT_B_PowerMeasurements.MPPT_Input_Voltage", "V", 0, MapScreen::Pit, false,
     false, false, false, 0.0, 0.0},
    {"mppt_b_iin", "MPPT B Iin", "MPPT_B_PowerMeasurements.MPPT_Input_Current", "A", 0, MapScreen::Pit, false,
     false, false, false, 0.0, 0.0},
    {"mppt_c_vin", "MPPT C Vin", "MPPT_C_PowerMeasurements.MPPT_Input_Voltage", "V", 0, MapScreen::Pit, false,
     false, false, false, 0.0, 0.0},
    {"mppt_c_iin", "MPPT C Iin", "MPPT_C_PowerMeasurements.MPPT_Input_Current", "A", 0, MapScreen::Pit, false,
     false, false, false, 0.0, 0.0},
    {"array_power", "Array P", "", "W", 0, MapScreen::Pit, true, false, false, false, 0.0, 0.0},
    {"bps_charge_ok", "Charge OK", "BPS_Status.BPS_Charge_OK", "", 0, MapScreen::Pit, false, false, true, false,
     1.0, 0.0},
    {"bps_regen_ok", "Regen OK", "BPS_Status.BPS_Regen_OK", "", 0, MapScreen::Pit, false, false, true, false,
     1.0, 0.0},
    {"vcu_bps_fault", "VCU BPS", "VCU_Status.VCU_BPS_FAULT_DETECTED", "", 0, MapScreen::Pit, false, false,
     false, true, 0.0, 0.0},
    {"vcu_controls_fault", "VCU Ctrl", "VCU_Status.VCU_CONTROLS_FAULT_DETECTED", "", 0, MapScreen::Pit, false,
     false, false, true, 0.0, 0.0},
    {"vcu_mtr_fault", "VCU Mtr", "VCU_Status.VCU_MTR_FAULT_DETECTED", "", 0, MapScreen::Pit, false, false, false,
     true, 0.0, 0.0},
    {"vcu_pedals_fault", "VCU Ped", "VCU_Status.VCU_PEDALS_FAULT_DETECTED", "", 0, MapScreen::Pit, false, false,
     false, true, 0.0, 0.0},
    {"vcu_steer_fault", "VCU Steer", "VCU_Status.VCU_STEERING_FAULT_DETECTED", "", 0, MapScreen::Pit, false,
     false, false, true, 0.0, 0.0},
    {"vcu_other_fault", "VCU Other", "VCU_Status.VCU_OTHER_FAULT", "", 0, MapScreen::Pit, false, false, false,
     true, 0.0, 0.0},
    {"mppt_a_fault", "MPPT A", "MPPT_A_Status.MPPT_Fault", "", 0, MapScreen::Pit, false, false, false, true,
     0.0, 0.0},
    {"mppt_b_fault", "MPPT B", "MPPT_B_Status.MPPT_Fault", "", 0, MapScreen::Pit, false, false, false, true,
     0.0, 0.0},
    {"mppt_c_fault", "MPPT C", "MPPT_C_Status.MPPT_Fault", "", 0, MapScreen::Pit, false, false, false, true,
     0.0, 0.0},
    {"steering_angle", "Steer", "LWS_Standard.LWS_Angle", "deg", 0, MapScreen::Dynamics | MapScreen::Scene3d,
     true, false, true, true, -540.0, 540.0},
    {"wheel_rpm_fl", "FL RPM", "WheelRPM_FL.RPM", "rpm", 0, MapScreen::Dynamics | MapScreen::Scene3d, true, true,
     false, true, 0.0, 1200.0},
    {"wheel_rpm_fr", "FR RPM", "WheelRPM_FR.RPM", "rpm", 0, MapScreen::Dynamics | MapScreen::Scene3d, true, true,
     false, true, 0.0, 1200.0},
    {"susp_fl", "Susp FL", "Suspension_FL.SUS_Data1", "mm", 0, MapScreen::Dynamics | MapScreen::Scene3d, true,
     true, false, false, 0.0, 0.0},
    {"susp_fr", "Susp FR", "Suspension_FR.SUS_Data1", "mm", 0, MapScreen::Dynamics | MapScreen::Scene3d, true,
     true, false, false, 0.0, 0.0},
    {"susp_rear", "Susp Rr", "Suspension_Back.SUS_Data1", "mm", 0, MapScreen::Dynamics | MapScreen::Scene3d, true,
     true, false, false, 0.0, 0.0},
    {"gps_lat", "GPS Lat", "Lattitude_GPS.Latitude", "deg", 0, MapScreen::None, true, false, false, false, 0.0,
     0.0},
    {"gps_lon", "GPS Lon", "Longitude_GPS.Longitude", "deg", 0, MapScreen::None, true, false, false, false, 0.0,
     0.0},
    {"gps_speed", "GPS Spd", "Speed_GPS.Speed", "m/s", 0, MapScreen::None, true, false, false, true, 0.0, 40.0},
    {"gps_valid", "GPS", "Valid_GPS.Valid", "", 0, MapScreen::None, true, false, true, false, 1.0, 0.0},
    {"imu_ax", "Long G", "IMU_B_Accel.Accel_X", "g", 0, MapScreen::Dynamics | MapScreen::Scene3d, true, true,
     true, true, -2.0, 2.0},
    {"imu_ay", "Lat G", "IMU_B_Accel.Accel_Y", "g", 0, MapScreen::Dynamics | MapScreen::Scene3d, true, true, true,
     true, -2.0, 2.0},
    {"imu_gx", "Gyro X", "IMU_Gyro_Back.Gyro_X", "dps", 0, MapScreen::Scene3d, false, false, false, false, 0.0,
     0.0},
    {"imu_gy", "Gyro Y", "IMU_Gyro_Back.Gyro_Y", "dps", 0, MapScreen::Scene3d, false, false, false, false, 0.0,
     0.0},
    {"yaw_rate", "Yaw", "IMU_Gyro_Back.Gyro_Z", "dps", 0, MapScreen::Dynamics, true, true, true, true, -90.0,
     90.0},
    {"vehicle_speed_mph", "Speed", "", "mph", 0, MapScreen::Dynamics, true, false, false, true, 0.0, 70.0},
    {"slip_fl", "Slip FL", "", "%", 0, MapScreen::Dynamics, true, false, true, true, -20.0, 20.0},
    {"slip_fr", "Slip FR", "", "%", 0, MapScreen::Dynamics, true, false, true, true, -20.0, 20.0},
    {"lws_fault", "LWS", "LWS_Standard.LWS_Fault", "", 0, MapScreen::Dynamics, false, false, false, true, 0.0,
     0.0},
};

constexpr MuxDef kMuxDefs[] = {
    {"cell_voltage", "BPS_Voltage_Aggregate_Arr.BPS_Tap_idx",
     "BPS_Voltage_Aggregate_Arr.BPS_Voltage_Tap_Data", 32},
    {"cell_temp", "BPS_Temperature_Aggregate_Arr.BPS_Tap_idx",
     "BPS_Temperature_Aggregate_Arr.BPS_Temperature_Tap_Data", 32},
};

std::string catalogKey(const ctrl::CatalogSignal& sig) {
    if (sig.message.empty()) {
        return sig.name;
    }
    return sig.message + "." + sig.name;
}

std::size_t ringIndex(const SignalRing& ring, std::size_t logicalOldest) {
    if (ring.count < ring.cap) {
        return logicalOldest;
    }
    return (ring.plotOffset() + logicalOldest) % ring.cap;
}

void fillFromMux(const SignalRing* idx, const SignalRing* val, std::vector<float>& out,
                 std::vector<double>* tapTs, int count) {
    out.assign(static_cast<std::size_t>(count), 0.0f);
    if (tapTs != nullptr) {
        tapTs->assign(static_cast<std::size_t>(count), 0.0);
    }
    if (idx == nullptr || val == nullptr || idx->count == 0 || val->count == 0 || count <= 0) {
        return;
    }
    std::unordered_map<std::uint64_t, int> tapByTs;
    tapByTs.reserve(idx->count);
    for (std::size_t i = 0; i < idx->count; ++i) {
        const std::size_t p = ringIndex(*idx, i);
        const int tap = static_cast<int>(std::lround(idx->v[p]));
        if (tap < 0 || tap >= count) {
            continue;
        }
        tapByTs[static_cast<std::uint64_t>(idx->t[p])] = tap;
    }
    std::vector<char> filled(static_cast<std::size_t>(count), 0);
    for (std::size_t n = val->count; n > 0; --n) {
        const std::size_t i = n - 1;
        const std::size_t p = ringIndex(*val, i);
        const auto it = tapByTs.find(static_cast<std::uint64_t>(val->t[p]));
        if (it == tapByTs.end()) {
            continue;
        }
        const int tap = it->second;
        if (filled[static_cast<std::size_t>(tap)] != 0) {
            continue;
        }
        if (!std::isfinite(val->v[p])) {
            continue;
        }
        out[static_cast<std::size_t>(tap)] = static_cast<float>(val->v[p]);
        if (tapTs != nullptr) {
            (*tapTs)[static_cast<std::size_t>(tap)] = val->t[p];
        }
        filled[static_cast<std::size_t>(tap)] = 1;
    }
}

bool catalogRangeUsable(double mn, double mx) {
    return mx > mn && std::isfinite(mn) && std::isfinite(mx);
}

}  // namespace

SignalMap::SignalMap() {
    scalars_.reserve(sizeof(kScalarDefs) / sizeof(kScalarDefs[0]));
    for (const auto& def : kScalarDefs) {
        ResolvedScalar slot;
        slot.def = &def;
        slot.unit = def.unitHint;
        scalars_.push_back(std::move(slot));
    }
    muxes_.reserve(sizeof(kMuxDefs) / sizeof(kMuxDefs[0]));
    for (const auto& def : kMuxDefs) {
        ResolvedMux slot;
        slot.def = &def;
        slot.count = def.count;
        muxes_.push_back(slot);
    }
}

void SignalMap::reset() {
    for (auto& slot : scalars_) {
        slot.id = 0;
        slot.mapped = false;
        slot.unit = slot.def != nullptr ? slot.def->unitHint : "";
        slot.catalogMin = 0.0;
        slot.catalogMax = 0.0;
        slot.haveCatalogRange = false;
    }
    for (auto& slot : muxes_) {
        slot.indexId = 0;
        slot.valueId = 0;
        slot.mapped = false;
    }
    resolvedOnce_ = false;
}

void SignalMap::resolve(const std::vector<ctrl::CatalogSignal>& catalog, ClientArena& arena) {
    std::unordered_map<std::string, const ctrl::CatalogSignal*> byKey;
    byKey.reserve(catalog.size());
    for (const auto& sig : catalog) {
        byKey.emplace(catalogKey(sig), &sig);
    }

    int hits = 0;
    int misses = 0;
    for (auto& slot : scalars_) {
        slot.id = 0;
        slot.mapped = false;
        slot.unit = slot.def->unitHint;
        slot.catalogMin = 0.0;
        slot.catalogMax = 0.0;
        slot.haveCatalogRange = false;
        if (slot.def->signal[0] == '\0') {
            continue;
        }
        const auto it = byKey.find(slot.def->signal);
        if (it == byKey.end()) {
            arena.pushLog(std::string("signal_map miss ") + slot.def->role + " " + slot.def->signal);
            ++misses;
            continue;
        }
        slot.mapped = true;
        slot.id = it->second->id;
        if (!it->second->unit.empty()) {
            slot.unit = it->second->unit;
        }
        slot.catalogMin = it->second->min;
        slot.catalogMax = it->second->max;
        slot.haveCatalogRange = catalogRangeUsable(it->second->min, it->second->max);
        ++hits;
    }
    for (auto& slot : muxes_) {
        slot.indexId = 0;
        slot.valueId = 0;
        slot.mapped = false;
        const auto idx = byKey.find(slot.def->indexSignal);
        const auto val = byKey.find(slot.def->valueSignal);
        if (idx == byKey.end() || val == byKey.end()) {
            arena.pushLog(std::string("signal_map miss ") + slot.def->role + " mux");
            ++misses;
            continue;
        }
        slot.mapped = true;
        slot.indexId = idx->second->id;
        slot.valueId = val->second->id;
        ++hits;
    }
    resolvedOnce_ = true;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "signal_map resolved hits=%d misses=%d", hits, misses);
    arena.pushLog(buf);
}

void SignalMap::subscribe(Client& client, MapScreen screen) const {
    std::vector<std::uint32_t> ids;
    ids.reserve(scalars_.size() + muxes_.size() * 2);
    for (const auto& slot : scalars_) {
        if (!slot.mapped || slot.id == 0 || !mapOn(slot.def->screens, screen)) {
            continue;
        }
        ids.push_back(slot.id);
    }
    if (mapOn(MapScreen::Pit, screen)) {
        for (const auto& slot : muxes_) {
            if (!slot.mapped) {
                continue;
            }
            ids.push_back(slot.indexId);
            ids.push_back(slot.valueId);
        }
    }
    client.ensureSubscribed(ids);
}

const ResolvedScalar* SignalMap::scalar(std::string_view role) const {
    for (const auto& slot : scalars_) {
        if (slot.def != nullptr && role == slot.def->role) {
            return &slot;
        }
    }
    return nullptr;
}

const ResolvedMux* SignalMap::mux(std::string_view role) const {
    for (const auto& slot : muxes_) {
        if (slot.def != nullptr && role == slot.def->role) {
            return &slot;
        }
    }
    return nullptr;
}

bool SignalMap::cellsMapped() const {
    const ResolvedMux* v = mux("cell_voltage");
    return v != nullptr && v->mapped;
}

bool SignalMap::valueAlarmed(const ResolvedScalar& slot, double value) {
    if (slot.def->hasWarnHigh && value > slot.def->warnHigh) {
        return true;
    }
    if (slot.def->hasWarnLow && value < slot.def->warnLow) {
        return true;
    }
    return false;
}

Vital SignalMap::syntheticVital(const ResolvedScalar& slot, const ClientArena& arena) const {
    Vital v;
    v.role = slot.def->role;
    v.label = slot.def->label;
    v.signal = slot.def->signal;
    v.unit = slot.def->unitHint;
    if (slot.def->syntheticId == 0) {
        return v;
    }
    if (const SignalDesc* desc = findSignal(slot.def->syntheticId)) {
        v.label = desc->label;
        v.unit = desc->unit;
        v.hasBar = desc->hasLow || desc->hasHigh;
        v.barMin = desc->hasLow ? desc->warnLow : 0.0;
        v.barMax = desc->hasHigh ? desc->warnHigh : (desc->hasLow ? 100.0 : 1.0);
        v.alarm = arena.has(desc->id) && signalAlarmed(*desc, arena.latest(desc->id));
    }
    v.id = slot.def->syntheticId;
    v.synthetic = true;
    v.have = arena.has(slot.def->syntheticId);
    v.value = arena.latest(slot.def->syntheticId);
    return v;
}

Vital SignalMap::vital(std::string_view role, const ClientArena& arena) const {
    const ResolvedScalar* slot = scalar(role);
    Vital v;
    if (slot == nullptr || slot->def == nullptr) {
        return v;
    }
    v.role = slot->def->role;
    v.label = slot->def->label;
    v.signal = slot->def->signal;
    v.unit = slot->unit.c_str();
    v.id = slot->id;

    if (role == "pack_soc") {
        const Vital packV = vital("pack_voltage", arena);
        if (packV.mapped && packV.have) {
            v.derived = true;
            v.have = true;
            v.value = estimatePackSoc(packV.value);
            v.unit = "%";
            v.hasBar = true;
            v.barMin = 0.0;
            v.barMax = 100.0;
            v.alarm = slot->def->hasWarnLow && v.value < slot->def->warnLow;
            return v;
        }
        return syntheticVital(*slot, arena);
    }
    if (role == "vehicle_speed_mph") {
        const Vital spd = vital("vehicle_speed", arena);
        v.unit = "mph";
        v.hasBar = true;
        v.barMin = 0.0;
        v.barMax = 70.0;
        v.derived = true;
        if (spd.mapped && spd.have && std::isfinite(spd.value)) {
            v.mapped = true;
            v.have = true;
            v.value = spd.value * kMpsToMph;
            v.alarm = slot->def->hasWarnHigh && v.value > slot->def->warnHigh;
        }
        return v;
    }
    if (role == "slip_fl" || role == "slip_fr") {
        const Vital rpm = vital(role == "slip_fl" ? "wheel_rpm_fl" : "wheel_rpm_fr", arena);
        const Vital spd = vital("vehicle_speed", arena);
        v.unit = "%";
        v.hasBar = true;
        v.barMin = -25.0;
        v.barMax = 25.0;
        v.derived = true;
        if (rpm.mapped && rpm.have && spd.mapped && spd.have && std::isfinite(rpm.value) &&
            std::isfinite(spd.value) && std::abs(spd.value) >= kMinGroundSpeedMps) {
            v.mapped = true;
            v.have = true;
            v.value = slipPercent(wheelSpeedMps(rpm.value), spd.value);
            v.alarm = std::abs(v.value) > 20.0;
        }
        return v;
    }
    if (role == "array_power") {
        double watts = 0.0;
        int pairs = 0;
        static constexpr const char* kVin[] = {"mppt_a_vin", "mppt_b_vin", "mppt_c_vin"};
        static constexpr const char* kIin[] = {"mppt_a_iin", "mppt_b_iin", "mppt_c_iin"};
        for (int i = 0; i < 3; ++i) {
            const Vital vin = vital(kVin[i], arena);
            const Vital iin = vital(kIin[i], arena);
            if (vin.mapped && iin.mapped && vin.have && iin.have) {
                watts += vin.value * iin.value;
                ++pairs;
            }
        }
        v.unit = "W";
        v.hasBar = true;
        v.barMin = 0.0;
        v.barMax = 1500.0;
        if (pairs > 0) {
            v.derived = true;
            v.have = true;
            v.value = watts;
            return v;
        }
        return v;
    }

    if (slot->mapped) {
        v.mapped = true;
        v.have = arena.has(slot->id);
        v.value = arena.latest(slot->id);
        v.alarm = v.have && valueAlarmed(*slot, v.value);
        if (slot->haveCatalogRange) {
            v.hasBar = true;
            v.barMin = slot->catalogMin;
            v.barMax = slot->catalogMax;
        } else if (slot->def->hasWarnLow || slot->def->hasWarnHigh) {
            v.hasBar = true;
            v.barMin = slot->def->hasWarnLow ? slot->def->warnLow : 0.0;
            v.barMax = slot->def->hasWarnHigh ? slot->def->warnHigh : (v.barMin + 1.0);
        }
        return v;
    }
    return syntheticVital(*slot, arena);
}

void SignalMap::fillCells(ClientArena& arena) const {
    const ResolvedMux* volt = mux("cell_voltage");
    const ResolvedMux* temp = mux("cell_temp");
    if (volt == nullptr || !volt->mapped) {
        return;
    }
    std::vector<float> voltages;
    std::vector<float> temps;
    std::vector<double> tapTs;
    fillFromMux(arena.ring(volt->indexId), arena.ring(volt->valueId), voltages, &tapTs, volt->count);
    if (temp != nullptr && temp->mapped) {
        fillFromMux(arena.ring(temp->indexId), arena.ring(temp->valueId), temps, nullptr, temp->count);
    } else {
        temps.assign(static_cast<std::size_t>(volt->count), 0.0f);
    }
    if (temps.size() < voltages.size()) {
        temps.resize(voltages.size(), 0.0f);
    }
    arena.setMappedCells(std::move(voltages), std::move(temps), std::move(tapTs));
}

CellCallout SignalMap::cellCallout(const std::vector<float>& voltages) {
    CellCallout c;
    int seen = 0;
    for (int i = 0; i < static_cast<int>(voltages.size()); ++i) {
        const float v = voltages[static_cast<std::size_t>(i)];
        if (!std::isfinite(v) || v <= 0.05f) {
            continue;
        }
        if (seen == 0 || v < c.minV) {
            c.minV = v;
            c.minIndex = i;
        }
        if (seen == 0 || v > c.maxV) {
            c.maxV = v;
            c.maxIndex = i;
        }
        ++seen;
    }
    if (seen > 0) {
        c.deltaV = c.maxV - c.minV;
        c.valid = true;
    }
    return c;
}

double SignalMap::estimatePackSoc(double packVoltage) {
    const double ratio = (packVoltage - 83.2) / 51.2;
    if (!(ratio > 0.0)) {
        return 0.0;
    }
    double soc = 100.0 * std::pow(ratio, 0.55);
    if (soc < 0.0) {
        return 0.0;
    }
    if (soc > 100.0) {
        return 100.0;
    }
    return soc;
}

}  // namespace photon::ui
