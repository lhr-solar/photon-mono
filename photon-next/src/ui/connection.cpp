#include "ui/connection.hpp"
#include "ui/client.hpp"
#include "ui/clock_format.hpp"
#include "ui/page_url.hpp"
#include "ui/screen.hpp"
#include "ui/theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace photon::ui {
namespace {

const char* kSourceLabels[] = {"pcan", "daq_tcp", "log_replay"};
const ctrl::SourceKind kSourceKinds[] = {ctrl::SourceKind::Pcan, ctrl::SourceKind::DaqTcp,
                                         ctrl::SourceKind::LogReplay};

int indexFor(ctrl::SourceKind kind) {
    for (int i = 0; i < 3; ++i) {
        if (kSourceKinds[i] == kind) {
            return i;
        }
    }
    return 1;
}

}  // namespace

void ConnectionSettings::draw(UiContext& ui) {
    auto dbcUpload = [&] {
        if (std::find(ui.arena.features.begin(), ui.arena.features.end(), "dbc_upload") == ui.arena.features.end()) return;
        ImGui::Spacing();
        ImGui::Text("DBC files");
#ifndef __EMSCRIPTEN__
        ImGui::InputText("Local DBC file", dbcPath_, sizeof(dbcPath_));
#endif
        if (ImGui::Button("Upload DBC")) ui.client.uploadDbc(dbcPath_);
        ImGui::TextWrapped("%s", ui.client.dbcUploadStatus().c_str());
        if (!ui.client.uploadedDbc().empty()) {
            if (ImGui::Button("Apply to live source")) ui.client.applyUploadedDbc();
            if (ui.arena.catalogPinned() && ImGui::Button("Re-decode this recording")) ui.client.redecodeWithUploadedDbc();
        }
        if (ui.arena.catalogPinned()) {
            if (ImGui::Button("Refresh decoding versions")) ui.client.requestCatalogList();
            for (const auto& recording : ui.client.historyCatalog()) if (recording.id == ui.arena.tiles().sessionId()) {
                if (ImGui::BeginCombo("Decoding", ui.client.selectedDecode().c_str())) {
                    for (const auto& version : recording.decodes) {
                        const auto label = version.dbcHash.substr(0, 12) + " / " + version.state + "##" + version.id;
                        if (ImGui::Selectable(label.c_str(), version.id == ui.client.selectedDecode()))
                            ui.client.selectDecoding(recording.id, version.id);
                    }
                    ImGui::EndCombo();
                }
            }
        }
    };
    auto diagnostics = [&] {
        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Diagnostics")) {
            for (const auto& feature : ui.arena.features) ImGui::BulletText("%s", feature.c_str());
            ImGui::BeginChild("##connection-log", ImVec2(0, 180), ImGuiChildFlags_None);
            for (const auto& line : ui.arena.logs()) ImGui::TextWrapped("%s", line.text.c_str());
            ImGui::EndChild();
        }
    };
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

    ImGui::Text("Connection");
    ImGui::Separator();
    const bool shared = pageIsSharedConsole();
    if (shared) {
        ImGui::TextWrapped("This website. Any laptop can open the page and sign in.");
        ImGui::TextColored(theme::mutedFg(), "%s", ui.client.stateLabel());
        if (ImGui::Button("Reconnect")) {
            ui.client.setUrl(pageLiveUrl());
            ui.client.disconnect("user reconnect");
        }
        const bool onLive = ui.arena.source == ctrl::SourceKind::DaqTcp ||
                            ui.arena.source == ctrl::SourceKind::Pcan ||
                            ui.arena.source == ctrl::SourceKind::Synthetic;
        ImGui::Spacing();
        if (onLive) {
            const auto host = sourceDisplayHost(ui.arena.sourceHost);
            ImGui::TextColored(theme::primary(), "Watching live");
            if (!host.empty()) {
                ImGui::TextColored(theme::mutedFg(), "%s", host.c_str());
            }
        } else if (ui.arena.source == ctrl::SourceKind::LogReplay) {
            ImGui::TextColored(theme::primary(), "Replaying a drive day");
            if (!ui.arena.sourcePath.empty()) {
                ImGui::TextColored(theme::mutedFg(), "%s", ui.arena.sourcePath.c_str());
            }
        }
        if (ImGui::Button(onLive ? "Already live" : "Back to live", ImVec2(-1.0f, 0.0f))) {
            ui.client.goLive();
        }
        dbcUpload();
        diagnostics();
        return;
    }
    ImGui::InputText("Backend URL", url_, sizeof(url_));
    if (ImGui::Button("Reconnect")) {
        ui.client.setUrl(url_);
        ui.client.disconnect("user reconnect");
    }
    ImGui::SameLine();
    ImGui::TextColored(theme::mutedFg(), "%s", ui.client.stateLabel());

    ImGui::Spacing();
    ImGui::Text("Source");
    ImGui::Separator();
    ImGui::Combo("Kind", &sourceIndex_, kSourceLabels, 3);
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
        if (pcanChannel_ < 1) {
            pcanChannel_ = 1;
        }
        if (pcanChannel_ > 8) {
            pcanChannel_ = 8;
        }
        ImGui::SliderInt("Channel", &pcanChannel_, 1, 8);
        const char* baudLabels[] = {"125 kbit/s", "250 kbit/s", "500 kbit/s", "1 Mbit/s"};
        ImGui::Combo("Baud", &pcanBaudIndex_, baudLabels, 4);
        if (pcanBaudIndex_ < 0) {
            pcanBaudIndex_ = 1;
        }
        if (pcanBaudIndex_ > 3) {
            pcanBaudIndex_ = 3;
        }
        ImGui::Checkbox("Listen only", &pcanListenOnly_);
        ImGui::TextColored(theme::mutedFg(), "USB PCAN. Listen-only is the safe default.");
    }

    if (ImGui::Button("Apply source", ImVec2(-1.0f, 0.0f))) {
        const int bauds[] = {125000, 250000, 500000, 1000000};
        const int baud = bauds[pcanBaudIndex_ < 0 || pcanBaudIndex_ > 3 ? 1 : pcanBaudIndex_];
        ui.client.selectSource(kind, host_, static_cast<std::uint16_t>(port_), path_, pcanChannel_, baud,
                               pcanListenOnly_);
    }

    ImGui::Spacing();
    ImGui::Text("DBC");
    ImGui::Separator();
    const bool dbcChanged = ui.arena.dbcId != syncedDbcId_ || ui.arena.dbcPath != syncedDbcPath_;
    if (dbcChanged) {
        syncedDbcId_ = ui.arena.dbcId;
        syncedDbcPath_ = ui.arena.dbcPath;
        std::snprintf(dbcPath_, sizeof(dbcPath_), "%s", ui.arena.dbcPath.c_str());
        dbcIndex_ = 0;
        for (int i = 0; i < static_cast<int>(ui.arena.dbcs.size()); ++i) {
            if (ui.arena.dbcs[static_cast<std::size_t>(i)] == ui.arena.dbcId) {
                dbcIndex_ = i;
                break;
            }
        }
    }
    if (ui.arena.dbcs.empty()) {
        ImGui::TextColored(theme::mutedFg(), "No bundled DBC in assets/dbc.");
    } else {
        const char* preview = dbcIndex_ >= 0 && dbcIndex_ < static_cast<int>(ui.arena.dbcs.size())
                                  ? ui.arena.dbcs[static_cast<std::size_t>(dbcIndex_)].c_str()
                                  : ui.arena.dbcId.c_str();
        if (ImGui::BeginCombo("File", preview[0] != '\0' ? preview : "(none)")) {
            for (int i = 0; i < static_cast<int>(ui.arena.dbcs.size()); ++i) {
                const auto& id = ui.arena.dbcs[static_cast<std::size_t>(i)];
                const bool selected = i == dbcIndex_;
                if (ImGui::Selectable(id.c_str(), selected) && i != dbcIndex_) {
                    dbcIndex_ = i;
                    ui.client.selectDbc(id, true);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::InputText("Path", dbcPath_, sizeof(dbcPath_));
    if (ImGui::Button("Load path", ImVec2(-1.0f, 0.0f))) {
        if (dbcPath_[0] != '\0') {
            ui.client.selectDbc(dbcPath_, false);
        }
    }
    if (ui.arena.dbcId.empty()) {
        ImGui::TextColored(theme::mutedFg(), "No DBC loaded.");
    } else {
        ImGui::TextColored(theme::mutedFg(), "Active %s  %zu signals", ui.arena.dbcId.c_str(),
                           ui.arena.signalCatalog.size());
    }

    dbcUpload();
    diagnostics();
}

}  // namespace photon::ui
