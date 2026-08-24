#include "photon_ui/client.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"

#include <cstdio>
#include <cstring>

namespace photon::ui {
namespace {

const char* kSourceLabels[] = {"pcan", "daq_tcp", "log_replay", "synthetic"};
const ctrl::SourceKind kSourceKinds[] = {ctrl::SourceKind::Pcan, ctrl::SourceKind::DaqTcp,
                                         ctrl::SourceKind::LogReplay, ctrl::SourceKind::Synthetic};

int indexFor(ctrl::SourceKind kind) {
    for (int i = 0; i < 4; ++i) {
        if (kSourceKinds[i] == kind) {
            return i;
        }
    }
    return 3;
}

}  // namespace

void NetworkScreen::draw(UiContext& ui) {
    const bool backendChanged = ui.arena.source != syncedSource_ || ui.arena.sourceHost != syncedHost_ ||
                                ui.arena.sourcePort != syncedPort_ || ui.arena.sourcePath != syncedPath_;
    if (!hydrated_ || backendChanged) {
        std::snprintf(url_, sizeof(url_), "%s", ui.arena.wsUrl.c_str());
        std::snprintf(host_, sizeof(host_), "%s", ui.arena.sourceHost.c_str());
        std::snprintf(path_, sizeof(path_), "%s", ui.arena.sourcePath.c_str());
        sourceIndex_ = indexFor(ui.arena.source);
        port_ = ui.arena.sourcePort;
        syncedSource_ = ui.arena.source;
        syncedHost_ = ui.arena.sourceHost;
        syncedPort_ = ui.arena.sourcePort;
        syncedPath_ = ui.arena.sourcePath;
        hydrated_ = true;
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float left = 340.0f;

    ImGui::BeginChild("##src", ImVec2(left, avail.y),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::Text("Connection");
    ImGui::Separator();
    ImGui::InputText("WS URL", url_, sizeof(url_));
    if (ImGui::Button("Reconnect")) {
        ui.client.setUrl(url_);
        ui.client.disconnect("user reconnect");
    }
    ImGui::SameLine();
    ImGui::TextColored(theme::mutedFg(), "%s", ui.client.stateLabel());

    ImGui::Spacing();
    ImGui::Text("Source");
    ImGui::Separator();
    ImGui::Combo("Kind", &sourceIndex_, kSourceLabels, 4);
    const ctrl::SourceKind kind = kSourceKinds[sourceIndex_];
    if (kind == ctrl::SourceKind::DaqTcp) {
        ImGui::InputText("Host", host_, sizeof(host_));
        ImGui::InputInt("Port", &port_);
        if (port_ < 1) {
            port_ = 1;
        }
        if (port_ > 65535) {
            port_ = 65535;
        }
    } else if (kind == ctrl::SourceKind::LogReplay) {
        ImGui::InputText("Path", path_, sizeof(path_));
    } else if (kind == ctrl::SourceKind::Pcan) {
        ImGui::TextColored(theme::warning(), "pcan is not implemented on the backend yet.");
    }

    if (ImGui::Button("select_source", ImVec2(-1.0f, 0.0f))) {
        ui.client.selectSource(kind, host_, static_cast<std::uint16_t>(port_), path_);
    }
    ImGui::TextWrapped("Backend features: ");
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
