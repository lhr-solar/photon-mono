#include "photon_ui/client.hpp"
#include "photon_ui/grafana_host.hpp"
#include "photon_ui/screen.hpp"
#include "photon_ui/theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(void, grafana_web_show, (int on), {
    var el = document.getElementById("photon-grafana");
    if (!el) {
        return;
    }
    el.style.display = on ? "block" : "none";
});
EM_JS(void, grafana_web_layout, (float x, float y, float w, float h), {
    var el = document.getElementById("photon-grafana");
    if (!el) {
        el = document.createElement("iframe");
        el.id = "photon-grafana";
        el.style.position = "fixed";
        el.style.border = "0";
        el.style.zIndex = "30";
        el.style.background = "#111";
        document.body.appendChild(el);
    }
    el.style.left = x + "px";
    el.style.top = y + "px";
    el.style.width = Math.max(0, w) + "px";
    el.style.height = Math.max(0, h) + "px";
});
EM_JS(void, grafana_web_navigate, (const char* url), {
    var el = document.getElementById("photon-grafana");
    if (!el) {
        el = document.createElement("iframe");
        el.id = "photon-grafana";
        el.style.position = "fixed";
        el.style.border = "0";
        el.style.zIndex = "30";
        document.body.appendChild(el);
    }
    var next = UTF8ToString(url);
    if (el.getAttribute("data-src") !== next) {
        el.setAttribute("data-src", next);
        el.src = next;
    }
});
#endif

namespace photon::ui {
namespace {

const char* defaultGrafanaUrl() {
    const char* env = std::getenv("PHOTON_GRAFANA_URL");
    if (env != nullptr && env[0] != '\0') {
        return env;
    }
    return "http://127.0.0.1:3000";
}

}  // namespace

#ifdef __EMSCRIPTEN__
void grafanaAttach(void*) {}
void grafanaShutdown() {}
void grafanaShow(int on) { grafana_web_show(on); }
void grafanaLayout(float x, float y, float w, float h) { grafana_web_layout(x, y, w, h); }
void grafanaNavigate(const char* url) { grafana_web_navigate(url); }
void grafanaStatus(char* buf, int n) {
    if (buf != nullptr && n > 0) {
        std::snprintf(buf, static_cast<std::size_t>(n), "Grafana");
    }
}
#endif

void PlotsScreen::draw(UiContext& ui) {
    static int mode = 0;
    if (ImGui::Button(mode == 0 ? "Grafana" : "Show Grafana")) {
        mode = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button(mode == 1 ? "Plots" : "Show plots")) {
        mode = 1;
        grafanaShow(0);
    }
    if (mode == 1) {
        views_.draw(ui);
        return;
    }
    static char url[256]{};
    static bool urlInit = false;
    static bool opened = false;
    if (!urlInit) {
        std::snprintf(url, sizeof(url), "%s", defaultGrafanaUrl());
        urlInit = true;
    }
    char status[96]{};
    grafanaStatus(status, static_cast<int>(sizeof(status)));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::primary(), "Grafana");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::min(420.0f, ImGui::GetContentRegionAvail().x * 0.55f));
    const bool edited = ImGui::InputText("##grafana-url", url, sizeof(url));
    ImGui::SameLine();
    if (ImGui::Button("Open") || (!opened && !edited)) {
        grafanaNavigate(url);
        opened = true;
    }
    ImGui::SameLine();
    ImGui::TextColored(theme::mutedFg(), "%s", status[0] != '\0' ? status : "Grafana");
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(avail);
    if (avail.x > 8.0f && avail.y > 8.0f) {
        grafanaLayout(origin.x, origin.y, avail.x, avail.y);
        grafanaShow(1);
    } else {
        grafanaShow(0);
    }
    (void)ui;
}

void PlotsScreen::onPlayOrLive() { views_.releasePlayheadScrub(); }

void PlotsScreen::onSessionChanged() { views_.followSessionWindow(); }

}  // namespace photon::ui
