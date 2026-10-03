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

// 16x2 at 1280: ~74 px wide tiles. Header is draw-list (no wrap).
// Voltage is the primary number; temp stays smaller underneath.
constexpr int kCellStripCols = 16;
constexpr int kCellStripRows = 2;
constexpr float kCellStripH = 148.0f;
constexpr float kCellStripGap = 8.0f;
constexpr float kCellHeaderH = 24.0f;
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

    ImFont* headerFont = theme::uiFont();
    const float headerSize = theme::kSmallFontSize;
    float hx = cardOrigin.x;
    const float hy = cardOrigin.y + (kCellHeaderH - headerSize) * 0.5f;
    auto stamp = [&](const char* text, ImVec4 color) {
        const ImVec2 sz = headerFont != nullptr ? headerFont->CalcTextSizeA(headerSize, 1e9f, 0.0f, text)
                                                : ImGui::CalcTextSize(text);
        if (headerFont != nullptr) {
            draw->AddText(headerFont, headerSize, ImVec2(hx, hy), theme::u32(color), text);
        } else {
            draw->AddText(ImVec2(hx, hy), theme::u32(color), text);
        }
        hx += sz.x + 10.0f;
    };
    stamp("Cells", theme::mutedFg());
    if (cells.synthetic) {
        stamp("SYNTHETIC", theme::warning());
    } else if (cells.voltage.empty()) {
        stamp("waiting for taps", theme::mutedFg());
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

            const float inset = 3.0f;
            const float textX = a.x + kCellHeatBarW + inset;
            const float textW = std::max(8.0f, cw - kCellHeatBarW - inset * 2.0f);
            ImFont* font = theme::uiFont();
            const float idxSize = theme::kSmallFontSize;
            const float valueSize = theme::kFontSize;
            const float tempSize = theme::kSmallFontSize;
            const float tempLift = 2.0f;
            if (font != nullptr) {
                draw->AddText(font, idxSize, ImVec2(textX, a.y + inset),
                              theme::u32(theme::mutedFg()), idxBuf);
            } else {
                draw->AddText(ImVec2(textX, a.y + inset), theme::u32(theme::mutedFg()), idxBuf);
            }
            const ImVec2 vSz = font != nullptr ? font->CalcTextSizeA(valueSize, 1e9f, 0.0f, vBuf)
                                               : ImGui::CalcTextSize(vBuf);
            const ImVec2 tSz = font != nullptr ? font->CalcTextSizeA(tempSize, 1e9f, 0.0f, tBuf)
                                               : ImGui::CalcTextSize(tBuf);
            float vY = a.y + inset + idxSize + 1.0f;
            float tY = b.y - tSz.y - tempLift;
            const float minGap = 2.0f;
            if (vY + vSz.y + minGap > tY) {
                vY = a.y + inset + idxSize;
                tY = b.y - tSz.y - tempLift;
            }
            draw->AddText(font, valueSize,
                          ImVec2(textX + std::max(0.0f, (textW - vSz.x) * 0.5f), vY),
                          theme::u32(haveV ? theme::foreground() : theme::mutedFg()), vBuf);
            draw->AddText(font, tempSize,
                          ImVec2(textX + std::max(0.0f, (textW - tSz.x) * 0.5f), tY),
                          theme::u32(tempColorF(tempF)), tBuf);

            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i);
            ImGui::InvisibleButton("cell", ImVec2(std::max(1.0f, cw), std::max(1.0f, ch)));
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
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
    theme::endCard();
}

const char* contactorWord(const Vital& v) {
    if (!v.have) {
        return "--";
    }
    return v.value > 0.5 ? "CLOSED" : "OPEN";
}

ImVec4 contactorColor(const Vital& v) {
    if (!v.have) {
        return theme::mutedFg();
    }
    return v.value > 0.5 ? theme::success() : theme::warning();
}

