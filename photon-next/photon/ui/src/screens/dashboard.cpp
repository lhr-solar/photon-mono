#include "photon_ui/alarms.hpp"
#include "photon_ui/client.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/signal_map.hpp"
#include "photon_ui/theme.hpp"
#include "photon_ui/video.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace photon::ui {
namespace {

constexpr float kPi = 3.14159265f;

bool on(const Vital& v) { return v.have && v.value > 0.5; }

float num(const Vital& v, float fallback = 0.0f) {
    return v.have && std::isfinite(v.value) ? static_cast<float>(v.value) : fallback;
}

float unitPct(const Vital& v) {
    const float x = num(v);
    return std::fabs(x) <= 1.5f ? x * 100.0f : x;
}

const char* yn(const Vital& v, const char* yes, const char* no) {
    if (!v.have) {
        return "--";
    }
    return on(v) ? yes : no;
}

const char* flt(const Vital& v) { return yn(v, "FLT", "ok"); }

const char* chargerStatus(std::uint8_t s) {
    switch (s) {
        case 0:
            return "Disabled";
        case 1:
            return "Done";
        case 2:
            return "Charging";
        case 3:
            return "Fault";
        default:
            return "?";
    }
}

std::string vcuFaultDetail(std::uint8_t code) {
    if (code == 0) {
        return "No Fault";
    }
    std::string out;
    auto append = [&](const char* s) {
        if (!out.empty()) {
            out += "+";
        }
        out += s;
    };
    if (code & (1u << 0)) {
        append("BPS");
    }
    if (code & (1u << 1)) {
        append("Ctrl");
    }
    if (code & (1u << 2)) {
        append("Motor");
    }
    if (code & (1u << 3)) {
        append("Pedal");
    }
    if (code & (1u << 4)) {
        append("Steer");
    }
    return out.empty() ? "Fault" : out;
}

void drawHeart(ImDrawList* dl, ImVec2 center, float size, ImU32 color) {
    const float s = size * 0.5f;
    const float sx = s / 8.0f;
    const float sy = s / 7.5f;
    const float ox = center.x;
    const float oy = center.y + s * 0.1f;
    auto P = [&](float x, float y) { return ImVec2(ox + (x - 8.0f) * sx, oy + (y - 7.5f) * sy); };
    dl->PathClear();
    dl->PathLineTo(P(8.0f, 15.0f));
    dl->PathLineTo(P(1.24f, 8.24f));
    dl->PathBezierCubicCurveTo(P(0.45f, 7.45f), P(0.0f, 6.37f), P(0.0f, 5.24f));
    dl->PathBezierCubicCurveTo(P(0.0f, 2.81f), P(1.81f, 1.0f), P(4.05f, 1.0f));
    dl->PathBezierCubicCurveTo(P(5.28f, 1.0f), P(6.45f, 1.56f), P(7.22f, 2.52f));
    dl->PathLineTo(P(8.0f, 3.5f));
    dl->PathLineTo(P(8.78f, 2.52f));
    dl->PathBezierCubicCurveTo(P(9.55f, 1.56f), P(10.72f, 1.0f), P(11.95f, 1.0f));
    dl->PathBezierCubicCurveTo(P(14.19f, 1.0f), P(16.0f, 2.81f), P(16.0f, 5.05f));
    dl->PathBezierCubicCurveTo(P(16.0f, 6.37f), P(15.55f, 7.45f), P(14.76f, 8.24f));
    dl->PathLineTo(P(8.0f, 15.0f));
    dl->PathFillConvex(color);
}

void drawLeftArrow(ImDrawList* dl, ImVec2 center, float size, ImU32 color) {
    const float s = size * 0.5f;
    const float sx = s / 12.0f;
    const float sy = s / 12.0f;
    dl->AddTriangleFilled(ImVec2(center.x + 7.0f * sx, center.y - 9.0f * sy),
                          ImVec2(center.x + 7.0f * sx, center.y + 9.0f * sy),
                          ImVec2(center.x - 7.0f * sx, center.y), color);
}

void drawRightArrow(ImDrawList* dl, ImVec2 center, float size, ImU32 color) {
    const float s = size * 0.5f;
    const float sx = s / 12.0f;
    const float sy = s / 12.0f;
    dl->AddTriangleFilled(ImVec2(center.x - 7.0f * sx, center.y - 9.0f * sy),
                          ImVec2(center.x - 7.0f * sx, center.y + 9.0f * sy),
                          ImVec2(center.x + 7.0f * sx, center.y), color);
}

void drawBrakeIcon(ImDrawList* dl, ImVec2 center, float size, ImU32 color) {
    dl->AddCircleFilled(center, size * 0.42f, color, 32);
}

void drawCruise(ImDrawList* dl, ImVec2 center, float size, ImU32 color) {
    const float r = size * 0.42f;
    float thick = size * 0.06f;
    if (thick < 1.0f) {
        thick = 1.0f;
    }
    const float startA = kPi * 0.75f;
    const float endA = kPi * 2.25f;
    dl->PathArcTo(center, r, startA, endA, 48);
    dl->PathStroke(color, 0, thick * 1.5f);
    for (int i = 0; i <= 8; ++i) {
        const float a = startA + (endA - startA) * static_cast<float>(i) / 8.0f;
        dl->AddLine(ImVec2(center.x + std::cos(a) * r * 0.82f, center.y + std::sin(a) * r * 0.82f),
                    ImVec2(center.x + std::cos(a) * r * 0.95f, center.y + std::sin(a) * r * 0.95f), color,
                    thick);
    }
    const float needle = kPi * 1.1f;
    dl->AddLine(center,
                ImVec2(center.x + std::cos(needle) * r * 0.65f, center.y + std::sin(needle) * r * 0.65f),
                color, thick * 2.0f);
    dl->AddCircleFilled(center, thick * 2.0f, color, 12);
}

void drawRegen(ImDrawList* dl, ImVec2 center, float size, ImU32 color) {
    const float r = size * 0.40f;
    float thick = size * 0.06f;
    if (thick < 1.0f) {
        thick = 1.0f;
    }
    for (int i = 0; i < 3; ++i) {
        const float base = kPi * 2.0f * static_cast<float>(i) / 3.0f - kPi * 0.5f;
        const float a1 = base - 0.35f;
        const float a2 = base + 0.35f;
        dl->PathArcTo(center, r, a1, a2, 16);
        dl->PathStroke(color, 0, thick * 1.5f);
        const float head = size * 0.1f;
        const float tipX = center.x + std::cos(a2) * r;
        const float tipY = center.y + std::sin(a2) * r;
        const float perp = a2 + kPi * 0.5f;
        dl->AddTriangleFilled(ImVec2(tipX + std::cos(a2) * head, tipY + std::sin(a2) * head),
                              ImVec2(tipX + std::cos(perp) * head * 0.5f, tipY + std::sin(perp) * head * 0.5f),
                              ImVec2(tipX - std::cos(perp) * head * 0.5f, tipY - std::sin(perp) * head * 0.5f),
                              color);
    }
    const float bw = size * 0.08f;
    const float bh = size * 0.22f;
    dl->PathClear();
    dl->PathLineTo(ImVec2(center.x + bw * 0.3f, center.y - bh));
    dl->PathLineTo(ImVec2(center.x - bw * 0.8f, center.y + bh * 0.1f));
    dl->PathLineTo(ImVec2(center.x + bw * 0.1f, center.y + bh * 0.1f));
    dl->PathLineTo(ImVec2(center.x - bw * 0.3f, center.y + bh));
    dl->PathLineTo(ImVec2(center.x + bw * 0.8f, center.y - bh * 0.1f));
    dl->PathLineTo(ImVec2(center.x - bw * 0.1f, center.y - bh * 0.1f));
    dl->PathLineTo(ImVec2(center.x + bw * 0.3f, center.y - bh));
    dl->PathFillConvex(color);
}

ImVec4 batteryColor(float soc) {
    if (soc < 20.0f) {
        return theme::danger();
    }
    if (soc < 40.0f) {
        return theme::warning();
    }
    return theme::primary();
}

ImVec4 speedColor(int speed) {
    if (speed > 80) {
        return theme::danger();
    }
    if (speed > 30) {
        return theme::warning();
    }
    return theme::primary();
}

struct MotorFault {
    const char* role;
    const char* message;
};

constexpr MotorFault kMotorFaults[] = {
    {"mc_fault_hw_oc", "Hardware overcurrent"},
    {"mc_fault_sw_oc", "Software overcurrent"},
    {"mc_fault_bus_ov", "DC bus overvoltage"},
    {"mc_fault_hall", "Bad motor position Hall sequence"},
    {"mc_fault_wd", "Watchdog caused last reset"},
    {"mc_fault_cfg", "Configuration read fault"},
    {"mc_fault_15v", "15 V rail undervoltage"},
    {"mc_fault_desat", "Desaturation fault"},
    {"mc_fault_overspeed", "Motor overspeed"},
};

struct MotorLimit {
    const char* role;
    const char* label;
};

constexpr MotorLimit kMotorLimits[] = {
    {"mc_limit_i", "Motor Current"},
    {"mc_limit_vel", "Velocity"},
    {"mc_limit_bus_i", "Bus Current"},
    {"mc_limit_bus_v_hi", "Bus Voltage High"},
    {"mc_limit_bus_v_lo", "Bus Voltage Low"},
    {"mc_limit_temp", "IPM/Motor Temp"},
    {"mc_limit_pwm", "Output PWM"},
};

struct BpsExtrema {
    int maxTempIdx = -1;
    int minVoltIdx = -1;
    int maxVoltIdx = -1;
    float maxTemp = 0.0f;
    float minVolt = 0.0f;
    float maxVolt = 0.0f;
};

BpsExtrema computeExtrema(const std::vector<float>& volts, const std::vector<float>& temps) {
    BpsExtrema e;
    float maxT = -1.0e9f;
    float minV = 1.0e9f;
    float maxV = -1.0e9f;
    for (int i = 0; i < static_cast<int>(volts.size()); ++i) {
        const float v = volts[static_cast<std::size_t>(i)];
        if (!std::isfinite(v) || v <= 0.05f) {
            continue;
        }
        if (v < minV) {
            minV = v;
            e.minVoltIdx = i;
            e.minVolt = v;
        }
        if (v > maxV) {
            maxV = v;
            e.maxVoltIdx = i;
            e.maxVolt = v;
        }
    }
    for (int i = 0; i < static_cast<int>(temps.size()); ++i) {
        const float t = temps[static_cast<std::size_t>(i)];
        if (!std::isfinite(t)) {
            continue;
        }
        if (t > maxT) {
            maxT = t;
            e.maxTempIdx = i;
            e.maxTemp = t;
        }
    }
    return e;
}

enum class Gear : int { Forward = 0, Neutral = 1, Reverse = 2 };
enum class Turn : int { None = 0, Left = 1, Right = 2 };

struct DashView {
    Vital packSoc{};
    Vital packV{};
    Vital packI{};
    Vital packT{};
    Vital busV{};
    Vital motorT{};
    Vital heatsinkT{};
    Vital driveVel{};
    Vital driveCur{};
    Vital powerCmd{};
    Vital preMotor{};
    Vital preBatt{};
    Vital hvPlus{};
    Vital hvMinus{};
    Vital arrayPre{};
    Vital arrayMain{};
    Vital motorPre{};
    Vital motorMain{};
    Vital ignOff{};
    Vital ignArray{};
    Vital ignMotor{};
    Vital throttle{};
    Vital brake{};
    Vital brakeP1{};
    Vital brakeP2{};
    Vital brakeP1F{};
    Vital brakeP2F{};
    Vital accelRed{};
    Vital brakeRed{};
    Vital accelMainF{};
    Vital accelRedF{};
    Vital brakeMainF{};
    Vital brakeRedF{};
    Vital gearF{};
    Vital gearN{};
    Vital gearR{};
    Vital blinkL{};
    Vital blinkR{};
    Vital hazard{};
    Vital cruise{};
    Vital cruiseSet{};
    Vital regen{};
    Vital regenAct{};
    Vital horn{};
    Vital ptt{};
    Vital bps{};
    Vital bpsCharge{};
    Vital bpsRegen{};
    Vital motorReady{};
    Vital vcuBps{};
    Vital vcuCtrl{};
    Vital vcuMtr{};
    Vital vcuPed{};
    Vital vcuSteer{};
    Vital vcuOther{};
    Vital vcuFsm{};
    Vital vcuPedWd{};
    Vital vcuDrvWd{};
    Vital vcuRegenOk{};
    Vital vcuRegenAct{};
    Vital auxV{};
    Vital auxI{};
    Vital mpptVin{};
    Vital mpptIin{};
    Vital mpptVout{};
    Vital mpptIout{};
    Vital mpptHs{};
    Vital mpptAmb{};
    Vital mpptFault{};
    Vital mpptMode{};
    Vital coolant1{};
    Vital coolant2{};
    Vital flow1{};
    Vital flow2{};
    Vital pumpDuty{};
    Vital pumpFault{};
    Vital ltcHvSel{};
    Vital ltcHvF{};
    Vital ltcHvV{};
    Vital ltcSuppSel{};
    Vital ltcSuppF{};
    Vital ltcSuppV{};
    Vital lvEnSupp{};
    Vital lvEnPsu{};
    Vital camBackup{};
    Vital camLeft{};
    Vital camRight{};
    Vital ctrlFault{};
    Vital lightF{};
    Vital lightL{};
    Vital lightR{};
    Vital lightRr{};
    Vital lightC{};
    Vital phaseB{};
    Vital phaseC{};
    Vital bemfQ{};
    Vital bemfD{};
    Vital suppChg{};
    Vital vicorV{};
    Vital vicorI{};
    Vital steer{};
    Vital lwsFault{};
    Vital gpsSpeed{};
    Vital limits[7]{};
    Vital mcFaults[9]{};
    std::vector<float> cellV;
    std::vector<float> cellT;
    int speed = 0;
    Gear gear = Gear::Neutral;
    Turn turn = Turn::None;
    bool brakeEngaged = false;
    bool canFault = false;
    bool canWarn = false;
    std::uint8_t bpsCode = 0;
    std::uint8_t vcuCode = 0;
    std::uint8_t heartbeat = 0;
    bool bpsStale = false;
    bool vcuStale = false;
    std::vector<std::string> faultLines;
    std::vector<std::string> limitLines;
    std::string motorFaultDetail;
};

DashView fillView(UiContext& ui, std::uint8_t heartbeat) {
    DashView d;
    const SignalMap& map = ui.arena.signalMap();
    auto take = [&](const char* role) { return map.vital(role, ui.arena); };
    d.packSoc = take("pack_soc");
    d.packV = take("pack_voltage");
    d.packI = take("pack_current");
    d.packT = take("pack_avg_temp");
    d.busV = take("mc_bus_voltage");
    d.motorT = take("motor_temp");
    d.heatsinkT = take("inverter_temp");
    d.driveVel = take("drive_vel");
    d.driveCur = take("drive_cur");
    d.powerCmd = take("power_cmd");
    d.preMotor = take("pre_motor");
    d.preBatt = take("pre_batt");
    d.hvPlus = take("contactor_hv_plus");
    d.hvMinus = take("contactor_hv_minus");
    d.arrayPre = take("contactor_array_precharge");
    d.arrayMain = take("contactor_array");
    d.motorPre = take("contactor_motor_precharge");
    d.motorMain = take("contactor_motor");
    d.ignOff = take("ign_off");
    d.ignArray = take("ign_array");
    d.ignMotor = take("ign_motor");
    d.throttle = take("throttle");
    d.brake = take("brake");
    d.brakeP1 = take("brake_p1");
    d.brakeP2 = take("brake_p2");
    d.brakeP1F = take("brake_p1_fault");
    d.brakeP2F = take("brake_p2_fault");
    d.accelRed = take("accel_redun");
    d.brakeRed = take("brake_redun");
    d.accelMainF = take("accel_main_fault");
    d.accelRedF = take("accel_redun_fault");
    d.brakeMainF = take("brake_main_fault");
    d.brakeRedF = take("brake_redun_fault");
    d.gearF = take("gear_forward");
    d.gearN = take("gear_neutral");
    d.gearR = take("gear_reverse");
    d.blinkL = take("blinker_left");
    d.blinkR = take("blinker_right");
    d.hazard = take("hazard");
    d.cruise = take("cruise_enable");
    d.cruiseSet = take("cruise_set");
    d.regen = take("regen_enable");
    d.regenAct = take("regen_activate");
    d.horn = take("horn");
    d.ptt = take("ptt");
    d.bps = take("bps_state");
    d.bpsCharge = take("bps_charge_ok");
    d.bpsRegen = take("bps_regen_ok");
    d.motorReady = take("motor_ready");
    d.vcuBps = take("vcu_bps_fault");
    d.vcuCtrl = take("vcu_controls_fault");
    d.vcuMtr = take("vcu_mtr_fault");
    d.vcuPed = take("vcu_pedals_fault");
    d.vcuSteer = take("vcu_steer_fault");
    d.vcuOther = take("vcu_other_fault");
    d.vcuFsm = take("vcu_fsm");
    d.vcuPedWd = take("vcu_pedals_wd");
    d.vcuDrvWd = take("vcu_driver_wd");
    d.vcuRegenOk = take("vcu_regen_ok");
    d.vcuRegenAct = take("vcu_regen_active");
    d.auxV = take("supp_voltage");
    d.auxI = take("supp_current");
    d.mpptVin = take("mppt_a_vin");
    d.mpptIin = take("mppt_a_iin");
    d.mpptVout = take("mppt_a_vout");
    d.mpptIout = take("mppt_a_iout");
    d.mpptHs = take("mppt_a_heatsink");
    d.mpptAmb = take("mppt_a_ambient");
    d.mpptFault = take("mppt_a_fault");
    d.mpptMode = take("mppt_a_mode");
    d.coolant1 = take("coolant_1");
    d.coolant2 = take("coolant_2");
    d.flow1 = take("flow_1");
    d.flow2 = take("flow_2");
    d.pumpDuty = take("pump_duty");
    d.pumpFault = take("pump_fault");
    d.ltcHvSel = take("ltc_hvdcdc_sel");
    d.ltcHvF = take("ltc_hvdcdc_fault");
    d.ltcHvV = take("ltc_hvdcdc_valid");
    d.ltcSuppSel = take("ltc_supp_sel");
    d.ltcSuppF = take("ltc_supp_fault");
    d.ltcSuppV = take("ltc_supp_valid");
    d.lvEnSupp = take("lv_en_supp");
    d.lvEnPsu = take("lv_en_psu");
    d.camBackup = take("cam_backup");
    d.camLeft = take("cam_left");
    d.camRight = take("cam_right");
    d.ctrlFault = take("controls_leader_fault");
    d.lightF = take("light_front");
    d.lightL = take("light_left");
    d.lightR = take("light_right");
    d.lightRr = take("light_rear");
    d.lightC = take("light_canopy");
    d.phaseB = take("mc_phase_b");
    d.phaseC = take("mc_phase_c");
    d.bemfQ = take("mc_bemf_q");
    d.bemfD = take("mc_bemf_d");
    d.suppChg = take("supp_charger");
    d.vicorV = take("supp_vicor_v");
    d.vicorI = take("supp_vicor_i");
    d.steer = take("steering_angle");
    d.lwsFault = take("lws_fault");
    d.gpsSpeed = take("gps_speed");
    for (int i = 0; i < 7; ++i) {
        d.limits[i] = take(kMotorLimits[static_cast<std::size_t>(i)].role);
    }
    for (int i = 0; i < 9; ++i) {
        d.mcFaults[i] = take(kMotorFaults[static_cast<std::size_t>(i)].role);
    }
    d.cellV = ui.arena.cells().voltage;
    d.cellT = ui.arena.cells().temp;
    d.heartbeat = heartbeat;

    const Vital mph = take("vehicle_speed_mph");
    if (mph.have && std::isfinite(mph.value) && std::fabs(mph.value) <= 200.0) {
        d.speed = static_cast<int>(std::lround(std::fabs(mph.value)));
    } else if (d.gpsSpeed.have && std::isfinite(d.gpsSpeed.value) && d.gpsSpeed.value >= 0.0 &&
               d.gpsSpeed.value <= 200.0) {
        d.speed = static_cast<int>(std::lround(std::fabs(d.gpsSpeed.value) * kMpsToMph));
    }

    if (on(d.gearF)) {
        d.gear = Gear::Forward;
    }
    if (on(d.gearR)) {
        d.gear = Gear::Reverse;
    }
    if (on(d.gearN)) {
        d.gear = Gear::Neutral;
    }

    if (on(d.blinkL)) {
        d.turn = Turn::Left;
    } else if (on(d.blinkR)) {
        d.turn = Turn::Right;
    }

    d.brakeEngaged = num(d.brake) > 5.0f || num(d.brakeP1) > 10.0f || num(d.brakeP2) > 10.0f;
    d.bpsCode = static_cast<std::uint8_t>(std::lround(std::max(0.0, d.bps.value)));
    if (on(d.vcuBps)) {
        d.vcuCode |= 1u << 0;
    }
    if (on(d.vcuCtrl)) {
        d.vcuCode |= 1u << 1;
    }
    if (on(d.vcuMtr)) {
        d.vcuCode |= 1u << 2;
    }
    if (on(d.vcuPed)) {
        d.vcuCode |= 1u << 3;
    }
    if (on(d.vcuSteer)) {
        d.vcuCode |= 1u << 4;
    }
    if (on(d.vcuOther) && d.vcuCode == 0) {
        d.vcuCode |= 1u << 1;
    }

    int motorCount = 0;
    for (int i = 0; i < 9; ++i) {
        if (!on(d.mcFaults[i])) {
            continue;
        }
        if (motorCount == 0) {
            d.motorFaultDetail = kMotorFaults[static_cast<std::size_t>(i)].message;
        }
        ++motorCount;
        d.faultLines.emplace_back(std::string("Motor - ") + kMotorFaults[static_cast<std::size_t>(i)].message);
    }
    if (motorCount > 1) {
        d.motorFaultDetail += " +" + std::to_string(motorCount - 1) + " more";
    }
    if (d.bps.have && d.bpsCode != 0) {
        d.faultLines.insert(d.faultLines.begin(), std::string("BPS - ") + bpsFaultName(d.bpsCode));
    }
    if (d.vcuCode != 0) {
        d.faultLines.emplace_back(std::string("VCU - ") + vcuFaultDetail(d.vcuCode));
    }
    for (int i = 0; i < 7; ++i) {
        if (on(d.limits[i])) {
            d.limitLines.emplace_back(kMotorLimits[static_cast<std::size_t>(i)].label);
            d.canWarn = true;
        }
    }
    d.canFault = (d.bps.have && d.bpsCode != 0) || motorCount != 0 || d.vcuCode != 0;
    for (int i = 0; i < 3; ++i) {
        const EcuPill pill = ui.arena.alarms().ecuPill(i);
        if (std::strcmp(pill.name, "BPS") == 0) {
            d.bpsStale = pill.health == EcuHealth::Stale || pill.health == EcuHealth::Silent;
        } else if (std::strcmp(pill.name, "VCU") == 0) {
            d.vcuStale = pill.health == EcuHealth::Stale || pill.health == EcuHealth::Silent;
        }
    }
    return d;
}

ImVec4 panelBg(const DashView& d) {
    if (d.canFault) {
        return theme::mixColor(theme::card(), theme::danger(), 0.10f);
    }
    if (d.canWarn) {
        return theme::mixColor(theme::card(), theme::accent(), 0.10f);
    }
    return theme::card();
}

ImVec4 screenBg(const DashView& d) {
    if (d.canFault) {
        return theme::mixColor(theme::background(), theme::danger(), 0.06f);
    }
    if (d.canWarn) {
        return theme::mixColor(theme::background(), theme::accent(), 0.06f);
    }
    return theme::background();
}

bool beginPanel(const char* id, ImVec2 size, ImVec4 bg, ImVec2 pad) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    return ImGui::BeginChild(id, size, ImGuiChildFlags_None,
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
}

void endPanel() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void stamp(ImDrawList* dl, ImFont* font, float fs, ImVec2 p, ImVec4 col, const char* text) {
    if (font != nullptr) {
        dl->AddText(font, fs, p, theme::u32(col), text);
    } else {
        dl->AddText(p, theme::u32(col), text);
    }
}

ImVec2 textSize(ImFont* font, float fs, const char* text) {
    if (font != nullptr) {
        return font->CalcTextSizeA(fs, 1.0e9f, 0.0f, text);
    }
    return ImGui::CalcTextSize(text);
}

bool formatBpsDetail(const DashView& d, char* buf, std::size_t n) {
    if (d.bpsCode == 0) {
        return false;
    }
    const BpsExtrema e = computeExtrema(d.cellV, d.cellT);
    switch (d.bpsCode) {
        case 1:
            if (e.maxVoltIdx >= 0) {
                std::snprintf(buf, n, "BPS: Mod %d (%.2fV) OVERVOLT", e.maxVoltIdx, static_cast<double>(e.maxVolt));
                return true;
            }
            break;
        case 2:
            if (e.minVoltIdx >= 0) {
                std::snprintf(buf, n, "BPS: Mod %d (%.2fV) UNDERVOLT", e.minVoltIdx, static_cast<double>(e.minVolt));
                return true;
            }
            break;
        case 4:
            if (e.maxTempIdx >= 0) {
                std::snprintf(buf, n, "BPS: Mod %d (%.1fC) OVERTEMP", e.maxTempIdx, static_cast<double>(e.maxTemp));
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

struct ActiveFault {
    std::string label;
    std::string detail;
};

bool activeFault(const DashView& d, ActiveFault& out) {
    if (d.bps.have && d.bpsCode != 0) {
        out.label = "BPS Fault";
        out.detail = bpsFaultName(d.bpsCode);
        return true;
    }
    if (!d.motorFaultDetail.empty()) {
        out.label = "Motor Fault";
        out.detail = d.motorFaultDetail;
        return true;
    }
    if (d.vcuCode != 0) {
        out.label = "VCU Fault";
        out.detail = vcuFaultDetail(d.vcuCode);
        return true;
    }
    return false;
}

void drawFaultBanner(const DashView& d) {
    ActiveFault info;
    if (!activeFault(d, info)) {
        return;
    }
    char text[128];
    std::snprintf(text, sizeof(text), "%s: %s", info.label.c_str(), info.detail.c_str());
    for (char* c = text; *c; ++c) {
        *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
    }
    ImFont* font = theme::uiFont();
    ImGuiIO& io = ImGui::GetIO();
    float fs = std::min(io.DisplaySize.x * 0.052f, 56.0f) * 2.0f;
    ImVec2 tsz = textSize(font, fs, text);
    const float padX = 14.0f;
    const float padY = 7.0f;
    const float maxW = io.DisplaySize.x * 0.92f;
    if (tsz.x + padX * 2.0f > maxW && tsz.x > 1.0f) {
        fs *= (maxW - padX * 2.0f) / tsz.x;
        tsz = textSize(font, fs, text);
    }
    const ImVec2 plate(tsz.x + padX * 2.0f, tsz.y + padY * 2.0f);
    const ImVec2 mn((io.DisplaySize.x - plate.x) * 0.5f, io.DisplaySize.y * 0.41f);
    const ImVec2 mx(mn.x + plate.x, mn.y + plate.y);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(mn, mx, theme::u32(theme::danger()), 12.0f);
    const ImU32 knock = theme::u32(theme::background());
    const ImVec2 origin(mn.x + padX, mn.y + padY);
    static const ImVec2 kOff[] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 0}, {0, 2}, {2, 1}, {1, 2}};
    for (const ImVec2& o : kOff) {
        stamp(dl, font, fs, ImVec2(origin.x + o.x, origin.y + o.y),
              ImGui::ColorConvertU32ToFloat4(knock), text);
    }
}

void drawCamera(const char* id, const char* label, ImVec2 size, ImVec4 bg, bool video, bool mirror) {
    beginPanel(id, size, bg, ImVec2(0.0f, 0.0f));
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (video && videoDecoder().hasFrame() && videoDecoder().width() > 0 && videoDecoder().height() > 0) {
        (void)mirror;
        videoDecoder().poll();
        blitDecodedVideo(avail);
    } else if (video) {
        drawVideoBuffering(avail);
    } else {
        const char* space = std::strchr(label, ' ');
        ImGui::PushStyleColor(ImGuiCol_Text, theme::mutedFg());
        if (space != nullptr) {
            char line1[32];
            char line2[32];
            std::size_t n1 = static_cast<std::size_t>(space - label);
            if (n1 >= sizeof(line1)) {
                n1 = sizeof(line1) - 1;
            }
            std::memcpy(line1, label, n1);
            line1[n1] = '\0';
            std::snprintf(line2, sizeof(line2), "%s", space + 1);
            const ImVec2 s1 = ImGui::CalcTextSize(line1);
            const ImVec2 s2 = ImGui::CalcTextSize(line2);
            const float totalH = s1.y + s2.y + 4.0f;
            ImGui::SetCursorPos(ImVec2((avail.x - s1.x) * 0.5f, (avail.y - totalH) * 0.5f));
            ImGui::TextUnformatted(line1);
            ImGui::SetCursorPosX((avail.x - s2.x) * 0.5f);
            ImGui::TextUnformatted(line2);
        } else {
            const ImVec2 t = ImGui::CalcTextSize(label);
            ImGui::SetCursorPos(ImVec2((avail.x - t.x) * 0.5f, (avail.y - t.y) * 0.5f));
            ImGui::TextUnformatted(label);
        }
        ImGui::PopStyleColor();
    }
    endPanel();
}

void kv(float col1, float col2, float col3, const char* l0, const char* v0, ImVec4 c0, const char* l1,
        const char* v1, ImVec4 c1) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::mutedFg());
    ImGui::TextUnformatted(l0);
    ImGui::PopStyleColor();
    ImGui::SameLine(col1);
    ImGui::PushStyleColor(ImGuiCol_Text, c0);
    ImGui::TextUnformatted(v0);
    ImGui::PopStyleColor();
    if (l1 != nullptr) {
        ImGui::SameLine(col2);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::mutedFg());
        ImGui::TextUnformatted(l1);
        ImGui::PopStyleColor();
        ImGui::SameLine(col3);
        ImGui::PushStyleColor(ImGuiCol_Text, c1);
        ImGui::TextUnformatted(v1);
        ImGui::PopStyleColor();
    }
}

