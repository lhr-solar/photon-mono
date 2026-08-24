#pragma once

#include "photon/control.hpp"
#include "photon_ui/catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace photon::ui {

class Client;
class ClientArena;

// Semantic role -> DBC "Message.Signal" mapping for Pit / Dynamics.
//
// Choice: compiled table in signal_map.cpp (not JSON-at-runtime). WASM has no
// filesystem unless we add --embed-file / generated headers; a constexpr table
// is warning-clean, has no parse failure mode, and matches catalog names
// exactly. Edit kScalarDefs / kMuxDefs in frontend/src/signal_map.cpp when the
// DBC role names change. Names are case-sensitive "Message.Signal".
//
// Resolve against the backend signal_catalog (FNV-1a ids). Missing names are
// logged to the control log and the screen falls back to synthetic ids 1-4
// (rpm/speed/soc/temp) with a SYNTHETIC watermark. Cell voltages/temps are
// muxed in the DBC (BPS_Tap_idx + tap data), not cell_voltage[i] scalars.

enum class MapScreen : unsigned {
    None = 0,
    Pit = 1u << 0,
    Dynamics = 1u << 1,
    Scene3d = 1u << 2,
    Both = Pit | Dynamics,
};

inline constexpr MapScreen operator|(MapScreen a, MapScreen b) {
    return static_cast<MapScreen>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

inline constexpr bool mapOn(MapScreen have, MapScreen want) {
    return (static_cast<unsigned>(have) & static_cast<unsigned>(want)) != 0;
}

// Same 21.56" rolling diameter as Photon/photon/gui/tireSlip.cpp.
inline constexpr double kMpsToMph = 2.2369362920544;
inline constexpr double kWheelDiameterInches = 21.56;
inline constexpr double kWheelDiameterMeters = kWheelDiameterInches * 0.0254;
inline constexpr double kMinGroundSpeedMps = 0.5;

inline double wheelSpeedMps(double rpm) {
    return rpm * (3.14159265358979323846 * kWheelDiameterMeters) / 60.0;
}

inline double slipPercent(double wheelMps, double groundMps) {
    const double denom = std::max(std::abs(groundMps), kMinGroundSpeedMps);
    return 100.0 * (wheelMps - groundMps) / denom;
}

struct ScalarDef {
    const char* role = "";
    const char* label = "";
    const char* signal = "";
    const char* unitHint = "";
    std::uint32_t syntheticId = 0;
    MapScreen screens = MapScreen::Pit;
    bool tile = true;
    bool plot = false;
    bool hasWarnLow = false;
    bool hasWarnHigh = false;
    double warnLow = 0.0;
    double warnHigh = 0.0;
};

struct MuxDef {
    const char* role = "";
    const char* indexSignal = "";
    const char* valueSignal = "";
    int count = 32;
};

struct HeatScale {
    double green = 3.30;
    double yellow = 3.70;
    double red = 4.20;
};

struct ResolvedScalar {
    const ScalarDef* def = nullptr;
    std::uint32_t id = 0;
    bool mapped = false;
    std::string unit;
    double catalogMin = 0.0;
    double catalogMax = 0.0;
    bool haveCatalogRange = false;
};

struct ResolvedMux {
    const MuxDef* def = nullptr;
    std::uint32_t indexId = 0;
    std::uint32_t valueId = 0;
    bool mapped = false;
    int count = 32;
};

struct Vital {
    const char* role = "";
    const char* label = "";
    const char* unit = "";
    const char* signal = "";
    std::uint32_t id = 0;
    double value = 0.0;
    bool have = false;
    bool mapped = false;
    bool synthetic = false;
    bool derived = false;
    bool alarm = false;
    double barMin = 0.0;
    double barMax = 0.0;
    bool hasBar = false;
};

struct CellCallout {
    int minIndex = -1;
    int maxIndex = -1;
    float minV = 0.0f;
    float maxV = 0.0f;
    float deltaV = 0.0f;
    bool valid = false;
};

class SignalMap {
public:
    SignalMap();

    void reset();
    void resolve(const std::vector<ctrl::CatalogSignal>& catalog, ClientArena& arena);
    void subscribe(Client& client, MapScreen screen) const;

    const ResolvedScalar* scalar(std::string_view role) const;
    Vital vital(std::string_view role, const ClientArena& arena) const;

    bool cellsMapped() const;
    const ResolvedMux* mux(std::string_view role) const;
    void fillCells(ClientArena& arena) const;
    static CellCallout cellCallout(const std::vector<float>& voltages);

    HeatScale heat() const { return heat_; }
    const std::vector<ResolvedScalar>& scalars() const { return scalars_; }

    static double estimatePackSoc(double packVoltage);

private:
    HeatScale heat_{};
    std::vector<ResolvedScalar> scalars_;
    std::vector<ResolvedMux> muxes_;
    bool resolvedOnce_ = false;

    Vital syntheticVital(const ResolvedScalar& slot, const ClientArena& arena) const;
    static bool valueAlarmed(const ResolvedScalar& slot, double value);
};

}  // namespace photon::ui
