#include "photon_ui/log_drawer.hpp"

#include "photon_ui/theme.hpp"

#include "imgui.h"

namespace photon::ui {
namespace {

constexpr float kTabW = 28.0f;

}  // namespace

void LogDrawer::draw(ImVec2 size) {
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
                      theme::u32(theme::withAlpha(theme::mutedFg(), 0.5f)), ch);
        y += sz.y + 2.0f;
    }
    theme::popSmallFont();

    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(kTabW, size.y));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Logs and replay: not in this build yet");
    }
}

float LogDrawer::occupiedWidth() const { return kTabW; }

}  // namespace photon::ui