void drawBattery(const DashView& d, ImVec2 size) {
    beginPanel("##BatteryPanel", size, panelBg(d), ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
    ImFont* font = theme::uiFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float avW = ImGui::GetContentRegionAvail().x;
    const float soc = std::clamp(num(d.packSoc), 0.0f, 100.0f);
    const float labelW = 48.0f;
    const float voltW = 52.0f;
    const float gap = 6.0f;
    const float currentW = 80.0f;
    const float barW = std::max(40.0f, avW - labelW - voltW - currentW - gap * 3.0f);
    const float barH = 22.0f;
    const float rowH = barH;

    ImGui::PushStyleColor(ImGuiCol_Text, theme::mutedFg());
    ImGui::TextUnformatted("Main");
    ImGui::PopStyleColor();
    ImGui::SameLine(labelW + gap);
    {
        ImVec2 barPos = ImGui::GetCursorScreenPos();
        barPos.y += (rowH - barH) * 0.5f;
        const ImVec4 bat = batteryColor(soc);
        const float rounding = 3.0f;
        dl->AddRectFilled(barPos, ImVec2(barPos.x + barW, barPos.y + barH),
                          theme::u32(theme::withAlpha(theme::muted(), 0.5f)), rounding);
        const float fillW = barW * (soc / 100.0f);
        if (fillW > 1.0f) {
            const ImDrawFlags ff =
                fillW >= barW - 0.5f ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft;
            dl->AddRectFilled(barPos, ImVec2(barPos.x + fillW, barPos.y + barH), theme::u32(bat),
                              std::min(rounding, fillW * 0.5f), ff);
        }
        dl->AddRect(barPos, ImVec2(barPos.x + barW, barPos.y + barH),
                    theme::u32(theme::withAlpha(theme::mutedFg(), 0.42f)), rounding);
        char socTxt[8];
        std::snprintf(socTxt, sizeof(socTxt), "%.0f%%", static_cast<double>(soc));
        const ImVec2 sSz = ImGui::CalcTextSize(socTxt);
        const ImVec2 tp(barPos.x + (barW - sSz.x) * 0.5f, barPos.y + (barH - sSz.y) * 0.5f);
        dl->AddText(tp, theme::u32(theme::foreground()), socTxt);
        if (fillW > 0.5f) {
            dl->PushClipRect(barPos, ImVec2(barPos.x + fillW, barPos.y + barH), true);
            dl->AddText(tp, theme::u32(theme::contrastingText(bat)), socTxt);
            dl->PopClipRect();
        }
        ImGui::Dummy(ImVec2(barW, rowH));
    }
    ImGui::SameLine(0.0f, gap);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::mutedFg());
    ImGui::Text("%.0fV", static_cast<double>(num(d.packV)));
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, gap);
    {
        char aTxt[16];
        std::snprintf(aTxt, sizeof(aTxt), "%.0f A", static_cast<double>(num(d.packI)));
        const float fs = 32.0f;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 sz = textSize(font, fs, aTxt);
        pos.y += (rowH - sz.y) * 0.5f;
        stamp(dl, font, fs, pos, theme::foreground(), aTxt);
        ImGui::Dummy(ImVec2(currentW, rowH));
    }

    ImGui::Dummy(ImVec2(1.0f, 2.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::mutedFg());
    ImGui::TextUnformatted("Aux");
    ImGui::PopStyleColor();
    ImGui::SameLine(56.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::foreground());
    ImGui::Text("%.0f V  %.0f A", static_cast<double>(num(d.auxV)), static_cast<double>(num(d.auxI)));
    ImGui::PopStyleColor();

    const float valColW = std::max(60.0f, avW * 0.22f);
    const float lblColW = std::max(70.0f, avW * 0.27f);
    const float col1 = lblColW;
    const float col2 = col1 + valColW + 8.0f;
    const float col3 = col2 + lblColW;
    char a[32];
    char b[32];
    const float mt = num(d.motorT);
    const ImVec4 mtCol = mt > 90.0f ? theme::danger() : (mt > 70.0f ? theme::warning() : theme::foreground());
    std::snprintf(a, sizeof(a), "%.0f \xC2\xB0""C", static_cast<double>(num(d.heatsinkT)));
    std::snprintf(b, sizeof(b), "%.0f \xC2\xB0""C", static_cast<double>(mt));
    kv(col1, col2, col3, "Heatsink", a, theme::foreground(), "Motor Temp", b, mtCol);
    std::snprintf(a, sizeof(a), "%.0f V", static_cast<double>(num(d.busV)));
    std::snprintf(b, sizeof(b), "%.0f A", static_cast<double>(num(d.packI)));
    kv(col1, col2, col3, "Bus Voltage", a, theme::foreground(), "Bus Current", b, theme::foreground());
    std::snprintf(a, sizeof(a), "%.0f rpm", static_cast<double>(num(d.driveVel)));
    std::snprintf(b, sizeof(b), "%.0f %%", static_cast<double>(unitPct(d.driveCur)));
    kv(col1, col2, col3, "Drive Vel", a, theme::foreground(), "Drive Cur", b, theme::foreground());
    std::snprintf(a, sizeof(a), "%.0f %%", static_cast<double>(unitPct(d.powerCmd)));
    kv(col1, col2, col3, "Power Cmd", a, theme::foreground(), nullptr, "", theme::foreground());
    std::snprintf(a, sizeof(a), "%.1f V", static_cast<double>(num(d.preMotor)));
    std::snprintf(b, sizeof(b), "%.1f V", static_cast<double>(num(d.preBatt)));
    kv(col1, col2, col3, "Pre Motor", a, theme::foreground(), "Pre Batt", b, theme::foreground());

    if (!d.limitLines.empty()) {
        ImGui::Dummy(ImVec2(1.0f, 4.0f));
        std::string joined;
        for (const auto& line : d.limitLines) {
            if (!joined.empty()) {
                joined += " | ";
            }
            joined += line;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::TextUnformatted("MOTOR LIMITS ACTIVE:");
        ImGui::TextUnformatted(joined.c_str());
        ImGui::PopStyleColor();
    }

    ActiveFault banner;
    const bool bannerOn = activeFault(d, banner);
    int shown = 0;
    for (const std::string& line : d.faultLines) {
        if (bannerOn) {
            if (banner.label == "BPS Fault" && line.rfind("BPS", 0) == 0) {
                continue;
            }
            if (banner.label == "Motor Fault" && line.rfind("Motor", 0) == 0) {
                continue;
            }
            if (banner.label == "VCU Fault" && line.rfind("VCU", 0) == 0) {
                continue;
            }
        }
        if (shown >= 4) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::mutedFg());
            ImGui::Text("+%d more", static_cast<int>(d.faultLines.size()) - shown);
            ImGui::PopStyleColor();
            break;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
        ++shown;
    }

    if (d.packT.have && num(d.packT) > 0.0f) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
        ImGui::Text("Avg Batt Temp: %.1f \xC2\xB0""C", static_cast<double>(num(d.packT)));
        ImGui::PopStyleColor();
    }

    const BpsExtrema e = computeExtrema(d.cellV, d.cellT);
    if (e.minVoltIdx >= 0 || e.maxVoltIdx >= 0 || e.maxTempIdx >= 0) {
        if (e.minVoltIdx >= 0) {
            std::snprintf(a, sizeof(a), "#%d  %.2f V", e.minVoltIdx, static_cast<double>(e.minVolt));
            std::snprintf(b, sizeof(b), "#%d  %.2f V", e.maxVoltIdx, static_cast<double>(e.maxVolt));
            kv(col1, col2, col3, "Low Cell", a, theme::foreground(),
               e.maxVoltIdx >= 0 ? "High Cell" : nullptr, b, theme::foreground());
        }
        if (e.maxTempIdx >= 0) {
            const ImVec4 hot = e.maxTemp > 50.0f ? theme::danger()
                                                 : (e.maxTemp > 35.0f ? theme::warning() : theme::foreground());
            std::snprintf(a, sizeof(a), "#%d  %.1f \xC2\xB0""C", e.maxTempIdx, static_cast<double>(e.maxTemp));
            kv(col1, col2, col3, "Hottest", a, hot, nullptr, "", theme::foreground());
        }
    }

    char detail[96];
    if (formatBpsDetail(d, detail, sizeof(detail))) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::danger());
        ImGui::TextUnformatted(detail);
        ImGui::PopStyleColor();
    }

    if (d.bpsStale || d.vcuStale) {
        const char* txt =
            (d.bpsStale && d.vcuStale) ? "NO BPS/VCU DATA" : (d.bpsStale ? "NO BPS DATA" : "NO VCU DATA");
        const float blink = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
        if (blink > 0.5f) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::warning());
            ImGui::TextUnformatted(txt);
            ImGui::PopStyleColor();
        } else {
            ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight()));
        }
    }

    ImGui::PopStyleVar();
    endPanel();
}

