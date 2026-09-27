#pragma once

namespace photon::ui {

// Embedded Grafana page. Native uses a WebView2 popup over the Plots pane.
// The page build uses an iframe. Coordinates are ImGui logical pixels.
void grafanaAttach(void* hwnd);
void grafanaShutdown();
// A frame starts hidden. The Plots screen claims the page while it is showing
// Grafana. grafanaEndFrame hides it when this frame did not claim it.
void grafanaBeginFrame();
void grafanaEndFrame();
void grafanaShow(int on);
void grafanaLayout(float x, float y, float w, float h);
void grafanaNavigate(const char* url);
void grafanaStatus(char* buf, int n);

}  // namespace photon::ui
