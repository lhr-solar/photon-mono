#include "photon_ui/client.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace photon::ui {

void CanScreen::draw(UiContext& ui) {
    ImGui::Text("CAN monitor");
    ImGui::SameLine();
    ImGui::TextColored(theme::mutedFg(), "%zu ids", ui.arena.can().size());
    ImGui::Separator();

    std::vector<const CanRow*> rows;
    rows.reserve(ui.arena.can().size());
    for (const auto& [id, row] : ui.arena.can()) {
        (void)id;
        rows.push_back(&row);
    }

    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_SizingStretchProp;
    theme::pushTableStyle();
    if (!ImGui::BeginTable("can", 5, flags)) {
        theme::popTableStyle();
        return;
    }
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn("DLC", ImGuiTableColumnFlags_WidthFixed, 56.0f);
    ImGui::TableSetupColumn("Data");
    ImGui::TableSetupColumn("Rate Hz", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 80.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
            std::sort(rows.begin(), rows.end(), [&](const CanRow* a, const CanRow* b) {
                int cmp = 0;
                switch (spec.ColumnIndex) {
                    case 0:
                        cmp = (a->canId > b->canId) - (a->canId < b->canId);
                        break;
                    case 1:
                        cmp = (a->dlc > b->dlc) - (a->dlc < b->dlc);
                        break;
                    case 3:
                        cmp = (a->rateHz > b->rateHz) - (a->rateHz < b->rateHz);
                        break;
                    case 4:
                        cmp = (a->count > b->count) - (a->count < b->count);
                        break;
                    default:
                        cmp = (a->canId > b->canId) - (a->canId < b->canId);
                        break;
                }
                return spec.SortDirection == ImGuiSortDirection_Ascending ? cmp < 0 : cmp > 0;
            });
        }
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const CanRow* row = rows[static_cast<std::size_t>(i)];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("0x%03X", row->canId);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", static_cast<unsigned>(row->dlc));
            ImGui::TableSetColumnIndex(2);
            char hex[32];
            const unsigned n = row->dlc > 8 ? 8u : row->dlc;
            int off = 0;
            for (unsigned b = 0; b < n; ++b) {
                off += std::snprintf(hex + off, sizeof(hex) - static_cast<std::size_t>(off),
                                     b + 1u < n ? "%02X " : "%02X", row->data[b]);
            }
            if (n == 0) {
                hex[0] = '-';
                hex[1] = '\0';
            }
            ImGui::TextUnformatted(hex);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%0.1f", static_cast<double>(row->rateHz));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%llu", static_cast<unsigned long long>(row->count));
        }
    }
    ImGui::EndTable();
    theme::popTableStyle();
}

}  // namespace photon::ui
