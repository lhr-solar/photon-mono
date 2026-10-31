#include "ui/client.hpp"
#include "ui/scene3d.hpp"
#include "ui/screen.hpp"
#include "ui/signal_map.hpp"
#include "ui/theme.hpp"
#include "ui/video.hpp"

#include "imgui.h"

#include <algorithm>

namespace photon::ui {
namespace {

constexpr float kSplitterW = 10.0f;

void paneHeader(const char* title) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float barH = std::max(28.0f, ImGui::GetFrameHeight());
    ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + barH),
                                              theme::u32(theme::titlebar()), 0.0f);
    ImGui::Dummy(ImVec2(w, barH));
    ImGui::SetCursorScreenPos(
        ImVec2(p0.x + 10.0f, p0.y + std::max(2.0f, (barH - ImGui::GetFontSize()) * 0.5f)));
    ImGui::TextColored(theme::primary(), "%s", title);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + barH));
}

}  // namespace

void LiveViewScreen::draw(UiContext& ui) {
    ui.client.markVideoWanted();
    ui.arena.signalMap().subscribe(ui.client, MapScreen::Scene3d);
    videoDecoder().syncToCursor(ui.arena);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float bodyH = std::max(1.0f, avail.y);
    const ImVec2 bodyMin = ImGui::GetCursorScreenPos();
    const float bodyW = std::max(avail.x, 2.0f);
    // Side by side only when the window is wide enough for both panes.
    // A narrow or tall window stacks 3D over video so neither is a sliver.
    const bool wide = bodyW >= 980.0f && bodyW >= bodyH;
    const bool split = wide && bodyW > kSplitterW * 3.0f;
    const bool stack = !split;
    const float minPane = std::min(180.0f, (split ? bodyW : bodyH) * 0.22f);

    float sceneW = 0.0f;
    float sceneH = bodyH;
    float videoW = 0.0f;
    float videoH = bodyH;
    if (split) {
        sceneW = std::clamp(bodyW * sceneFraction_, minPane, bodyW - minPane - kSplitterW);
        videoW = bodyW - sceneW - kSplitterW;
        sceneFraction_ = sceneW / bodyW;
    } else if (stack) {
        sceneW = bodyW;
        videoW = bodyW;
        sceneH = std::clamp(bodyH * sceneFraction_, minPane, bodyH - minPane - kSplitterW);
        videoH = bodyH - sceneH - kSplitterW;
        sceneFraction_ = sceneH / bodyH;
    }

    {
        ImGui::SetCursorScreenPos(bodyMin);
        theme::beginCard("livescene", ImVec2(sceneW, sceneH), theme::background(), ImVec2(0.0f, 0.0f),
                         theme::CardFit::Band);
        paneHeader("3D");
        const ImVec2 sceneAvail = ImGui::GetContentRegionAvail();
        drawScene3DView(ui.arena, sceneAvail, false, true);
        theme::endCard();
    }

    if (split || stack) {
        const float along = split ? bodyW : bodyH;
        const ImVec2 splitPos =
            split ? ImVec2(bodyMin.x + sceneW, bodyMin.y) : ImVec2(bodyMin.x, bodyMin.y + sceneH);
        const ImVec2 splitSz = split ? ImVec2(kSplitterW, sceneH) : ImVec2(bodyW, kSplitterW);
        ImGui::SetCursorScreenPos(splitPos);
        ImGui::InvisibleButton("##LiveViewSplitter", splitSz);
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        if (active && along > 1.0f) {
            const float delta = split ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
            sceneFraction_ = std::clamp(sceneFraction_ + delta / along, minPane / along,
                                        1.0f - minPane / along - kSplitterW / along);
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

    {
        const float videoX = split ? bodyMin.x + sceneW + kSplitterW : bodyMin.x;
        const float videoY = stack ? bodyMin.y + sceneH + kSplitterW : bodyMin.y;
        ImGui::SetCursorScreenPos(ImVec2(videoX, videoY));
        theme::beginCard("livevideo", ImVec2(videoW, stack ? videoH : bodyH), theme::background(),
                         ImVec2(0.0f, 0.0f), theme::CardFit::Band);
        paneHeader("Video");
        drawVideoImage(ui.arena, ImGui::GetContentRegionAvail());
        theme::endCard();
    }
}

}  // namespace photon::ui
