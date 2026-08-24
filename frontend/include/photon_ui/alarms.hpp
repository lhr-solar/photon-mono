#pragma once

#include "photon/control.hpp"

#include "imgui.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace photon::ui {

class Client;
class ClientArena;

// Measured over 264 h of recorded telemetry (docs/pdr/log-coverage.md):
// BPS_Status ~0.14 Hz, VCU_Status ~0.055 Hz. 2 s / 5 s would false-alarm.
// See also docs/pdr/critical-signals.md (warn 15 s / alarm 45 s).
inline constexpr double kStaleWarnMs = 15000.0;
inline constexpr double kStaleAlarmMs = 45000.0;
inline constexpr std::size_t kRecentClearedCap = 20;
inline constexpr float kAlarmStripH = 40.0f;
inline constexpr int kStaleChipKeyBase = 1000;

enum class EcuHealth { Unknown, Ok, Stale, Silent, Fault };

struct EcuPill {
    const char* name = "";
    EcuHealth health = EcuHealth::Unknown;
    double ageMs = 0.0;
};

// VAL_ 1 BPS_Fault — assets/dbc/carcan_telemcan.dbc:927
enum class BpsFaultCode : int {
    Ok = 0,
    Overvoltage = 1,
    Undervoltage = 2,
    Regen = 3,
    Overtemperature = 4,
    Elcon = 5,
    ArrayPrechargeTimeout = 6,
    InternalWatchdog = 7,
    SegmentWatchdog = 8,
    HvPlusContactorSense = 9,
    HvMinusContactorSense = 10,
    ArrayContactorSense = 11,
    ArrayPchgContactorSense = 12,
    Estop1 = 13,
    Estop2 = 14,
    Estop3 = 15,
    ChargingOvercurrent = 16,
    DischargingOvercurrent = 17,
    AmperesWatchdog = 18,
};

inline constexpr const char* kBpsFaultName[] = {
    "OK",
    "Overvoltage",
    "Undervoltage",
    "Regen",
    "Overtemperature",
    "Elcon",
    "Array Precharge Timeout",
    "Internal Watchdog",
    "Segment Watchdog",
    "HV Plus Contactor Sense",
    "HV Minus Contactor Sense",
    "Array Contactor Sense",
    "Array Pchg Contactor Sense",
    "Estop 1",
    "Estop 2",
    "Estop 3",
    "Charging Overcurrent",
    "Discharging Overcurrent",
    "Amperes Watchdog",
};
inline constexpr int kBpsFaultNameCount =
    static_cast<int>(sizeof(kBpsFaultName) / sizeof(kBpsFaultName[0]));
static_assert(kBpsFaultNameCount == static_cast<int>(BpsFaultCode::AmperesWatchdog) + 1);

const char* bpsFaultName(int code);
std::string bpsFaultChipLabel(int code);

enum class AlarmSeverity : unsigned { Fault = 0, Warn = 1 };

enum class FaultKind : unsigned { Bit = 0, Tap = 1 };

struct FaultDef {
    const char* signal = "";
    const char* label = "";
    AlarmSeverity severity = AlarmSeverity::Fault;
    FaultKind kind = FaultKind::Bit;
};

struct AlarmChip {
    int key = 0;
    AlarmSeverity severity = AlarmSeverity::Fault;
    std::string label;
    std::string signal;
    std::string message;
    double value = 0.0;
    double sinceMs = 0.0;
};

struct ClearedFault {
    int key = 0;
    AlarmSeverity severity = AlarmSeverity::Fault;
    std::string label;
    std::string signal;
    std::string message;
    double value = 0.0;
    double sinceMs = 0.0;
    double clearedMs = 0.0;
};

class AlarmEngine {
public:
    AlarmEngine();

    void resetResolve();
    void resetRuntime();
    void resolve(const std::vector<ctrl::CatalogSignal>& catalog, ClientArena& arena);
    void subscribe(Client& client) const;
    void evaluate(const ClientArena& arena, bool connected, bool playing, double nowMs);

    const std::vector<AlarmChip>& chips() const { return chips_; }
    const std::vector<ClearedFault>& recent() const { return recent_; }
    const AlarmChip* chip(int key) const;
    int selectedKey() const { return selectedKey_; }
    void select(int key) { selectedKey_ = key; }
    double nowMs() const { return nowMs_; }
    EcuPill ecuPill(int index) const;

private:
    struct ResolvedFault {
        const FaultDef* def = nullptr;
        std::uint32_t id = 0;
        std::uint32_t voltId = 0;
        std::uint32_t tempId = 0;
        std::uint32_t gateId = 0;
        bool mapped = false;
        bool gateMapped = false;
        bool on = false;
        double sinceMs = 0.0;
        double value = 0.0;
        std::string liveLabel;
    };

    struct StaleWatch {
        const char* message = "";
        const char* staleLabel = "";
        const char* silentLabel = "";
        std::vector<std::uint32_t> ids;
        bool watching = false;
        bool haveStart = false;
        double startMs = 0.0;
        bool on = false;
        bool silent = false;
        double sinceMs = 0.0;
        double ageMs = 0.0;
    };

    std::vector<ResolvedFault> faults_;
    std::vector<StaleWatch> watches_;
    std::vector<AlarmChip> chips_;
    std::vector<ClearedFault> recent_;
    int selectedKey_ = -1;
    double nowMs_ = 0.0;

    void pushCleared(int key, AlarmSeverity severity, std::string label, std::string signal,
                     std::string message, double value, double sinceMs, double clearedMs);
    void setBit(ResolvedFault& slot, int key, bool active, double value, double eventMs, double nowMs);
    static std::string messageOf(std::string_view signal);
};

void drawAlarmStrip(AlarmEngine& engine, ImVec2 size);

}  // namespace photon::ui
