#pragma once

namespace photon::ui {

// One Win32 popup + WebView2 page. Maps and Grafana are two instances.
// ImGui passes logical pixels. This host multiplies by the window DPI.
struct OverlayConfig {
    const wchar_t* windowClass = L"PhotonOverlay";
    const wchar_t* profileLeaf = L"photon-ui-overlay";
    const char* label = "page";
    bool devTools = false;
};

class OverlayPage {
public:
    // webview and controller are ICoreWebView2* and ICoreWebView2Controller*.
    using PrepareFn = void (*)(void* webview, void* controller, void* user);
    using NavigatedFn = void (*)(void* webview, bool ok, void* user);

    explicit OverlayPage(OverlayConfig cfg);
    ~OverlayPage();

    OverlayPage(const OverlayPage&) = delete;
    OverlayPage& operator=(const OverlayPage&) = delete;

    void attach(void* parentHwnd);
    void shutdown();
    void show(bool on);
    void layout(float x, float y, float w, float h);
    void navigate(const wchar_t* url);
    void execute(const wchar_t* js);
    void copyStatus(char* buf, int n);
    void setStatus(const char* text);
    bool navigationOk();

    // A frame starts unclaimed. endFrame hides the page when nobody claimed it.
    void beginFrame();
    void endFrame();

    void setPrepare(PrepareFn fn, void* user);
    void setNavigated(NavigatedFn fn, void* user);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace photon::ui
