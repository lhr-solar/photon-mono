#include "photon_ui/client.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/signal_map.hpp"
#include "photon_ui/scene3d.hpp"
#include "photon_ui/theme.hpp"
#include "photon_ui/video.hpp"
#include "photon_ui/widgets.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace photon::ui {
namespace {

// Li-ion pit limits in Fahrenheit (DBC temps are Celsius).
//   < 95 F  (~35 C)  green  — typical pack comfort
//   < 113 F (~45 C)  yellow — charge/pit caution
//   >= 113 F         red    — at or above common BMS charge cut
constexpr float kCellTempGreenF = 95.0f;
constexpr float kCellTempYellowF = 113.0f;

// 16x2 at 1280: ~74 px wide tiles. Header is draw-list (no wrap). Each
// tile needs default-size voltage (~14) plus small index/temp — ~50 px
// row height. 3D / Live keep the leftover vertical space.
constexpr int kCellStripCols = 16;
constexpr int kCellStripRows = 2;
constexpr float kCellStripH = 152.0f;
constexpr float kCellStripGap = 6.0f;
constexpr float kCellHeaderH = 22.0f;
constexpr float kCellHeatBarW = 4.0f;

ImVec4 heatColor(float t01) {
    const float t = t01 < 0.0f ? 0.0f : (t01 > 1.0f ? 1.0f : t01);
    if (t < 0.5f) {
        const float u = t * 2.0f;
        return ImVec4(0.20f + 0.80f * u, 0.95f - 0.15f * u, 0.55f * (1.0f - u), 1.0f);
    }
    const float u = (t - 0.5f) * 2.0f;
    return ImVec4(1.00f, 0.80f - 0.55f * u, 0.00f + 0.30f * u, 1.0f);
}

ImVec4 voltageHeat(float v, const HeatScale& scale) {
    const double x = static_cast<double>(v);
    double t = 0.0;
    if (x <= scale.green) {
        t = 0.0;
    } else if (x >= scale.red) {
        t = 1.0;
    } else if (x <= scale.yellow) {
        const double span = scale.yellow - scale.green;
        t = span > 0.0 ? 0.5 * (x - scale.green) / span : 0.5;
    } else {
        const double span = scale.red - scale.yellow;
        t = span > 0.0 ? 0.5 + 0.5 * (x - scale.yellow) / span : 1.0;
    }
    return heatColor(static_cast<float>(t));
}

float celsiusToFahrenheit(float c) { return c * (9.0f / 5.0f) + 32.0f; }

ImVec4 tempColorF(float f) {
    if (f < kCellTempGreenF) {
        return theme::success();
    }
    if (f < kCellTempYellowF) {
        return theme::warning();
    }
    return theme::danger();
}

void drawCellStrip(const CellPack& cells, const HeatScale& scale, ImVec2 size) {
    theme::beginCard("cells", size, theme::card(), theme::kStripPadding, theme::CardFit::Band);

    const CellCallout call = SignalMap::cellCallout(cells.voltage);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 cardOrigin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    theme::pushSmallFont();
    float hx = cardOrigin.x;
    const float hy = cardOrigin.y + (kCellHeaderH - ImGui::GetFontSize()) * 0.5f;
    auto stamp = [&](const char* text, ImVec4 color) {
        draw->AddText(ImVec2(hx, hy), theme::u32(color), text);
        hx += ImGui::CalcTextSize(text).x + 10.0f;
    };
    stamp("Cells", theme::mutedFg());
    if (cells.synthetic) {
        stamp("SYNTHETIC", theme::warning());
    } else {
        char taps[24];
        std::snprintf(taps, sizeof(taps), "%d taps", static_cast<int>(cells.voltage.size()));
        stamp(taps, theme::mutedFg());
    }
    if (call.valid) {
        char minBuf[40];
        char maxBuf[40];
        char dBuf[32];
        std::snprintf(minBuf, sizeof(minBuf), "min #%d  %0.3f V", call.minIndex + 1,
                      static_cast<double>(call.minV));
        std::snprintf(maxBuf, sizeof(maxBuf), "max #%d  %0.3f V", call.maxIndex + 1,
                      static_cast<double>(call.maxV));
        std::snprintf(dBuf, sizeof(dBuf), "dV %0.3f", static_cast<double>(call.deltaV));
        stamp(minBuf, theme::danger());
        stamp(maxBuf, theme::primary());
        stamp(dBuf, theme::mutedFg());
    }
    theme::popSmallFont();

    const int n = static_cast<int>(cells.voltage.size());
    int cols = kCellStripCols;
    int rows = kCellStripRows;
    if (n > 0 && n != 32) {
        cols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
        if (cols < 1) {
            cols = 1;
        }
        rows = (n + cols - 1) / cols;
    }

    const float gap = kCellStripGap;
    const float gridH = std::max(8.0f, avail.y - kCellHeaderH);
    const float cw =
        (avail.x - gap * static_cast<float>(std::max(cols - 1, 0))) / static_cast<float>(std::max(cols, 1));
    const float ch =
        (gridH - gap * static_cast<float>(std::max(rows - 1, 0))) / static_cast<float>(std::max(rows, 1));
    const ImVec2 origin(cardOrigin.x, cardOrigin.y + kCellHeaderH);

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            const int i = r * cols + c;
            if (i >= n) {
                continue;
            }
            const float v = cells.voltage[static_cast<std::size_t>(i)];
            const float tempC =
                i < static_cast<int>(cells.temp.size()) ? cells.temp[static_cast<std::size_t>(i)] : 0.0f;
            const float tempF = celsiusToFahrenheit(tempC);
            const ImVec2 a(origin.x + static_cast<float>(c) * (cw + gap),
                           origin.y + static_cast<float>(r) * (ch + gap));
            const ImVec2 b(a.x + cw, a.y + ch);
            const bool haveV = std::isfinite(v) && v > 0.05f;
            const ImVec4 heat = haveV ? voltageHeat(v, scale) : theme::muted();
            draw->AddRectFilled(a, b, theme::u32(theme::mixColor(theme::card(), theme::muted(), 0.22f)),
                               theme::kTinyTileRounding);
            draw->AddRectFilled(a, ImVec2(a.x + kCellHeatBarW, b.y), theme::u32(heat), 0.0f);

            char idxBuf[8];
            char vBuf[16];
            char tBuf[16];
            std::snprintf(idxBuf, sizeof(idxBuf), "%d", i + 1);
            if (haveV) {
                std::snprintf(vBuf, sizeof(vBuf), "%0.3f V", static_cast<double>(v));
            } else {
                std::snprintf(vBuf, sizeof(vBuf), "-- V");
            }
            if (std::isfinite(tempC)) {
                std::snprintf(tBuf, sizeof(tBuf), "%0.0f F", static_cast<double>(tempF));
            } else {
                std::snprintf(tBuf, sizeof(tBuf), "-- F");
            }

            const float inset = 6.0f;
            const float textX = a.x + kCellHeatBarW + inset;
            const float textW = std::max(8.0f, cw - kCellHeatBarW - inset * 2.0f);
            theme::pushSmallFont();
            const float smallH = ImGui::GetFontSize();
            draw->AddText(ImVec2(textX, a.y + inset), theme::u32(theme::mutedFg()), idxBuf);
            const ImVec2 tSz = ImGui::CalcTextSize(tBuf);
            draw->AddText(ImVec2(textX + std::max(0.0f, (textW - tSz.x) * 0.5f), b.y - smallH - inset),
                          theme::u32(tempColorF(tempF)), tBuf);
            theme::popSmallFont();

            const ImVec2 vSz = ImGui::CalcTextSize(vBuf);
            const float vY = a.y + (ch - vSz.y) * 0.52f;
            draw->AddText(ImVec2(textX + std::max(0.0f, (textW - vSz.x) * 0.5f), vY),
                          theme::u32(haveV ? theme::foreground() : theme::mutedFg()), vBuf);

            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i);
            ImGui::InvisibleButton("cell", ImVec2(cw, ch));
            if (ImGui::IsItemHovered()) {
                const double tap =
                    i < static_cast<int>(cells.tapTs.size()) ? cells.tapTs[static_cast<std::size_t>(i)] : 0.0;
                if (tap > 0.0) {
                    ImGui::SetTooltip("cell %d\nV  %0.3f\nT  %0.1f C  (%0.1f F)\ntap %0.3f", i + 1,
                                      static_cast<double>(v), static_cast<double>(tempC),
                                      static_cast<double>(tempF), tap);
                } else {
                    ImGui::SetTooltip("cell %d\nV  %0.3f\nT  %0.1f C  (%0.1f F)", i + 1,
                                      static_cast<double>(v), static_cast<double>(tempC),
                                      static_cast<double>(tempF));
                }
            }
            ImGui::PopID();
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + gridH));
    theme::endCard();
}

