#pragma once

#include <chrono>
#include <string>
#include <string_view>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace photon::ui {

#ifdef __EMSCRIPTEN__
inline double nowMs() {
    return emscripten_get_now();
}
#else
inline double nowMs() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point t0 = Clock::now();
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
#endif

// Root of fonts/ and models/. PHOTON_ASSETS, then compile-time assets dir, then "assets".
std::string assetRoot();
std::string assetPath(std::string_view relative);

struct NativePageOptions {
    std::string liveUrl = "ws://127.0.0.1:8080/ws";
    std::string recordingId;
    bool pcan = false;
    bool sharedConsole = true;
    std::string publicHost;
    std::string screen;
    std::string basicUser = "photon";
    std::string basicPassword;
    std::string capturePath;
    int captureWidth = 1440, captureHeight = 900;
    int captureFrames = 60;
    double quitAfterMs = 0.0;
};

// No-op under Emscripten. Call before ClientArena is constructed.
void configureNativePage(NativePageOptions opt);
std::string nativeScreenParam();
std::string nativeWsBasicB64();
#ifndef __EMSCRIPTEN__
const NativePageOptions& nativePageOptions();
void nativeHostAttach(void* hwnd);
void nativeHostShutdown();
#endif

}  // namespace photon::ui