void drawContactorRow(const char* id, const char* name, const Vital& v, float width) {
    const float h = 22.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::InvisibleButton("row", ImVec2(width, h));
    if (ImGui::IsItemHovered()) {
        if (v.signal != nullptr && v.signal[0] != '\0') {
            ImGui::SetTooltip("%s  %s\n%s", name, contactorWord(v), v.signal);
        } else {
            ImGui::SetTooltip("%s  %s", name, contactorWord(v));
        }
    }
    ImGui::PopID();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec4 col = contactorColor(v);
    const float pipR = 4.5f;
    const ImVec2 pip(p.x + pipR + 1.0f, p.y + h * 0.5f);
    if (v.have) {
        draw->AddCircleFilled(pip, pipR, theme::u32(col), 12);
    } else {
        draw->AddCircle(pip, pipR, theme::u32(col), 12, 1.2f);
    }

    const char* word = contactorWord(v);
    const ImVec2 nameSz = ImGui::CalcTextSize(name);
    const float textY = p.y + (h - nameSz.y) * 0.5f;
    const float nameX = p.x + pipR * 2.0f + 8.0f;
    draw->AddText(ImVec2(nameX, textY), theme::u32(theme::foreground()), name);
    draw->AddText(ImVec2(nameX + nameSz.x + 10.0f, textY), theme::u32(col), word);
}

void drawContactorGrid(const Vital& hvPlus, const Vital& hvMinus, const Vital& arrayMain,
                       const Vital& arrayPchg, const Vital& motorMain, const Vital& motorPchg, ImVec2 size) {
    theme::beginCard("prot-contactors", size, theme::card(), theme::kTilePadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 2.0f));
    const float inner = ImGui::GetContentRegionAvail().x;
    const float colGap = 8.0f;
    const float colW = (inner - colGap) * 0.5f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float rowH = 22.0f;
    const float blockH = rowH * 3.0f + 4.0f;
    const float yOff = std::max(0.0f, (ImGui::GetContentRegionAvail().y - blockH) * 0.5f);

    struct Cell {
        const char* id;
        const char* name;
        const Vital* v;
    };
    const Cell cells[3][2] = {
        {{"hv+", "HV+", &hvPlus}, {"hv-", "HV-", &hvMinus}},
        {{"arrp", "Arr P", &arrayPchg}, {"arr", "Array", &arrayMain}},
        {{"mtrp", "Mtr P", &motorPchg}, {"mtr", "Motor", &motorMain}},
    };
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 2; ++c) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + static_cast<float>(c) * (colW + colGap),
                                             origin.y + yOff + static_cast<float>(r) * (rowH + 2.0f)));
            drawContactorRow(cells[r][c].id, cells[r][c].name, *cells[r][c].v, colW);
        }
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(inner, std::max(blockH, ImGui::GetContentRegionAvail().y)));
    ImGui::PopStyleVar();
    theme::endCard();
}

void drawOkRow(const char* id, const char* name, const Vital& v) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 22.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::InvisibleButton("ok", ImVec2(w, h));
    ImGui::PopID();
    const bool on = v.have && v.value > 0.5;
    const char* word = !v.have ? "--" : (on ? "OK" : "NO");
    const ImVec4 col = !v.have ? theme::mutedFg() : (on ? theme::success() : theme::warning());
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float pipR = 4.5f;
    const ImVec2 pip(p.x + pipR + 1.0f, p.y + h * 0.5f);
    if (v.have) {
        draw->AddCircleFilled(pip, pipR, theme::u32(col), 12);
    } else {
        draw->AddCircle(pip, pipR, theme::u32(col), 12, 1.2f);
    }
    const ImVec2 nSz = ImGui::CalcTextSize(name);
    const ImVec2 wSz = ImGui::CalcTextSize(word);
    const float y = p.y + (h - nSz.y) * 0.5f;
    draw->AddText(ImVec2(p.x + pipR * 2.0f + 7.0f, y), theme::u32(theme::foreground()), name);
    draw->AddText(ImVec2(p.x + pipR * 2.0f + 7.0f + nSz.x + 8.0f, p.y + (h - wSz.y) * 0.5f), theme::u32(col),
                  word);
}

