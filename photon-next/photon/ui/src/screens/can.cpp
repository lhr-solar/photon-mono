#include "photon_ui/client.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace photon::ui {
namespace {

bool containsI(std::string_view hay, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > hay.size()) {
        return false;
    }
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            const unsigned char a = static_cast<unsigned char>(hay[i + j]);
            const unsigned char b = static_cast<unsigned char>(needle[j]);
            if (std::tolower(a) != std::tolower(b)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            return true;
        }
    }
    return false;
}

bool idMatches(std::uint32_t canId, std::string_view filter) {
    if (filter.empty()) {
        return true;
    }
    char hex[16];
    char hexBare[16];
    char dec[16];
    std::snprintf(hex, sizeof(hex), "0x%X", canId);
    std::snprintf(hexBare, sizeof(hexBare), "%X", canId);
    std::snprintf(dec, sizeof(dec), "%u", canId);
    return containsI(hex, filter) || containsI(hexBare, filter) || containsI(dec, filter);
}

void formatAge(char* buf, std::size_t size, double ageS) {
    if (!(ageS >= 0.0) || !std::isfinite(ageS)) {
        std::snprintf(buf, size, "no data");
        return;
    }
    if (ageS < 1.0) {
        std::snprintf(buf, size, "%.0f ms", ageS * 1000.0);
        return;
    }
    std::snprintf(buf, size, "%.1f s", ageS);
}

void formatSignalValue(char* buf, std::size_t size, double value, std::string_view unit) {
    const double magnitude = std::fabs(value);
    if (!std::isfinite(value)) {
        std::snprintf(buf, size, "%g", value);
        return;
    }
    if (!unit.empty() && unit != "NULL") {
        if ((magnitude > 0.0 && magnitude < 0.001) || magnitude >= 100000.0) {
            std::snprintf(buf, size, "%.4g %s", value, std::string(unit).c_str());
        } else {
            std::snprintf(buf, size, "%.3f %s", value, std::string(unit).c_str());
        }
        return;
    }
    if ((magnitude > 0.0 && magnitude < 0.001) || magnitude >= 100000.0) {
        std::snprintf(buf, size, "%.4g", value);
    } else {
        std::snprintf(buf, size, "%.3f", value);
    }
}

struct CatalogMessage {
    std::uint32_t canId = 0;
    std::string name;
    std::vector<const ctrl::CatalogSignal*> signals;
};

std::vector<CatalogMessage> groupCatalog(const std::vector<ctrl::CatalogSignal>& catalog) {
    std::map<std::uint32_t, CatalogMessage> byId;
    for (const ctrl::CatalogSignal& sig : catalog) {
        CatalogMessage& msg = byId[sig.messageId];
        msg.canId = sig.messageId;
        if (msg.name.empty()) {
            msg.name = sig.message.empty() ? ("0x" + std::to_string(sig.messageId)) : sig.message;
        }
        msg.signals.push_back(&sig);
    }
    std::vector<CatalogMessage> out;
    out.reserve(byId.size());
    for (auto& [id, msg] : byId) {
        (void)id;
        out.push_back(std::move(msg));
    }
    return out;
}

}  // namespace

void CanScreen::draw(UiContext& ui) {
    const auto messages = groupCatalog(ui.arena.signalCatalog);
    ImGui::Text("CAN");
    ImGui::SameLine();
    ImGui::TextColored(theme::mutedFg(), "%zu messages  %zu signals", messages.size(),
                       ui.arena.signalCatalog.size());
    if (!ui.arena.catalogSha.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(theme::mutedFg(), "  %s", ui.arena.catalogSha.substr(0, 12).c_str());
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##can_search", "Search name, id, or signal", search_, sizeof(search_));
    const std::string_view filter = search_;
    ImGui::Separator();

    if (!ui.arena.haveSignalCatalog) {
        ImGui::TextColored(theme::mutedFg(), "Waiting for the signal catalog from the server.");
    } else if (messages.empty()) {
        ImGui::TextColored(theme::mutedFg(), "The catalog has no messages.");
    }

    int visible = 0;
    if (!messages.empty()) {
        ImGui::BeginChild("##can_messages", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        for (const CatalogMessage& msg : messages) {
            bool show = containsI(msg.name, filter) || idMatches(msg.canId, filter);
            if (!show) {
                for (const ctrl::CatalogSignal* sig : msg.signals) {
                    if (containsI(sig->name, filter)) {
                        show = true;
                        break;
                    }
                }
            }
            if (!show) {
                continue;
            }
            ++visible;
            ImGui::PushID(static_cast<int>(msg.canId));
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed;
            if (!filter.empty() && !containsI(msg.name, filter) && !idMatches(msg.canId, filter)) {
                flags |= ImGuiTreeNodeFlags_DefaultOpen;
            }
            const bool open = ImGui::TreeNodeEx(msg.name.c_str(), flags);
            ImGui::SameLine();
            char meta[96];
            std::snprintf(meta, sizeof(meta), "0x%03X   %zu signals", msg.canId, msg.signals.size());
            ImGui::TextColored(theme::mutedFg(), "%s", meta);
            if (open) {
                std::vector<std::uint32_t> ids;
                ids.reserve(msg.signals.size());
                for (const ctrl::CatalogSignal* sig : msg.signals) {
                    ids.push_back(sig->id);
                }
                ui.client.ensureSubscribed(ids);

                theme::pushTableStyle();
                constexpr ImGuiTableFlags tableFlags =
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
                if (ImGui::BeginTable("signals", 4, tableFlags)) {
                    ImGui::TableSetupColumn("Signal", ImGuiTableColumnFlags_WidthStretch, 1.6f);
                    ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthFixed, 72.0f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.1f);
                    ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 72.0f);
                    ImGui::TableHeadersRow();
                    for (const ctrl::CatalogSignal* sig : msg.signals) {
                        if (!filter.empty() && !containsI(sig->name, filter) && !containsI(msg.name, filter) &&
                            !idMatches(msg.canId, filter)) {
                            continue;
                        }
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(sig->name.c_str());
                        ImGui::TableSetColumnIndex(1);
                        if (sig->unit.empty() || sig->unit == "NULL") {
                            ImGui::TextColored(theme::mutedFg(), "-");
                        } else {
                            ImGui::TextUnformatted(sig->unit.c_str());
                        }
                        ImGui::TableSetColumnIndex(2);
                        char value[64];
                        if (ui.arena.has(sig->id)) {
                            formatSignalValue(value, sizeof(value), ui.arena.readout(sig->id), "");
                            ImGui::TextUnformatted(value);
                        } else {
                            ImGui::TextColored(theme::mutedFg(), "no data");
                        }
                        ImGui::TableSetColumnIndex(3);
                        const SignalRing* ring = ui.arena.ring(sig->id);
                        if (ring != nullptr && ring->hasLatest && ring->latestTs > 0.0) {
                            const auto cursor = static_cast<double>(ui.arena.displayCursorMs());
                            const double ageS =
                                cursor > 0.0 ? std::max(0.0, (cursor - ring->latestTs) / 1000.0) : 0.0;
                            formatAge(value, sizeof(value), ageS);
                            ImGui::TextColored(theme::mutedFg(), "%s", value);
                        } else {
                            ImGui::TextColored(theme::mutedFg(), "no data");
                        }
                    }
                    ImGui::EndTable();
                }
                theme::popTableStyle();
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (visible == 0) {
            ImGui::TextColored(theme::mutedFg(), "No messages match");
        }
        ImGui::EndChild();
    }
}

}  // namespace photon::ui
