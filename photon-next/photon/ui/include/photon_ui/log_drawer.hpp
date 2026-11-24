#pragma once

#include "imgui.h"
#include "photon_ui/arena.hpp"

namespace photon::ui {

// Left-edge LOGS tab. It opens a panel with the runs from runs_list: a click pins
// a past run for replay (with a playhead, play and pause), and Live goes back to
// the live run. The log lines are listed under the runs.
class LogDrawer {
public:
    void draw(ImVec2 size, ClientArena& arena);
    float occupiedWidth() const;

private:
    bool open_ = false;
    bool logAutoScroll_ = true;

    void drawPanel(ImVec2 size, ClientArena& arena);
};

}  // namespace photon::ui
