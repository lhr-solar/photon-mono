#include "ui/chart_grid.hpp"

#include "base/json.hpp"
#include "ui/theme.hpp"

#include <cstdlib>
#include <map>
#include <numeric>

extern "C" char* photon_ls_get(const char* key);
extern "C" void photon_ls_set(const char* key, const char* value);

namespace photon::ui {

void ChartGrid::restore(const std::vector<GridPanel>& panels, const std::vector<GridEntry>& entries) {
    for (const auto& p : panels)
        for (const auto& e : entries) {
            if (p.id == e.id && p.rect) *p.rect = e.rect;
        }
}

void ChartGrid::beginGesture(const std::string& id, bool resize) {
    if (active_ < 0) {
        pending_ = id;
        pendingResize_ = resize;
    }
}

void ChartGrid::remember(const std::vector<GridPanel>& panels) {
    undo_.clear();
    for (const auto& p : panels) undo_.push_back({p.id, *p.rect, p.fixed});
}

bool ChartGrid::undo(const std::vector<GridPanel>& panels) {
    if (undo_.empty()) return false;
    restore(panels, undo_);
    undo_.clear();
    return true;
}

bool ChartGrid::draw(const std::vector<GridPanel>& panels, ImVec2 size, bool locked) {
    ImGui::PushID(this);
    ImGui::BeginChild("##chart-grid", size, ImGuiChildFlags_None);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = 10.0f;
    const float unitW = std::max(1.0f, avail.x / kGridCols);
    const bool narrow =
        avail.x < 680.0f || std::any_of(
                                panels.begin(), panels.end(),
                                [&](const auto& p) { return !p.fixed && !p.compact && p.rect->w * unitW - gap < 320.0f; });
    const bool editable = !locked && !narrow;
    const ImVec2 scroll(ImGui::GetScrollX(), ImGui::GetScrollY());
    bool committed = false;
    if (!pending_.empty() && editable && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        snapshot_.clear();
        for (int i = 0; i < static_cast<int>(panels.size()); ++i) {
            snapshot_.push_back({panels[i].id, *panels[i].rect, panels[i].fixed,
                                 static_cast<int>(std::ceil((panels[i].compact ? 90.0f : 330.0f) / unitW))});
            if (panels[i].id == pending_ && !panels[i].fixed) active_ = i;
        }
        if (active_ >= 0) {
            resize_ = pendingResize_;
            mouse0_ = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
            scroll0_ = scroll;
            unitW_ = unitW;
            candidate_ = snapshot_[active_].rect;
        }
    }
    pending_.clear();
    if (active_ >= 0) {
        if (!editable || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            restore(panels, snapshot_);
            active_ = -1;
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            for (std::size_t i = 0; i < panels.size() && i < snapshot_.size(); ++i) {
                committed |= !sameGridRect(*panels[i].rect, snapshot_[i].rect);
            }
            if (committed) undo_ = snapshot_;
            active_ = -1;
        } else {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const float dx = mouse.x - mouse0_.x;
            const float dy = mouse.y - mouse0_.y + scroll.y - scroll0_.y;
            candidate_ = snapshot_[active_].rect;
            if (resize_) {
                candidate_.w += static_cast<int>(std::lround(dx / unitW_));
                candidate_.h += static_cast<int>(std::lround(dy / kGridRowH));
            } else {
                candidate_.x += static_cast<int>(std::lround(dx / unitW_));
                candidate_.y += static_cast<int>(std::lround(dy / kGridRowH));
            }
            candidate_ = boundedGridRect(candidate_, snapshot_[active_].minW, panels[active_].compact ? 1 : 13);
            const auto preview = reflowGrid(snapshot_, active_, candidate_);
            candidate_ = preview[active_].rect;
            restore(panels, preview);
            // Scroll while moving toward a board edge, without moving its origin.
            const float edge = 32.0f;
            if (mouse.y > origin.y + avail.y - edge)
                ImGui::SetScrollY(scroll.y + 8.0f);
            else if (mouse.y < origin.y + edge)
                ImGui::SetScrollY(std::max(0.0f, scroll.y - 8.0f));
        }
    }
    std::vector<int> order(panels.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return panels[a].rect->y < panels[b].rect->y; });
    float extent = 0.0f, stackedY = 0.0f;
    for (int index : order) {
        const auto& p = panels[index];
        const ViewRect& r = *p.rect;
        float x = r.x * unitW, y = r.y * kGridRowH;
        float w = std::max(40.0f, r.w * unitW - gap);
        float h = std::max(p.fixed || p.compact ? 48.0f : 288.0f, r.h * kGridRowH - gap);
        if (narrow) {
            x = 0.0f;
            y = stackedY;
            w = avail.x;
            stackedY += h + gap;
        }
        if (index == active_) {
            const auto& before = snapshot_[index].rect;
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const float dx = mouse.x - mouse0_.x;
            const float dy = mouse.y - mouse0_.y + scroll.y - scroll0_.y;
            if (resize_) {
                w = std::clamp(before.w * unitW_ - gap + dx, p.compact ? 80.0f : 320.0f, avail.x - before.x * unitW);
                h = std::max(p.compact ? 48.0f : 288.0f, before.h * kGridRowH - gap + dy);
            } else {
                x = std::clamp(before.x * unitW_ + dx, 0.0f, std::max(0.0f, avail.x - w));
                y = std::max(0.0f, before.y * kGridRowH + dy);
            }
        }
        extent = std::max(extent, std::max(y + h + gap, (r.y + r.h) * kGridRowH));
        const ImVec2 pos(origin.x + x, origin.y + y);
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushID(p.id.c_str());
        if (p.chrome) {
            ImGui::BeginChild("##panel", ImVec2(w, h), ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::TextUnformatted(p.title.c_str());
            if (editable && !p.fixed) {
                ImGui::SetCursorScreenPos(ImVec2(pos.x + 4, pos.y + 4));
                ImGui::InvisibleButton("##move", ImVec2(w - 8, 22));
                if (ImGui::IsItemActivated()) beginGesture(p.id, false);
                if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            }
            ImGui::SetCursorScreenPos(ImVec2(pos.x + 6, pos.y + 30));
            p.render(ImVec2(std::max(1.0f, w - 12), std::max(1.0f, h - 36)));
            if (editable && !p.fixed) {
                ImGui::SetCursorScreenPos(ImVec2(pos.x + w - 20, pos.y + h - 20));
                ImGui::InvisibleButton("##resize", ImVec2(18, 18));
                if (ImGui::IsItemActivated()) beginGesture(p.id, true);
                if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
                ImGui::GetWindowDrawList()->AddTriangleFilled(
                    ImVec2(pos.x + w - 5, pos.y + h - 17), ImVec2(pos.x + w - 5, pos.y + h - 5),
                    ImVec2(pos.x + w - 17, pos.y + h - 5), theme::u32(theme::mutedFg()));
            }
            ImGui::EndChild();
        } else
            p.render(ImVec2(w, h));
        ImGui::PopID();
    }
    if (active_ >= 0) {
        const ImVec2 a(origin.x + candidate_.x * unitW, origin.y + candidate_.y * kGridRowH);
        const ImVec2 b(a.x + candidate_.w * unitW - gap, a.y + candidate_.h * kGridRowH - gap);
        ImGui::GetWindowDrawList()->AddRect(a, b, theme::u32(theme::primary()), 3.0f, 0, 2.0f);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(avail.x, extent));
    ImGui::EndChild();
    ImGui::PopID();
    return committed;
}

namespace {
struct Board {
    bool loaded = false, locked = true;
    std::map<std::string, ViewRect> rects;
    ChartGrid grid;
};
std::map<std::string, Board> boards;
void saveBoard(const std::string& key, const Board& board) {
    json::Object rects;
    for (const auto& [id, r] : board.rects) {
        json::Object rect{{"x", json::Value(r.x)},
                          {"y", json::Value(r.y)},
                          {"w", json::Value(r.w)},
                          {"h", json::Value(r.h)}};
        rects.emplace(id, json::Value(std::move(rect)));
    }
    const auto text = json::stringify(json::Value(json::Object{{"schemaVersion", json::Value(1)},
                                                               {"locked", json::Value(board.locked)},
                                                               {"panels", json::Value(std::move(rects))}}));
    photon_ls_set(key.c_str(), text.c_str());
}
}  // namespace

void drawChartBoard(const char* workspace, const std::vector<ChartCard>& cards, ImVec2 size) {
    const std::string key = std::string("photon-next.chart-layout.") + workspace;
    Board& board = boards[key];
    if (!board.loaded) {
        board.loaded = true;
        char* raw = photon_ls_get(key.c_str());
        if (raw) {
            auto doc = json::parse(raw);
            if (doc && (*doc)["schemaVersion"].asInt() == 1) {
                board.locked = (*doc)["locked"].asBool(true);
                for (const auto& [id, value] : (*doc)["panels"].asObject()) {
                    board.rects[id] = boundedGridRect(
                        {static_cast<int>(value["x"].asInt()), static_cast<int>(value["y"].asInt()),
                         static_cast<int>(value["w"].asInt(12)), static_cast<int>(value["h"].asInt(14))},
                        1, 13);
                }
            }
            std::free(raw);
        }
    }
    std::vector<GridPanel> panels;
    for (const auto& c : cards) {
        auto [at, unused] = board.rects.try_emplace(c.id, boundedGridRect(c.defaultRect, 1, 13));
        (void)unused;
        panels.push_back({c.id, c.title, &at->second, c.render});
    }
    if (!board.grid.editing() && !ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_Z, false) && board.grid.undo(panels))
        saveBoard(key, board);
    ImGui::PushID(workspace);
    ImGui::BeginDisabled(board.grid.editing());
    if (ImGui::SmallButton(board.locked ? "Unlock layout" : "Lock layout")) {
        board.locked = !board.locked;
        saveBoard(key, board);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Undo") && board.grid.undo(panels)) saveBoard(key, board);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset layout")) {
        board.grid.remember(panels);
        for (const auto& c : cards) board.rects[c.id] = boundedGridRect(c.defaultRect, 1, 13);
        saveBoard(key, board);
    }
    ImGui::EndDisabled();
    if (!board.locked) {
        if (ImGui::GetContentRegionAvail().x >= 720) ImGui::SameLine();
        ImGui::TextDisabled("Drag headers / resize corners. Autosaved on release.");
    }
    if (board.grid.draw(panels, size, board.locked)) saveBoard(key, board);
    ImGui::PopID();
}

}  // namespace photon::ui
