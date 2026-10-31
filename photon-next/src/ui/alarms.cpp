#include "ui/alarms.hpp"

#include "ui/arena.hpp"
#include "ui/client.hpp"
#include "ui/clock_format.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace photon::ui {
namespace {

// DBC names from assets/dbc/carcan_telemcan.dbc (Message.Signal), verified
// 2026-08-19. Bits: nonzero => chip. BPS_Status.BPS_Fault is a VAL_ code
// (dbc:927), not a boolean. MC_LIMIT_* are Tier-1 derate (yellow).
// VCU_TIPPING_WARNING is a warn bit. BPS_Fault_Val_Arr is a tap composite.
constexpr FaultDef kFaultDefs[] = {
    {"BPS_Status.BPS_Fault", "BPS fault", AlarmSeverity::Fault},
    {"BPS_Status.BPS_Segment0_Status", "BPS seg 0", AlarmSeverity::Fault},
    {"BPS_Status.BPS_Segment1_Status", "BPS seg 1", AlarmSeverity::Fault},
    {"BPS_Status.BPS_Segment2_Status", "BPS seg 2", AlarmSeverity::Fault},
    {"BPS_Status.BPS_Segment3_Status", "BPS seg 3", AlarmSeverity::Fault},
    {"BPS_Status.BPS_Segment4_Status", "BPS seg 4", AlarmSeverity::Fault},
    {"BPS_Status.BPS_Segment5_Status", "BPS seg 5", AlarmSeverity::Fault},
    {"BPS_Status.BPS_Segment6_Status", "BPS seg 6", AlarmSeverity::Fault},
    {"BPS_Status.BPS_Segment7_Status", "BPS seg 7", AlarmSeverity::Fault},
    {"BPS_Fault_Val_Arr", "BPS fault tap", AlarmSeverity::Fault, FaultKind::Tap},
    {"VCU_Status.VCU_BPS_FAULT_DETECTED", "VCU BPS", AlarmSeverity::Fault},
    {"VCU_Status.VCU_CONTROLS_FAULT_DETECTED", "VCU controls", AlarmSeverity::Fault},
    {"VCU_Status.VCU_MTR_FAULT_DETECTED", "VCU motor", AlarmSeverity::Fault},
    {"VCU_Status.VCU_PEDALS_FAULT_DETECTED", "VCU pedals", AlarmSeverity::Fault},
    {"VCU_Status.VCU_STEERING_FAULT_DETECTED", "VCU steering", AlarmSeverity::Fault},
    {"VCU_Status.VCU_PCHG_OV", "VCU pchg OV", AlarmSeverity::Fault},
    {"VCU_Status.VCU_PCHG_UV", "VCU pchg UV", AlarmSeverity::Fault},
    {"VCU_Status.VCU_MTR_OV", "VCU motor OV", AlarmSeverity::Fault},
    {"VCU_Status.VCU_MTR_UV", "VCU motor UV", AlarmSeverity::Fault},
    {"VCU_Status.VCU_TIPPING_WARNING", "VCU tipping", AlarmSeverity::Warn},
    {"VCU_Status.VCU_OTHER_FAULT", "VCU other", AlarmSeverity::Fault},
    {"VCU_Status.VCU_MTR_CONT_MISMATCH", "VCU mtr cont mismatch", AlarmSeverity::Fault},
    {"VCU_Status.VCU_MTR_CONT_TIMEOUT", "VCU mtr cont timeout", AlarmSeverity::Fault},
    {"VCU_Status.VCU_MTR_PCHG_TIMEOUT", "VCU mtr pchg timeout", AlarmSeverity::Fault},
    {"VCU_Status.VCU_MTR_PCHG_CONT_TIMEOUT", "VCU mtr pchg cont timeout", AlarmSeverity::Fault},
    {"VCU_Status.VCU_MTR_PCHG_CONT_MISMATCH", "VCU mtr pchg cont mismatch", AlarmSeverity::Fault},
    {"MC_Status.MC_FAULT_HardwareOverCurrent", "MC hw OC", AlarmSeverity::Fault},
    {"MC_Status.MC_FAULT_SoftwareOverCurrent", "MC sw OC", AlarmSeverity::Fault},
    {"MC_Status.MC_FAULT_DcBusOverVoltage", "MC bus OV", AlarmSeverity::Fault},
    {"MC_Status.MC_FAULT_BadMotorPositionHallSeq", "MC hall seq", AlarmSeverity::Fault},
    {"MC_Status.MC_FAULT_WatchdogCausedLastReset", "MC watchdog", AlarmSeverity::Fault},
    {"MC_Status.MC_FAULT_ConfigRead", "MC config", AlarmSeverity::Fault},
    {"MC_Status.MC_FAULT_15vRailUnderVoltage", "MC 15V UV", AlarmSeverity::Fault},
    {"MC_Status.MC_FAULT_DesaturationFault", "MC desat", AlarmSeverity::Fault},
    {"MC_Status.MC_FAULT_MotorOverSpeed", "MC overspeed", AlarmSeverity::Fault},
    {"MC_Status.MC_LIMIT_OutputVoltagePWM", "MC lim PWM", AlarmSeverity::Warn},
    {"MC_Status.MC_LIMIT_MotorCurrent", "MC lim I", AlarmSeverity::Warn},
    {"MC_Status.MC_LIMIT_Velocity", "MC lim vel", AlarmSeverity::Warn},
    {"MC_Status.MC_LIMIT_BusCurrent", "MC lim bus I", AlarmSeverity::Warn},
    {"MC_Status.MC_LIMIT_BusVoltageUpper", "MC lim bus V hi", AlarmSeverity::Warn},
    {"MC_Status.MC_LIMIT_BusVoltageLower", "MC lim bus V lo", AlarmSeverity::Warn},
    {"MC_Status.MC_LIMIT_MotorTemp", "MC lim motor T", AlarmSeverity::Warn},
    {"Pedal_Status.AccelPedal_Main_Fault", "Accel main", AlarmSeverity::Fault},
    {"Pedal_Status.AccelPedal_Redundant_Fault", "Accel redun", AlarmSeverity::Fault},
    {"Pedal_Status.BrakePedal_Main_Fault", "Brake main", AlarmSeverity::Fault},
    {"Pedal_Status.BrakePedal_Redundant_Fault", "Brake redun", AlarmSeverity::Fault},
    {"Pedal_Status.Brake_Pressure_1_Fault", "Brake P1", AlarmSeverity::Fault},
    {"Pedal_Status.Brake_Pressure_2_Fault", "Brake P2", AlarmSeverity::Fault},
    {"Supp_Battery_Status.Supplemental_Battery_Fault", "Supp batt", AlarmSeverity::Fault},
    {"LV_Carrier_Status.LTC4421_HVDCDC_Fault", "HV-DCDC", AlarmSeverity::Fault},
    {"LV_Carrier_Status.LTC4421_SuppBatt_Fault", "LTC supp", AlarmSeverity::Fault},
    {"Pump_Status.Pump_Fault", "Pump", AlarmSeverity::Fault},
    {"ChargerInterface_Status.Elcon_HW_FAULT", "Elcon HW", AlarmSeverity::Fault},
    {"ChargerInterface_Status.Elcon_OVER_TEMPERATURE", "Elcon OT", AlarmSeverity::Fault},
    {"ChargerInterface_Status.Elcon_INPUT_VOL_ERROR", "Elcon Vin", AlarmSeverity::Fault},
    {"ChargerInterface_Status.Elcon_CMD_TIMEOUT", "Elcon timeout", AlarmSeverity::Fault},
};


struct StaleWatchDef {
    const char* message;
    const char* staleLabel;
    const char* silentLabel;
};

constexpr StaleWatchDef kStaleWatchDefs[] = {
    {"BPS_Status", "BPS stale", "BPS silent"},
    {"VCU_Status", "VCU stale", "VCU silent"},
    {"MC_Status", "MC stale", "MC silent"},
};

std::string catalogKey(const ctrl::CatalogSignal& sig) {
    if (sig.message.empty()) {
        return sig.name;
    }
    return sig.message + "." + sig.name;
}

void formatClock(double ms, char* buf, std::size_t n) {
    formatLocalClock(ms, buf, n, ClockParts::DateTime);
}

void formatAge(double ms, char* buf, std::size_t n) {
    if (!(ms >= 0.0) || !std::isfinite(ms)) {
        std::snprintf(buf, n, "--");
        return;
    }
    const double sec = ms / 1000.0;
    if (sec < 60.0) {
        std::snprintf(buf, n, "%0.1f s", sec);
        return;
    }
    const int total = static_cast<int>(sec);
    const int s = total % 60;
    const int m = (total / 60) % 60;
    const int h = total / 3600;
    if (h > 0) {
        std::snprintf(buf, n, "%dh %02dm", h, m);
    } else {
        std::snprintf(buf, n, "%dm %02ds", m, s);
    }
}

ImVec4 severityColor(AlarmSeverity sev) {
    return sev == AlarmSeverity::Fault ? theme::danger() : theme::warning();
}

double sampleTs(const ClientArena& arena, std::uint32_t id, double fallback) {
    const SignalRing* ring = arena.ring(id);
    if (ring == nullptr || !ring->hasLatest) {
        return fallback;
    }
    return ring->latestTs;
}

}  // namespace

