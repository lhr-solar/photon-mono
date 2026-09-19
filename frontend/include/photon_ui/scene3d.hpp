#pragma once

#include "imgui.h"
#include "photon_ui/arena.hpp"

namespace photon::ui {

// Desktop GPU is on by default. PHOTON_GL3D=0 disables it. WASM always on.
bool scene3dGpuEnabled();

// Compact Pit dock tile — same card pattern as drawVideoTile.
void drawScene3DTile(ClientArena& arena, ImVec2 size);

// Full-region 3D view used by the 3D screen (overlay = camera / stats panel).
void drawScene3DView(ClientArena& arena, ImVec2 size, bool overlay, bool hostMaps = false);

// Hide the OSM / MapLibre 3D HTML overlay when leaving the 3D tab.
void hideMaps3d();

// ImGui popup IDs are hashed with the current window, so name checks miss.
// A hold-off flag is what actually keeps the DOM / WebView2 overlay down.
void setMapsOverlayHoldOff(bool on);
bool mapsOverlayHoldOff();

inline bool mapsOverlayBlockedByPopup() {
    return mapsOverlayHoldOff();
}

// Drop map pointer events so ImGui popups (drive-day combo) stay clickable.
void setMaps3dInteractive(bool on);

// Compact OSM 3D status for Live View.
void drawMaps3dKeyRow();

}  // namespace photon::ui
