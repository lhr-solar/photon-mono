#pragma once

namespace photon::ui {

// Embedded Grafana page. Native uses a WebView2 popup over the Plots pane.
// The page build uses an iframe. Coordinates are ImGui logical pixels.
void grafanaAttach(void* hwnd);
void grafanaShutdown();
void grafanaShow(int on);
void grafanaLayout(float x, float y, float w, float h);
void grafanaNavigate(const char* url);
void grafanaStatus(char* buf, int n);

}  // namespace photon::ui