const char* bpsFaultName(int code) {
    if (code < 0 || code >= kBpsFaultNameCount) {
        return nullptr;
    }
    return kBpsFaultName[code];
}

std::string bpsFaultChipLabel(int code) {
    if (const char* name = bpsFaultName(code); name != nullptr && code != 0) {
        return std::string("BPS: ") + name;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "BPS fault %d", code);
    return buf;
}

AlarmEngine::AlarmEngine() {
    faults_.reserve(sizeof(kFaultDefs) / sizeof(kFaultDefs[0]));
    for (const auto& def : kFaultDefs) {
        ResolvedFault slot;
        slot.def = &def;
        slot.liveLabel = def.label;
        faults_.push_back(std::move(slot));
    }
    watches_.reserve(sizeof(kStaleWatchDefs) / sizeof(kStaleWatchDefs[0]));
    for (const auto& def : kStaleWatchDefs) {
        StaleWatch watch;
        watch.message = def.message;
        watch.staleLabel = def.staleLabel;
        watch.silentLabel = def.silentLabel;
        watches_.push_back(std::move(watch));
    }
}

void AlarmEngine::resetResolve() {
    for (auto& slot : faults_) {
        slot.id = 0;
        slot.voltId = 0;
        slot.tempId = 0;
        slot.gateId = 0;
        slot.mapped = false;
        slot.gateMapped = false;
        if (slot.def != nullptr) {
            slot.liveLabel = slot.def->label;
        }
    }
    for (auto& watch : watches_) {
        watch.ids.clear();
        watch.watching = false;
    }
}

