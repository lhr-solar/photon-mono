#include <algorithm>
#include "ui/app.hpp"
#include "ui/page_url.hpp"
#include "ui/platform.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include "net/net.hpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dbghelp.h>
#include <objbase.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#endif

#ifdef __EMSCRIPTEN__
namespace {
photon::ui::App gApp;
void loop() {
    gApp.frame();
}
}  // namespace
#endif

#ifndef __EMSCRIPTEN__
void photonSymbolize(void* const* frames, USHORT n) {
    HANDLE proc = GetCurrentProcess();
    // Deferred loads only — do not talk to a symbol server from a crash path.
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    if (!SymInitialize(proc, nullptr, TRUE)) {
        return;
    }
    char buf[sizeof(SYMBOL_INFO) + 256]{};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    for (USHORT i = 0; i < n; ++i) {
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        DWORD lineDisp = 0;
        if (SymFromAddr(proc, addr, &disp, sym)) {
            if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line)) {
                std::fprintf(stderr, "  #%u %s + 0x%llx  %s:%lu\n", static_cast<unsigned>(i), sym->Name,
                             static_cast<unsigned long long>(disp), line.FileName,
                             static_cast<unsigned long>(line.LineNumber));
            } else {
                std::fprintf(stderr, "  #%u %s + 0x%llx\n", static_cast<unsigned>(i), sym->Name,
                             static_cast<unsigned long long>(disp));
            }
        }
    }
    SymCleanup(proc);
}

LONG WINAPI photonCrashFilter(EXCEPTION_POINTERS* info) {
    void* frames[32]{};
    const USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
    const auto* rec = info != nullptr ? info->ExceptionRecord : nullptr;
    std::fprintf(stderr, "photon-ui crash code=0x%lx addr=%p frames=%u\n",
                 rec != nullptr ? static_cast<unsigned long>(rec->ExceptionCode) : 0ul,
                 rec != nullptr ? rec->ExceptionAddress : nullptr, static_cast<unsigned>(n));
    for (USHORT i = 0; i < n; ++i) {
        std::fprintf(stderr, "  #%u %p\n", static_cast<unsigned>(i), frames[i]);
    }
    std::fflush(stderr);
    photonSymbolize(frames, n);
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI photonVectored(EXCEPTION_POINTERS* info) {
    const DWORD code =
        info != nullptr && info->ExceptionRecord != nullptr ? info->ExceptionRecord->ExceptionCode : 0;
    // Heap corruption / failfast often skip the unhandled filter. Log first-chance.
    if (code != 0xC0000005ul && code != 0xC0000374ul && code != 0xC0000409ul) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    return photonCrashFilter(info);
}

void printUsage() {
    std::fprintf(
        stderr,
        "photon-ui: native ImGui frontend (same sources as the WASM UI)\n"
        "\n"
        "  photon-ui [--ws URL] [--password PASS] [--private|--shared] [--screen ID]\n"
        "\n"
        "  --ws URL        WebSocket (default ws://127.0.0.1:8080/ws, or PHOTON_WS)\n"
        "  --password P    HTTP Basic password (user photon). Needed for :8080/ws\n"
        "  --shared        Shared-console path (default). Catalog pin matches laptop UI\n"
        "  --private       Loopback console path\n"
        "  --screen ID     Open this tab (pit, dashboard, plots, mechanical, pdu, live, can)\n"
        "  --capture BMP   Render a hidden QA frame and exit (--width, --height, --frames)\n");
}

bool parseArgs(int argc, char** argv, photon::ui::NativePageOptions& opt) {
    if (const char* env = std::getenv("PHOTON_WS")) {
        if (env[0] != '\0') {
            opt.liveUrl = env;
        }
    }
    if (const char* env = std::getenv("PHOTON_PASSWORD")) {
        if (env[0] != '\0') {
            opt.basicPassword = env;
        }
    }
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const auto take = [&](const char* name) -> const char* {
            const std::size_t n = std::strlen(name);
            if (std::strncmp(arg, name, n) != 0) {
                return nullptr;
            }
            if (arg[n] == '=' && arg[n + 1] != '\0') {
                return arg + n + 1;
            }
            if (arg[n] == '\0' && i + 1 < argc) {
                return argv[++i];
            }
            return "";
        };
        if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
            printUsage();
            std::exit(0);
        }
        if (const char* v = take("--capture")) {
            opt.capturePath = v;
            continue;
        }
        if (const char* v = take("--width")) {
            opt.captureWidth = std::clamp(std::atoi(v), 400, 3840);
            continue;
        }
        if (const char* v = take("--height")) {
            opt.captureHeight = std::clamp(std::atoi(v), 400, 2160);
            continue;
        }
        if (const char* v = take("--frames")) {
            opt.captureFrames = std::clamp(std::atoi(v), 3, 3600);
            continue;
        }
        if (const char* v = take("--quit-after-ms")) {
            opt.quitAfterMs = std::strtod(v, nullptr);
            continue;
        }
        if (const char* v = take("--ws")) {
            opt.liveUrl = v;
            continue;
        }
        if (const char* v = take("--recording")) { opt.recordingId = v; continue; }
        if (std::strcmp(arg, "--pcan") == 0) { opt.pcan = true; opt.sharedConsole = false; continue; }
        if (const char* v = take("--password")) {
            opt.basicPassword = v;
            continue;
        }
        if (const char* v = take("--screen")) {
            opt.screen = v;
            continue;
        }
        if (const char* v = take("--host")) {
            opt.publicHost = v;
            continue;
        }
        if (std::strcmp(arg, "--shared") == 0) {
            opt.sharedConsole = true;
            continue;
        }
        if (std::strcmp(arg, "--private") == 0) {
            opt.sharedConsole = false;
            continue;
        }
        std::fprintf(stderr, "photon-ui: unknown argument %s\n", arg);
        printUsage();
        return false;
    }
    return true;
}
#endif

int main(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
    (void)argc;
    (void)argv;
    if (!gApp.init()) {
        return 1;
    }
    emscripten_set_main_loop(loop, 0, true);
    gApp.shutdown();
    return 0;
#else
    SDL_SetMainReady();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    AddVectoredExceptionHandler(1, photonVectored);
    SetUnhandledExceptionFilter(photonCrashFilter);
    photon::ui::NativePageOptions opt;
    if (!parseArgs(argc, argv, opt)) {
        return 2;
    }
    photon::ui::configureNativePage(std::move(opt));
    photon::net::Guard sockets;
    photon::ui::App app;
    if (!app.init()) {
        return 1;
    }
    std::fprintf(stdout, "photon-ui ws=%s shared=%d quitAfterMs=%.0f\n", photon::ui::pageLiveUrl().c_str(),
                 photon::ui::pageIsSharedConsole() ? 1 : 0, photon::ui::nativePageOptions().quitAfterMs);
    std::fflush(stdout);
    const double quitAfter = photon::ui::nativePageOptions().quitAfterMs;
    while (app.running()) {
        app.frame();
        if (quitAfter > 0.0 && photon::ui::nowMs() >= quitAfter) {
            std::fprintf(stdout, "photon-ui timed quit\n");
            std::fflush(stdout);
            break;
        }
    }
    std::fprintf(stdout, "photon-ui shutting down\n");
    std::fflush(stdout);
    app.shutdown();
    CoUninitialize();
    std::fprintf(stdout, "photon-ui shutdown ok\n");
    std::fflush(stdout);
    return 0;
#endif
}
