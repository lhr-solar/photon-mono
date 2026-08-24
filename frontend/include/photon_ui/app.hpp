#pragma once

#include "imgui.h"
#include "photon_ui/arena.hpp"
#include "photon_ui/client.hpp"
#include "photon_ui/screen.hpp"

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
    bool running_ = true;
    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_ = nullptr;

    void drawShell();
    void drawTitlebar(ImVec2 size);
};

}  // namespace photon::ui