void drawButtons(const DashView& d, ImVec2 size) {
    beginPanel("##ButtonGrid", size, panelBg(d), ImVec2(0.0f, 0.0f));
    struct Btn {
        const char* label;
        bool active;
    };
    const bool lvOn = d.ignOff.have && d.ignOff.value == 0.0;
    const Btn contactors[3][2] = {
        {{"HV+", on(d.hvPlus)}, {"HV-", on(d.hvMinus)}},
        {{"Array Pre", on(d.arrayPre)}, {"Array", on(d.arrayMain)}},
        {{"Motor Pre", on(d.motorPre)}, {"Motor", on(d.motorMain)}},
    };
    const Btn ignition[3] = {{"Low Voltage", lvOn}, {"Array", on(d.ignArray)}, {"Motor", on(d.ignMotor)}};

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = theme::uiFont();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = 1.0f;
    const float headerH = 16.0f;
    const float dividerH = 8.0f;
    float buttonTotalH = avail.y - (headerH * 2.0f) - dividerH - gap * 3.0f;
    if (buttonTotalH < 80.0f) {
        buttonTotalH = 80.0f;
    }
    const float btnH = buttonTotalH / 4.0f;

    auto drawLabel = [&](const char* text) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const ImVec2 tSz = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(pos.x + (avail.x - tSz.x) * 0.5f, pos.y + (headerH - tSz.y) * 0.5f),
                    theme::u32(theme::mutedFg()), text);
        ImGui::Dummy(ImVec2(avail.x, headerH));
    };
    auto drawButton = [&](const Btn& b, float bw, const char* suffix) {
        char id[48];
        std::snprintf(id, sizeof(id), "%s##%s", b.label, suffix);
        const ImVec2 cellPos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id, ImVec2(bw, btnH));
        const ImVec4 fill = b.active ? theme::primary() : theme::muted();
        const ImVec4 text = b.active ? theme::contrastingText(fill) : theme::mutedFg();
        const ImVec4 outline = b.active ? theme::withAlpha(theme::foreground(), 0.45f)
                                        : theme::withAlpha(theme::mutedFg(), 0.34f);
        const float pad = 3.0f;
        const ImVec2 mn(cellPos.x + pad, cellPos.y + pad);
        const ImVec2 mx(cellPos.x + bw - pad, cellPos.y + btnH - pad);
        const float round = std::min((bw - 2.0f * pad) * 0.15f, (btnH - 2.0f * pad) * 0.35f);
        dl->AddRectFilled(mn, mx, theme::u32(fill), round);
        dl->AddRect(mn, mx, theme::u32(outline), round, 0, 1.0f);
        float fs = std::min(btnH * 0.45f, 22.0f);
        ImVec2 probe = textSize(font, fs, b.label);
        const float budget = (bw - 2.0f * pad) - 10.0f;
        if (probe.x > budget && probe.x > 0.0f) {
            fs *= budget / probe.x;
        }
        if (fs < 10.0f) {
            fs = 10.0f;
        }
        const ImVec2 tSz = textSize(font, fs, b.label);
        stamp(dl, font, fs,
              ImVec2(cellPos.x + bw * 0.5f - tSz.x * 0.5f, cellPos.y + btnH * 0.5f - tSz.y * 0.5f), text,
              b.label);
    };

    drawLabel("CONTACTORS");
    const float contW = (avail.x - gap) / 2.0f;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 2; ++col) {
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "ct%d%d", row, col);
            drawButton(contactors[row][col], contW, suffix);
            if (col < 1) {
                ImGui::SameLine(0.0f, gap);
            }
        }
        if (row < 2) {
            ImGui::Dummy(ImVec2(0.0f, gap));
        }
    }
    ImGui::Dummy(ImVec2(0.0f, dividerH * 0.5f));
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(pos.x + 8.0f, pos.y), ImVec2(pos.x + avail.x - 8.0f, pos.y),
                    theme::u32(theme::withAlpha(theme::mutedFg(), 0.35f)), 1.0f);
    }
    ImGui::Dummy(ImVec2(0.0f, dividerH * 0.5f));
    drawLabel("IGNITION");
    const float ignW = (avail.x - gap * 2.0f) / 3.0f;
    for (int col = 0; col < 3; ++col) {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "ig%d", col);
        drawButton(ignition[col], ignW, suffix);
        if (col < 2) {
            ImGui::SameLine(0.0f, gap);
        }
    }
    endPanel();
}

