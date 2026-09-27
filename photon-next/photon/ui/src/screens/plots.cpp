#include "photon_ui/client.hpp"
#include "photon_ui/grafana_host.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace photon::ui {
namespace {

const char* defaultGrafanaUrl() {
    const char* env = std::getenv("PHOTON_GRAFANA_URL");
    if (env != nullptr && env[0] != '\0') {
        return env;
    }
    return "http://127.0.0.1:3000";
}

}  // namespace

void PlotsScreen::draw(UiContext& ui) {
    if (!grafanaUrlReady_) {
        std::snprintf(grafanaUrl_, sizeof(grafanaUrl_), "%s", defaultGrafanaUrl());
        grafanaUrlReady_ = true;
    }
    if (ImGui::Button(grafanaMode_ == 0 ? "Grafana" : "Show Grafana")) {
        grafanaMode_ = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button(grafanaMode_ == 1 ? "Plots" : "Show plots")) {
        grafanaMode_ = 1;
        grafanaShow(0);
    }
    if (grafanaMode_ == 1) {
        views_.draw(ui);
        return;
    }
    char status[96]{};
    grafanaStatus(status, static_cast<int>(sizeof(status)));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::primary(), "Grafana");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::min(420.0f, ImGui::GetContentRegionAvail().x * 0.55f));
    const bool edited = ImGui::InputText("##grafana-url", grafanaUrl_, sizeof(grafanaUrl_));
    ImGui::SameLine();
    if (ImGui::Button("Open") || (!grafanaOpened_ && !edited)) {
        grafanaNavigate(grafanaUrl_);
        grafanaOpened_ = true;
    }
    ImGui::SameLine();
    ImGui::TextColored(theme::mutedFg(), "%s", status[0] != '\0' ? status : "Grafana");
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(avail);
    if (avail.x > 8.0f && avail.y > 8.0f) {
        grafanaLayout(origin.x, origin.y, avail.x, avail.y);
        grafanaShow(1);
    } else {
        grafanaShow(0);
    }
}

void PlotsScreen::onPlayOrLive() { views_.releasePlayheadScrub(); }

void PlotsScreen::onSessionChanged() { views_.followSessionWindow(); }

}  // namespace photon::ui