void AlarmEngine::resetRuntime() {
    for (auto& slot : faults_) {
        slot.on = false;
        slot.sinceMs = 0.0;
        slot.value = 0.0;
        if (slot.def != nullptr) {
            slot.liveLabel = slot.def->label;
        }
    }
    for (auto& watch : watches_) {
        watch.haveStart = false;
        watch.startMs = 0.0;
        watch.on = false;
        watch.silent = false;
        watch.sinceMs = 0.0;
        watch.ageMs = 0.0;
    }
    chips_.clear();
    selectedKey_ = -1;
    nowMs_ = 0.0;
}

void AlarmEngine::resolve(const std::vector<ctrl::CatalogSignal>& catalog, ClientArena& arena) {
    std::unordered_map<std::string, const ctrl::CatalogSignal*> byKey;
    byKey.reserve(catalog.size());
    std::unordered_map<std::string, std::vector<std::uint32_t>> byMessage;
    for (const auto& sig : catalog) {
        byKey.emplace(catalogKey(sig), &sig);
        if (!sig.message.empty()) {
            byMessage[sig.message].push_back(sig.id);
        }
    }

    int hits = 0;
    int misses = 0;
    for (auto& slot : faults_) {
        slot.id = 0;
        slot.voltId = 0;
        slot.tempId = 0;
        slot.gateId = 0;
        slot.mapped = false;
        slot.gateMapped = false;
        if (slot.def == nullptr) {
            continue;
        }
        if (slot.def->kind == FaultKind::Tap) {
            const auto idx = byKey.find("BPS_Fault_Val_Arr.BPS_Tap_idx");
            const auto volt = byKey.find("BPS_Fault_Val_Arr.BPS_Voltage_Tap_Data");
            const auto temp = byKey.find("BPS_Fault_Val_Arr.BPS_Temperature_Tap_Data");
            const auto gate = byKey.find("BPS_Status.BPS_Fault");
            if (idx == byKey.end() || volt == byKey.end() || temp == byKey.end()) {
                arena.pushLog(std::string("alarm_map miss ") + slot.def->signal);
                ++misses;
                continue;
            }
            slot.mapped = true;
            slot.id = idx->second->id;
            slot.voltId = volt->second->id;
            slot.tempId = temp->second->id;
            if (gate != byKey.end()) {
                slot.gateMapped = true;
                slot.gateId = gate->second->id;
            }
            ++hits;
            continue;
        }
        const auto it = byKey.find(slot.def->signal);
        if (it == byKey.end()) {
            arena.pushLog(std::string("alarm_map miss ") + slot.def->signal);
            ++misses;
            continue;
        }
        slot.mapped = true;
        slot.id = it->second->id;
        ++hits;
    }

    for (auto& watch : watches_) {
        watch.ids.clear();
        const auto it = byMessage.find(watch.message);
        if (it == byMessage.end() || it->second.empty()) {
            watch.watching = false;
            continue;
        }
        watch.ids = it->second;
        watch.watching = true;
    }

    char buf[96];
    std::snprintf(buf, sizeof(buf), "alarm_map resolved hits=%d misses=%d", hits, misses);
    arena.pushLog(buf);
}

