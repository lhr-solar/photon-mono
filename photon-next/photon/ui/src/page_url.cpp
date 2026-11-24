#include "photon_ui/page_url.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#include <string_view>

namespace photon::ui {
namespace {

}  // namespace

#ifdef __EMSCRIPTEN__

std::string pageLiveUrl() {
    char buf[256] = {};
    MAIN_THREAD_EM_ASM(
        {
            var url = (typeof window !== "undefined" && window.photonLiveUrl)
                          ? window.photonLiveUrl()
                          : "ws://127.0.0.1:6900/v2";
            stringToUTF8(url, $0, $1);
        },
        buf, static_cast<int>(sizeof(buf)));
    if (buf[0] != '\0') {
        return std::string(buf);
    }
    return "ws://127.0.0.1:6900/v2";
}

std::string pageStoredToken() {
    char buf[512] = {};
    MAIN_THREAD_EM_ASM(
        {
            var token = "";
            try {
                token = window.photonStoredToken ? window.photonStoredToken() : "";
            } catch (e) {
                token = "";
            }
            stringToUTF8(token, $0, $1);
        },
        buf, static_cast<int>(sizeof(buf)));
    return std::string(buf);
}

std::string pageAskToken() {
    char buf[512] = {};
    MAIN_THREAD_EM_ASM(
        {
            var token = "";
            try {
                token = window.photonAskToken ? window.photonAskToken() : "";
            } catch (e) {
                token = "";
            }
            stringToUTF8(token, $0, $1);
        },
        buf, static_cast<int>(sizeof(buf)));
    return std::string(buf);
}

bool pageIsSharedConsole() {
    return MAIN_THREAD_EM_ASM_INT({
        if (typeof window !== "undefined" && window.photonSharedConsole) {
            return window.photonSharedConsole() ? 1 : 0;
        }
        return 0;
    }) != 0;
}

std::string pagePublicHost() {
    char buf[256] = {};
    MAIN_THREAD_EM_ASM(
        {
            var host = "";
            try {
                host = (location.hostname || "").toLowerCase();
                // Bracket classes, not backslash escapes: this body passes through the C++ preprocessor.
                var m = host.match(/^([0-9]+[.][0-9]+[.][0-9]+[.][0-9]+)[.]sslip[.]io$/);
                if (m) {
                    host = m[1];
                }
            } catch (e) {
                host = "";
            }
            stringToUTF8(host, $0, $1);
        },
        buf, static_cast<int>(sizeof(buf)));
    return std::string(buf);
}

#else

std::string pageLiveUrl() { return nativePageOptions().liveUrl; }

std::string pageStoredToken() { return nativePassword(); }

std::string pageAskToken() { return {}; }

bool pageIsSharedConsole() { return nativePageOptions().sharedConsole; }

std::string pagePublicHost() { return nativePageOptions().publicHost; }

#endif

}  // namespace photon::ui
