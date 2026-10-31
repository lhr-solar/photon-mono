#include "ui/video.hpp"

#include "ui/arena.hpp"
#include "ui/signal_map.hpp"
#include "ui/theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace photon::ui {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kA0 = kPi * 0.75f;
constexpr float kSweep = kPi * 1.50f;

constexpr ImU32 kGold = IM_COL32(240, 190, 55, 255);
constexpr ImU32 kGreen = IM_COL32(90, 210, 110, 255);
constexpr ImU32 kRed = IM_COL32(220, 50, 70, 255);
constexpr ImU32 kAmber = IM_COL32(230, 160, 70, 255);
constexpr ImU32 kWhite = IM_COL32(230, 230, 224, 230);

ImVec2 polar(ImVec2 c, float r, float a) {
    return ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
}

ImU32 rgba(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

float wrap01(float t) {
    if (t < 0.0f) {
        return 0.0f;
    }
    if (t > 1.0f) {
        return 1.0f;
    }
    return t;
}

float smoothToward(float& slot, float target, float dt) {
    const float k = 1.0f - std::exp(-14.0f * std::max(dt, 0.0f));
    slot += (target - slot) * k;
    return slot;
}

void strokeArc(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 col, float thick, int seg) {
    if (r < 2.0f || thick < 0.4f) {
        return;
    }
    dl->PathArcTo(c, r, a0, a1, std::max(8, seg));
    dl->PathStroke(col, ImDrawFlags_None, thick);
}

void glowArc(ImDrawList* dl, ImVec2 c, float r, float a0, float a1, ImU32 col, float thick) {
    const int seg = std::max(18, static_cast<int>(std::fabs(a1 - a0) * 28.0f));
    const ImVec4 f = ImGui::ColorConvertU32ToFloat4(col);
    strokeArc(dl, c, r, a0, a1, ImGui::ColorConvertFloat4ToU32(ImVec4(f.x, f.y, f.z, f.w * 0.12f)), thick * 3.4f,
              seg);
    strokeArc(dl, c, r, a0, a1, ImGui::ColorConvertFloat4ToU32(ImVec4(f.x, f.y, f.z, f.w * 0.28f)), thick * 1.8f,
              seg);
    strokeArc(dl, c, r, a0, a1, col, thick, seg);
}

bool onBit(const Vital& v) {
    return v.have && v.value > 0.5;
}

void strokeUnder(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float w) {
    dl->AddLine(a, b, rgba(8, 8, 10, 230), w * 1.6f);
    dl->AddLine(a, b, col, w);
}

void drawNeedle(ImDrawList* dl, ImVec2 c, float r, float a, ImU32 col) {
    const float inner = r * 0.40f;
    const ImVec2 tip = polar(c, r * 0.88f, a);
    const ImVec2 base = polar(c, inner, a);
    const float nx = std::cos(a + kPi * 0.5f);
    const float ny = std::sin(a + kPi * 0.5f);
    const float w = std::max(2.0f, r * 0.032f);
    dl->AddTriangleFilled(tip, ImVec2(base.x + nx * w, base.y + ny * w), ImVec2(base.x - nx * w, base.y - ny * w),
                          col);
    dl->AddLine(base, tip, col, std::max(1.6f, r * 0.022f));
}

void analogGauge(ImDrawList* dl, ImVec2 c, float r, float value, float vmax, int major, const char* unit) {
    if (r < 18.0f) {
        return;
    }
    const float t = wrap01(vmax > 1.0e-3f ? value / vmax : 0.0f);
    const float aVal = kA0 + t * kSweep;
    const float aHot = kA0 + 0.88f * kSweep;
    const float aEnd = kA0 + kSweep;

    dl->AddCircleFilled(c, r, rgba(8, 8, 10, 150));
    dl->AddCircle(c, r, kGold, 64, std::max(1.2f, r * 0.018f));
    strokeArc(dl, c, r * 0.90f, kA0, aEnd, rgba(70, 70, 74, 160), std::max(3.2f, r * 0.08f), 48);
    glowArc(dl, c, r * 0.90f, kA0, aVal, kGold, std::max(3.2f, r * 0.08f));
    if (t > 0.88f) {
        glowArc(dl, c, r * 0.90f, aHot, aVal, rgba(230, 70, 120, 255), std::max(3.2f, r * 0.08f));
    }

    ImFont* font = theme::uiFont();
    const float tickFs = std::max(8.0f, r * 0.095f);
    for (int i = 0; i <= major; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(major);
        const float a = kA0 + u * kSweep;
        const bool big = (i % 2) == 0;
        dl->AddLine(polar(c, big ? r * 0.82f : r * 0.86f, a), polar(c, r * 0.90f, a),
                    big ? rgba(235, 235, 230, 220) : rgba(180, 180, 176, 140), big ? 2.0f : 1.0f);
        if (!big || font == nullptr) {
            continue;
        }
        char lab[12];
        std::snprintf(lab, sizeof(lab), "%d", static_cast<int>(std::lround(u * vmax)));
        const ImVec2 sz = font->CalcTextSizeA(tickFs, 1e9f, 0.0f, lab);
        const ImVec2 p = polar(c, r * 0.72f, a);
        dl->AddText(font, tickFs, ImVec2(p.x - sz.x * 0.5f, p.y - sz.y * 0.5f), rgba(230, 230, 224, 230), lab);
    }

    drawNeedle(dl, c, r * 0.90f, aVal, kGold);
    if (font == nullptr) {
        return;
    }
    char num[16];
    std::snprintf(num, sizeof(num), "%d", static_cast<int>(std::lround(value)));
    const float numFs = std::max(28.0f, r * 0.46f);
    const ImVec2 nsz = font->CalcTextSizeA(numFs, 1e9f, 0.0f, num);
    const ImVec2 numPos(c.x - nsz.x * 0.5f, c.y - nsz.y * 0.62f);
    dl->AddText(font, numFs, ImVec2(numPos.x + 1.2f, numPos.y + 1.2f), rgba(8, 8, 10, 220), num);
    dl->AddText(font, numFs, numPos, rgba(255, 255, 255, 255), num);
    if (unit == nullptr || unit[0] == '\0') {
        unit = "mph";
    }
    const float uFs = std::max(11.0f, r * 0.13f);
    const ImVec2 usz = font->CalcTextSizeA(uFs, 1e9f, 0.0f, unit);
    dl->AddText(font, uFs, ImVec2(c.x - usz.x * 0.5f, numPos.y + nsz.y + 1.0f), kGold, unit);
}

void drawProw(ImDrawList* dl, ImVec2 p, float r, int gear, float w) {
    const ImU32 col = gear == 0 ? kGold : (gear == 2 ? kAmber : kWhite);
    ImFont* font = theme::uiFont();
    if (font == nullptr) {
        return;
    }
    const char* letter = gear == 0 ? "F" : (gear == 2 ? "R" : "N");
    const float fs = std::clamp(r * 0.20f, 13.0f, 18.0f);
    const ImVec2 sz = font->CalcTextSizeA(fs, 1e9f, 0.0f, letter);
    dl->AddText(font, fs, ImVec2(p.x - sz.x * 0.5f + 1.0f, p.y - sz.y * 0.5f + 1.0f), rgba(8, 8, 10, 230), letter);
    dl->AddText(font, fs, ImVec2(p.x - sz.x * 0.5f, p.y - sz.y * 0.5f), col, letter);

    const float keelW = std::max(2.2f, w);
    const float half = 0.09f * r;
    const float tick = std::max(4.0f, 0.055f * r);
    const float keelY = p.y + sz.y * 0.55f + 2.0f;
    strokeUnder(dl, ImVec2(p.x - half, keelY), ImVec2(p.x + half, keelY), col, keelW);
    if (gear == 0) {
        strokeUnder(dl, ImVec2(p.x - half * 0.45f, keelY), ImVec2(p.x - half * 0.45f, keelY - tick), col, keelW);
        strokeUnder(dl, ImVec2(p.x + half * 0.45f, keelY), ImVec2(p.x + half * 0.45f, keelY - tick), col, keelW);
    } else if (gear == 2) {
        strokeUnder(dl, ImVec2(p.x, keelY), ImVec2(p.x, keelY + tick), col, keelW);
    }
}

void drawLash(ImDrawList* dl, ImVec2 c, float r, float a, float w) {
    const float thick = std::max(2.0f, w);
    const ImVec2 u(std::cos(a), std::sin(a));
    const ImVec2 L = polar(c, r * 1.24f, a);
    const ImVec2 H(L.x - 0.08f * r * u.x, L.y - 0.08f * r * u.y);
    const float a0 = a - 0.55f;
    const float a1 = a + 0.55f;
    strokeArc(dl, H, 0.15f * r, a0, a1, rgba(8, 8, 10, 230), thick * 1.6f, 12);
    strokeArc(dl, H, 0.15f * r, a0, a1, kGold, thick, 12);
    strokeArc(dl, H, 0.08f * r, a - 0.45f, a + 0.45f, kGold, thick, 10);
    strokeUnder(dl, polar(H, 0.08f * r, a0), polar(H, 0.15f * r, a0), kGold, thick);
    strokeUnder(dl, polar(H, 0.08f * r, a1), polar(H, 0.15f * r, a1), kGold, thick);
    const float barb = std::max(6.0f, 0.10f * r);
    strokeUnder(dl, polar(H, 0.15f * r, a), ImVec2(H.x + u.x * (0.15f * r + barb), H.y + u.y * (0.15f * r + barb)),
                kGold, thick);
}

void drawShearGate(ImDrawList* dl, ImVec2 b, float r, float w, float pedalT) {
    const float thick = std::max(2.2f, w);
    const float slashW = thick * (1.0f + 0.8f * wrap01(pedalT));
    strokeUnder(dl, ImVec2(b.x - 0.055f * r, b.y - 0.12f * r), ImVec2(b.x - 0.055f * r, b.y + 0.12f * r), kRed, thick);
    strokeUnder(dl, ImVec2(b.x + 0.055f * r, b.y - 0.12f * r), ImVec2(b.x + 0.055f * r, b.y + 0.12f * r), kRed, thick);
    strokeUnder(dl, ImVec2(b.x - 0.10f * r, b.y - 0.10f * r), ImVec2(b.x + 0.10f * r, b.y + 0.10f * r), kRed,
                slashW * 1.25f);
    const float ember = std::max(3.0f, 0.036f * r);
    dl->AddCircleFilled(b, ember + 1.4f, rgba(8, 8, 10, 230), 12);
    dl->AddCircleFilled(b, ember, kRed, 12);
}

void drawInfold(ImDrawList* dl, ImVec2 g, float r, float w, bool active) {
    const float thick = std::max(2.2f, w);
    const float span = kPi * (220.0f / 180.0f);
    const float a0 = kPi * 0.15f;
    const float rads[3] = {std::max(8.0f, 0.12f * r), std::max(5.5f, 0.08f * r), std::max(4.0f, 0.04f * r)};
    for (float rad : rads) {
        strokeArc(dl, g, rad, a0, a0 + span, rgba(8, 8, 10, 230), thick * 1.6f, 16);
        strokeArc(dl, g, rad, a0, a0 + span, kGreen, thick, 16);
    }
    const float core = std::max(3.0f, 0.036f * r);
    if (active) {
        dl->AddCircleFilled(g, core, kGreen, 14);
    } else {
        dl->AddCircle(g, core, kGreen, 14, thick);
    }
}

void drawLockHorizon(ImDrawList* dl, ImVec2 c, float r, float w) {
    const float thick = std::max(2.0f, w);
    const float half = 0.14f * r;
    const ImVec2 inner = polar(c, r * 0.96f, kPi * 1.50f);
    const ImVec2 outer = polar(c, r * 1.10f, kPi * 1.50f);
    strokeUnder(dl, ImVec2(inner.x - half, inner.y), ImVec2(inner.x + half, inner.y), kGold, thick);
    strokeUnder(dl, ImVec2(outer.x - half, outer.y), ImVec2(outer.x + half, outer.y), kGold, thick);
    const float d = std::max(5.0f, 0.06f * r);
    const ImVec2 k(outer.x, outer.y);
    strokeUnder(dl, ImVec2(k.x, k.y - d), ImVec2(k.x + d, k.y), kGold, thick);
    strokeUnder(dl, ImVec2(k.x + d, k.y), ImVec2(k.x, k.y + d), kGold, thick);
    strokeUnder(dl, ImVec2(k.x, k.y + d), ImVec2(k.x - d, k.y), kGold, thick);
    strokeUnder(dl, ImVec2(k.x - d, k.y), ImVec2(k.x, k.y - d), kGold, thick);
}

void drawDialStates(ImDrawList* dl, ImVec2 c, float r, int gear, bool brakeOn, float pedalT, bool regenArmed,
                    bool regenOn, bool cruiseOn, bool leftOn, bool rightOn, bool hazard) {
    const float s = std::clamp(r * 0.36f, 24.0f, 32.0f);
    const float w = std::max(1.6f, s * 0.07f);

    drawProw(dl, polar(c, r * 0.82f, kPi * 0.5f), r, gear, w);

    if (hazard) {
        strokeArc(dl, c, r * 1.08f, kPi * 0.25f, kPi * 0.75f, kGold, std::max(2.0f, w), 16);
    }
    if (regenArmed || regenOn) {
        drawInfold(dl, polar(c, r * 1.32f, kPi * 1.32f), r, w, regenOn);
    }
    if (cruiseOn) {
        drawLockHorizon(dl, c, r, w);
    }
    if (brakeOn) {
        drawShearGate(dl, polar(c, r * 1.32f, kPi * 1.08f), r, w, pedalT);
    }
    if (leftOn) {
        drawLash(dl, c, r, kPi * 0.75f, w);
    }
    if (rightOn) {
        drawLash(dl, c, r, kPi * 0.25f, w);
    }
}

}  // namespace

