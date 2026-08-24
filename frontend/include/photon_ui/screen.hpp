#pragma once

#include "photon/control.hpp"
#include "photon_ui/catalog.hpp"
#include "photon_ui/custom_view.hpp"

#include <cstdint>
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
};

class PitScreen final : public Screen {
public:
    const char* id() const override { return "pit"; }
    const char* label() const override { return "Pit"; }
    void draw(UiContext& ui) override;
};

class PlotsScreen final : public Screen {
public:
    const char* id() const override { return "plots"; }
    const char* label() const override { return "Plots"; }
    void draw(UiContext& ui) override;

private:
    CustomViews views_;
};

class DynamicsScreen final : public Screen {
public:
    const char* id() const override { return "dynamics"; }
    const char* label() const override { return "Dynamics"; }
    void draw(UiContext& ui) override;
};

class LiveViewScreen final : public Screen {
public:
    const char* id() const override { return "live"; }
    const char* label() const override { return "Live View"; }
    void draw(UiContext& ui) override;

private:
    std::uint64_t replayMs_ = 0;
};

class Scene3dScreen final : public Screen {
public:
    const char* id() const override { return "scene3d"; }
    const char* label() const override { return "3D"; }
    void draw(UiContext& ui) override;
};

class TrackSimScreen final : public Screen {
public:
    const char* id() const override { return "tracksim"; }
    const char* label() const override { return "Track Sim"; }
    void draw(UiContext& ui) override;

private:
    bool pickedDefault_ = false;
    int trackIndex_ = 0;
    int budgetMin_ = 6;
    int budgetSec_ = 40;
    float budgetS_ = 400.0f;
    float initialSoc_ = 1.0f;
    bool useMph_ = false;
    float massKg_ = 280.0f;
    float cdaM2_ = 0.12f;
    float crr_ = 0.0045f;
    float motorPowerW_ = 3500.0f;
    bool debouncePending_ = false;
    double debounceUntilS_ = 0.0;

    void syncBudgetFromParts();
    void syncPartsFromBudget();
    void sendSolve(UiContext& ui);
};

class CanScreen final : public Screen {
public:
    const char* id() const override { return "can"; }
    const char* label() const override { return "CAN"; }
    void draw(UiContext& ui) override;
};

class NetworkScreen final : public Screen {
public:
    const char* id() const override { return "network"; }
    const char* label() const override { return "Network"; }
    void draw(UiContext& ui) override;

private:
    int sourceIndex_ = 3;
    char host_[64] = "127.0.0.1";
    int port_ = 6500;
    char path_[512] = "";
    char url_[128] = "ws://127.0.0.1:8765";
    bool hydrated_ = false;
    ctrl::SourceKind syncedSource_ = ctrl::SourceKind::None;
    std::string syncedHost_;
    std::uint16_t syncedPort_ = 0;
    std::string syncedPath_;
};

void drawStubScreen(const char* title, const char* body);

}  // namespace photon::ui
