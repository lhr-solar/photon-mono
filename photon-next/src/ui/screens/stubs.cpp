#include "ui/screen.hpp"
#include "ui/theme.hpp"

#include "imgui.h"

namespace photon::ui {

void drawStubScreen(const char* title, const char* body) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 text = ImGui::CalcTextSize(title);
    ImGui::SetCursorPosX((avail.x - text.x) * 0.5f);
    ImGui::SetCursorPosY(avail.y * 0.38f);
    ImGui::TextColored(theme::primary(), "%s", title);
    ImGui::SetCursorPosX(avail.x * 0.18f);
    ImGui::PushTextWrapPos(avail.x * 0.82f);
    ImGui::TextWrapped("%s", body);
    ImGui::PopTextWrapPos();
}

}  // namespace photon::ui