void drawPermitCard(const Vital& charge, const Vital& regen, const Vital& ready, ImVec2 size) {
    theme::beginCard("prot-permits", size, theme::card(), theme::kTilePadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 3.0f));
    drawOkRow("chg", "Charge", charge);
    drawOkRow("reg", "Regen", regen);
    drawOkRow("rdy", "Ready", ready);
    ImGui::PopStyleVar();
    theme::endCard();
}

void drawOnRow(const char* id, const char* name, const Vital& v) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 22.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::InvisibleButton("on", ImVec2(w, h));
    ImGui::PopID();
    const bool on = v.have && v.value > 0.5;
    const char* word = !v.have ? "--" : (on ? "ON" : "--");
    const ImVec4 col = on ? theme::success() : theme::mutedFg();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float pipR = 4.5f;
    const ImVec2 pip(p.x + pipR + 1.0f, p.y + h * 0.5f);
    if (on) {
        draw->AddCircleFilled(pip, pipR, theme::u32(col), 12);
    } else {
        draw->AddCircle(pip, pipR, theme::u32(col), 12, 1.2f);
    }
    const ImVec2 nSz = ImGui::CalcTextSize(name);
    const ImVec2 wSz = ImGui::CalcTextSize(word);
    const float y = p.y + (h - nSz.y) * 0.5f;
    draw->AddText(ImVec2(p.x + pipR * 2.0f + 7.0f, y), theme::u32(theme::foreground()), name);
    draw->AddText(ImVec2(p.x + nSz.x + pipR * 2.0f + 17.0f, p.y + (h - wSz.y) * 0.5f), theme::u32(col), word);
}

void drawIgnitionCard(const Vital& off, const Vital& array, const Vital& motor, ImVec2 size) {
    theme::beginCard("prot-ign", size, theme::card(), theme::kTilePadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 3.0f));
    drawOnRow("off", "Off", off);
    drawOnRow("arr", "Array", array);
    drawOnRow("mtr", "Motor", motor);
    ImGui::PopStyleVar();
    theme::endCard();
}

void drawPowerRow(const char* name, const Vital& vin, const Vital& iin) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 22.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool have = vin.have && iin.have;
    char val[16];
    if (have) {
        std::snprintf(val, sizeof(val), "%0.0f W", vin.value * iin.value);
    } else {
        std::snprintf(val, sizeof(val), "--");
    }
    const ImVec2 nSz = ImGui::CalcTextSize(name);
    const ImVec2 vSz = ImGui::CalcTextSize(val);
    const float y = p.y + (h - nSz.y) * 0.5f;
    draw->AddText(ImVec2(p.x, y), theme::u32(theme::mutedFg()), name);
    draw->AddText(ImVec2(p.x + nSz.x + 10.0f, p.y + (h - vSz.y) * 0.5f),
                  theme::u32(have ? theme::foreground() : theme::mutedFg()), val);
}

void drawSolarCard(const Vital& aVin, const Vital& aIin, const Vital& bVin, const Vital& bIin,
                   const Vital& cVin, const Vital& cIin, ImVec2 size) {
    theme::beginCard("prot-solar", size, theme::card(), theme::kTilePadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 3.0f));
    drawPowerRow("MPPT A", aVin, aIin);
    drawPowerRow("MPPT B", bVin, bIin);
    drawPowerRow("MPPT C", cVin, cIin);
    ImGui::PopStyleVar();
    theme::endCard();
}

