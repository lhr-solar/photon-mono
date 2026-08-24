#include "photon_ui/client.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"
#include "implot.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace photon::ui {
namespace {

constexpr float kDefaultMassKg = 280.0f;
constexpr float kDefaultCda = 0.12f;
constexpr float kDefaultCrr = 0.0045f;
constexpr float kDefaultMotorW = 3500.0f;
constexpr double kMpsToKmh = 3.6;
constexpr double kDebounceS = 0.30;

void formatMmSs(double seconds, char* buf, std::size_t n) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        std::snprintf(buf, n, "--:--");
        return;
    }
    const int total = static_cast<int>(seconds + 0.5);
    const int m = total / 60;
    const int s = total % 60;
    std::snprintf(buf, n, "%d:%02d", m, s);
}

void drawSpinner(ImVec2 center, float radius, ImU32 color) {
    const float t = static_cast<float>(ImGui::GetTime());
    const float a0 = t * 6.2831853f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PathClear();
    draw->PathArcTo(center, radius, a0, a0 + 2.2f, 14);
    draw->PathStroke(color, ImDrawFlags_None, 2.0f);
}

void dEdTExplanation(double dEdT, char* buf, std::size_t n) {
    const double mag = std::fabs(dEdT);
    if (mag < 1.0e-6) {
        std::snprintf(buf, n, "A little more time barely changes energy use.");
        return;
    }
    if (dEdT < 0.0) {
        std::snprintf(buf, n, "Each extra second of budget saves about %.3f Wh (slowing down is cheaper).", mag);
        return;
    }
    std::snprintf(buf, n, "Each extra second of budget costs about %.3f Wh.", mag);
}

void drawSummaryCard(const char* id, const char* label, const char* value, ImVec4 valueColor, ImVec2 size) {
    theme::beginCard(id, size, theme::card());
    ImGui::TextColored(theme::mutedFg(), "%s", label);
    ImGui::TextColored(valueColor, "%s", value);
    theme::endCard();
}

}  // namespace

void TrackSimScreen::syncBudgetFromParts() {
    if (budgetSec_ >= 60) {
        budgetMin_ += budgetSec_ / 60;
        budgetSec_ %= 60;
    }
    while (budgetSec_ < 0 && budgetMin_ > 0) {
        budgetSec_ += 60;
        --budgetMin_;
    }
    if (budgetMin_ < 0) {
        budgetMin_ = 0;
        budgetSec_ = 1;
    }
    if (budgetSec_ < 0) {
        budgetSec_ = 1;
    }
    budgetS_ = static_cast<float>(budgetMin_ * 60 + budgetSec_);
    if (budgetS_ < 1.0f) {
        budgetS_ = 1.0f;
        budgetMin_ = 0;
        budgetSec_ = 1;
    }
}

void TrackSimScreen::syncPartsFromBudget() {
    if (budgetS_ < 1.0f) {
        budgetS_ = 1.0f;
    }
    const int total = static_cast<int>(budgetS_ + 0.5f);
    budgetMin_ = total / 60;
    budgetSec_ = total % 60;
}

void TrackSimScreen::sendSolve(UiContext& ui) {
    debouncePending_ = false;
    const auto& tracks = ui.arena.trackSim.tracks;
    if (!ui.client.helloOk()) {
        ui.arena.trackSim.error = "not connected";
        return;
    }
    if (tracks.empty() || trackIndex_ < 0 || trackIndex_ >= static_cast<int>(tracks.size())) {
        ui.arena.trackSim.error = "select a track";
        return;
    }
    if (!(budgetS_ > 0.0f)) {
        ui.arena.trackSim.error = "time budget must be positive";
        return;
    }
    float soc = initialSoc_;
    if (soc < 0.0f) {
        soc = 0.0f;
        initialSoc_ = 0.0f;
    }
    if (soc > 1.0f) {
        soc = 1.0f;
        initialSoc_ = 1.0f;
    }

    tracksim::CarOverride car;
    if (massKg_ != kDefaultMassKg) {
        car.massKg = static_cast<double>(massKg_);
    }
    if (cdaM2_ != kDefaultCda) {
        car.cdaM2 = static_cast<double>(cdaM2_);
    }
    if (crr_ != kDefaultCrr) {
        car.crr = static_cast<double>(crr_);
    }
    if (motorPowerW_ != kDefaultMotorW) {
        car.motorPowerW = static_cast<double>(motorPowerW_);
    }
    ui.client.requestTrackSim(tracks[static_cast<std::size_t>(trackIndex_)].id, static_cast<double>(budgetS_),
                              static_cast<double>(soc), car);
}

