#include "photon_ui/log_drawer.hpp"

#include "photon_ui/clock_format.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace photon::ui {
namespace {

constexpr float kTabW = 28.0f;
constexpr float kPanelW = 340.0f;
constexpr std::size_t kLogLinesShown = 200;

// "HH:MM:SS" in local time, or "-" for no time.
std::string clockText(std::uint64_t ms) {
    if (ms == 0) {
        return "-";
    }
    char buf[48];
    formatLocalClock(ms, buf, sizeof(buf));
    return buf;
}

// "YYYY-MM-DD HH:MM:SS" when the run starts on another day than it ends; else the clock.
std::string rangeText(std::uint64_t startMs, std::uint64_t endMs) {
    if (startMs == 0) {
        return "no start";
    }
    if (endMs == 0 || endMs < startMs) {
        return localYmdFromUnixMs(startMs) + " " + clockText(startMs) + " - now";
    }
    if (localYmdFromUnixMs(startMs) != localYmdFromUnixMs(endMs)) {
        return localYmdFromUnixMs(startMs) + " " + clockText(startMs) + " - " + localYmdFromUnixMs(endMs) + " " +
               clockText(endMs);
    }
    return localYmdFromUnixMs(startMs) + " " + clockText(startMs) + " - " + clockText(endMs);
}

}  // namespace

void LogDrawer::draw(ImVec2 size, ClientArena& arena) {
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1 = ImVec2(p0.x + kTabW, p0.y + size.y);
    list->AddRectFilled(p0, p1, theme::u32(theme::titlebar()));
    list->AddLine(ImVec2(p1.x - 1.0f, p0.y), p1, theme::u32(theme::border()));

    theme::pushSmallFont();
    float y = p0.y + 16.0f;
    for (const char* c = "LOGS"; *c != '\0'; ++c) {
        const char ch[2] = {*c, 0};
        const ImVec2 sz = ImGui::CalcTextSize(ch);
        list->AddText(ImVec2(p0.x + (kTabW - sz.x) * 0.5f, y),
                      theme::u32(theme::withAlpha(theme::mutedFg(), open_ ? 0.9f : 0.5f)), ch);
        y += sz.y + 2.0f;
    }
    theme::popSmallFont();

    ImGui::SetCursorScreenPos(p0);
    if (ImGui::InvisibleButton("##logs-tab", ImVec2(kTabW, size.y))) {
        open_ = !open_;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(open_ ? "Hide runs and logs" : "Runs, replay and logs");
    }
    if (open_) {
        ImGui::SetCursorScreenPos(ImVec2(p0.x + kTabW, p0.y));
        drawPanel(ImVec2(kPanelW, size.y), arena);
    }
}

float LogDrawer::occupiedWidth() const {
    return open_ ? kTabW + kPanelW : kTabW;
}

void LogDrawer::drawPanel(ImVec2 size, ClientArena& arena) {
    ImGui::BeginChild("##logs-panel", size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);

    const bool pinned = arena.catalogPinned();
    theme::pushSmallFont();
    ImGui::TextColored(theme::mutedFg(), "RUNS");
    theme::popSmallFont();
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f);
    if (pinned) {
        if (ImGui::SmallButton("Live")) {
            arena.unpinRun();
        }
    } else {
        ImGui::TextColored(theme::accent(), "live");
    }
    ImGui::Separator();

    if (arena.runs.empty()) {
        ImGui::TextColored(theme::mutedFg(), "No runs yet.");
    } else {
        const float listH = pinned ? size.y * 0.40f : size.y * 0.52f;
        ImGui::BeginChild("##runs-list", ImVec2(0.0f, listH), ImGuiChildFlags_None);
        for (const auto& run : arena.runs) {
            ImGui::PushID(run.id.c_str());
            const bool selected = pinned && arena.pinnedRunId() == run.id;
            std::string label = run.label.empty() ? run.id : run.label;
            if (run.live) {
                label += "  [LIVE]";
            }
            if (ImGui::Selectable(label.c_str(), selected)) {
                if (run.live) {
                    arena.unpinRun();
                } else {
                    arena.pinRun(run);
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\nrev %lld, dbc %s", run.id.c_str(), static_cast<long long>(run.rev),
                                  run.dbcSha.c_str());
            }
            theme::pushSmallFont();
            const auto startMs = run.startMs > 0 ? static_cast<std::uint64_t>(run.startMs) : std::uint64_t{0};
            const auto endMs = run.endMs > 0 ? static_cast<std::uint64_t>(run.endMs) : std::uint64_t{0};
            ImGui::TextColored(theme::mutedFg(), "%s", rangeText(startMs, endMs).c_str());
            if (!run.source.empty()) {
                ImGui::TextColored(theme::mutedFg(), "%s", run.source.c_str());
            }
            theme::popSmallFont();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    if (pinned) {
        ImGui::Separator();
        const auto& tl = arena.timeline();
        const auto mode = static_cast<proto::TimelineMode>(tl.mode);
        const bool playing = mode == proto::TimelineMode::Play;
        if (ImGui::Button(playing ? "Pause" : "Play")) {
            arena.applyPinnedTimeline(playing ? proto::TimelineMode::Pause : proto::TimelineMode::Play, tl.cursorMs);
        }
        ImGui::SameLine();
        const double lo = static_cast<double>(tl.minTimeMs);
        const double hi = static_cast<double>(std::max(tl.maxTimeMs, tl.minTimeMs + 1));
        float frac = static_cast<float>((static_cast<double>(tl.cursorMs) - lo) / (hi - lo));
        frac = std::clamp(frac, 0.0f, 1.0f);
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::SliderFloat("##replay-scrub", &frac, 0.0f, 1.0f, "")) {
            const auto cursor = static_cast<std::uint64_t>(lo + static_cast<double>(frac) * (hi - lo));
            arena.applyPinnedTimeline(proto::TimelineMode::Pause, cursor);
        }
        theme::pushSmallFont();
        ImGui::TextColored(theme::mutedFg(), "%s / %s", clockText(tl.cursorMs).c_str(),
                           clockText(tl.maxTimeMs).c_str());
        theme::popSmallFont();
    }

    ImGui::Separator();
    theme::pushSmallFont();
    ImGui::TextColored(theme::mutedFg(), "LOG");
    theme::popSmallFont();
    ImGui::BeginChild("##log-lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    const auto& lines = arena.logs();
    const std::size_t first = lines.size() > kLogLinesShown ? lines.size() - kLogLinesShown : 0;
    theme::pushSmallFont();
    for (std::size_t i = first; i < lines.size(); ++i) {
        const LogLine& line = lines[i];
        const ImVec4 color = line.error ? theme::danger() : theme::mutedFg();
        ImGui::TextColored(color, "%s  %s", clockText(line.tMs).c_str(), line.text.c_str());
    }
    theme::popSmallFont();
    if (logAutoScroll_) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::EndChild();
}

}  // namespace photon::ui
