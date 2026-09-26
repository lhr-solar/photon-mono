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
#include <unordered_set>
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

void formatHex(char* buf, std::size_t size, const CanRow& row) {
    const unsigned n = row.dlc > 8 ? 8u : row.dlc;
    if (n == 0) {
        std::snprintf(buf, size, "-");
        return;
    }
    int off = 0;
    for (unsigned b = 0; b < n; ++b) {
        off += std::snprintf(buf + off, size - static_cast<std::size_t>(off), b + 1u < n ? "%02X " : "%02X",
                             row.data[b]);
        if (off <= 0 || static_cast<std::size_t>(off) >= size) {
            break;
        }
    }
}

double messageAgeS(const ClientArena& arena, const CanRow* row) {
    if (row == nullptr || row->lastTsMs == 0) {
        return -1.0;
    }
    const auto cursor = arena.displayCursorMs();
    if (cursor == 0 || cursor < row->lastTsMs) {
        return 0.0;
    }
    return static_cast<double>(cursor - row->lastTsMs) / 1000.0;
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

void drawRawTable(const std::vector<const CanRow*>& rows) {
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    theme::pushTableStyle();
    if (!ImGui::BeginTable("can-raw", 5, flags)) {
        theme::popTableStyle();
        return;
    }
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupColumn("DLC", ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableSetupColumn("Data");
    ImGui::TableSetupColumn("Rate Hz", ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
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
            formatHex(hex, sizeof(hex), *row);
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

}  // namespace

void CanScreen::draw(UiContext& ui) {
    ui.client.markCanWanted();
    const auto messages = groupCatalog(ui.arena.signalCatalog);
    ImGui::Text("CAN");
    ImGui::SameLine();
    if (ui.arena.haveSignalCatalog && !messages.empty()) {
        ImGui::TextColored(theme::mutedFg(), "%zu messages  %zu signals  %zu on bus", messages.size(),
                           ui.arena.signalCatalog.size(), ui.arena.can().size());
    } else {
        ImGui::TextColored(theme::mutedFg(), "%zu ids on bus", ui.arena.can().size());
    }
    if (!ui.arena.dbcId.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(theme::mutedFg(), "  %s", ui.arena.dbcId.c_str());
    }

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##can_search", "Search name, id, or signal", search_, sizeof(search_));
    const std::string_view filter = search_;
    ImGui::Separator();

    if (!ui.arena.haveSignalCatalog) {
        ImGui::TextColored(theme::mutedFg(), "Waiting for DBC catalog. Raw bus IDs below.");
    } else if (messages.empty()) {
        ImGui::TextColored(theme::mutedFg(), "DBC has no messages. Raw bus IDs below.");
    }

    std::unordered_set<std::uint32_t> catalogIds;
    catalogIds.reserve(messages.size());
    for (const CatalogMessage& msg : messages) {
        catalogIds.insert(msg.canId);
    }
    std::vector<const CanRow*> unknown;
    for (const auto& [id, row] : ui.arena.can()) {
        if (!catalogIds.contains(id) && idMatches(id, filter)) {
            unknown.push_back(&row);
        }
    }
    std::sort(unknown.begin(), unknown.end(),
              [](const CanRow* a, const CanRow* b) { return a->canId < b->canId; });

    int visible = 0;
    if (!messages.empty()) {
        ImGui::BeginChild("##can_messages", ImVec2(0.0f, unknown.empty() ? 0.0f : -160.0f),
                          ImGuiChildFlags_None);
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
            const CanRow* bus = nullptr;
            if (const auto it = ui.arena.can().find(msg.canId); it != ui.arena.can().end()) {
                bus = &it->second;
            }
            float histHz = 0.0f;
            if (ui.arena.catalogPinned()) {
                for (const ctrl::CatalogSignal* sig : msg.signals) {
                    histHz = std::max(histHz, ui.arena.historyRateHz(sig->id));
                }
            }
            ImGui::PushID(static_cast<int>(msg.canId));
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_Framed;
            if (!filter.empty() && !containsI(msg.name, filter) && !idMatches(msg.canId, filter)) {
                flags |= ImGuiTreeNodeFlags_DefaultOpen;
            }
            const bool open = ImGui::TreeNodeEx(msg.name.c_str(), flags);
            ImGui::SameLine();
            char meta[96];
            if (ui.arena.catalogPinned() && histHz > 0.0f) {
                std::snprintf(meta, sizeof(meta), "0x%03X   %.1f Hz", msg.canId, static_cast<double>(histHz));
            } else if (bus != nullptr) {
                std::snprintf(meta, sizeof(meta), "0x%03X   %.1f Hz   %llu", msg.canId,
                              static_cast<double>(bus->rateHz), static_cast<unsigned long long>(bus->count));
            } else {
                std::snprintf(meta, sizeof(meta), "0x%03X   %zu signals", msg.canId, msg.signals.size());
            }
            ImGui::TextColored(theme::mutedFg(), "%s", meta);
            if (open) {
                std::vector<std::uint32_t> ids;
                ids.reserve(msg.signals.size());
                for (const ctrl::CatalogSignal* sig : msg.signals) {
                    ids.push_back(sig->id);
                }
                ui.client.ensureSubscribed(ids);
                if (ui.arena.catalogPinned() && !ids.empty()) {
                    ui.arena.tiles().requestCursorWindow(ui.client, ids, ui.arena.effectiveCursorMs());
                }

                if (bus != nullptr) {
                    char hex[32];
                    char age[32];
                    formatHex(hex, sizeof(hex), *bus);
                    formatAge(age, sizeof(age), messageAgeS(ui.arena, bus));
                    ImGui::TextColored(theme::mutedFg(), "DLC %u   %s   age %s",
                                       static_cast<unsigned>(bus->dlc), hex, age);
                } else if (ui.arena.catalogPinned()) {
                    ImGui::TextColored(theme::mutedFg(), "catalog history at cursor");
                } else {
                    ImGui::TextColored(theme::mutedFg(), "No frame on the bus yet");
                }

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

    if (!unknown.empty()) {
        ImGui::Separator();
        ImGui::Text("On bus, not in DBC");
        ImGui::SameLine();
        ImGui::TextColored(theme::mutedFg(), "%zu ids", unknown.size());
        drawRawTable(unknown);
    } else if (messages.empty()) {
        std::vector<const CanRow*> rows;
        rows.reserve(ui.arena.can().size());
        for (const auto& [id, row] : ui.arena.can()) {
            if (idMatches(id, filter)) {
                rows.push_back(&row);
            }
        }
        std::sort(rows.begin(), rows.end(),
                  [](const CanRow* a, const CanRow* b) { return a->canId < b->canId; });
        drawRawTable(rows);
    }
}

}  // namespace photon::ui
