#include "photon_ui/app.hpp"

#include "photon_ui/alarms.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "implot.h"

#include <SDL.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include <GLES3/gl3.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace photon::ui {
namespace {

const char* timelineLabel(std::uint8_t mode) {
    switch (static_cast<proto::TimelineMode>(mode)) {
        case proto::TimelineMode::Pause:
            return "PAUSE";
        case proto::TimelineMode::Play:
            return "PLAY";
        case proto::TimelineMode::Live:
            return "LIVE";
        case proto::TimelineMode::Buffering:
            return "BUFFERING";
        default:
            return "----";
    }
}

std::string_view baseName(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

std::string sourceStatus(const ClientArena& arena, Client::State /*state*/) {
    // Do not gate on HelloOk: adopt writes the source as soon as hello_ack
    // arrives, and the label must stay visible across brief reconnects.
    ctrl::SourceKind kind = arena.source;
    if (kind == ctrl::SourceKind::None && !arena.sourcePath.empty()) {
        kind = ctrl::SourceKind::LogReplay;
    }
    if (!arena.haveActiveSource && kind == ctrl::SourceKind::None) {
        return "--";
    }
    std::string label(ctrl::sourceName(kind));
    if (kind == ctrl::SourceKind::LogReplay && !arena.sourcePath.empty()) {
        label += " ";
        label += std::string(baseName(arena.sourcePath));
    } else if ((kind == ctrl::SourceKind::DaqTcp || kind == ctrl::SourceKind::Pcan) &&
               !arena.sourceHost.empty()) {
        label += " ";
        label += arena.sourceHost;
        if (arena.sourcePort != 0) {
            label += ":";
            label += std::to_string(arena.sourcePort);
        }
    }
    return label;
}

void formatClock(std::uint64_t ms, char* buf, std::size_t n) {
    const auto sec = ms / 1000;
    const auto msec = static_cast<unsigned>(ms % 1000);
    const auto s = static_cast<unsigned>(sec % 60);
    const auto m = static_cast<unsigned>((sec / 60) % 60);
    const auto h = static_cast<unsigned>((sec / 3600) % 24);
    std::snprintf(buf, n, "%02u:%02u:%02u.%03u", h, m, s, msec);
}

ImVec4 stateColor(Client::State state) {
    switch (state) {
        case Client::State::HelloOk:
            return theme::success();
        case Client::State::Connecting:
        case Client::State::Connected:
            return theme::warning();
        default:
            return theme::danger();
    }
}

}  // namespace

bool App::init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    window_ = SDL_CreateWindow("photon", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1440, 900,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window_ == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    gl_ = SDL_GL_CreateContext(window_);
    if (gl_ == nullptr) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, gl_);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigDpiScaleFonts = true;
    io.IniFilename = nullptr;
    if (!theme::loadFonts()) {
        std::fprintf(stderr, "photon: Inter/Tabler fonts missing from MEMFS /fonts — using default\n");
    }
    theme::apply();

    ImGui_ImplSDL2_InitForOpenGL(window_, gl_);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    screens_.push_back(std::make_unique<PitScreen>());
    screens_.push_back(std::make_unique<PlotsScreen>());
    screens_.push_back(std::make_unique<DynamicsScreen>());
    screens_.push_back(std::make_unique<LiveViewScreen>());
    screens_.push_back(std::make_unique<Scene3dScreen>());
    screens_.push_back(std::make_unique<TrackSimScreen>());
    screens_.push_back(std::make_unique<CanScreen>());
    screens_.push_back(std::make_unique<NetworkScreen>());
    return true;
}

void App::shutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    if (gl_ != nullptr) {
        SDL_GL_DeleteContext(gl_);
        gl_ = nullptr;
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

void App::frame() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);
        if (event.type == SDL_QUIT) {
            running_ = false;
        }
    }

    double cssW = 0.0;
    double cssH = 0.0;
    emscripten_get_element_css_size("#canvas", &cssW, &cssH);
    const double dpr = emscripten_get_device_pixel_ratio();
    if (cssW > 1.0 && cssH > 1.0) {
        SDL_SetWindowSize(window_, static_cast<int>(cssW + 0.5), static_cast<int>(cssH + 0.5));
        emscripten_set_canvas_element_size("#canvas", static_cast<int>(cssW * dpr + 0.5),
                                           static_cast<int>(cssH * dpr + 0.5));
    }

    client_.tick(emscripten_get_now());
    arena_.alarms().subscribe(client_);
    const auto tlMode = static_cast<proto::TimelineMode>(arena_.timeline().mode);
    const bool playing = tlMode == proto::TimelineMode::Live || tlMode == proto::TimelineMode::Play;
    arena_.alarms().evaluate(arena_, client_.helloOk(), playing,
                             static_cast<double>(arena_.timeline().cursorMs));
    if (arena_.signalMap().cellsMapped()) {
        arena_.refreshMappedCells();
    } else {
        arena_.updatePlaceholderCells(emscripten_get_now());
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    drawShell();
    ImGui::Render();

    ImGuiIO& io = ImGui::GetIO();
    const int fbW = static_cast<int>(io.DisplaySize.x * io.DisplayFramebufferScale.x);
    const int fbH = static_cast<int>(io.DisplaySize.y * io.DisplayFramebufferScale.y);
    glViewport(0, 0, fbW, fbH);
    const ImVec4 clear = theme::background();
    glClearColor(clear.x, clear.y, clear.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window_);
}