void drawAuxCard(const Vital& volts, const Vital& amps, ImVec2 size) {
    theme::beginCard("prot-aux", size, theme::card(), theme::kTilePadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    ImGui::TextColored(theme::mutedFg(), "Aux");
    auto line = [](const Vital& v, const char* fmt, const char* empty) {
        char buf[24];
        if (v.have) {
            std::snprintf(buf, sizeof(buf), fmt, v.value);
            ImGui::TextUnformatted(buf);
        } else {
            ImGui::TextColored(theme::mutedFg(), "%s", empty);
        }
    };
    line(volts, "%0.1f V", "-- V");
    line(amps, "%0.2f A", "-- A");
    ImGui::PopStyleVar();
    theme::endCard();
}

void drawBpsCard(const Vital& bps, ImVec2 size) {
    const ImVec4 bg = bps.alarm ? theme::withAlpha(theme::danger(), 0.22f) : theme::card();
    theme::beginCard("prot-bps", size, bg, theme::kTilePadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 2.0f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImFont* font = theme::uiFont();
    const char* word = !bps.have ? "--" : (bps.alarm ? "FAULT" : "OK");
    const ImVec4 fg = !bps.have ? theme::mutedFg() : (bps.alarm ? theme::danger() : theme::success());
    if (font != nullptr) {
        draw->AddText(font, theme::kSmallFontSize, p, theme::u32(theme::mutedFg()), "BPS");
        draw->AddText(font, theme::kLargeFontSize, ImVec2(p.x, p.y + theme::kSmallFontSize + 4.0f),
                      theme::u32(fg), word);
    } else {
        ImGui::TextColored(theme::mutedFg(), "BPS");
        ImGui::TextColored(fg, "%s", word);
    }
    ImGui::Dummy(ImVec2(1.0f, theme::kSmallFontSize + theme::kLargeFontSize + 6.0f));
    ImGui::PopStyleVar();
    theme::endCard();
}

void drawPedalRow(const char* name, const Vital& v, ImVec4 fill) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 18.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* draw = ImGui::GetWindowDrawList();

    char pct[24];
    if (v.have) {
        const char* unit = (v.unit != nullptr && v.unit[0] != '\0') ? v.unit : "psi";
        std::snprintf(pct, sizeof(pct), "%0.0f %s", v.value, unit);
    } else {
        std::snprintf(pct, sizeof(pct), "--");
    }
    const ImVec2 nameSz = ImGui::CalcTextSize(name);
    const ImVec2 pctSz = ImGui::CalcTextSize(pct);
    const float textY = p.y + (h - nameSz.y) * 0.5f;
    draw->AddText(ImVec2(p.x, textY), theme::u32(theme::mutedFg()), name);

    const float nameCol = std::max(72.0f, nameSz.x + 12.0f);
    const float barH = 6.0f;
    const float barX = p.x + nameCol;
    const float barW = std::max(8.0f, w - nameCol - pctSz.x - 8.0f);
    const float barY = p.y + (h - barH) * 0.5f;
    draw->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), theme::u32(theme::muted()), 3.0f);
    const double full = (v.barMax > v.barMin) ? v.barMax : 3000.0;
    if (v.have) {
        const float frac = static_cast<float>(std::clamp(v.value / full, 0.0, 1.0));
        const float fw = frac * barW;
        if (fw > 1.0f) {
            draw->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + fw, barY + barH), theme::u32(fill), 3.0f);
        }
    }
    draw->AddText(ImVec2(p.x + w - pctSz.x, p.y + (h - pctSz.y) * 0.5f),
                  theme::u32(v.have ? theme::foreground() : theme::mutedFg()), pct);
}

void drawPedalCard(const Vital& throttle, const Vital& front, const Vital& rear, ImVec2 size) {
    theme::beginCard("prot-pedals", size, theme::card(), theme::kTilePadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 1.0f));
    ImGui::TextColored(theme::mutedFg(), "Pedals");
    drawPedalRow("Throttle", throttle, theme::accent());
    drawPedalRow("Front", front, theme::primary());
    drawPedalRow("Rear", rear, theme::warning());
    ImGui::PopStyleVar();
    theme::endCard();
}

}  // namespace

