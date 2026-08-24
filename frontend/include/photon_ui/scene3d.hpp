#pragma once

#include "imgui.h"
#include "photon_ui/arena.hpp"

namespace photon::ui {

// Compact Pit dock tile — same card pattern as drawVideoTile.
void drawScene3DTile(ClientArena& arena, ImVec2 size);

// Full-region 3D view used by the 3D screen (overlay = camera / stats panel).
void drawScene3DView(ClientArena& arena, ImVec2 size, bool overlay);

}  // namespace photon::ui
