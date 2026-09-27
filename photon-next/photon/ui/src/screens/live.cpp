#include "photon_ui/client.hpp"
#include "photon_ui/scene3d.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/signal_map.hpp"
#include "photon_ui/theme.hpp"
#include "photon_ui/video.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace photon::ui {
namespace {

constexpr float kSplitterW = 10.0f;

float barHeight() {
    return std::max(36.0f, ImGui::GetFrameHeight() + 10.0f);
}

bool paneToggle(const char* id, const char* label, bool on) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(theme::active(), 0.72f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::foreground());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(theme::muted(), 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::mutedFg());
    }
    const bool hit = ImGui::Button(label, ImVec2(0.0f, 28.0f));
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("%s pane", on ? "Hide" : "Show");
    (void)id;
    return hit;
}

void paneHeader(const char* title, bool* open) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float barH = std::max(28.0f, ImGui::GetFrameHeight());
    ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + barH),
                                              theme::u32(theme::titlebar()), 0.0f);
    ImGui::Dummy(ImVec2(w, barH));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 10.0f, p0.y + std::max(2.0f, (barH - ImGui::GetFontSize()) * 0.5f)));
    ImGui::TextColored(theme::primary(), "%s", title);
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + w - 28.0f, p0.y + std::max(2.0f, (barH - ImGui::GetFrameHeight()) * 0.5f)));
    ImGui::PushID(title);
    if (ImGui::SmallButton("x")) {
        *open = false;
    }
    ImGui::SetItemTooltip("Close pane");
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + barH));
}

}  // namespace

