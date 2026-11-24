#include "photon_ui/client.hpp"
#include "photon_ui/page_url.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace photon::ui {
namespace {

void statusRow(const char* label, const std::string& value) {
    ImGui::TextColored(theme::mutedFg(), "%s", label);
    ImGui::SameLine(110.0f);
    ImGui::TextUnformatted(value.empty() ? "--" : value.c_str());
}

}  // namespace

void NetworkScreen::draw(UiContext& ui) {
    if (!hydrated_) {
        std::snprintf(url_, sizeof(url_), "%s", ui.arena.wsUrl.c_str());
        hydrated_ = true;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float left = 380.0f;

    ImGui::BeginChild("##src", ImVec2(left, avail.y),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::Text("Connection");
    ImGui::Separator();
    const bool shared = pageIsSharedConsole();
    if (shared) {
        ImGui::TextWrapped("This website. Any laptop can open the page and sign in.");
    } else {
        ImGui::InputText("Server URL", url_, sizeof(url_));
    }
    if (ImGui::Button("Reconnect")) {
        ui.client.setUrl(shared ? pageLiveUrl() : std::string(url_));
        ui.client.disconnect("user reconnect");
    }
    ImGui::SameLine();
    ImGui::TextColored(theme::mutedFg(), "%s", ui.client.stateLabel());

    ImGui::Spacing();
    ImGui::Text("Server");
    ImGui::Separator();
    if (shared) {
        statusRow("URL", ui.client.url());
    }
    statusRow("User", ui.arena.server.user);
    statusRow("Live run", ui.arena.server.liveRun);
    statusRow("Catalog", ui.arena.catalogSha.substr(0, 12));
    statusRow("Signals", ui.arena.haveSignalCatalog ? std::to_string(ui.arena.signalCatalog.size()) : std::string());

    ImGui::Spacing();
    ImGui::Text("Backend features: ");
    if (ui.arena.features.empty()) {
        ImGui::TextColored(theme::mutedFg(), "(none yet)");
    } else {
        for (const auto& f : ui.arena.features) {
            ImGui::BulletText("%s", f.c_str());
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(0.0f, theme::kGridGap);
    ImGui::BeginChild("##log", ImVec2(avail.x - left - theme::kGridGap, avail.y),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::Text("Control plane log");
    ImGui::Separator();
    ImGui::BeginChild("##logscroll", ImVec2(0, 0), ImGuiChildFlags_None);
    for (const auto& line : ui.arena.logs()) {
        const ImVec4 color = line.error ? theme::danger() : theme::foreground();
        ImGui::TextColored(theme::mutedFg(), "%llu", static_cast<unsigned long long>(line.tMs));
        ImGui::SameLine();
        ImGui::TextColored(color, "%s", line.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

}  // namespace photon::ui
