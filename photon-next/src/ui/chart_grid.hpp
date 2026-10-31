#pragma once

#include "imgui.h"
#include "ui/chart_grid_model.hpp"

#include <functional>
#include <string>
#include <vector>

namespace photon::ui {

struct GridPanel {
    std::string id, title;
    ViewRect* rect = nullptr;
    std::function<void(ImVec2)> render;
    bool fixed = false;
    bool chrome = true;
    bool compact = false;
};

class ChartGrid {
public:
    bool draw(const std::vector<GridPanel>& panels, ImVec2 size, bool locked);
    void beginGesture(const std::string& id, bool resize);
    bool undo(const std::vector<GridPanel>& panels);
    bool editing() const { return active_ >= 0; }
    void remember(const std::vector<GridPanel>& panels);
    void clearUndo() { undo_.clear(); }
    bool canUndo() const { return !undo_.empty(); }

private:
    std::string pending_;
    bool pendingResize_ = false;
    int active_ = -1;
    bool resize_ = false;
    ImVec2 mouse0_{}, scroll0_{};
    float unitW_ = 1.0f;
    ViewRect candidate_{};
    std::vector<GridEntry> snapshot_, undo_;
    void restore(const std::vector<GridPanel>& panels, const std::vector<GridEntry>& entries);
};

struct ChartCard {
    std::string id, title;
    ViewRect defaultRect;
    std::function<void(ImVec2)> render;
};

void drawChartBoard(const char* workspace, const std::vector<ChartCard>& cards, ImVec2 size = ImVec2(0, 0));

}  // namespace photon::ui
