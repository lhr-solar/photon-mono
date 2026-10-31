#include "ui/widgets.hpp"

#include "ui/signal_map.hpp"
#include "ui/theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace photon::ui {

void drawSyntheticWatermark() {
    theme::pushSmallFont();
    ImGui::TextColored(theme::warning(), "syn");
    theme::popSmallFont();
}

void drawMinMaxBar(double value, double lo, double hi, ImVec4 fill) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 5.0f;
    if (w < 8.0f) {
        return;
    }
    double span = hi - lo;
    if (!(span > 0.0) || !std::isfinite(span)) {
        span = 1.0;
    }
    double frac = (value - lo) / span;
    frac = std::clamp(frac, 0.0, 1.0);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::u32(theme::muted()), 3.0f);
    const float fw = static_cast<float>(frac) * w;
    if (fw > 1.0f) {
        draw->AddRectFilled(p, ImVec2(p.x + fw, p.y + h), theme::u32(fill), 3.0f);
    }
    ImGui::Dummy(ImVec2(w, h));
}

void drawVitalTile(const char* id, const Vital& vital, ImVec2 size) {
    const ImVec4 bg = vital.alarm ? theme::withAlpha(theme::danger(), 0.22f) : theme::card();
    const ImVec2 tileSize = size.y > 0.0f ? size : ImVec2(size.x, theme::kVitalTileHeight);
    theme::beginCard(id, tileSize, bg, theme::kTilePadding);

    theme::pushSmallFont();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    ImGui::TextColored(theme::mutedFg(), "%s", vital.label);
    if (vital.unit != nullptr && vital.unit[0] != '\0') {
        ImGui::SameLine();
        ImGui::TextColored(theme::mutedFg(), "%s", vital.unit);
    }
    if (vital.synthetic) {
        ImGui::SameLine();
        ImGui::TextColored(theme::warning(), "syn");
    } else if (vital.derived) {
        ImGui::SameLine();
        ImGui::TextColored(theme::mutedFg(), "est.");
    }
    ImGui::PopStyleVar();
    theme::popSmallFont();

    char valueBuf[32];
    if (vital.have) {
        std::snprintf(valueBuf, sizeof(valueBuf), "%0.1f", vital.value);
    } else {
        std::snprintf(valueBuf, sizeof(valueBuf), "--");
    }

    theme::pushLargeFont();
    if (vital.have) {
        ImGui::TextUnformatted(valueBuf);
    } else {
        ImGui::TextColored(theme::mutedFg(), "%s", valueBuf);
    }
    theme::popLargeFont();

    if (vital.hasBar && vital.have) {
        const ImVec4 fill = vital.alarm ? theme::danger() : theme::primary();
        drawMinMaxBar(vital.value, vital.barMin, vital.barMax, fill);
    }
    theme::endCard();
}

}  // namespace photon::ui