void TrackSimScreen::draw(UiContext& ui) {
    TrackSimState& sim = ui.arena.trackSim;
    if (ui.client.helloOk() && !sim.catalogReady && !sim.catalogPending) {
        ui.client.requestTrackSimList();
    }

    if (sim.catalogReady && !sim.tracks.empty()) {
        if (trackIndex_ < 0 || trackIndex_ >= static_cast<int>(sim.tracks.size())) {
            trackIndex_ = 0;
        }
        if (!pickedDefault_) {
            pickedDefault_ = true;
            for (int i = 0; i < static_cast<int>(sim.tracks.size()); ++i) {
                if (sim.tracks[static_cast<std::size_t>(i)].id == "hill_loop") {
                    trackIndex_ = i;
                    break;
                }
            }
        }
    }

    if (debouncePending_ && ImGui::GetTime() >= debounceUntilS_) {
        const bool canAuto = ui.client.helloOk() && !sim.tracks.empty();
        if (canAuto) {
            sendSolve(ui);
        } else {
            debouncePending_ = false;
        }
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = theme::kGridGap;
    const float left = 300.0f;

    ImGui::BeginChild("##tracksim-ctrl", ImVec2(left, avail.y),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::Text("Track Sim");
    ImGui::SameLine();
    ImGui::TextColored(theme::mutedFg(), "strategy solver");
    ImGui::Separator();

    if (sim.tracks.empty()) {
        if (sim.catalogPending) {
            ImGui::TextColored(theme::mutedFg(), "Loading tracks…");
        } else if (!ui.client.helloOk()) {
            ImGui::TextColored(theme::mutedFg(), "Waiting for backend…");
        } else {
            ImGui::TextColored(theme::mutedFg(), "No tracks in catalog.");
        }
    } else {
        const tracksim::TrackInfo& selected = sim.tracks[static_cast<std::size_t>(trackIndex_)];
        const char* preview = selected.name.empty() ? selected.id.c_str() : selected.name.c_str();
        if (ImGui::BeginCombo("Track", preview)) {
            for (int i = 0; i < static_cast<int>(sim.tracks.size()); ++i) {
                const tracksim::TrackInfo& info = sim.tracks[static_cast<std::size_t>(i)];
                const bool isSel = i == trackIndex_;
                const char* label = info.name.empty() ? info.id.c_str() : info.name.c_str();
                if (ImGui::Selectable(label, isSel)) {
                    trackIndex_ = i;
                }
                if (isSel) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextColored(theme::mutedFg(), "%.0f m · %d segments", selected.lengthM, selected.segmentCount);
    }

    ImGui::Spacing();
    ImGui::Text("Time budget");
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("min", &budgetMin_)) {
        syncBudgetFromParts();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("sec", &budgetSec_)) {
        syncBudgetFromParts();
    }
    if (ImGui::SliderFloat("##budget", &budgetS_, 90.0f, 1200.0f, "%.0f s")) {
        syncPartsFromBudget();
        if (!sim.tracks.empty()) {
            debouncePending_ = true;
            debounceUntilS_ = ImGui::GetTime() + kDebounceS;
        }
    }

    ImGui::Spacing();
    if (ImGui::InputFloat("Initial SOC", &initialSoc_, 0.05f, 0.10f, "%.2f")) {
        if (initialSoc_ < 0.0f) {
            initialSoc_ = 0.0f;
        }
        if (initialSoc_ > 1.0f) {
            initialSoc_ = 1.0f;
        }
    }

    if (ImGui::CollapsingHeader("Car overrides")) {
        ImGui::TextColored(theme::mutedFg(), "Leave at defaults to use the backend car.");
        ImGui::InputFloat("Mass (kg)", &massKg_, 1.0f, 10.0f, "%.1f");
        ImGui::InputFloat("CdA (m^2)", &cdaM2_, 0.005f, 0.02f, "%.3f");
        ImGui::InputFloat("Crr", &crr_, 0.0005f, 0.001f, "%.4f");
        ImGui::InputFloat("Motor (W)", &motorPowerW_, 50.0f, 250.0f, "%.0f");
    }

    ImGui::Spacing();
    const bool canSolve = ui.client.helloOk() && !sim.tracks.empty();
    ImGui::BeginDisabled(!canSolve);
    if (ImGui::Button("Solve", ImVec2(-1.0f, 0.0f))) {
        sendSolve(ui);
    }
    ImGui::EndDisabled();

    if (sim.solving) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        drawSpinner(ImVec2(p.x + 10.0f, p.y + 10.0f), 7.0f, theme::u32(theme::primary()));
        ImGui::Dummy(ImVec2(24.0f, 20.0f));
        ImGui::SameLine();
        ImGui::TextColored(theme::mutedFg(), "Solving…");
    } else if (debouncePending_) {
        ImGui::TextColored(theme::mutedFg(), "Updating…");
    }

    if (!sim.error.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(theme::danger(), "%s", sim.error.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    ImGui::SameLine(0.0f, gap);
    ImGui::BeginChild("##tracksim-main", ImVec2(avail.x - left - gap, avail.y), ImGuiChildFlags_None);

    const ImVec2 main = ImGui::GetContentRegionAvail();
    const float summaryH = 72.0f;
    const float explainH = 22.0f;
    const float toggleH = 28.0f;
    const float bannerH = (sim.haveResult && !sim.feasible) ? 36.0f : 0.0f;
    const float plotGaps = gap * (bannerH > 0.0f ? 3.0f : 2.0f);
    float plotsH = main.y - summaryH - explainH - toggleH - bannerH - plotGaps;
    if (plotsH < 120.0f) {
        plotsH = 120.0f;
    }
    const float speedH = plotsH * 0.56f;
    const float energyH = plotsH - speedH;

    const float cardW = (main.x - gap * 4.0f) / 5.0f;
    char timeBuf[32];
    char budgetBuf[32];
    char slackBuf[48];
    char energyBuf[32];
    char socBuf[32];
    char dedtBuf[32];
    formatMmSs(sim.totalTimeS, timeBuf, sizeof(timeBuf));
    formatMmSs(sim.timeBudgetS, budgetBuf, sizeof(budgetBuf));
    if (sim.haveResult) {
        const double slack = sim.timeBudgetS - sim.totalTimeS;
        char slackMm[16];
        formatMmSs(std::fabs(slack), slackMm, sizeof(slackMm));
        if (slack >= 0.0) {
            std::snprintf(slackBuf, sizeof(slackBuf), "%s / %s  +%s", timeBuf, budgetBuf, slackMm);
        } else {
            std::snprintf(slackBuf, sizeof(slackBuf), "%s / %s  %s over", timeBuf, budgetBuf, slackMm);
        }
        std::snprintf(energyBuf, sizeof(energyBuf), "%.1f Wh", sim.energyWh);
        std::snprintf(socBuf, sizeof(socBuf), "%.1f %%", sim.endSoc * 100.0);
        std::snprintf(dedtBuf, sizeof(dedtBuf), "%.4f Wh/s", sim.dEdT);
    } else {
        std::snprintf(slackBuf, sizeof(slackBuf), "-- / %s", budgetBuf);
        std::snprintf(energyBuf, sizeof(energyBuf), "--");
        std::snprintf(socBuf, sizeof(socBuf), "--");
        std::snprintf(dedtBuf, sizeof(dedtBuf), "--");
    }

    const ImVec4 badgeColor = !sim.haveResult ? theme::mutedFg() : (sim.feasible ? theme::success() : theme::danger());
    const char* badge = !sim.haveResult ? "—" : (sim.feasible ? "FEASIBLE" : "INFEASIBLE");
    drawSummaryCard("sum-feas", "Status", badge, badgeColor, ImVec2(cardW, summaryH));
    ImGui::SameLine(0.0f, gap);
    drawSummaryCard("sum-time", "Time vs budget", slackBuf, theme::foreground(), ImVec2(cardW, summaryH));
    ImGui::SameLine(0.0f, gap);
    drawSummaryCard("sum-wh", "Energy", energyBuf, theme::foreground(), ImVec2(cardW, summaryH));
    ImGui::SameLine(0.0f, gap);
    drawSummaryCard("sum-soc", "End SOC", socBuf, theme::foreground(), ImVec2(cardW, summaryH));
    ImGui::SameLine(0.0f, gap);
    drawSummaryCard("sum-dedt", "dE/dT", dedtBuf, theme::foreground(), ImVec2(cardW, summaryH));

    if (sim.haveResult) {
        char explain[160];
        dEdTExplanation(sim.dEdT, explain, sizeof(explain));
        ImGui::TextColored(theme::mutedFg(), "%s", explain);
    } else {
        ImGui::TextColored(theme::mutedFg(), "Solve a track to get a recommended speed profile.");
    }

    if (bannerH > 0.0f) {
        theme::beginCard("infeas-banner", ImVec2(main.x, bannerH), theme::withAlpha(theme::danger(), 0.22f),
                         theme::CardFit::Band);
        ImGui::TextColored(theme::danger(), "Time budget too tight — profile shown is at limits");
        theme::endCard();
    }

    if (ImGui::RadioButton("km/h", !useMph_)) {
        useMph_ = false;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("mph", useMph_)) {
        useMph_ = true;
    }

    const double speedScale = useMph_ ? kMpsToMph : kMpsToKmh;
    const char* speedUnit = useMph_ ? "mph" : "km/h";

    std::vector<double> stairX;
    std::vector<double> stairV;
    std::vector<double> stairG;
    std::vector<double> energyX;
    std::vector<double> energyY;
    if (sim.haveResult && !sim.segments.empty()) {
        stairX.reserve(sim.segments.size() * 2);
        stairV.reserve(sim.segments.size() * 2);
        stairG.reserve(sim.segments.size() * 2);
        energyX.reserve(sim.segments.size() + 1);
        energyY.reserve(sim.segments.size() + 1);
        energyX.push_back(0.0);
        energyY.push_back(0.0);
        double distM = 0.0;
        for (const auto& seg : sim.segments) {
            const double x0 = distM / 1000.0;
            const double x1 = (distM + seg.lengthM) / 1000.0;
            const double v = seg.speedMps * speedScale;
            stairX.push_back(x0);
            stairX.push_back(x1);
            stairV.push_back(v);
            stairV.push_back(v);
            stairG.push_back(seg.grade);
            stairG.push_back(seg.grade);
            distM += seg.lengthM;
            energyX.push_back(distM / 1000.0);
            energyY.push_back(seg.whCum);
        }
    }

    if (ImPlot::BeginPlot("##speed-profile", ImVec2(-1.0f, speedH))) {
        ImPlot::SetupAxes("distance (km)", speedUnit);
        ImPlot::SetupAxis(ImAxis_Y2, "grade", ImPlotAxisFlags_AuxDefault);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.0, ImPlotCond_Once);
        if (!stairX.empty()) {
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
            ImPlot::SetNextFillStyle(theme::plotSeries(1), 0.22f);
            ImPlot::SetNextLineStyle(theme::plotSeries(1), theme::plotLineWeight());
            ImPlot::PlotShaded("grade", stairX.data(), stairG.data(), static_cast<int>(stairX.size()));
            ImPlot::PlotLine("grade", stairX.data(), stairG.data(), static_cast<int>(stairX.size()));
            ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
            ImPlot::SetNextLineStyle(theme::plotSeries(0), theme::plotLineWeight());
            ImPlot::PlotLine("speed", stairX.data(), stairV.data(), static_cast<int>(stairX.size()));
        }
        ImPlot::EndPlot();
    }

    if (ImPlot::BeginPlot("##energy-profile", ImVec2(-1.0f, energyH))) {
        ImPlot::SetupAxes("distance (km)", "Wh");
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.0, ImPlotCond_Once);
        if (!energyX.empty()) {
            ImPlot::SetNextLineStyle(theme::success(), theme::plotLineWeight());
            ImPlot::PlotLine("energy", energyX.data(), energyY.data(), static_cast<int>(energyX.size()));
            if (sim.haveResult) {
                ImPlot::Annotation(energyX.back(), energyY.back(), theme::foreground(), ImVec2(8.0f, -8.0f), false,
                                   "end SOC %.1f%%", sim.endSoc * 100.0);
            }
        }
        ImPlot::EndPlot();
    }

    ImGui::EndChild();
}

}  // namespace photon::ui