void App::drawShell() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##photon", nullptr, flags);
    ImGui::PopStyleVar(2);

    const float barH = theme::kTitlebarH;
    const float alarmH = kAlarmStripH;
    const float width = ImGui::GetContentRegionAvail().x;
    drawTitlebar(ImVec2(width, barH));

    ImGui::SetCursorPos(ImVec2(0.0f, barH));
    drawAlarmStrip(arena_.alarms(), ImVec2(width, alarmH));

    ImGui::SetCursorPos(ImVec2(0.0f, barH + alarmH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, theme::kContentWindowPadding);
    ImGui::BeginChild("##screen", ImGui::GetContentRegionAvail(),
                      ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    if (active_ >= 0 && active_ < static_cast<int>(screens_.size())) {
        UiContext ctx{arena_, client_};
        screens_[static_cast<std::size_t>(active_)]->draw(ctx);
    }
    ImGui::EndChild();
    ImGui::End();
}

void App::drawTitlebar(ImVec2 size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
    draw->AddRectFilled(p0, p1, theme::u32(theme::titlebar()));
    draw->AddLine(ImVec2(p0.x, p1.y - 1.0f), ImVec2(p1.x, p1.y - 1.0f), theme::u32(theme::border()));

    const ImVec2 pad = theme::kTitlebarPadding;
    const float btnH = size.y - pad.y * 2.0f;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad.x, p0.y + pad.y));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::kFrameRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 0.0f));
    for (int i = 0; i < static_cast<int>(screens_.size()); ++i) {
        const bool selected = i == active_;
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  theme::withAlpha(theme::mixColor(theme::titlebar(), theme::active(), 0.68f),
                                                   0.88f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::withAlpha(theme::active(), 0.72f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::withAlpha(theme::active(), 0.88f));
            ImGui::PushStyleColor(ImGuiCol_Text, theme::foreground());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(theme::muted(), 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::withAlpha(theme::active(), 0.52f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::withAlpha(theme::active(), 0.68f));
            ImGui::PushStyleColor(ImGuiCol_Text, theme::mutedFg());
        }
        if (ImGui::Button(screens_[static_cast<std::size_t>(i)]->label(), ImVec2(0.0f, btnH))) {
            active_ = i;
        }
        ImGui::PopStyleColor(4);
        ImGui::SameLine();
    }
    ImGui::PopStyleVar(3);
    ImGui::EndGroup();

    const auto& tl = arena_.timeline();
    char clock[32];
    formatClock(tl.cursorMs, clock, sizeof(clock));
    const std::string source = sourceStatus(arena_, client_.state());
    const char* mode = timelineLabel(tl.mode);
    const bool buffering = tl.mode == static_cast<std::uint8_t>(proto::TimelineMode::Buffering);
    const ImVec4 modeColor = buffering ? theme::warning() : theme::foreground();

    const float clusterGap = 8.0f;
    const float liveW = 52.0f;
    const float playW = 52.0f;
    const float pauseW = 58.0f;
    const ImVec2 stateSz = ImGui::CalcTextSize(client_.stateLabel());
    const ImVec2 sourceSz = ImGui::CalcTextSize(source.c_str());
    const ImVec2 modeSz = ImGui::CalcTextSize(mode);
    const ImVec2 clockSz = ImGui::CalcTextSize(clock);
    const ImVec2 indexSz = buffering ? ImGui::CalcTextSize("indexing log") : ImVec2(0.0f, 0.0f);
    const float clusterW = liveW + playW + pauseW + clusterGap * 2.0f + clusterGap + stateSz.x + clusterGap +
                           sourceSz.x + clusterGap + modeSz.x +
                           (buffering ? clusterGap + indexSz.x : 0.0f) + clusterGap + clockSz.x;
    ImGui::SetCursorScreenPos(ImVec2(p1.x - clusterW - pad.x, p0.y + pad.y));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(clusterGap, 0.0f));
    if (ImGui::Button("Live", ImVec2(liveW, btnH))) {
        client_.sendTimeline(ctrl::TimelineMode::Live, tl.cursorMs);
    }
    ImGui::SameLine();
    if (ImGui::Button("Play", ImVec2(playW, btnH))) {
        client_.sendTimeline(ctrl::TimelineMode::Play, tl.cursorMs);
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause", ImVec2(pauseW, btnH))) {
        client_.sendTimeline(ctrl::TimelineMode::Pause, tl.cursorMs);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(stateColor(client_.state()), "%s", client_.stateLabel());
    ImGui::SameLine();
    ImGui::TextColored(theme::primary(), "%s", source.c_str());
    ImGui::SameLine();
    ImGui::TextColored(modeColor, "%s", mode);
    if (buffering) {
        ImGui::SameLine();
        ImGui::TextColored(theme::warning(), "indexing log");
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(clock);
    ImGui::PopStyleVar();
    ImGui::EndGroup();

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
}

}  // namespace photon::ui
