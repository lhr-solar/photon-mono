#include "photon_ui/client.hpp"
#include "photon_ui/clock_format.hpp"
#include "photon_ui/page_url.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/theme.hpp"

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

bool pathMentionsDay(const std::string& path, const ctrl::Message::DriveDay& day) {
    if (path.empty()) {
        return false;
    }
    if (!day.path.empty() && path == day.path) {
        return true;
    }
    return !day.date.empty() && path.find(day.date) != std::string::npos;
}

std::vector<std::string> runDates(const UiContext& ui) {
    std::vector<std::string> dates;
    dates.reserve(ui.arena.driveDays.size() + ui.client.historyCatalog().size());
    for (const auto& day : ui.arena.driveDays) {
        if (isReplayLogDate(day.date)) {
            dates.push_back(day.date);
        }
    }
    for (const auto& row : ui.client.historyCatalog()) {
        if (row.maxMs <= row.minMs) {
            continue;
        }
        const auto a = localYmdFromUnixMs(row.minMs);
        const auto b = localYmdFromUnixMs(row.maxMs);
        if (isReplayLogDate(a)) {
            dates.push_back(a);
        }
        if (isReplayLogDate(b) && b != a) {
            dates.push_back(b);
        }
    }
    std::sort(dates.begin(), dates.end());
    dates.erase(std::unique(dates.begin(), dates.end()), dates.end());
    return dates;
}

const ctrl::Message::DriveDay* findDriveDay(const ClientArena& arena, const std::string& ymd) {
    for (const auto& day : arena.driveDays) {
        if (day.date == ymd) {
            return &day;
        }
    }
    return nullptr;
}

bool runDateSelected(const UiContext& ui, const std::string& ymd) {
    if (ui.arena.catalogPinned() && localYmdFromUnixMs(ui.arena.timeline().minTimeMs) == ymd) {
        return true;
    }
    const auto* day = findDriveDay(ui.arena, ymd);
    return day != nullptr && ui.arena.source == ctrl::SourceKind::LogReplay &&
           pathMentionsDay(ui.arena.sourcePath, *day);
}

std::string formatDuration(double seconds) {
    if (seconds >= 3600.0) {
        const int h = static_cast<int>(seconds / 3600.0);
        const int m = static_cast<int>((seconds - static_cast<double>(h) * 3600.0) / 60.0);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
        return buf;
    }
    if (seconds >= 60.0) {
        const int m = static_cast<int>(seconds / 60.0);
        const int s = static_cast<int>(seconds - static_cast<double>(m) * 60.0);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
        return buf;
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%.0fs", seconds);
    return buf;
}

void drawRunDates(UiContext& ui, char* pathBuf, std::size_t pathN, int* sourceIndex) {
    ImGui::Text("Runs");
    ImGui::Separator();
    const auto dates = runDates(ui);
    if (dates.empty()) {
        ImGui::TextColored(theme::mutedFg(), "No runs yet. Catalog and extracts share this list.");
        return;
    }
    ImGui::TextColored(theme::mutedFg(), "Same runs as Logs. Click a day to pin it.");
    for (const auto& ymd : dates) {
        const auto* day = findDriveDay(ui.arena, ymd);
        const bool selected = runDateSelected(ui, ymd);
        char label[96];
        if (day != nullptr) {
            std::snprintf(label, sizeof(label), "%s  %d stint%s  %s", day->date.c_str(), day->sessionCount,
                          day->sessionCount == 1 ? "" : "s", formatDuration(day->durationS).c_str());
        } else {
            std::snprintf(label, sizeof(label), "%s", ymd.c_str());
        }
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(theme::primary(), 0.22f));
        }
        if (ImGui::Button(label, ImVec2(-1.0f, 0.0f))) {
            if (day != nullptr && pathBuf != nullptr && pathN > 0) {
                std::snprintf(pathBuf, pathN, "%s", day->path.c_str());
            }
            if (day != nullptr && sourceIndex != nullptr) {
                *sourceIndex = indexFor(ctrl::SourceKind::LogReplay);
            }
            ui.client.playDate(ymd);
        }
        if (selected) {
            ImGui::PopStyleColor();
        }
    }
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
    const float left = 380.0f;

    ImGui::BeginChild("##src", ImVec2(left, avail.y),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
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
        ImGui::Spacing();
        drawRunDates(ui, path_, sizeof(path_), &sourceIndex_);
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

    if (ImGui::Button("select_source", ImVec2(-1.0f, 0.0f))) {
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

    ImGui::Spacing();
    drawRunDates(ui, path_, sizeof(path_), &sourceIndex_);

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
