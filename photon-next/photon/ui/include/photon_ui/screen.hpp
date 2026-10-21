#pragma once

#include "photon/control.hpp"
#include "photon_ui/analysis_workbook.hpp"
#include "photon_ui/catalog.hpp"
#include "photon_ui/custom_view.hpp"

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
    std::uint64_t replayMs_ = 0;
    bool showScene_ = true;
    bool showVideo_ = true;
    float sceneFraction_ = 0.5f;
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
    bool useMph_ = true;
    bool debouncePending_ = false;
    double debounceUntilS_ = 0.0;
    double lastMeasurePollS_ = -10.0;

    void syncBudgetFromParts();
    void syncPartsFromBudget();
    void queueSolve();
    void sendSolve(UiContext& ui);
};

class ArraySimScreen final : public Screen {
public:
    const char* id() const override { return "arraysim"; }
    const char* label() const override { return "Array Sim"; }
    void draw(UiContext& ui) override;

private:
    char stlPath_[1024]{};
    float azimuthDeg_ = 125.75f;
    float elevationDeg_ = 72.05125f;
    float dniWm2_ = 850.0f;
    float dhiWm2_ = 110.0f;
    int directSamples_ = 3;
    int hemisphereSamples_ = 64;
    int reflectionDepth_ = 2;
};

class CanScreen final : public Screen {
public:
    const char* id() const override { return "can"; }
    const char* label() const override { return "CAN"; }
    void draw(UiContext& ui) override;

private:
    char search_[128]{};
};

class NetworkScreen final : public Screen {
public:
    const char* id() const override { return "network"; }
    const char* label() const override { return "Network"; }
    void draw(UiContext& ui) override;

private:
    int sourceIndex_ = 1;
    char host_[64] = "127.0.0.1";
    int port_ = 6500;
    char path_[512] = "";
    char url_[128] = "ws://127.0.0.1:8765";
    int pcanChannel_ = 1;
    int pcanBaudIndex_ = 1;
    bool pcanListenOnly_ = true;
    bool hydrated_ = false;
    ctrl::SourceKind syncedSource_ = ctrl::SourceKind::None;
    std::string syncedHost_;
    std::uint16_t syncedPort_ = 0;
    std::string syncedPath_;
    int dbcIndex_ = 0;
    char dbcPath_[512] = "";
    std::string syncedDbcId_;
    std::string syncedDbcPath_;
};

class AnalysisRuntime;

class AnalysisScreen final : public Screen {
public:
    AnalysisScreen();
    ~AnalysisScreen() override;
    AnalysisScreen(const AnalysisScreen&) = delete;
    AnalysisScreen& operator=(const AnalysisScreen&) = delete;

    const char* id() const override { return "analysis"; }
    const char* label() const override { return "Analysis"; }
    void draw(UiContext& ui) override;
    void onPlayOrLive() override;

    AnalysisRuntime* runtime() { return runtime_.get(); }
    const AnalysisRuntime* runtime() const { return runtime_.get(); }
    void attachTransport(Client& client);

    struct State;

private:
    std::unique_ptr<AnalysisRuntime> runtime_;
    std::unique_ptr<State> state_;
};

void drawStubScreen(const char* title, const char* body);

}  // namespace photon::ui
