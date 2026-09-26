#ifndef __EMSCRIPTEN__

#include "photon_ui/grafana_host.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>
#include <unknwn.h>
#ifndef interface
#define interface struct
#endif
#include <WebView2.h>
#include <wrl.h>

#include <cstdio>
#include <mutex>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
std::mutex gMu;
HWND gHwnd = nullptr;
HWND gHostWnd = nullptr;
ComPtr<ICoreWebView2Controller> gController;
ComPtr<ICoreWebView2> gWebView;
bool gCreating = false;
bool gWantShow = false;
int gLogical[4] = {0, 0, 8, 8};
std::wstring gUserData;
std::wstring gUrl = L"http://127.0.0.1:3000";
std::wstring gLoaded;
std::string gStatus = "Grafana: starting";

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

void setStatusLocked(std::string s) { gStatus = std::move(s); }

void applyBoundsLocked() {
    if (gHwnd == nullptr || gHostWnd == nullptr) {
        return;
    }
    const UINT dpi = GetDpiForWindow(gHwnd);
    const float scale = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    const int w = static_cast<int>(gLogical[2] * scale);
    const int h = static_cast<int>(gLogical[3] * scale);
    POINT origin{static_cast<LONG>(gLogical[0] * scale), static_cast<LONG>(gLogical[1] * scale)};
    ClientToScreen(gHwnd, &origin);
    if (!gWantShow || w < 8 || h < 8) {
        ShowWindow(gHostWnd, SW_HIDE);
        if (gController != nullptr) {
            gController->put_IsVisible(FALSE);
        }
        return;
    }
    SetWindowPos(gHostWnd, HWND_TOP, origin.x, origin.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(gHostWnd, SW_SHOWNA);
    if (gController != nullptr) {
        RECT fill{0, 0, w, h};
        gController->put_Bounds(fill);
        gController->put_IsVisible(TRUE);
    }
}

HWND ensureHostLocked() {
    if (gHostWnd != nullptr || gHwnd == nullptr) {
        return gHostWnd;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PhotonGrafanaHost";
    RegisterClassW(&wc);
    gHostWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"",
                               WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 8, 8, gHwnd, nullptr,
                               wc.hInstance, nullptr);
    return gHostWnd;
}

void navigateLocked() {
    if (gWebView == nullptr || gUrl.empty() || gUrl == gLoaded) {
        return;
    }
    gLoaded = gUrl;
    gWebView->Navigate(gUrl.c_str());
    setStatusLocked("Grafana: loading");
}

void finishController(ICoreWebView2Controller* controller) {
    ComPtr<ICoreWebView2> webview;
    if (controller == nullptr || FAILED(controller->get_CoreWebView2(&webview)) || webview == nullptr) {
        std::lock_guard<std::mutex> lock(gMu);
        gCreating = false;
        setStatusLocked("Grafana: WebView2 controller failed");
        return;
    }
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview->get_Settings(&settings)) && settings != nullptr) {
        settings->put_AreDevToolsEnabled(TRUE);
        settings->put_IsStatusBarEnabled(FALSE);
    }
    webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                BOOL ok = FALSE;
                if (args != nullptr) {
                    args->get_IsSuccess(&ok);
                }
                std::lock_guard<std::mutex> lock(gMu);
                setStatusLocked(ok == TRUE ? "Grafana" : "Grafana: not reachable at 127.0.0.1:3000");
                applyBoundsLocked();
                return S_OK;
            })
            .Get(),
        nullptr);
    {
        std::lock_guard<std::mutex> lock(gMu);
        gController = controller;
        gWebView = webview;
        gCreating = false;
        navigateLocked();
        applyBoundsLocked();
    }
}

void ensureCreated() {
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(gMu);
        if (gController != nullptr || gCreating || gHwnd == nullptr) {
            return;
        }
        if (ensureHostLocked() == nullptr) {
            setStatusLocked("Grafana: host window failed");
            return;
        }
        gCreating = true;
        hwnd = gHostWnd;
        setStatusLocked("Grafana: creating WebView2");
    }
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, gUserData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT envHr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(envHr) || env == nullptr) {
                    std::lock_guard<std::mutex> lock(gMu);
                    gCreating = false;
                    setStatusLocked("Grafana: install the WebView2 runtime");
                    return S_OK;
                }
                env->CreateCoreWebView2Controller(
                    hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                              [](HRESULT ctrlHr, ICoreWebView2Controller* controller) -> HRESULT {
                                  if (FAILED(ctrlHr) || controller == nullptr) {
                                      std::lock_guard<std::mutex> lock(gMu);
                                      gCreating = false;
                                      setStatusLocked("Grafana: WebView2 controller failed");
                                      return S_OK;
                                  }
                                  finishController(controller);
                                  return S_OK;
                              })
                              .Get());
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) {
        std::lock_guard<std::mutex> lock(gMu);
        gCreating = false;
        setStatusLocked("Grafana: WebView2 runtime missing");
    }
}

}  // namespace

namespace photon::ui {

void grafanaAttach(void* hwnd) {
    std::lock_guard<std::mutex> lock(gMu);
    gHwnd = static_cast<HWND>(hwnd);
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    gUserData = std::wstring(tmp) + L"photon-ui-grafana";
}

void grafanaShutdown() {
    std::lock_guard<std::mutex> lock(gMu);
    if (gController != nullptr) {
        gController->Close();
    }
    gWebView.Reset();
    gController.Reset();
    if (gHostWnd != nullptr) {
        DestroyWindow(gHostWnd);
        gHostWnd = nullptr;
    }
    gHwnd = nullptr;
    gCreating = false;
    gWantShow = false;
    gLoaded.clear();
}

void grafanaShow(int on) {
    ensureCreated();
    std::lock_guard<std::mutex> lock(gMu);
    gWantShow = on != 0;
    applyBoundsLocked();
}

void grafanaLayout(float x, float y, float w, float h) {
    ensureCreated();
    std::lock_guard<std::mutex> lock(gMu);
    gLogical[0] = static_cast<int>(x);
    gLogical[1] = static_cast<int>(y);
    gLogical[2] = static_cast<int>(w);
    gLogical[3] = static_cast<int>(h);
    applyBoundsLocked();
}

void grafanaNavigate(const char* url) {
    ensureCreated();
    std::lock_guard<std::mutex> lock(gMu);
    const std::wstring next = widen(url);
    if (next == gUrl && next == gLoaded) {
        return;
    }
    gUrl = next;
    gLoaded.clear();
    navigateLocked();
}

void grafanaStatus(char* buf, int n) {
    if (buf == nullptr || n <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(gMu);
    std::snprintf(buf, static_cast<std::size_t>(n), "%s", gStatus.c_str());
}

}  // namespace photon::ui

#endif
