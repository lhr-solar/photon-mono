#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace photon::ui {

// Synthetic fallback signals. They sit far above the catalog range (client id =
// catalog index + 1), so they never collide with a server signal.
inline constexpr std::uint32_t kSignalRpm = 0xFFFFFF01u;
inline constexpr std::uint32_t kSignalSpeed = 0xFFFFFF02u;
inline constexpr std::uint32_t kSignalSoc = 0xFFFFFF03u;
inline constexpr std::uint32_t kSignalTemp = 0xFFFFFF04u;
inline constexpr std::size_t kCellCount = 32;
inline constexpr std::size_t kRingCap = 100000;

struct SignalDesc {
    std::uint32_t id = 0;
    const char* name = "";
    const char* label = "";
    const char* unit = "";
    double warnLow = 0.0;
    double warnHigh = 0.0;
    bool hasLow = false;
    bool hasHigh = false;
};

inline constexpr SignalDesc kCatalog[] = {
    {kSignalRpm, "rpm", "Motor RPM", "rpm", 0.0, 7500.0, false, true},
    {kSignalSpeed, "speed", "Speed", "mph", 0.0, 70.0, false, true},
    {kSignalSoc, "soc", "Pack SOC", "%", 30.0, 0.0, true, false},
    {kSignalTemp, "temp", "Motor Temp", "C", 0.0, 75.0, false, true},
};

inline constexpr std::size_t kCatalogCount = sizeof(kCatalog) / sizeof(kCatalog[0]);

inline const SignalDesc* findSignal(std::uint32_t id) {
    for (const auto& s : kCatalog) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

inline bool signalAlarmed(const SignalDesc& desc, double value) {
    if (desc.hasHigh && value > desc.warnHigh) {
        return true;
    }
    if (desc.hasLow && value < desc.warnLow) {
        return true;
    }
    return false;
}

// Native default; the web build uses the page's own /v2 (web/shell.html).
inline std::string_view defaultWsUrl() { return "ws://127.0.0.1:6900/v2"; }
}  // namespace photon::ui