void AlarmEngine::subscribe(Client& client) const {
    std::vector<std::uint32_t> ids;
    ids.reserve(faults_.size() * 2 + 32);
    for (const auto& slot : faults_) {
        if (!slot.mapped) {
            continue;
        }
        ids.push_back(slot.id);
        if (slot.def != nullptr && slot.def->kind == FaultKind::Tap) {
            ids.push_back(slot.voltId);
            ids.push_back(slot.tempId);
        }
    }
    for (const auto& watch : watches_) {
        ids.insert(ids.end(), watch.ids.begin(), watch.ids.end());
    }
    client.ensureSubscribed(ids);
}

std::string AlarmEngine::messageOf(std::string_view signal) {
    const auto dot = signal.find('.');
    if (dot == std::string_view::npos) {
        return std::string(signal);
    }
    return std::string(signal.substr(0, dot));
}

void AlarmEngine::pushCleared(int key, AlarmSeverity severity, std::string label, std::string signal,
                             std::string message, double value, double sinceMs, double clearedMs) {
    ClearedFault row;
    row.key = key;
    row.severity = severity;
    row.label = std::move(label);
    row.signal = std::move(signal);
    row.message = std::move(message);
    row.value = value;
    row.sinceMs = sinceMs;
    row.clearedMs = clearedMs;
    recent_.insert(recent_.begin(), std::move(row));
    if (recent_.size() > kRecentClearedCap) {
        recent_.resize(kRecentClearedCap);
    }
}

void AlarmEngine::setBit(ResolvedFault& slot, int key, bool active, double value, double eventMs,
                         double nowMs) {
    const char* signal = slot.def != nullptr ? slot.def->signal : "";
    const std::string message = messageOf(signal);
    const std::string label = slot.liveLabel.empty() && slot.def != nullptr ? slot.def->label : slot.liveLabel;
    if (active) {
        if (!slot.on) {
            slot.on = true;
            slot.sinceMs = eventMs;
        }
        slot.value = value;
        return;
    }
    if (slot.on) {
        pushCleared(key, slot.def != nullptr ? slot.def->severity : AlarmSeverity::Fault, label, signal,
                    message, slot.value, slot.sinceMs, nowMs);
        slot.on = false;
        slot.sinceMs = 0.0;
        slot.value = 0.0;
        if (slot.def != nullptr) {
            slot.liveLabel = slot.def->label;
        }
    }
}