void drawSpeed(const DashView& d, ImVec2 size) {
    beginPanel("##SpeedGauge", size, panelBg(d), ImVec2(12.0f, 8.0f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = theme::uiFont();
    const ImVec2 cs = ImGui::GetContentRegionAvail();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    origin.y -= 12.0f;
    const float cX = origin.x + cs.x * 0.5f;
    const float heartbeatH = 28.0f;
    const float fnrH = 92.0f;
    const float iconsH = 58.0f;
    float arcSpace = cs.y - heartbeatH - iconsH - fnrH - 18.0f;
    if (arcSpace < 60.0f) {
        arcSpace = 60.0f;
    }
    const float radius = std::min(cs.x * 0.48f, arcSpace * 0.6f);
    const float thickness = 14.0f;
    const ImVec2 center(cX, origin.y + 16.0f + radius);

    {
        const float t = static_cast<float>(ImGui::GetTime());
        const float pulse = 0.5f + 0.5f * std::sin(t * 4.0f * kPi);
        const ImVec4 hb = theme::withAlpha(theme::danger(), 0.4f + 0.6f * pulse);
        drawHeart(dl, ImVec2(origin.x + 28.0f, origin.y + 42.0f), 30.0f, theme::u32(hb));
        (void)d.heartbeat;
    }

    const float startA = kPi;
    const float sweep = kPi;
    dl->PathArcTo(center, radius, startA, startA + sweep, 64);
    dl->PathStroke(theme::u32(theme::muted()), 0, thickness);
    const float maxSpeed = 80.0f;
    const float pct = std::clamp(static_cast<float>(d.speed) / maxSpeed, 0.0f, 1.0f);
    if (pct > 0.001f) {
        dl->PathArcTo(center, radius, startA, startA + sweep * pct, 64);
        dl->PathStroke(theme::u32(speedColor(d.speed)), 0, thickness);
    }

    char speedTxt[8];
    std::snprintf(speedTxt, sizeof(speedTxt), "%d", d.speed);
    const float fontSize = std::min(radius * 2.2f, 96.0f);
    const ImVec2 sSz = textSize(font, fontSize, speedTxt);
    stamp(dl, font, fontSize, ImVec2(cX - sSz.x * 0.5f, center.y - sSz.y * 0.75f), theme::foreground(),
          speedTxt);

    const float maxIcon = std::min(48.0f, (arcSpace - sSz.y) * 0.4f);
    const float iconSize = std::max(20.0f, maxIcon);
    const float iconY = center.y + sSz.y * 0.25f + iconSize * 0.5f + 4.0f;
    const float iconSp = iconSize * 1.2f;
    const bool flashOn = (static_cast<int>(ImGui::GetTime() * 3.0) % 2) == 0;
    const bool hazard = on(d.hazard);
    const ImVec4 lC = ((d.turn == Turn::Left || hazard) && flashOn) ? theme::accent() : theme::mutedFg();
    const ImVec4 rC = ((d.turn == Turn::Right || hazard) && flashOn) ? theme::accent() : theme::mutedFg();
    drawLeftArrow(dl, ImVec2(cX - iconSp * 2.0f, iconY), iconSize, theme::u32(lC));
    drawCruise(dl, ImVec2(cX - iconSp, iconY), iconSize,
               theme::u32(on(d.cruise) ? theme::warning() : theme::mutedFg()));
    drawBrakeIcon(dl, ImVec2(cX, iconY), iconSize,
                  theme::u32(d.brakeEngaged ? theme::danger() : theme::mutedFg()));
    drawRegen(dl, ImVec2(cX + iconSp, iconY), iconSize,
              theme::u32(on(d.regen) ? theme::success() : theme::mutedFg()));
    drawRightArrow(dl, ImVec2(cX + iconSp * 2.0f, iconY), iconSize, theme::u32(rC));

    const float iconBottomY = center.y + sSz.y * 0.25f + iconSize + 4.0f;
    const float fnrY = iconBottomY + 8.0f;
    const float fnrBox = 50.0f;
    const char* letters[] = {"F", "N", "R"};
    const Gear vals[] = {Gear::Forward, Gear::Neutral, Gear::Reverse};
    const float spacing = 96.0f;
    const float startX = cX - spacing;
    for (int i = 0; i < 3; ++i) {
        const bool sel = d.gear == vals[i];
        const float fs = sel ? std::min(72.0f, fnrBox * 1.2f) : std::min(72.0f, fnrBox * 1.2f) * 0.75f;
        const ImVec2 lSz = textSize(font, fs, letters[i]);
        stamp(dl, font, fs,
              ImVec2(startX + spacing * static_cast<float>(i) - lSz.x * 0.5f,
                     fnrY + (fnrBox - lSz.y) * 0.5f),
              sel ? theme::primary() : theme::mutedFg(), letters[i]);
    }

    const float pbW = radius * 1.5f;
    const float pbH = 8.0f;
    const float pbY = fnrY + fnrBox + 5.0f;
    const float pedalPct = std::clamp(num(d.throttle) / 100.0f, 0.0f, 1.0f);
    char throttleTxt[8];
    std::snprintf(throttleTxt, sizeof(throttleTxt), "%d%%", static_cast<int>(std::lround(pedalPct * 100.0f)));
    const float throttleFs = std::min(28.0f, std::max(18.0f, pbH * 2.5f));
    const ImVec2 throttleSz = textSize(font, throttleFs, throttleTxt);
    const float groupW = pbW + 6.0f + throttleSz.x;
    const ImVec2 pPos(cX - groupW * 0.5f, pbY);
    dl->AddRectFilled(pPos, ImVec2(pPos.x + pbW, pPos.y + pbH), theme::u32(theme::muted()), pbH * 0.5f);
    const float fillW = pbW * pedalPct;
    if (fillW > 2.0f) {
        ImDrawFlags flags = ImDrawFlags_RoundCornersLeft;
        if (fillW >= pbW - 0.5f) {
            flags = ImDrawFlags_RoundCornersAll;
        }
        dl->AddRectFilled(pPos, ImVec2(pPos.x + fillW, pPos.y + pbH), theme::u32(theme::accent()), pbH * 0.5f,
                          flags);
    }
    stamp(dl, font, throttleFs, ImVec2(pPos.x + pbW + 6.0f, pPos.y + (pbH - throttleSz.y) * 0.5f),
          theme::accent(), throttleTxt);

    char pressureTxt[48];
    std::snprintf(pressureTxt, sizeof(pressureTxt), "FRONT %.0f   REAR %.0f",
                  static_cast<double>(std::max(0.0f, num(d.brakeP1))),
                  static_cast<double>(std::max(0.0f, num(d.brakeP2))));
    const ImVec4 pCol = (on(d.brakeP1F) || on(d.brakeP2F)) ? theme::warning() : theme::mutedFg();
    const float pFs = std::min(26.0f, std::max(18.0f, iconSize * 0.55f));
    const ImVec2 pSz = textSize(font, pFs, pressureTxt);
    stamp(dl, font, pFs, ImVec2(cX - pSz.x * 0.5f, pbY + pbH + 2.0f), pCol, pressureTxt);
    endPanel();
}

void drawBpsGrid(const DashView& d) {
    ImGui::TextColored(theme::warning(), "BPS 32-MODULE GRID");
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(2.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.0f, 0.0f));
    if (ImGui::BeginTable("BpsGrid", 8,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame)) {
        const int nV = static_cast<int>(d.cellV.size());
        const int nT = static_cast<int>(d.cellT.size());
        for (int i = 0; i < 32; ++i) {
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::mutedFg(), "#%d", i);
            if (i < nV || i < nT) {
                const float v = i < nV ? d.cellV[static_cast<std::size_t>(i)] : 0.0f;
                const float t = i < nT ? d.cellT[static_cast<std::size_t>(i)] : 0.0f;
                ImVec4 vCol = theme::foreground();
                if (v > 4.2f || v < 2.5f) {
                    vCol = theme::danger();
                } else if (v > 4.15f || v < 3.0f) {
                    vCol = theme::warning();
                }
                ImVec4 tCol = theme::foreground();
                if (t > 45.0f) {
                    tCol = theme::danger();
                } else if (t > 35.0f) {
                    tCol = theme::warning();
                }
                ImGui::TextColored(vCol, "%.2fV", static_cast<double>(v));
                ImGui::SameLine();
                ImGui::TextColored(tCol, "%.0fC", static_cast<double>(t));
            } else {
                ImGui::TextDisabled("-  -");
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);
}

std::vector<std::pair<std::string, DashView>> g_snaps;
int g_snapIndex = -1;

void drawDebug(DashView& live) {
    const DashView& d =
        (g_snapIndex >= 0 && g_snapIndex < static_cast<int>(g_snaps.size())) ? g_snaps[static_cast<std::size_t>(g_snapIndex)].second
                                                                             : live;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    beginPanel("DebugScreenPanel", avail, theme::withAlpha(theme::background(), 0.95f), ImVec2(10.0f, 8.0f));
    ImGui::TextColored(theme::primary(), "DEBUG");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(400.0f);
    const char* preview = g_snapIndex < 0 ? "[LIVE DATA]" : g_snaps[static_cast<std::size_t>(g_snapIndex)].first.c_str();
    if (ImGui::BeginCombo("##Snapshots", preview)) {
        if (ImGui::Selectable("[LIVE DATA]", g_snapIndex == -1)) {
            g_snapIndex = -1;
        }
        for (int i = 0; i < static_cast<int>(g_snaps.size()); ++i) {
            if (ImGui::Selectable(g_snaps[static_cast<std::size_t>(i)].first.c_str(), g_snapIndex == i)) {
                g_snapIndex = i;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button("Clear Snapshots")) {
        g_snaps.clear();
        g_snapIndex = -1;
    }
    ImGui::SameLine();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("HB:%d %.0fFPS %.1fms", static_cast<int>(d.heartbeat), static_cast<double>(io.Framerate),
                static_cast<double>(io.DeltaTime * 1000.0f));
    ImGui::Separator();

    const float colW = avail.x * 0.5f - 10.0f;
    ImGui::BeginChild("DebugL", ImVec2(colW, avail.y - 70.0f), ImGuiChildFlags_None);
    ImGui::TextColored(theme::primary(), "INPUTS");
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f), "STEERING SENSOR (LWS)");
    ImGui::Text(" Angle: %.1f deg  %s", static_cast<double>(num(d.steer)),
                (d.lwsFault.have && d.lwsFault.value == 0.0) ? "OK" : "FAULT");
    ImGui::Dummy(ImVec2(1.0f, 4.0f));
    ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "DRIVER INPUTS");
    ImGui::Text(" Horn:%s Hazard:%s PTT:%s Cruise Set:%s Regen Act:%s", yn(d.horn, "ON", "-"),
                yn(d.hazard, "ON", "-"), yn(d.ptt, "ON", "-"), yn(d.cruiseSet, "ON", "-"),
                yn(d.regenAct, "ON", "-"));
    ImGui::Text(" Gear: %s  Turn: %s",
                d.gear == Gear::Forward ? "F" : (d.gear == Gear::Reverse ? "R" : "N"),
                d.turn == Turn::Left ? "L" : (d.turn == Turn::Right ? "R" : "-"));
    ImGui::Dummy(ImVec2(1.0f, 4.0f));
    ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f), "ACCEL / BRAKE PEDALS");
    ImGui::Text(" Accel: Main %.0f%% Red %.0f%%", static_cast<double>(num(d.throttle)),
                static_cast<double>(num(d.accelRed)));
    ImGui::Text("        Faults: Main:%s Red:%s", flt(d.accelMainF), flt(d.accelRedF));
    ImGui::Text(" Brake: Engaged:%s  Main %.0f%% Red %.0f%%", d.brakeEngaged ? "YES" : "NO",
                static_cast<double>(num(d.brake)), static_cast<double>(num(d.brakeRed)));
    ImGui::Text("        Faults: Main:%s Red:%s", flt(d.brakeMainF), flt(d.brakeRedF));
    ImGui::Text(" Pressure: %.0f / %.0f psi (%s/%s)", static_cast<double>(num(d.brakeP1)),
                static_cast<double>(num(d.brakeP2)), flt(d.brakeP1F), flt(d.brakeP2F));
    ImGui::Dummy(ImVec2(1.0f, 4.0f));
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "CONTACTORS / IGNITION SWITCHES");
    ImGui::Text(" HV+:%s -:%s ArrP:%s Arr:%s MtrP:%s Mtr:%s", yn(d.hvPlus, "C", "o"), yn(d.hvMinus, "C", "o"),
                yn(d.arrayPre, "C", "o"), yn(d.arrayMain, "C", "o"), yn(d.motorPre, "C", "o"),
                yn(d.motorMain, "C", "o"));
    ImGui::Text(" Ignition: LV:%s  Array:%s  Motor:%s",
                (d.ignOff.have && d.ignOff.value == 0.0) ? "ON" : "off", yn(d.ignArray, "ON", "off"),
                yn(d.ignMotor, "ON", "off"));
    ImGui::Dummy(ImVec2(1.0f, 4.0f));
    ImGui::TextColored(theme::danger(), "FAULTS");
    if (d.faultLines.empty()) {
        ImGui::TextUnformatted(" None");
    } else {
        for (const std::string& line : d.faultLines) {
            ImGui::Text(" %s", line.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("DebugR", ImVec2(colW, avail.y - 70.0f), ImGuiChildFlags_None);
    ImGui::TextColored(theme::accent(), "OUTPUTS & STATUS");
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "MOTOR CONTROLLER (MoCo)");
    ImGui::Text(" Vel: %d mph  Bus: %.1fV %.1fA", d.speed, static_cast<double>(num(d.busV)),
                static_cast<double>(num(d.packI)));
    ImGui::Text(" Phase: %.1fA (B) %.1fA (C)  BEMF: %.1fV (Q) %.1fV (D)", static_cast<double>(num(d.phaseB)),
                static_cast<double>(num(d.phaseC)), static_cast<double>(num(d.bemfQ)),
                static_cast<double>(num(d.bemfD)));
    ImGui::Text(" Temp: Heatsink %.1fC  Precharge Motor: %.1fV", static_cast<double>(num(d.heatsinkT)),
                static_cast<double>(num(d.preMotor)));
    std::string lim = " Limits Active: ";
    if (d.limitLines.empty()) {
        lim += "None";
    } else {
        for (const auto& line : d.limitLines) {
            lim += line;
            lim += " ";
        }
    }
    ImGui::TextColored(theme::warning(), "%s", lim.c_str());
    ImGui::Dummy(ImVec2(1.0f, 4.0f));
    const ImVec4 vcuFCol = d.vcuCode != 0 ? theme::danger() : ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "VCU STATUS");
    ImGui::TextColored(vcuFCol, " Fault: %d (%s)  FSM State: %d", static_cast<int>(d.vcuCode),
                       vcuFaultDetail(d.vcuCode).c_str(), static_cast<int>(num(d.vcuFsm)));
    ImGui::Text(" Motor Ready:%s Pedals:%s Driver Input:%s", yn(d.motorReady, "Y", "N"),
                (d.vcuPedWd.have && d.vcuPedWd.value == 0.0) ? "OK" : "NO",
                (d.vcuDrvWd.have && d.vcuDrvWd.value == 0.0) ? "OK" : "NO");
    ImGui::Text(" Regen:%s (Active:%s)", yn(d.vcuRegenOk, "OK", "NO"), yn(d.vcuRegenAct, "Y", "N"));
    ImGui::Dummy(ImVec2(1.0f, 4.0f));
    const float pin = num(d.mpptVin) * num(d.mpptIin);
    const float pout = num(d.mpptVout) * num(d.mpptIout);
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "MPPT SOLAR (A)");
    ImGui::Text(" In: %.1fV %.2fA (%.0fW)  Out: %.1fV %.2fA (%.0fW)", static_cast<double>(num(d.mpptVin)),
                static_cast<double>(num(d.mpptIin)), static_cast<double>(pin),
                static_cast<double>(num(d.mpptVout)), static_cast<double>(num(d.mpptIout)),
                static_cast<double>(pout));
    ImGui::Text(" Heatsink: %.0fC  Ambient: %.0fC  Fault:%d  Mode:%d", static_cast<double>(num(d.mpptHs)),
                static_cast<double>(num(d.mpptAmb)), static_cast<int>(num(d.mpptFault)),
                static_cast<int>(num(d.mpptMode)));
    ImGui::Dummy(ImVec2(1.0f, 4.0f));
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "COOLING");
    ImGui::Text(" Coolant: %.1f / %.1fC  Flow: %.2f / %.2f L/min", static_cast<double>(num(d.coolant1)),
                static_cast<double>(num(d.coolant2)), static_cast<double>(num(d.flow1)),
                static_cast<double>(num(d.flow2)));
    ImGui::TextColored(on(d.pumpFault) ? theme::danger() : theme::foreground(), " Pump: %.0f%% %s",
                       static_cast<double>(num(d.pumpDuty)), on(d.pumpFault) ? "FAULT" : "OK");
    ImGui::Dummy(ImVec2(1.0f, 4.0f));
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.9f, 1.0f), "LV / CAMERAS / LIGHTS");
    ImGui::Text(" HV DCDC: Sel:%s Fault:%s Valid:%s", yn(d.ltcHvSel, "Y", "N"), yn(d.ltcHvF, "YES", "no"),
                yn(d.ltcHvV, "Y", "N"));
    ImGui::Text(" Supp Batt: Sel:%s Fault:%s Valid:%s", yn(d.ltcSuppSel, "Y", "N"), yn(d.ltcSuppF, "YES", "no"),
                yn(d.ltcSuppV, "Y", "N"));
    ImGui::Text(" Enable: SuppBatt:%s PSU:%s", yn(d.lvEnSupp, "ON", "off"), yn(d.lvEnPsu, "ON", "off"));
    ImGui::Text(" Supp Batt Info: %.1fV  %.1fA  Charger: %s  DCDC: %.1fV %.1fA",
                static_cast<double>(num(d.auxV)), static_cast<double>(num(d.auxI)),
                chargerStatus(static_cast<std::uint8_t>(num(d.suppChg))), static_cast<double>(num(d.vicorV)),
                static_cast<double>(num(d.vicorI)));
    ImGui::Text(" Cameras: Backup:%s Left:%s Right:%s", yn(d.camBackup, "Y", "N"), yn(d.camLeft, "Y", "N"),
                yn(d.camRight, "Y", "N"));
    const int lightingFaults = (on(d.lightF) ? 1 : 0) | (on(d.lightL) ? 2 : 0) | (on(d.lightR) ? 4 : 0) |
                               (on(d.lightRr) ? 8 : 0) | (on(d.lightC) ? 16 : 0);
    const ImVec4 ctlCol =
        (lightingFaults != 0 || num(d.ctrlFault) != 0.0f) ? theme::danger() : theme::foreground();
    ImGui::TextColored(ctlCol, " Light Faults: %d  Controls Faults: %d", lightingFaults,
                       static_cast<int>(num(d.ctrlFault)));
    ImGui::Dummy(ImVec2(1.0f, 4.0f));
    const ImVec4 bpsFCol = d.bpsCode != 0 ? theme::danger() : ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "BPS STATUS");
    ImGui::TextColored(bpsFCol, " Fault: %d (%s)  Regen: %s  Charge: %s", static_cast<int>(d.bpsCode),
                       bpsFaultName(d.bpsCode), yn(d.bpsRegen, "OK", "NO"), yn(d.bpsCharge, "OK", "NO"));
    if (d.packT.have && num(d.packT) > 0.0f) {
        ImGui::Text(" Avg Temp: %.1fC", static_cast<double>(num(d.packT)));
    }
    const BpsExtrema e = computeExtrema(d.cellV, d.cellT);
    if (e.minVoltIdx >= 0 || e.maxVoltIdx >= 0 || e.maxTempIdx >= 0) {
        ImGui::Text(" Cells: Lo #%d %.2fV  Hi #%d %.2fV  Hot #%d %.1fC", e.minVoltIdx,
                    static_cast<double>(e.minVolt), e.maxVoltIdx, static_cast<double>(e.maxVolt), e.maxTempIdx,
                    static_cast<double>(e.maxTemp));
    }
    char detail[96];
    if (formatBpsDetail(d, detail, sizeof(detail))) {
        ImGui::TextColored(theme::danger(), " %s", detail);
    }
    ImGui::Dummy(ImVec2(1.0f, 4.0f));
    drawBpsGrid(d);
    ImGui::EndChild();
    endPanel();
}

