#include "photon/tracksim.hpp"
#include "photon_ui/client.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/signal_map.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"
#include "implot.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace photon::ui {
namespace {

constexpr double kDefaultSoc = 0.90;
constexpr double kDefaultSolarW = 850.0;
constexpr double kMpsToKmh = 3.6;
constexpr double kDebounceS = 0.30;
constexpr float kBudgetMinS = 90.0f;
constexpr float kBudgetMaxS = 1200.0f;

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
    theme::beginCard(id, size, theme::card(), theme::kStripPadding);
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

void TrackSimScreen::queueSolve() {
    debouncePending_ = true;
    debounceUntilS_ = ImGui::GetTime() + kDebounceS;
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

    const tracksim::MeasuredDrive& m = ui.arena.trackSim.measured;
    double soc = kDefaultSoc;
    if (m.havePackV && std::isfinite(m.socEst) && m.socEst > 0.0) {
        soc = m.socEst;
        if (soc > 1.0) {
            soc = 1.0;
        }
    }

    tracksim::CarOverride car;
    if (m.haveArray && std::isfinite(m.arrayWMean) && m.arrayWMean >= 0.0) {
        car.solarW = m.arrayWMean;
    }
    ui.client.requestTrackSim(tracks[static_cast<std::size_t>(trackIndex_)].id, static_cast<double>(budgetS_), soc,
                              car);
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
            queueSolve();
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
    const float left = 328.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, theme::kStripPadding);
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
                    queueSolve();
                }
                if (isSel) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextColored(theme::mutedFg(), "%.0f m · %d segments", selected.lengthM, selected.segmentCount);
    }

    if (ui.client.helloOk() && ImGui::GetTime() - lastMeasurePollS_ > 2.5) {
        lastMeasurePollS_ = ImGui::GetTime();
        ui.client.requestTrackSimList();
    }

    const tracksim::MeasuredDrive& m = sim.measured;
    double trackLenM = 0.0;
    if (!sim.tracks.empty() && trackIndex_ >= 0 && trackIndex_ < static_cast<int>(sim.tracks.size())) {
        trackLenM = sim.tracks[static_cast<std::size_t>(trackIndex_)].lengthM;
    }

    ImGui::Spacing();
    const float liveH = m.have ? (m.haveSpeed ? 118.0f : 96.0f) : 80.0f;
    theme::beginCard("measured", ImVec2(-1.0f, liveH), theme::card(), ImVec2(8.0f, 6.0f));
    theme::pushSmallFont();
    ImGui::TextColored(theme::mutedFg(), "Live / this log");
    if (!m.have) {
        ImGui::TextWrapped("Play replay or connect until pack V and array power arrive. Solar and starting SOC come from there.");
        theme::popSmallFont();
    } else {
        if (m.haveArray) {
            ImGui::Text("Solar %0.0f W  (array, used by solver)", m.arrayWMean);
        } else {
            ImGui::Text("Solar %.0f W  (car default, no array yet)", kDefaultSolarW);
        }
        if (m.havePackV) {
            ImGui::Text("Start SOC %0.0f%%  (%0.1f V pack)", m.socEst * 100.0, m.packVMean);
        } else {
            ImGui::Text("Start SOC %.0f%%  (default)", kDefaultSoc * 100.0);
        }
        if (m.haveSpeed) {
            ImGui::Text("Speed %0.1f m/s  max %0.1f", m.speedMpsMean, m.speedMpsMax);
        }
        ImGui::TextColored(theme::mutedFg(), "%d samp · %0.0f s", m.samples, m.windowS);
        theme::popSmallFont();
    }
    theme::endCard();

    ImGui::Spacing();
    ImGui::Text("Target time");
    theme::pushSmallFont();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(theme::mutedFg(),
                       "How long this lap should take. More time = slower, cheaper. Less time = faster, more energy.");
    ImGui::PopTextWrapPos();
    theme::popSmallFont();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("min", &budgetMin_)) {
        syncBudgetFromParts();
        queueSolve();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("sec", &budgetSec_)) {
        syncBudgetFromParts();
        queueSolve();
    }
    if (ImGui::SliderFloat("##budget", &budgetS_, kBudgetMinS, kBudgetMaxS, "%.0f s")) {
        syncPartsFromBudget();
        queueSolve();
    }
    if (trackLenM > 1.0 && budgetS_ > 0.0f) {
        const double vMps = trackLenM / static_cast<double>(budgetS_);
        theme::pushSmallFont();
        const double speedScale = useMph_ ? kMpsToMph : kMpsToKmh;
        const char* speedUnit = useMph_ ? "mph" : "km/h";
        ImGui::TextColored(theme::mutedFg(), "Average %.1f %s  (%.1f m/s) over %.2f km", vMps * speedScale, speedUnit,
                           vMps, trackLenM / 1000.0);
        theme::popSmallFont();
    }
    if (m.haveSpeed && m.speedMpsMean > 0.5 && trackLenM > 1.0) {
        const double paceS = trackLenM / m.speedMpsMean;
        char paceBuf[16];
        formatMmSs(paceS, paceBuf, sizeof(paceBuf));
        theme::pushSmallFont();
        ImGui::TextColored(theme::mutedFg(), "Logged pace would take %s", paceBuf);
        theme::popSmallFont();
        if (ImGui::Button("Match logged pace", ImVec2(-1.0f, 0.0f))) {
            budgetS_ = static_cast<float>(paceS);
            if (budgetS_ < kBudgetMinS) {
                budgetS_ = kBudgetMinS;
            }
            if (budgetS_ > kBudgetMaxS) {
                budgetS_ = kBudgetMaxS;
            }
            syncPartsFromBudget();
            queueSolve();
        }
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
    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, gap);
    ImGui::BeginChild("##tracksim-main", ImVec2(avail.x - left - gap, avail.y), ImGuiChildFlags_None);

    const ImVec2 main = ImGui::GetContentRegionAvail();
    const float summaryH = 56.0f;
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
    const char* badge = !sim.haveResult ? "--" : (sim.feasible ? "FEASIBLE" : "INFEASIBLE");
    drawSummaryCard("sum-feas", "Status", badge, badgeColor, ImVec2(cardW, summaryH));
    ImGui::SameLine(0.0f, gap);
    drawSummaryCard("sum-time", "Time vs target", slackBuf, theme::foreground(), ImVec2(cardW, summaryH));
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
        ImGui::TextColored(theme::mutedFg(), "Set a target time and Solve. Drag the slider to see speed vs energy.");
    }

    if (bannerH > 0.0f) {
        theme::beginCard("infeas-banner", ImVec2(main.x, bannerH), theme::withAlpha(theme::danger(), 0.22f),
                         theme::CardFit::Band);
        ImGui::TextColored(theme::danger(), "Time budget too tight: profile shown is at limits");
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

    theme::pushPlotFont();
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
    theme::popPlotFont();

    ImGui::EndChild();
}

}  // namespace photon::ui
