#include "ui/page_url.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

#include <string_view>

namespace photon::ui {
namespace {

bool hostIsLoopback(std::string_view host) {
    return host.empty() || host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]";
}

}  // namespace

#ifdef __EMSCRIPTEN__

std::string pageLiveUrl() {
    char buf[256] = {};
    MAIN_THREAD_EM_ASM(
        {
            var url = (typeof window !== "undefined" && window.photonLiveUrl)
                          ? window.photonLiveUrl()
                          : "ws://127.0.0.1:8080/ws";
            stringToUTF8(url, $0, $1);
        },
        buf, static_cast<int>(sizeof(buf)));
    if (buf[0] != '\0') {
        return std::string(buf);
    }
    return "ws://127.0.0.1:8080/ws";
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
                var m = host.match(/^(\d+\.\d+\.\d+\.\d+)\.sslip\.io$/);
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

bool pageIsSharedConsole() { return nativePageOptions().sharedConsole; }

std::string pagePublicHost() { return nativePageOptions().publicHost; }

#endif

std::string sourceDisplayHost(std::string_view backendHost) {
    if (pageIsSharedConsole() && hostIsLoopback(backendHost)) {
        const auto pub = pagePublicHost();
        return pub.empty() ? "150.136.85.207" : pub;
    }
    return std::string(backendHost);
}

}  // namespace photon::ui