void PitScreen::draw(UiContext& ui) {
    ui.client.markVideoWanted();
    if (ui.client.helloOk() && !ui.arena.video.subscribed && !ui.arena.catalogPinned()) {
        ui.client.goLive();
    }
    ui.arena.signalMap().subscribe(ui.client, MapScreen::Pit | MapScreen::Scene3d);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float tileH = theme::kVitalTileHeight;
    const float protH = theme::kProtectionRowH;
    const float gap = theme::kGridGap;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));

    static constexpr const char* kRow1[] = {"pack_soc", "pack_voltage", "pack_current", "motor_rpm"};
    static constexpr const char* kRow2[] = {"vehicle_speed_mph", "motor_temp", "inverter_temp", "array_power"};
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

    const auto& map = ui.arena.signalMap();
    const Vital bps = map.vital("bps_state", ui.arena);
    const Vital hvPlus = map.vital("contactor_hv_plus", ui.arena);
    const Vital hvMinus = map.vital("contactor_hv_minus", ui.arena);
    const Vital arrayMain = map.vital("contactor_array", ui.arena);
    const Vital arrayPchg = map.vital("contactor_array_precharge", ui.arena);
    const Vital motorMain = map.vital("contactor_motor", ui.arena);
    const Vital motorPchg = map.vital("contactor_motor_precharge", ui.arena);
    const Vital chargeOk = map.vital("bps_charge_ok", ui.arena);
    const Vital regenOk = map.vital("bps_regen_ok", ui.arena);
    const Vital motorReady = map.vital("motor_ready", ui.arena);
    const Vital mpptAVin = map.vital("mppt_a_vin", ui.arena);
    const Vital mpptAIin = map.vital("mppt_a_iin", ui.arena);
    const Vital mpptBVin = map.vital("mppt_b_vin", ui.arena);
    const Vital mpptBIin = map.vital("mppt_b_iin", ui.arena);
    const Vital mpptCVin = map.vital("mppt_c_vin", ui.arena);
    const Vital mpptCIin = map.vital("mppt_c_iin", ui.arena);
    const Vital auxV = map.vital("supp_voltage", ui.arena);
    const Vital auxI = map.vital("supp_current", ui.arena);
    const Vital throttle = map.vital("throttle", ui.arena);
    const Vital front = map.vital("brake_p1", ui.arena);
    const Vital rear = map.vital("brake_p2", ui.arena);

    const Vital ignArr = map.vital("ign_array", ui.arena);
    const Vital ignMtr = map.vital("ign_motor", ui.arena);
    const Vital ignOff = map.vital("ign_off", ui.arena);
    const float bpsW = 120.0f;
    const float ctW = 280.0f;
    const float permitW = 156.0f;
    const float solarW = 186.0f;
    const float ignW = 148.0f;
    const float auxW = 108.0f;
    const float pedalW =
        std::max(200.0f, avail.x - bpsW - ctW - permitW - solarW - ignW - auxW - gap * 6.0f);
    drawBpsCard(bps, ImVec2(bpsW, protH));
    ImGui::SameLine(0.0f, gap);
    drawContactorGrid(hvPlus, hvMinus, arrayMain, arrayPchg, motorMain, motorPchg, ImVec2(ctW, protH));
    ImGui::SameLine(0.0f, gap);
    drawPermitCard(chargeOk, regenOk, motorReady, ImVec2(permitW, protH));
    ImGui::SameLine(0.0f, gap);
    drawSolarCard(mpptAVin, mpptAIin, mpptBVin, mpptBIin, mpptCVin, mpptCIin, ImVec2(solarW, protH));
    ImGui::SameLine(0.0f, gap);
    drawIgnitionCard(ignOff, ignArr, ignMtr, ImVec2(ignW, protH));
    ImGui::SameLine(0.0f, gap);
    drawAuxCard(auxV, auxI, ImVec2(auxW, protH));
    ImGui::SameLine(0.0f, gap);
    drawPedalCard(throttle, front, rear, ImVec2(pedalW, protH));

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