void AlarmEngine::evaluate(const ClientArena& arena, bool connected, bool playing, double nowMs) {
    nowMs_ = nowMs;
    chips_.clear();
    for (std::size_t i = 0; i < faults_.size(); ++i) {
        ResolvedFault& slot = faults_[i];
        if (slot.def == nullptr || !slot.mapped) {
            continue;
        }
        if (slot.def->kind == FaultKind::Tap) {
            const bool haveTap = arena.has(slot.id);
            bool gateOk = true;
            if (slot.gateMapped && arena.has(slot.gateId)) {
                gateOk = arena.readout(slot.gateId) != 0.0;
            }
            const bool active = haveTap && gateOk;
            if (active) {
                const int tap = static_cast<int>(std::lround(arena.readout(slot.id)));
                const double volts = arena.has(slot.voltId) ? arena.readout(slot.voltId) : 0.0;
                const double temp = arena.has(slot.tempId) ? arena.readout(slot.tempId) : 0.0;
                char label[80];
                std::snprintf(label, sizeof(label), "BPS fault tap %d (V=%0.3f, T=%0.1f)", tap, volts, temp);
                slot.liveLabel = label;
                slot.value = static_cast<double>(tap);
            }
            setBit(slot, static_cast<int>(i), active, slot.value, sampleTs(arena, slot.id, nowMs), nowMs);
            if (!slot.on) {
                continue;
            }
            AlarmChip chip;
            chip.key = static_cast<int>(i);
            chip.severity = slot.def->severity;
            chip.label = slot.liveLabel;
            chip.signal = slot.def->signal;
            chip.message = "BPS_Fault_Val_Arr";
            chip.value = slot.value;
            chip.sinceMs = slot.sinceMs;
            chips_.push_back(std::move(chip));
            continue;
        }

        const bool have = arena.has(slot.id);
        const double value = have ? arena.readout(slot.id) : 0.0;
        const bool active = have && value != 0.0;
        if (active && std::strcmp(slot.def->signal, "BPS_Status.BPS_Fault") == 0) {
            slot.liveLabel = bpsFaultChipLabel(static_cast<int>(std::lround(value)));
        }
        setBit(slot, static_cast<int>(i), active, value, sampleTs(arena, slot.id, nowMs), nowMs);
        if (!slot.on) {
            continue;
        }
        AlarmChip chip;
        chip.key = static_cast<int>(i);
        chip.severity = slot.def->severity;
        chip.label = slot.liveLabel;
        chip.signal = slot.def->signal;
        chip.message = messageOf(slot.def->signal);
        chip.value = slot.value;
        chip.sinceMs = slot.sinceMs;
        chips_.push_back(std::move(chip));
    }

    if (connected && playing) {
        for (std::size_t w = 0; w < watches_.size(); ++w) {
            StaleWatch& watch = watches_[w];
            if (!watch.watching) {
                continue;
            }
            double lastTs = 0.0;
            bool any = false;
            for (const std::uint32_t id : watch.ids) {
                const SignalRing* ring = arena.ring(id);
                if (ring == nullptr || !ring->hasLatest) {
                    continue;
                }
                if (!any || ring->latestTs > lastTs) {
                    lastTs = ring->latestTs;
                }
                any = true;
            }
            double age = 0.0;
            if (!any) {
                if (!watch.haveStart) {
                    watch.haveStart = true;
                    watch.startMs = nowMs;
                }
                age = nowMs - watch.startMs;
            } else {
                watch.haveStart = false;
                age = nowMs - lastTs;
            }
            if (!(age > 0.0) || !std::isfinite(age)) {
                age = 0.0;
            }
            watch.ageMs = age;

            const bool silent = age > kStaleAlarmMs;
            const bool stale = age > kStaleWarnMs;
            const int key = kStaleChipKeyBase + static_cast<int>(w);
            if (stale) {
                if (!watch.on) {
                    watch.on = true;
                    watch.sinceMs = nowMs - age;
                    if (watch.sinceMs < 0.0) {
                        watch.sinceMs = nowMs;
                    }
                }
                watch.silent = silent;
                AlarmChip chip;
                chip.key = key;
                chip.severity = silent ? AlarmSeverity::Fault : AlarmSeverity::Warn;
                chip.label = silent ? watch.silentLabel : watch.staleLabel;
                chip.signal = watch.message;
                chip.message = watch.message;
                chip.value = age / 1000.0;
                chip.sinceMs = watch.sinceMs;
                chips_.push_back(std::move(chip));
            } else if (watch.on) {
                pushCleared(key, watch.silent ? AlarmSeverity::Fault : AlarmSeverity::Warn,
                            watch.silent ? watch.silentLabel : watch.staleLabel, watch.message,
                            watch.message, watch.ageMs / 1000.0, watch.sinceMs, nowMs);
                watch.on = false;
                watch.silent = false;
                watch.sinceMs = 0.0;
                watch.ageMs = 0.0;
            }
        }
    } else {
        for (auto& watch : watches_) {
            watch.haveStart = false;
            if (!watch.on) {
                continue;
            }
            // Not live/connected: drop watchdog chips without filing history.
            watch.on = false;
            watch.silent = false;
            watch.sinceMs = 0.0;
            watch.ageMs = 0.0;
        }
    }

    std::stable_sort(chips_.begin(), chips_.end(), [](const AlarmChip& a, const AlarmChip& b) {
        if (a.severity != b.severity) {
            return a.severity < b.severity;
        }
        return a.key < b.key;
    });
}