void LiveViewScreen::draw(UiContext& ui) {
    if (showVideo_) {
        ui.client.markVideoWanted();
    }
    if (ui.client.helloOk() && showVideo_ && !ui.arena.video.subscribed && !ui.arena.catalogPinned()) {
        ui.client.goLive();
    }
    if (showScene_) {
        ui.arena.signalMap().subscribe(ui.client, MapScreen::Scene3d);
    }

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = theme::kGridGap;
    const float barH = barHeight();

    theme::beginCard("livebar", ImVec2(avail.x, barH), theme::card(), theme::kStripPadding,
                     theme::CardFit::Band);
    ImGui::BeginChild("##livebarscroll", ImVec2(std::max(1.0f, avail.x - 8.0f), std::max(1.0f, barH - 8.0f)),
                      ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    if (paneToggle("live3d", "3D", showScene_)) {
        showScene_ = !showScene_;
    }
    ImGui::SameLine();
    if (paneToggle("livevid", "Video", showVideo_)) {
        showVideo_ = !showVideo_;
        if (showVideo_ && !ui.arena.video.subscribed && !ui.arena.catalogPinned()) {
            ui.client.goLive();
        }
    }
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(12.0f, 0.0f));
    ImGui::SameLine();
    if (ImGui::Button("Live", ImVec2(72.0f, 0.0f))) {
        showVideo_ = true;
        ui.client.goLive();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputScalar("##replay_ms", ImGuiDataType_U64, &replayMs_);
    ImGui::SameLine();
    if (ImGui::Button("Go", ImVec2(48.0f, 0.0f))) {
        showVideo_ = true;
        ui.arena.video.subscribed = true;
        videoDecoder().requestLocate(replayMs_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(64.0f, 0.0f))) {
        ui.client.stopVideo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    int nudge = static_cast<int>(ui.arena.video.clockOffsetMs);
    if (ImGui::SliderInt("A/V ms", &nudge, -5000, 5000)) {
        ui.arena.video.clockOffsetMs = nudge;
        ui.arena.video.clockOffsetDirty = true;
        ui.arena.video.clockOffsetUser = true;
        videoDecoder().setClockOffsetMs(nudge);
    }
    ImGui::SameLine();
    drawMaps3dKeyRow();
    ImGui::EndChild();
    theme::endCard();

    const auto& v = ui.arena.video;
    const float statusH = std::max(24.0f, ImGui::GetFontSize() + 10.0f);
    theme::beginCard("livestat", ImVec2(avail.x, statusH), theme::muted(), theme::kStripPadding,
                     theme::CardFit::Band);
    if (!videoDecoder().supported()) {
        ImGui::TextColored(theme::warning(), "Video decoder unavailable");
    } else if (videoDecoder().hasFrame()) {
        ImGui::TextColored(theme::success(), "video cache  %s  %0.1f fps  %zu kB", v.mode.c_str(), v.fps,
                           videoDecoder().bufferedBytes() / 1024);
    } else if (v.haveStatus) {
        ImGui::TextColored(v.connected ? theme::success() : theme::warning(),
                           "%s  %s  %0.1f fps  frames=%llu  drop=%llu", v.connected ? "video up" : "video down",
                           v.mode.c_str(), v.fps, static_cast<unsigned long long>(v.frames),
                           static_cast<unsigned long long>(v.dropped));
    } else if (videoDecoder().error()[0] != '\0') {
        ImGui::TextColored(theme::danger(), "%s", videoDecoder().error());
    } else if (v.subscribed && !v.lastError.empty()) {
        ImGui::TextColored(theme::warning(), "video store down  %s", v.lastError.c_str());
    } else if (v.subscribed) {
        ImGui::TextColored(theme::mutedFg(), "pointer cache: locating GOP");
    } else {
        ImGui::TextColored(theme::mutedFg(), "Video idle: press Live");
    }
    theme::endCard();

    const float bodyH = std::max(1.0f, avail.y - barH - statusH - gap * 2.0f);
    const ImVec2 bodyMin = ImGui::GetCursorScreenPos();
    const float bodyW = std::max(avail.x, 2.0f);
    // Side by side only when the window is wide enough for both panes.
    // A narrow or tall window stacks 3D over video so neither is a sliver.
    const bool wide = bodyW >= 980.0f && bodyW >= bodyH;
    const bool split = showScene_ && showVideo_ && wide && bodyW > kSplitterW * 3.0f;
    const bool stack = showScene_ && showVideo_ && !split;
    const float minPane = std::min(180.0f, (split ? bodyW : bodyH) * 0.22f);

    float sceneW = 0.0f;
    float sceneH = bodyH;
    float videoW = 0.0f;
    float videoH = bodyH;
    if (split) {
        sceneW = std::clamp(bodyW * sceneFractionSide_, minPane, bodyW - minPane - kSplitterW);
        videoW = bodyW - sceneW - kSplitterW;
        sceneFractionSide_ = sceneW / bodyW;
    } else if (stack) {
        sceneW = bodyW;
        videoW = bodyW;
        sceneH = std::clamp(bodyH * sceneFractionStack_, minPane, bodyH - minPane - kSplitterW);
        videoH = bodyH - sceneH - kSplitterW;
        sceneFractionStack_ = sceneH / bodyH;
    } else if (showScene_) {
        sceneW = bodyW;
    } else if (showVideo_) {
        videoW = bodyW;
    }

    if (!showScene_ && !showVideo_) {
        theme::beginCard("liveempty", ImVec2(bodyW, bodyH), theme::card(), theme::kTilePadding,
                         theme::CardFit::Band);
        ImGui::TextColored(theme::mutedFg(), "3D and video are closed. Toggle 3D or Video above.");
        theme::endCard();
        return;
    }

    if (showScene_) {
        ImGui::SetCursorScreenPos(bodyMin);
        theme::beginCard("livescene", ImVec2(sceneW, sceneH), theme::background(), ImVec2(0.0f, 0.0f),
                         theme::CardFit::Band);
        paneHeader("3D", &showScene_);
        const ImVec2 sceneAvail = ImGui::GetContentRegionAvail();
        drawScene3DView(ui.arena, sceneAvail, false, true);
        theme::endCard();
    } else {
        hideMaps3d();
    }

    if (split || stack) {
        const float along = split ? bodyW : bodyH;
        const ImVec2 splitPos = split ? ImVec2(bodyMin.x + sceneW, bodyMin.y)
                                      : ImVec2(bodyMin.x, bodyMin.y + sceneH);
        const ImVec2 splitSz = split ? ImVec2(kSplitterW, sceneH) : ImVec2(bodyW, kSplitterW);
        ImGui::SetCursorScreenPos(splitPos);
        ImGui::InvisibleButton("##LiveViewSplitter", splitSz);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        if (active && along > 1.0f) {
            const float delta = split ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
            float& fraction = split ? sceneFractionSide_ : sceneFractionStack_;
            fraction = std::clamp(fraction + delta / along, minPane / along, 1.0f - minPane / along - kSplitterW / along);
        }
        const ImVec2 s0 = ImGui::GetItemRectMin();
        const ImVec2 s1 = ImGui::GetItemRectMax();
        const ImVec4 col = active ? theme::accent() : hovered ? theme::foreground() : theme::border();
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(s0.x + 3.0f, s0.y + 6.0f), ImVec2(s1.x - 3.0f, s1.y - 6.0f),
            theme::u32(theme::withAlpha(col, active ? 0.9f : 0.55f)), 3.0f);
        if (hovered || active) {
            ImGui::SetMouseCursor(split ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
        }
    }

    if (showVideo_) {
        const float videoX = split ? bodyMin.x + sceneW + kSplitterW : bodyMin.x;
        const float videoY = stack ? bodyMin.y + sceneH + kSplitterW : bodyMin.y;
        ImGui::SetCursorScreenPos(ImVec2(videoX, videoY));
        theme::beginCard("livevideo", ImVec2(videoW, stack ? videoH : bodyH), theme::background(),
                         ImVec2(0.0f, 0.0f), theme::CardFit::Band);
        paneHeader("Video", &showVideo_);
        drawVideoImage(ui.arena, ImGui::GetContentRegionAvail());
        theme::endCard();
    }
}

}  // namespace photon::ui
