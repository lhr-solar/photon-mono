#include "photon_ui/platform.hpp"

#include "photon/base64.hpp"

#include <SDL.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>

namespace photon::ui {
namespace {

#ifndef __EMSCRIPTEN__
NativePageOptions gNative{};
#endif

}  // namespace

#ifndef __EMSCRIPTEN__
const NativePageOptions& nativePageOptions() { return gNative; }
#endif

namespace {

std::filesystem::path storeDir() {
    if (const char* env = std::getenv("PHOTON_UI_STORE")) {
        if (env[0] != '\0') {
            return env;
        }
    }
    return std::filesystem::temp_directory_path() / "photon-ui-store";
}

std::filesystem::path keyFile(const char* key) {
    std::string name = key != nullptr ? key : "";
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') {
            c = '_';
        }
    }
    if (name.empty()) {
        name = "empty";
    }
    std::error_code ec;
    std::filesystem::create_directories(storeDir(), ec);
    return storeDir() / name;
}

}  // namespace

std::string assetRoot() {
    if (const char* env = std::getenv("PHOTON_ASSETS")) {
        if (env[0] != '\0') {
            return env;
        }
    }
    static const std::string root = [] {
        std::error_code ec;
#ifdef PHOTON_ASSETS_DIR
        // The checkout this exe was built from, so asset edits show without a relink.
        if (std::filesystem::is_directory(PHOTON_ASSETS_DIR, ec)) {
            return std::string(PHOTON_ASSETS_DIR);
        }
#endif
        // A copied bundle carries assets\ beside photon-ui.exe.
        if (char* base = SDL_GetBasePath()) {
            const std::filesystem::path beside =
                std::filesystem::path(reinterpret_cast<const char8_t*>(base)) / "assets";
            SDL_free(base);
            if (std::filesystem::is_directory(beside, ec)) {
                return beside.string();
            }
        }
        return std::string("assets");
    }();
    return root;
}

std::string assetPath(std::string_view relative) {
    while (!relative.empty() && (relative.front() == '/' || relative.front() == '\\')) {
        relative.remove_prefix(1);
    }
    std::filesystem::path p = std::filesystem::path(assetRoot()) / std::string(relative);
    return p.lexically_normal().string();
}

void configureNativePage(NativePageOptions opt) {
#ifdef __EMSCRIPTEN__
    (void)opt;
#else
    if (opt.liveUrl.empty()) {
        opt.liveUrl = "ws://127.0.0.1:8765";
    }
    gNative = std::move(opt);
#endif
}

std::string nativeScreenParam() {
#ifdef __EMSCRIPTEN__
    return {};
#else
    return gNative.screen;
#endif
}

std::filesystem::path nativeStorePath(const char* key) { return keyFile(key); }

std::string nativeWsBasicB64() {
#ifdef __EMSCRIPTEN__
    return {};
#else
    if (gNative.basicPassword.empty()) {
        return {};
    }
    const std::string raw = gNative.basicUser.empty() ? ("photon:" + gNative.basicPassword)
                                                      : (gNative.basicUser + ":" + gNative.basicPassword);
    return crypto::base64Encode(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()));
#endif
}

}  // namespace photon::ui

#ifndef __EMSCRIPTEN__

namespace {

char* dupCString(const std::string& s) {
    char* buf = static_cast<char*>(std::malloc(s.size() + 1));
    if (buf == nullptr) {
        return nullptr;
    }
    std::memcpy(buf, s.c_str(), s.size() + 1);
    return buf;
}

}  // namespace

extern "C" {

char* photon_ls_get(const char* key) {
    std::ifstream in(photon::ui::nativeStorePath(key), std::ios::binary);
    if (!in) {
        return nullptr;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return dupCString(text);
}

void photon_ls_set(const char* key, const char* value) {
    std::ofstream out(photon::ui::nativeStorePath(key), std::ios::binary | std::ios::trunc);
    if (!out) {
        return;
    }
    if (value != nullptr) {
        out.write(value, static_cast<std::streamsize>(std::strlen(value)));
    }
}

void photon_download_text(const char* name, const char* text) {
    const char* file = (name != nullptr && name[0] != '\0') ? name : "photon-export.json";
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out || text == nullptr) {
        return;
    }
    out.write(text, static_cast<std::streamsize>(std::strlen(text)));
}

void photon_download_png(const char* /*name*/, int /*w*/, int /*h*/, const unsigned char* /*rgba*/) {}

void photon_pick_json() {}

void photon_pick_analysis_json() {}

char* photon_theme_get() { return photon_ls_get("photon-next.theme"); }

void photon_theme_set(const char* name) { photon_ls_set("photon-next.theme", name); }

void photon_copy_screen_param(char* buf, int n) {
    if (buf == nullptr || n <= 0) {
        return;
    }
    const std::string screen = photon::ui::nativeScreenParam();
    std::snprintf(buf, static_cast<std::size_t>(n), "%s", screen.c_str());
}

}  // extern "C"

#endif