EcuPill AlarmEngine::ecuPill(int index) const {
    EcuPill pill;
    if (index < 0 || index >= static_cast<int>(watches_.size())) {
        return pill;
    }
    const StaleWatch& watch = watches_[static_cast<std::size_t>(index)];
    pill.name = watch.message != nullptr && std::strncmp(watch.message, "BPS", 3) == 0
                    ? "BPS"
                    : (watch.message != nullptr && std::strncmp(watch.message, "VCU", 3) == 0
                           ? "VCU"
                           : "MC");
    pill.ageMs = watch.ageMs;
    if (!watch.watching) {
        pill.health = EcuHealth::Unknown;
        return pill;
    }
    bool fault = false;
    for (const AlarmChip& chip : chips_) {
        if (chip.key >= kStaleChipKeyBase) {
            continue;
        }
        if (chip.message == watch.message ||
            (watch.message != nullptr && chip.signal.find(watch.message) == 0)) {
            if (chip.severity == AlarmSeverity::Fault) {
                fault = true;
                break;
            }
        }
    }
    if (fault) {
        pill.health = EcuHealth::Fault;
    } else if (watch.ageMs > kStaleAlarmMs) {
        pill.health = EcuHealth::Silent;
    } else if (watch.ageMs > kStaleWarnMs) {
        pill.health = EcuHealth::Stale;
    } else {
        pill.health = EcuHealth::Ok;
    }
    return pill;
}

const AlarmChip* AlarmEngine::chip(int key) const {
    for (const auto& c : chips_) {
        if (c.key == key) {
            return &c;
        }
    }
    return nullptr;
}

