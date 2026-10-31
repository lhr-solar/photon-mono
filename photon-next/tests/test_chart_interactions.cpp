#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include "imgui.h"
#include "implot.h"
#include "ui/chart_grid.hpp"
#include "ui/plot_axis.hpp"
#include "ui/theme.hpp"

namespace photon::ui::theme {
const ColorScheme& scheme() {
    return kDark;
}
}  // namespace photon::ui::theme
static std::map<std::string, std::string> storage;
static int writes = 0;
extern "C" char* photon_ls_get(const char* key) {
    auto it = storage.find(key);
    if (it == storage.end()) return nullptr;
    auto* p = static_cast<char*>(std::malloc(it->second.size() + 1));
    std::memcpy(p, it->second.c_str(), it->second.size() + 1);
    return p;
}
extern "C" void photon_ls_set(const char* key, const char* value) {
    storage[key] = value;
    ++writes;
}
static int failures = 0, checks = 0;
#define REQUIRE(x)                                      \
    do {                                                \
        ++checks;                                       \
        if (!(x)) {                                     \
            ++failures;                                 \
            std::printf("line %d: %s\n", __LINE__, #x); \
        }                                               \
    } while (false)

int main() {
    using namespace photon::ui;
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto& io = ImGui::GetIO();
    io.MouseWheel = 1;
    io.KeyCtrl = false;
    REQUIRE(!plotWheelIsZoom());
    io.KeyAlt = true;
    REQUIRE(!plotWheelIsValueZoom());
    io.KeyShift = true;
    REQUIRE(plotWheelScrubDeltaMs(1000) == 0);
    io.KeyAlt = io.KeyShift = false;
    io.KeyCtrl = true;
    REQUIRE(plotWheelIsZoom());
    io.KeyAlt = true;
    REQUIRE(plotWheelIsValueZoom());
    io.KeyCtrl = io.KeyAlt = false;
    io.MouseWheel = 0;
    io.IniFilename = nullptr;
    io.DisplaySize = {1200, 800};
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    ChartGrid grid;
    ViewRect a{0, 0, 12, 14}, b{12, 0, 12, 14};
    ImVec2 body{}, dimensions{};
    std::vector<GridPanel> panels{{"a", "A", &a,
                                   [&](ImVec2 size) {
                                       body = ImGui::GetCursorScreenPos();
                                       dimensions = size;
                                       ImGui::TextUnformatted("Chart body");
                                   }},
                                  {"b", "B", &b, [](ImVec2) { ImGui::TextUnformatted("Chart B"); }}};
    auto frame = [&](bool locked = false, float width = 1200) {
        io.DisplaySize.x = width;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({width, 800});
        ImGui::Begin("Fixture", nullptr, ImGuiWindowFlags_NoDecoration);
        bool commit = grid.draw(panels, {0, 0}, locked);
        ImGui::End();
        ImGui::Render();
        return commit;
    };
    frame();
    frame();
    frame();
    const auto originalA = a, originalB = b;
    // A body drag cannot activate the layout editor.
    io.AddMousePosEvent(body.x + 50, body.y + 60);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMousePosEvent(body.x + 100, body.y + 60);
    frame();
    REQUIRE(!grid.editing());
    io.AddMouseButtonEvent(0, false);
    frame();
    REQUIRE(sameGridRect(a, originalA));
    // Capture a header, move into B, commit only on release, then undo.
    io.AddMousePosEvent(body.x + 30, body.y - 18);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    frame();
    REQUIRE(grid.editing());
    io.AddMousePosEvent(660, body.y - 18);
    REQUIRE(!frame());
    REQUIRE(a.x > 0);
    REQUIRE(!gridOverlap(a, b));
    io.AddMouseButtonEvent(0, false);
    REQUIRE(frame());
    REQUIRE(grid.canUndo());
    REQUIRE(grid.undo(panels));
    REQUIRE(sameGridRect(a, originalA));
    REQUIRE(sameGridRect(b, originalB));
    frame();
    // Escape restores both active and displaced panels without committing.
    io.AddMousePosEvent(body.x + 30, body.y - 18);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    frame();
    io.AddMousePosEvent(660, body.y - 18);
    frame();
    io.AddKeyEvent(ImGuiKey_Escape, true);
    REQUIRE(!frame());
    REQUIRE(!grid.editing());
    REQUIRE(sameGridRect(a, originalA));
    REQUIRE(sameGridRect(b, originalB));
    io.AddKeyEvent(ImGuiKey_Escape, false);
    io.AddMouseButtonEvent(0, false);
    frame();
    // Corner resize keeps minimum height; narrow rendering preserves saved geometry.
    io.AddMousePosEvent(body.x + dimensions.x - 5, body.y + dimensions.y + 10);
    frame();
    grid.beginGesture("a", true);
    io.AddMouseButtonEvent(0, true);
    frame();
    io.AddMousePosEvent(400, 100);
    frame();
    REQUIRE(a.h >= 13);
    io.AddMouseButtonEvent(0, false);
    frame();
    const auto savedA = a, savedB = b;
    frame(false, 500);
    REQUIRE(sameGridRect(a, savedA));
    REQUIRE(sameGridRect(b, savedB));
    REQUIRE(dimensions.y >= 252);
    REQUIRE(dimensions.x > 400);
    grid.beginGesture("a", false);
    io.AddMouseButtonEvent(0, true);
    frame(true, 1200);
    REQUIRE(!grid.editing());
    io.AddMouseButtonEvent(0, false);
    frame();
    // Compact value cards remain short and do not force the whole board into
    // a stacked layout. Their header drag uses the same commit/undo path.
    const auto beforeValueA = a, beforeValueB = b;
    a = {0, 0, 4, 4};
    b = {4, 0, 20, 14};
    panels[0].compact = true;
    frame();
    REQUIRE(dimensions.y < 100 && dimensions.x < 300);
    io.AddMousePosEvent(body.x + 30, body.y - 18);
    frame();
    io.AddMouseButtonEvent(0, true);
    frame();
    frame();
    REQUIRE(grid.editing());
    io.AddMousePosEvent(body.x + 200, body.y + 120);
    frame();
    REQUIRE(a.y > 0);
    REQUIRE(a.h == 4 && a.w == 4);
    io.AddMouseButtonEvent(0, false);
    REQUIRE(frame());
    REQUIRE(grid.undo(panels));
    REQUIRE(a.x == 0 && a.y == 0 && a.h == 4);
    panels[0].compact = false;
    a = beforeValueA;
    b = beforeValueB;
    frame();
    // Actual board persistence must not write during a gesture.
    storage["photon-next.chart-layout.test"] = R"({"schemaVersion":1,"locked":false,"panels":{}})";
    ImVec2 savedBody;
    ChartCard card{"saved", "Saved", {0, 0, 24, 14}, [&](ImVec2) {
                       savedBody = ImGui::GetCursorScreenPos();
                       ImGui::TextUnformatted("Saved chart");
                   }};
    auto boardFrame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({1200, 800});
        ImGui::Begin("Board", nullptr, ImGuiWindowFlags_NoDecoration);
        drawChartBoard("test", {card});
        ImGui::End();
        ImGui::Render();
    };
    boardFrame();
    boardFrame();
    REQUIRE(writes == 0);
    io.AddMousePosEvent(savedBody.x + 30, savedBody.y - 18);
    boardFrame();
    io.AddMouseButtonEvent(0, true);
    boardFrame();
    boardFrame();
    io.AddMousePosEvent(savedBody.x + 30, savedBody.y + 80);
    boardFrame();
    REQUIRE(writes == 0);
    io.AddMouseButtonEvent(0, false);
    boardFrame();
    REQUIRE(writes == 1);
    boardFrame();
    REQUIRE(writes == 1);
    // CPU frame cost of 24 chart shells; excludes GPU and series rendering.
    panels.clear();
    std::vector<ViewRect> rects(24);
    for (int i = 0; i < 24; ++i) {
        rects[i] = {i % 2 * 12, i / 2 * 14, 12, 14};
        panels.push_back(
            {std::to_string(i), "Panel", &rects[i], [](ImVec2) { ImGui::TextUnformatted("Chart"); }});
    }
    std::vector<double> ts(2048), ys(2048);
    for (int i = 0; i < 2048; ++i) {
        ts[i] = i;
        ys[i] = std::sin(i * 0.01);
    }
    for (auto& panel : panels)
        panel.render = [&](ImVec2 size) {
            if (ImPlot::BeginPlot("##trace", size, ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxes("Time", "Value", ImPlotAxisFlags_Lock, ImPlotAxisFlags_Lock);
                ImPlot::PlotLine("Signal", ts.data(), ys.data(), 2048);
                ImPlot::EndPlot();
            }
        };
    frame();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 300; ++i) frame();
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 300;
    std::printf(
        "chart interactions: %d checks, %d failures; 24-panel CPU frame %.3f ms (2048 samples/panel, "
        "viewport clipping; no GPU/IO)\n",
        checks, failures, ms);
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return failures ? 1 : 0;
}