void maybeSnapshot(const DashView& d, bool browsing) {
    static std::uint8_t lastBps = 0;
    static std::uint8_t lastVcu = 0;
    static bool lastMotor = false;
    if (browsing) {
        lastBps = d.bpsCode;
        lastVcu = d.vcuCode;
        lastMotor = !d.motorFaultDetail.empty();
        return;
    }
    auto push = [&](const std::string& reason) {
        constexpr std::size_t kMax = 100;
        if (g_snaps.size() >= kMax) {
            g_snaps.erase(g_snaps.begin());
        }
        g_snaps.push_back({reason, d});
    };
    char timeBuf[64];
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &local);
    if (d.bpsCode != 0 && lastBps == 0) {
        push(std::string(timeBuf) + " BPS Fault: " + bpsFaultName(d.bpsCode));
    }
    if (d.vcuCode != 0 && lastVcu == 0) {
        push(std::string(timeBuf) + " VCU Fault: " + vcuFaultDetail(d.vcuCode));
    }
    const bool motor = !d.motorFaultDetail.empty();
    if (motor && !lastMotor) {
        push(std::string(timeBuf) + " Motor Fault: " + d.motorFaultDetail);
    }
    lastBps = d.bpsCode;
    lastVcu = d.vcuCode;
    lastMotor = motor;
}

