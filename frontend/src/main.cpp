#include "photon_ui/app.hpp"

#include <emscripten.h>

#include <cstdlib>

namespace {

photon::ui::App gApp;

void loop() { gApp.frame(); }

}  // namespace

int main() {
    if (!gApp.init()) {
        return 1;
    }
    emscripten_set_main_loop(loop, 0, true);
    gApp.shutdown();
    return 0;
}
