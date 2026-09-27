#include "photon_ui/grafana_host.hpp"

#ifdef __EMSCRIPTEN__

#include <emscripten.h>

#include <cstdio>

namespace {
bool gClaimed = false;

EM_JS(void, grafana_web_show, (int on), {
    var el = document.getElementById("photon-grafana");
    if (!el) {
        return;
    }
    el.style.display = on ? "block" : "none";
});

EM_JS(void, grafana_web_ensure, (), {
    var el = document.getElementById("photon-grafana");
    if (el) {
        return;
    }
    el = document.createElement("iframe");
    el.id = "photon-grafana";
    el.style.position = "fixed";
    el.style.border = "0";
    el.style.zIndex = "30";
    el.style.background = "#111";
    el.style.display = "none";
    document.body.appendChild(el);
});

EM_JS(void, grafana_web_layout, (float x, float y, float w, float h), {
    var el = document.getElementById("photon-grafana");
    if (!el) {
        return;
    }
    el.style.left = x + "px";
    el.style.top = y + "px";
    el.style.width = Math.max(0, w) + "px";
    el.style.height = Math.max(0, h) + "px";
});

EM_JS(void, grafana_web_navigate, (const char* url), {
    var el = document.getElementById("photon-grafana");
    if (!el) {
        return;
    }
    var next = UTF8ToString(url);
    if (el.getAttribute("data-src") !== next) {
        el.setAttribute("data-src", next);
        el.src = next;
    }
});
}  // namespace

namespace photon::ui {

void grafanaAttach(void*) {}
void grafanaShutdown() {}

void grafanaBeginFrame() { gClaimed = false; }

void grafanaShow(int on) {
    gClaimed = on != 0;
    grafana_web_ensure();
    grafana_web_show(on);
}

void grafanaLayout(float x, float y, float w, float h) {
    grafana_web_ensure();
    grafana_web_layout(x, y, w, h);
}

void grafanaNavigate(const char* url) {
    grafana_web_ensure();
    grafana_web_navigate(url != nullptr ? url : "");
}

void grafanaStatus(char* buf, int n) {
    if (buf != nullptr && n > 0) {
        std::snprintf(buf, static_cast<std::size_t>(n), "Grafana");
    }
}

void grafanaEndFrame() {
    if (!gClaimed) {
        grafana_web_show(0);
    }
}

}  // namespace photon::ui

#else

#include "photon_ui/overlay_host.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace {
photon::ui::OverlayPage gPage({L"PhotonGrafanaHost", L"photon-ui-grafana", "Grafana", true});

std::wstring widen(const char* utf8) {
    if (utf8 == nullptr || utf8[0] == '\0') {
        return L"http://127.0.0.1:3000";
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) {
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
    }
    return out;
}
}  // namespace

namespace photon::ui {

void grafanaAttach(void* hwnd) { gPage.attach(hwnd); }
void grafanaShutdown() { gPage.shutdown(); }
void grafanaBeginFrame() { gPage.beginFrame(); }
void grafanaShow(int on) { gPage.show(on != 0); }
void grafanaLayout(float x, float y, float w, float h) { gPage.layout(x, y, w, h); }
void grafanaNavigate(const char* url) { gPage.navigate(widen(url).c_str()); }
void grafanaStatus(char* buf, int n) { gPage.copyStatus(buf, n); }
void grafanaEndFrame() { gPage.endFrame(); }

}  // namespace photon::ui

#endif
