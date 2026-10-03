#include "photon_ui/app.hpp"

#include "photon_ui/alarms.hpp"
#include "photon_ui/clock_format.hpp"
#include "photon_ui/custom_view.hpp"
#include "photon_ui/gl_api.hpp"
#include "photon_ui/page_url.hpp"
#include "photon_ui/platform.hpp"
#include "photon_ui/scene3d.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "implot.h"

#include <SDL.h>
#ifndef __EMSCRIPTEN__
#include <SDL_syswm.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifdef __EMSCRIPTEN__
EM_JS(void, photon_copy_screen_param, (char* buf, int n), {
    var params = new URLSearchParams(window.location.search);
    var s = params.get("screen");
    if (!s) {
        s = "";
    }
    stringToUTF8(s, buf, n);
});
#else
extern "C" void photon_copy_screen_param(char* buf, int n);
#endif

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

bool pathMentionsDay(const std::string& path, const ctrl::Message::DriveDay& day) {
    if (path.empty()) {
        return false;
    }
    if (!day.path.empty() && path == day.path) {
        return true;
    }
    return !day.date.empty() && path.find(day.date) != std::string::npos;
}

bool liveFeed(const ClientArena& arena) {
    if (arena.catalogPinned()) {
        return false;
    }
    return arena.source == ctrl::SourceKind::DaqTcp || arena.source == ctrl::SourceKind::Pcan ||
           arena.source == ctrl::SourceKind::Synthetic;
}

const ctrl::Message::DriveDay* replayDay(const ClientArena& arena) {
    if (arena.source != ctrl::SourceKind::LogReplay) {
        return nullptr;
    }
    for (const auto& day : arena.driveDays) {
        if (pathMentionsDay(arena.sourcePath, day)) {
            return &day;
        }
    }
    return nullptr;
}

std::string sourceStatus(const ClientArena& arena, Client::State /*state*/) {
    // Do not gate on HelloOk: adopt writes the source as soon as hello_ack
    // arrives, and the label must stay visible across brief reconnects.
    if (arena.catalogPinned()) {
        std::string label = "Replay";
        const auto day = localYmdFromUnixMs(arena.timeline().minTimeMs);
        if (!day.empty()) {
            label += " · ";
            label += day;
        }
        return label;
    }
    if (liveFeed(arena)) {
        std::string label = "Live";
        const auto host = sourceDisplayHost(arena.sourceHost);
        if (!host.empty()) {
            label += " · ";
            label += host;
        }
        return label;
    }
    if (arena.source == ctrl::SourceKind::LogReplay || !arena.sourcePath.empty()) {
        std::string label = "Replay";
        if (const auto* day = replayDay(arena)) {
            label += " · ";
            label += day->date;
            return label;
        }
        if (!arena.sourcePath.empty()) {
            label += " · ";
            label += std::string(baseName(arena.sourcePath));
        }
        return label;
    }
    if (!arena.haveActiveSource) {
        return "--";
    }
    return std::string(ctrl::sourceName(arena.source));
}

void formatClock(std::uint64_t ms, char* buf, std::size_t n) {
    formatLocalClock(ms, buf, n, ClockParts::DateTime);
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
#ifdef __EMSCRIPTEN__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
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
#ifdef __EMSCRIPTEN__
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
#else
    // Native: Pit/custom-view SetCursorScreenPos is recoverable. A hard
    // assert is not a fix.
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
#endif
    if (!theme::loadFonts()) {
        std::fprintf(stderr, "photon: Inter/Tabler fonts missing from MEMFS /fonts — using default\n");
    }
    theme::loadPersisted();
    theme::apply();

    ImGui_ImplSDL2_InitForOpenGL(window_, gl_);
#ifdef __EMSCRIPTEN__
    ImGui_ImplOpenGL3_Init("#version 300 es");
#else
    ImGui_ImplOpenGL3_Init("#version 330");
    if (!photonGlLoadExtras()) {
        std::fprintf(stderr, "photon-ui: GL extras missing — 3D view will fail\n");
    }
    SDL_SysWMinfo wminfo{};
    SDL_VERSION(&wminfo.version);
    if (SDL_GetWindowWMInfo(window_, &wminfo) && wminfo.subsystem == SDL_SYSWM_WINDOWS) {
        nativeHostAttach(wminfo.info.win.window);
    }
#endif

    screens_.push_back(std::make_unique<PitScreen>());
    screens_.push_back(std::make_unique<DashboardScreen>());
    screens_.push_back(std::make_unique<PlotsScreen>());
    screens_.push_back(std::make_unique<DynamicsScreen>());
    screens_.push_back(std::make_unique<MechanicalScreen>());
    screens_.push_back(std::make_unique<PduScreen>());
    screens_.push_back(std::make_unique<LiveViewScreen>());
    screens_.push_back(std::make_unique<Scene3dScreen>());
    screens_.push_back(std::make_unique<TrackSimScreen>());
    screens_.push_back(std::make_unique<CanScreen>());
    screens_.push_back(std::make_unique<NetworkScreen>());
    screens_.push_back(std::make_unique<AnalysisScreen>());
    for (auto& screen : screens_) {
        if (auto* analysis = dynamic_cast<AnalysisScreen*>(screen.get())) {
            analysis->attachTransport(client_);
            break;
        }
    }

    char want[32]{};
    photon_copy_screen_param(want, static_cast<int>(sizeof(want)));
    if (want[0] != '\0') {
        for (int i = 0; i < static_cast<int>(screens_.size()); ++i) {
            if (std::strcmp(screens_[static_cast<std::size_t>(i)]->id(), want) == 0) {
                active_ = i;
                break;
            }
        }
    }
    return true;
}

