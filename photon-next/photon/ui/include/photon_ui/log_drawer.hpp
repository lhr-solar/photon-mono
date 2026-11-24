#pragma once

#include "imgui.h"

namespace photon::ui {

// Left-edge LOGS tab. Logs and replay come back in the next step, with runs and
// tiles, so the tab stays greyed out until then.
class LogDrawer {
public:
    void draw(ImVec2 size);
    float occupiedWidth() const;
};

}  // namespace photon::ui