void drawAlarmStrip(AlarmEngine& engine, ImVec2 size) {
    const auto& chips = engine.chips();
    bool anyFault = false;
    int extraFaults = 0;
    for (const auto& c : chips) {
        if (c.key >= kStaleChipKeyBase) {
            continue;
        }
        ++extraFaults;
        if (c.severity == AlarmSeverity::Fault) {
            anyFault = true;
        }
    }
    EcuHealth worst = EcuHealth::Ok;
    for (int i = 0; i < 3; ++i) {
        const EcuHealth h = engine.ecuPill(i).health;
        if (h == EcuHealth::Fault || h == EcuHealth::Silent) {
            anyFault = true;
            worst = EcuHealth::Fault;
        } else if (h == EcuHealth::Stale && worst == EcuHealth::Ok) {
            worst = EcuHealth::Stale;
        }
    }
    const ImVec4 bg = anyFault ? theme::withAlpha(theme::danger(), 0.22f)
                               : (worst == EcuHealth::Stale ? theme::withAlpha(theme::warning(), 0.18f)
                                                           : theme::withAlpha(theme::success(), 0.14f));
    theme::beginCard("##alarm_strip", size, bg, theme::kStripPadding, theme::CardFit::Flush);
    theme::pushSmallFont();

    const float rowH = std::max(22.0f, ImGui::GetContentRegionAvail().y);
    const ImVec2 chipOrigin = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(chipOrigin.x + 4.0f, chipOrigin.y + 1.0f));
    bool openPopup = false;
    ImGui::BeginChild("##alarm_chips", ImVec2(ImGui::GetContentRegionAvail().x - 4.0f, rowH),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const auto drawPill = [&](const char* id, const char* text, ImVec4 col, int selectKey) {
        const ImVec2 sz = ImGui::CalcTextSize(text);
        const ImVec2 pill(sz.x + 16.0f, rowH - 2.0f);
        ImGui::PushID(id);
        ImGui::InvisibleButton("pill", pill);
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(a, b, theme::u32(theme::withAlpha(col, 0.40f)), theme::kChipRounding);
        draw->AddText(ImVec2(a.x + 8.0f, a.y + (pill.y - sz.y) * 0.5f), theme::u32(theme::foreground()),
                      text);
        if (ImGui::IsItemClicked()) {
            engine.select(selectKey);
            openPopup = true;
        }
        ImGui::PopID();
    };

    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            ImGui::SameLine(0.0f, 8.0f);
        }
        const EcuPill pill = engine.ecuPill(i);
        const char* state = "--";
        ImVec4 col = theme::mutedFg();
        switch (pill.health) {
            case EcuHealth::Ok:
                state = "OK";
                col = theme::success();
                break;
            case EcuHealth::Stale:
                state = "stale";
                col = theme::warning();
                break;
            case EcuHealth::Silent:
                state = "silent";
                col = theme::danger();
                break;
            case EcuHealth::Fault:
                state = "FAULT";
                col = theme::danger();
                break;
            case EcuHealth::Unknown:
            default:
                state = "--";
                col = theme::mutedFg();
                break;
        }
        char label[32];
        std::snprintf(label, sizeof(label), "%s  %s", pill.name, state);
        drawPill(pill.name, label, col, kStaleChipKeyBase + i);
    }

    for (const AlarmChip& chip : chips) {
        if (chip.key >= kStaleChipKeyBase) {
            continue;
        }
        ImGui::SameLine(0.0f, 8.0f);
        const ImVec4 col = severityColor(chip.severity);
        drawPill(chip.label.c_str(), chip.label.c_str(), col, chip.key);
    }
    if (extraFaults == 0 && worst == EcuHealth::Ok) {
        ImGui::SameLine(0.0f, 10.0f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 sz = ImGui::CalcTextSize("All systems nominal");
        ImGui::Dummy(ImVec2(sz.x, rowH - 2.0f));
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + (rowH - 2.0f - sz.y) * 0.5f),
                                            theme::u32(theme::success()), "All systems nominal");
    }
    ImGui::EndChild();
    theme::popSmallFont();
    theme::endCard();

    if (openPopup) {
        ImGui::OpenPopup("##alarm_detail");
    }
    ImGui::SetNextWindowSize(ImVec2(440.0f, 300.0f), ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopup("##alarm_detail")) {
        if (ImGui::BeginTabBar("##alarm_tabs")) {
            const AlarmChip* selected = engine.chip(engine.selectedKey());
            if (ImGui::BeginTabItem("Active")) {
                if (selected != nullptr) {
                    char since[32];
                    char age[32];
                    formatClock(selected->sinceMs, since, sizeof(since));
                    formatAge(engine.nowMs() - selected->sinceMs, age, sizeof(age));
                    ImGui::Text("Label    %s", selected->label.c_str());
                    ImGui::Text("Signal   %s", selected->signal.c_str());
                    ImGui::Text("Message  %s", selected->message.c_str());
                    ImGui::Text("Value    %0.3f", selected->value);
                    ImGui::Text("Since    %s (%s)", since, age);
                    ImGui::TextColored(severityColor(selected->severity), "%s",
                                       selected->severity == AlarmSeverity::Fault ? "fault" : "warn");
                } else if (chips.empty()) {
                    ImGui::TextColored(theme::success(), "All systems nominal");
                } else {
                    ImGui::TextUnformatted("Select a chip for details.");
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("History")) {
                if (engine.recent().empty()) {
                    ImGui::TextColored(theme::mutedFg(), "No recently cleared faults.");
                } else {
                    if (ImGui::BeginTable("##cleared", 5,
                                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                              ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Label");
                        ImGui::TableSetupColumn("Signal");
                        ImGui::TableSetupColumn("Value");
                        ImGui::TableSetupColumn("Since");
                        ImGui::TableSetupColumn("Cleared");
                        ImGui::TableHeadersRow();
                        for (const auto& row : engine.recent()) {
                            char since[32];
                            char cleared[32];
                            formatClock(row.sinceMs, since, sizeof(since));
                            formatClock(row.clearedMs, cleared, sizeof(cleared));
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextColored(severityColor(row.severity), "%s", row.label.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(row.signal.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("%0.3f", row.value);
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(since);
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(cleared);
                        }
                        ImGui::EndTable();
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndPopup();
    }
}

}  // namespace photon::ui
