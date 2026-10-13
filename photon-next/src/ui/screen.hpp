#pragma once

#include "ui/analysis_workbook.hpp"
#include "ui/catalog.hpp"
#include "ui/custom_view.hpp"
#include "wire/control.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace photon::ui {

class Client;
class ClientArena;

struct UiContext {
    ClientArena& arena;
    Client& client;
};

class Screen {
public:
    virtual ~Screen() = default;
    virtual const char* id() const = 0;
    virtual const char* label() const = 0;
    virtual void draw(UiContext& ui) = 0;
    virtual void onPlayOrLive() {}
    virtual void onSessionChanged() { onPlayOrLive(); }
};

class PitScreen final : public Screen {
public:
    const char* id() const override { return "pit"; }
    const char* label() const override { return "Pit"; }
    void draw(UiContext& ui) override;
};

class DashboardScreen final : public Screen {
public:
    const char* id() const override { return "dashboard"; }
    const char* label() const override { return "Dashboard"; }
    void draw(UiContext& ui) override;

private:
    bool showDebug_ = false;
    std::uint8_t heartbeat_ = 0;
};

class PlotsScreen final : public Screen {
public:
    const char* id() const override { return "plots"; }
    const char* label() const override { return "Plots"; }
    void draw(UiContext& ui) override;
    void onPlayOrLive() override;
    void onSessionChanged() override;

private:
    CustomViews views_;
};

class DynamicsScreen final : public Screen {
public:
    const char* id() const override { return "dynamics"; }
    const char* label() const override { return "Dynamics"; }
    void draw(UiContext& ui) override;
    void drawRaceValues(UiContext& ui);

private:
    int imuIndex_ = 0;
    std::string peakKey_;
    bool peakLong_ = false;
    double peakLongMax_ = 0.0;
    double peakLongMin_ = 0.0;
    std::uint64_t peakLongMaxMs_ = 0;
    std::uint64_t peakLongMinMs_ = 0;
    bool peakLat_ = false;
    double peakLatMax_ = 0.0;
    double peakLatMin_ = 0.0;
    std::uint64_t peakLatMaxMs_ = 0;
    std::uint64_t peakLatMinMs_ = 0;
    bool peakCombHave_ = false;
    double peakComb_ = 0.0;
    std::uint64_t peakCombMs_ = 0;
    bool peakWhFlHave_ = false;
    bool peakWhFrHave_ = false;
    double peakWhFl_ = 0.0;
    double peakWhFr_ = 0.0;
    std::uint64_t peakWhFlMs_ = 0;
    std::uint64_t peakWhFrMs_ = 0;
};

class MechanicalScreen final : public Screen {
public:
    const char* id() const override { return "mechanical"; }
    const char* label() const override { return "Mechanical"; }
    void draw(UiContext& ui) override;
    void openDynamics() { wantDynamics_ = true; }

private:
    DynamicsScreen dynamics_;
    bool wantDynamics_ = false;
};

class PduScreen final : public Screen {
public:
    const char* id() const override { return "pdu"; }
    const char* label() const override { return "PDU"; }
    void draw(UiContext& ui) override;

private:
    int selected_ = 0;
};

class LiveViewScreen final : public Screen {
public:
    const char* id() const override { return "live"; }
    const char* label() const override { return "Live View"; }
    void draw(UiContext& ui) override;

private:
    float sceneFraction_ = 0.5f;
};

class CanScreen final : public Screen {
public:
    const char* id() const override { return "can"; }
    const char* label() const override { return "CAN"; }
    void draw(UiContext& ui) override;

private:
    char search_[128]{};
};

void drawStubScreen(const char* title, const char* body);

}  // namespace photon::ui