void App::shutdown() {
#ifndef __EMSCRIPTEN__
    // Scene3dRuntime and VideoDecoder are process-lifetime objects. Their
    // destructors call glDelete* after this function has already killed the
    // context — that is an access violation on desktop.
    photonGlMarkContextLost();
    nativeHostShutdown();
#endif
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

#ifdef __EMSCRIPTEN__
    double cssW = 0.0;
    double cssH = 0.0;
    emscripten_get_element_css_size("#canvas", &cssW, &cssH);
    const double dpr = emscripten_get_device_pixel_ratio();
    if (cssW > 1.0 && cssH > 1.0) {
        SDL_SetWindowSize(window_, static_cast<int>(cssW + 0.5), static_cast<int>(cssH + 0.5));
        emscripten_set_canvas_element_size("#canvas", static_cast<int>(cssW * dpr + 0.5),
                                           static_cast<int>(cssH * dpr + 0.5));
    }
#endif

    client_.tick(nowMs());
    arena_.tickPinnedPlayback(nowMs());
    if (arena_.consumeSessionFollow()) {
        for (auto& screen : screens_) {
            screen->onSessionChanged();
        }
    }
    if (arena_.consumeIndexReady() && client_.helloOk() && !arena_.catalogPinned()) {
        const auto& tl = arena_.timeline();
        const auto mode = static_cast<ctrl::TimelineMode>(tl.mode);
        if (mode != ctrl::TimelineMode::Play && mode != ctrl::TimelineMode::Live) {
            client_.sendTimeline(ctrl::TimelineMode::Play, tl.cursorMs == 0 ? tl.minTimeMs : tl.cursorMs);
        }
    }
    client_.beginInterest();
    arena_.alarms().subscribe(client_);
    arena_.maybePrimeHistory(client_);
    const auto tlMode = static_cast<proto::TimelineMode>(arena_.timeline().mode);
    const bool playing = tlMode == proto::TimelineMode::Live || tlMode == proto::TimelineMode::Play;
    arena_.alarms().evaluate(arena_, client_.helloOk(), playing,
                             static_cast<double>(arena_.timeline().cursorMs));
    if (arena_.signalMap().cellsMapped()) {
        arena_.refreshMappedCells();
    } else {
        arena_.holdReplayCells();
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    drawShell();
    client_.commitInterest();
    ImGui::Render();

    ImGuiIO& io = ImGui::GetIO();
    const int fbW = static_cast<int>(io.DisplaySize.x * io.DisplayFramebufferScale.x);
    const int fbH = static_cast<int>(io.DisplaySize.y * io.DisplayFramebufferScale.y);
    glViewport(0, 0, fbW, fbH);
    const ImVec4 clear = theme::background();
    glClearColor(clear.x, clear.y, clear.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#ifndef __EMSCRIPTEN__
    // 3D GLB parse can stall a frame; pump again so hello/catalog are not starved.
    client_.tick(nowMs());
#endif
    flushPlotImageExport();
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

    const float barH =
        std::max(theme::kTitlebarH, ImGui::GetFontSize() + theme::kTitlebarPadding.y * 2.0f + 6.0f);
    const float alarmH = std::max(kAlarmStripH, ImGui::GetFontSize() + 16.0f);
    const float width = ImGui::GetContentRegionAvail().x;
    drawTitlebar(ImVec2(width, barH));

    ImGui::SetCursorPos(ImVec2(0.0f, barH));
    drawAlarmStrip(arena_.alarms(), ImVec2(width, alarmH));

    ImGui::SetCursorPos(ImVec2(0.0f, barH + alarmH));
    const ImVec2 rest = ImGui::GetContentRegionAvail();
    UiContext ctx{arena_, client_};
    ImGui::SetCursorScreenPos(ImGui::GetCursorScreenPos());
    logDrawer_.draw(ctx, rest);
    const float drawerW = logDrawer_.occupiedWidth();
    ImGui::SetCursorPos(ImVec2(drawerW, barH + alarmH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, theme::kContentWindowPadding);
    ImGui::BeginChild("##screen", ImVec2(std::max(80.0f, rest.x - drawerW), rest.y),
                      ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    if (active_ >= 0 && active_ < static_cast<int>(screens_.size())) {
        screens_[static_cast<std::size_t>(active_)]->draw(ctx);
        const char* sid = screens_[static_cast<std::size_t>(active_)]->id();
        if (std::strcmp(sid, "scene3d") != 0 && std::strcmp(sid, "live") != 0 &&
            std::strcmp(sid, "pit") != 0) {
            hideMaps3d();
        }
    }
    ImGui::EndChild();
    ImGui::End();
    logDrawer_.drawExportPopup(ctx);
    const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
    setMaps3dInteractive(!popupOpen);
    if (logDrawer_.exportDialogOpen() || mapsOverlayHoldOff()) {
        hideMaps3d();
    }
}

void App::drawTitlebar(ImVec2 size) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
    draw->AddRectFilled(p0, p1, theme::u32(theme::titlebar()));
    if (theme::mode() == theme::Mode::Falcon) {
        draw->AddRectFilled(ImVec2(p0.x, p1.y - 2.0f), p1, theme::u32(theme::accent()));
    } else {
        draw->AddLine(ImVec2(p0.x, p1.y - 1.0f), ImVec2(p1.x, p1.y - 1.0f), theme::u32(theme::border()));
    }

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
            ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(theme::foreground(), 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::withAlpha(theme::foreground(), 0.06f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::withAlpha(theme::foreground(), 0.10f));
            ImGui::PushStyleColor(ImGuiCol_Text, theme::foreground());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(theme::muted(), 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::withAlpha(theme::foreground(), 0.05f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::withAlpha(theme::foreground(), 0.08f));
            ImGui::PushStyleColor(ImGuiCol_Text, theme::mutedFg());
        }
        if (ImGui::Button(screens_[static_cast<std::size_t>(i)]->label(), ImVec2(0.0f, btnH))) {
            active_ = i;
        }
        if (selected) {
            const ImVec2 a = ImGui::GetItemRectMin();
            const ImVec2 b = ImGui::GetItemRectMax();
            draw->AddRectFilled(ImVec2(a.x, b.y - 2.0f), ImVec2(b.x, b.y), theme::u32(theme::chrome()));
        }
        ImGui::PopStyleColor(4);
        ImGui::SameLine();
    }
    ImGui::PopStyleVar(3);
    ImGui::EndGroup();

    const auto& tl = arena_.timeline();
    const bool onLive = liveFeed(arena_);
    char clock[48];
    formatClock(onLive ? liveTitlebarClockMs(tl.cursorMs) : arena_.displayCursorMs(), clock, sizeof(clock));
    const auto* day = replayDay(arena_);
    const std::string source = sourceStatus(arena_, client_.state());
    const char* mode = timelineLabel(tl.mode);
    const bool buffering = tl.mode == static_cast<std::uint8_t>(proto::TimelineMode::Buffering);
    const bool showTransport = !onLive;
    const bool showMode = !onLive || buffering;
    const ImVec4 modeColor = buffering ? theme::warning() : theme::foreground();

    const float clusterGap = 8.0f;
    const float liveW = 52.0f;
    const float playW = 52.0f;
    const float pauseW = 58.0f;
    const ImVec2 stateSz = ImGui::CalcTextSize(client_.stateLabel());
    const ImVec2 sourceSz = ImGui::CalcTextSize(source.c_str());
    const ImVec2 modeSz = showMode ? ImGui::CalcTextSize(mode) : ImVec2(0.0f, 0.0f);
    const ctrl::CatalogSignal* antenna = findAntennaSignal(arena_);
    char antBuf[72];
    if (antenna != nullptr) {
        const std::uint32_t antId = antenna->id;
        client_.ensureSubscribed(std::span<const std::uint32_t>(&antId, 1));
        const double antV = arena_.readout(antId, 0.0);
        if (antenna->unit.empty()) {
            std::snprintf(antBuf, sizeof(antBuf), "Ant  %.0f", antV);
        } else {
            std::snprintf(antBuf, sizeof(antBuf), "Ant  %.0f %s", antV, antenna->unit.c_str());
        }
    } else {
        std::snprintf(antBuf, sizeof(antBuf), "Ant  n/a");
    }
    const ImVec2 antSz = ImGui::CalcTextSize(antBuf);
    const ImVec2 clockSz = ImGui::CalcTextSize(clock);
    const ImVec2 indexSz = buffering ? ImGui::CalcTextSize("indexing log") : ImVec2(0.0f, 0.0f);
    const float themeW = 64.0f;
    std::vector<std::string> dayChoices;
    dayChoices.reserve(arena_.driveDays.size() + 8);
    for (const auto& row : arena_.driveDays) {
        if (isReplayLogDate(row.date)) {
            dayChoices.push_back(row.date);
        }
    }
    for (const auto& row : client_.historyCatalog()) {
        if (row.maxMs <= row.minMs) {
            continue;
        }
        const auto a = localYmdFromUnixMs(row.minMs);
        const auto b = localYmdFromUnixMs(row.maxMs);
        if (isReplayLogDate(a)) {
            dayChoices.push_back(a);
        }
        if (isReplayLogDate(b) && b != a) {
            dayChoices.push_back(b);
        }
    }
    std::sort(dayChoices.begin(), dayChoices.end());
    dayChoices.erase(std::unique(dayChoices.begin(), dayChoices.end()), dayChoices.end());
    const std::string pinnedDay =
        arena_.catalogPinned() ? localYmdFromUnixMs(arena_.timeline().minTimeMs) : std::string();
    const float dayW = dayChoices.empty() ? 0.0f : 128.0f;
    const float clusterW = themeW + clusterGap + (dayW > 0.0f ? dayW + clusterGap : 0.0f) + liveW +
                           (showTransport ? playW + pauseW + clusterGap : 0.0f) + clusterGap * 2.0f + clusterGap +
                           stateSz.x + clusterGap + sourceSz.x + clusterGap + antSz.x +
                           (showMode ? clusterGap + modeSz.x : 0.0f) +
                           (buffering ? clusterGap + indexSz.x : 0.0f) + clusterGap + clockSz.x;
    ImGui::SetCursorScreenPos(ImVec2(p1.x - clusterW - pad.x, p0.y + pad.y));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(clusterGap, 0.0f));
    theme::drawSwitcher(ImVec2(themeW, btnH));
    if (!dayChoices.empty()) {
        ImGui::SameLine();
        const char* preview = onLive ? "Live"
                                     : (day != nullptr ? day->date.c_str()
                                                       : (!pinnedDay.empty() ? pinnedDay.c_str() : "Replay"));
        ImGui::SetNextItemWidth(dayW);
        if (ImGui::BeginCombo("##drive_day", preview)) {
            if (ImGui::Selectable("Live", onLive)) {
                client_.goLive();
                arena_.setPreviewCursor(0, false);
                for (auto& screen : screens_) {
                    screen->onPlayOrLive();
                }
            }
            for (const auto& date : dayChoices) {
                const bool sel =
                    (day != nullptr && day->date == date) || (!onLive && pinnedDay == date);
                if (ImGui::Selectable(date.c_str(), sel)) {
                    client_.playDate(date);
                    arena_.setPreviewCursor(0, false);
                    for (auto& screen : screens_) {
                        screen->onPlayOrLive();
                    }
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    if (onLive) {
        ImGui::PushStyleColor(ImGuiCol_Button, theme::withAlpha(theme::primary(), 0.28f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::withAlpha(theme::primary(), 0.38f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::withAlpha(theme::primary(), 0.45f));
    }
    if (ImGui::Button("Live", ImVec2(liveW, btnH))) {
        client_.goLive();
        arena_.setPreviewCursor(0, false);
        for (auto& screen : screens_) {
            screen->onPlayOrLive();
        }
    }
    if (onLive) {
        ImGui::PopStyleColor(3);
    }
    if (showTransport) {
        ImGui::SameLine();
        if (ImGui::Button("Play", ImVec2(playW, btnH))) {
            client_.sendTimeline(ctrl::TimelineMode::Play, tl.cursorMs);
            arena_.setPreviewCursor(0, false);
            for (auto& screen : screens_) {
                screen->onPlayOrLive();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Pause", ImVec2(pauseW, btnH))) {
            client_.sendTimeline(ctrl::TimelineMode::Pause, tl.cursorMs);
        }
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(stateColor(client_.state()), "%s", client_.stateLabel());
    ImGui::SameLine();
    ImGui::TextColored(theme::primary(), "%s", source.c_str());
    ImGui::SameLine();
    ImGui::TextColored(antenna != nullptr ? theme::success() : theme::mutedFg(), "%s", antBuf);
    if (showMode) {
        ImGui::SameLine();
        ImGui::TextColored(modeColor, "%s", mode);
    }
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