void drawVideoHud(ClientArena& arena, ImVec2 a, ImVec2 b) {
    const float w = b.x - a.x;
    const float h = b.y - a.y;
    if (w < 80.0f || h < 60.0f) {
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(a, b, true);

    const SignalMap& map = arena.signalMap();
    const Vital spd = map.vital("vehicle_speed", arena);
    const Vital brake = map.vital("brake", arena);
    const Vital fwd = map.vital("gear_forward", arena);
    const Vital ntr = map.vital("gear_neutral", arena);
    const Vital rev = map.vital("gear_reverse", arena);
    const Vital regenEn = map.vital("regen_enable", arena);
    const Vital regenAct = map.vital("regen_activate", arena);
    const Vital cruise = map.vital("cruise_enable", arena);
    const Vital left = map.vital("blinker_left", arena);
    const Vital right = map.vital("blinker_right", arena);
    const Vital hazard = map.vital("hazard", arena);
    const float mph = spd.have ? static_cast<float>(spd.value * kMpsToMph) : 0.0f;

    static float sMph = 0.0f;
    smoothToward(sMph, mph, ImGui::GetIO().DeltaTime);

    const float speedR = std::clamp(std::min(w * 0.42f, h * 0.38f), 72.0f, 118.0f);
    const ImVec2 c(a.x + w * 0.5f, a.y + h * 0.52f);
    analogGauge(dl, c, speedR, sMph, 80.0f, 8, "mph");

    int gear = 1;
    if (onBit(fwd)) {
        gear = 0;
    }
    if (onBit(rev)) {
        gear = 2;
    }
    if (onBit(ntr) && !onBit(fwd) && !onBit(rev)) {
        gear = 1;
    }

    const bool flash = (static_cast<int>(ImGui::GetTime() * 3.0) % 2) == 0;
    const bool haz = onBit(hazard);
    const bool brakeOn = brake.have && brake.value > 5.0;
    const float pedalT = brakeOn ? wrap01(static_cast<float>((brake.value - 5.0) / 95.0)) : 0.0f;
    drawDialStates(dl, c, speedR, gear, brakeOn, pedalT, onBit(regenEn), onBit(regenAct), onBit(cruise),
                   (onBit(left) || haz) && flash, (onBit(right) || haz) && flash, haz);

    dl->PopClipRect();
}

}  // namespace photon::ui
