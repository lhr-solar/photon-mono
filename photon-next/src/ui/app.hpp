#pragma once

#include "imgui.h"
#include "ui/arena.hpp"
#include "ui/client.hpp"
#include "ui/connection.hpp"
#include "ui/log_drawer.hpp"
#include "ui/screen.hpp"

#include <memory>
#include <vector>

struct SDL_Window;
typedef void* SDL_GLContext;

namespace photon::ui {

class App {
public:
    bool init();
    void frame();
    void shutdown();
    bool running() const { return running_; }

private:
    ClientArena arena_;
    Client client_{arena_};
    std::vector<std::unique_ptr<Screen>> screens_;
    int active_ = 0;
    int captureFrame_ = 0;
    bool running_ = true;
    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_ = nullptr;
    LogDrawer logDrawer_{};
    ConnectionSettings connection_{};
    bool openConnection_ = false;

    void drawShell();
    void drawTitlebar(ImVec2 size);
};

}  // namespace photon::ui
