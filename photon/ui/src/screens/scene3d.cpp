#include "photon_ui/client.hpp"
#include "photon_ui/scene3d.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/signal_map.hpp"

#include "imgui.h"

namespace photon::ui {

void Scene3dScreen::draw(UiContext& ui) {
    ui.arena.signalMap().subscribe(ui.client, MapScreen::Scene3d);
    drawScene3DView(ui.arena, ImGui::GetContentRegionAvail(), true, true);
}

}  // namespace photon::ui