void drawCluster(const DashView& d) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float rowTop = avail.y * 0.5f;
    const float rowBot = avail.y - rowTop;
    const float side = avail.x * 0.33f;
    const float center = avail.x - side * 2.0f;
    const float colCenterX = side;
    const float colRightX = side + center;

    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    drawCamera("leftcam", "LEFT VIEW", ImVec2(side, rowTop), panelBg(d), false, false);
    ImGui::SetCursorPos(ImVec2(colCenterX, 0.0f));
    drawSpeed(d, ImVec2(center, rowTop));
    ImGui::SetCursorPos(ImVec2(colRightX, 0.0f));
    drawCamera("rightcam", "RIGHT VIEW", ImVec2(side, rowTop), panelBg(d), false, false);

    ImGui::SetCursorPos(ImVec2(0.0f, rowTop));
    drawBattery(d, ImVec2(side, rowBot));
    ImGui::SetCursorPos(ImVec2(colCenterX, rowTop));
    drawCamera("rearcam", "REAR VIEW", ImVec2(center, rowBot), panelBg(d), true, true);
    ImGui::SetCursorPos(ImVec2(colRightX, rowTop));
    drawButtons(d, ImVec2(side, rowBot));
}

}  // namespace

void DashboardScreen::draw(UiContext& ui) {
    ui.client.markVideoWanted();
    if (ui.client.helloOk() && !ui.arena.video.subscribed && !ui.arena.catalogPinned()) {
        ui.client.goLive();
    }
    ui.arena.signalMap().subscribe(ui.client, MapScreen::Pit | MapScreen::Dashboard);
    videoDecoder().syncToCursor(ui.arena);
    videoDecoder().poll();

    heartbeat_ = static_cast<std::uint8_t>((heartbeat_ + 1) % 256);
    DashView view = fillView(ui, heartbeat_);
    maybeSnapshot(view, showDebug_);

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    {
        constexpr float kCorner = 80.0f;
        constexpr double kChord = 1.0;
        static double lastL = -1.0e9;
        static double lastR = -1.0e9;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const ImVec2 mp = io.MousePos;
            const double nowT = ImGui::GetTime();
            if (mp.y >= wp.y && mp.y <= wp.y + kCorner) {
                if (mp.x >= wp.x && mp.x <= wp.x + kCorner) {
                    lastL = nowT;
                }
                if (mp.x >= wp.x + ws.x - kCorner && mp.x <= wp.x + ws.x) {
                    lastR = nowT;
                }
            }
            if ((nowT - lastL) < kChord && (nowT - lastR) < kChord) {
                showDebug_ = !showDebug_;
                lastL = lastR = -1.0e9;
            }
        }
    }
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_D)) {
        showDebug_ = !showDebug_;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, screenBg(view));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##dashroot", avail, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (showDebug_) {
        drawDebug(view);
    } else {
        drawCluster(view);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    drawFaultBanner(view);
}

}  // namespace photon::ui