void drawContactor(const Vital& v) {
    const bool closed = v.have && v.value > 0.5;
    const ImVec4 col = !v.have ? theme::mutedFg() : (closed ? theme::success() : theme::warning());
    ImGui::TextColored(col, "%s %s", v.label, !v.have ? "--" : (closed ? "CL" : "OP"));
}

}  // namespace

void PitScreen::draw(UiContext& ui) {
    ui.arena.signalMap().subscribe(ui.client, MapScreen::Pit | MapScreen::Scene3d);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float tileH = theme::kVitalTileHeight;
    const float protH = theme::kProtectionRowH;
    const float gap = theme::kGridGap;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));

    static constexpr const char* kRow1[] = {"pack_soc", "pack_voltage", "pack_current", "motor_rpm"};
    static constexpr const char* kRow2[] = {"vehicle_speed", "motor_temp", "inverter_temp", "array_power"};
    const float tileW = (avail.x - gap * 3.0f) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            ImGui::SameLine(0.0f, gap);
        }
        const Vital v = ui.arena.signalMap().vital(kRow1[i], ui.arena);
        drawVitalTile(kRow1[i], v, ImVec2(tileW, tileH));
    }
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            ImGui::SameLine(0.0f, gap);
        }
        const Vital v = ui.arena.signalMap().vital(kRow2[i], ui.arena);
        drawVitalTile(kRow2[i], v, ImVec2(tileW, tileH));
    }

    const float protW = (avail.x - gap * 3.0f) / 4.0f;
    const Vital bps = ui.arena.signalMap().vital("bps_state", ui.arena);
    theme::beginCard("bps", ImVec2(protW, protH),
                     bps.alarm ? theme::withAlpha(theme::danger(), 0.22f) : theme::card(),
                     theme::kStripPadding);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::mutedFg(), "BPS");
    ImGui::SameLine(0.0f, theme::kGridGap);
    if (bps.have) {
        ImGui::TextColored(bps.alarm ? theme::danger() : theme::success(), "%s",
                           bps.alarm ? "FAULT" : "OK");
    } else {
        ImGui::TextColored(theme::mutedFg(), "--");
    }
    if (bps.synthetic) {
        drawSyntheticWatermark();
    }
    theme::endCard();
    ImGui::SameLine(0.0f, gap);

    theme::beginCard("contactors", ImVec2(protW * 2.0f + gap, protH), theme::card(), theme::kStripPadding);
    theme::pushSmallFont();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::mutedFg(), "Contactors");
    static constexpr const char* kCt[] = {"contactor_hv_plus",          "contactor_hv_minus",
                                          "contactor_array",           "contactor_array_precharge",
                                          "contactor_motor",           "contactor_motor_precharge"};
    for (const char* role : kCt) {
        ImGui::SameLine(0.0f, theme::kGridGap);
        drawContactor(ui.arena.signalMap().vital(role, ui.arena));
    }
    theme::popSmallFont();
    theme::endCard();
    ImGui::SameLine(0.0f, gap);

    const Vital thr = ui.arena.signalMap().vital("throttle", ui.arena);
    const Vital brk = ui.arena.signalMap().vital("brake", ui.arena);
    theme::beginCard("pedals", ImVec2(protW, protH), theme::card(), theme::kStripPadding);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::mutedFg(), "Thr %0.0f%%  Brk %0.0f%%", thr.have ? thr.value : 0.0,
                       brk.have ? brk.value : 0.0);
    if (thr.hasBar) {
        drawMinMaxBar(thr.have ? thr.value : 0.0, 0.0, 100.0, theme::primary());
    }
    theme::endCard();

    const float remain = ImGui::GetContentRegionAvail().y;
    const float embedH =
        std::max(96.0f, remain - kCellStripH - ImGui::GetStyle().ItemSpacing.y);
    const float embedW = (avail.x - gap) * 0.5f;
    drawScene3DTile(ui.arena, ImVec2(embedW, embedH));
    ImGui::SameLine(0.0f, gap);
    drawVideoTile(ui.arena, ImVec2(embedW, embedH));

    drawCellStrip(ui.arena.cells(), ui.arena.signalMap().heat(), ImVec2(avail.x, kCellStripH));
    ImGui::PopStyleVar();
}

}  // namespace photon::ui
