#pragma once

#include "imgui.h"

namespace photon::ui {

struct Vital;

void drawSyntheticWatermark();
void drawVitalTile(const char* id, const Vital& vital, ImVec2 size);
void drawMinMaxBar(double value, double lo, double hi, ImVec4 fill);

}  // namespace photon::ui
