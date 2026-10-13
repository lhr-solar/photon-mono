#ifndef __EMSCRIPTEN__

#include "ui/platform.hpp"

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

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
std::mutex gMu;

HWND gHwnd = nullptr;
HWND gHostWnd = nullptr;
ComPtr<ICoreWebView2Controller> gController;
ComPtr<ICoreWebView2> gWebView;
bool gCreating = false;
bool gNavOk = false;
bool gJsReady = false;
bool gWantShow = false;
bool gTriedFile = false;
int gLogical[4]{};
std::string gStatus = "OSM 3D: starting WebView2";
std::vector<std::wstring> gPendingJs;
std::wstring gUserData;
std::wstring gAssets;

std::wstring utf16(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string jsEscape(const char* s) {
    std::string out;
    if (s == nullptr) {
        return out;
    }
    out.reserve(std::strlen(s) + 8);
    for (const char* p = s; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c == '\\' || c == '\'' || c == '"') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c == '\n') {
            out += "\\n";
        } else if (c < 32) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

void setStatusLocked(std::string s) { gStatus = std::move(s); }

HWND ensureHostWindowLocked() {
    if (gHostWnd != nullptr || gHwnd == nullptr) {
        return gHostWnd;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"PhotonMaps3dHost";
    RegisterClassW(&wc);
    // Owned popup, not a child of the SDL/OpenGL HWND. A child is painted
    // over by the GL swapchain, which is why OSM never appeared.
    gHostWnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"",
                               WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 8, 8, gHwnd, nullptr,
                               wc.hInstance, nullptr);
    return gHostWnd;
}

void applyBoundsLocked() {
    if (gHwnd == nullptr) {
        return;
    }
    if (ensureHostWindowLocked() == nullptr) {
        return;
    }
    const UINT dpi = GetDpiForWindow(gHwnd);
    const float scale = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    const int w = static_cast<int>(std::lround(static_cast<double>(gLogical[2]) * scale));
    const int h = static_cast<int>(std::lround(static_cast<double>(gLogical[3]) * scale));
    POINT origin{static_cast<LONG>(std::lround(static_cast<double>(gLogical[0]) * scale)),
                 static_cast<LONG>(std::lround(static_cast<double>(gLogical[1]) * scale))};
    ClientToScreen(gHwnd, &origin);
    if (w < 8 || h < 8 || !gWantShow) {
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

void execLocked(std::wstring js) {
    if (gWebView == nullptr || !gNavOk) {
        gPendingJs.push_back(std::move(js));
        return;
    }
    gWebView->ExecuteScript(js.c_str(), nullptr);
}

void flushJsLocked() {
    if (gWebView == nullptr || !gNavOk) {
        return;
    }
    for (const auto& js : gPendingJs) {
        gWebView->ExecuteScript(js.c_str(), nullptr);
    }
    gPendingJs.clear();
}

HRESULT onWebMessage(ICoreWebView2* /*sender*/, ICoreWebView2WebMessageReceivedEventArgs* args) {
    if (args == nullptr) {
        return S_OK;
    }
    LPWSTR raw = nullptr;
    if (FAILED(args->TryGetWebMessageAsString(&raw)) || raw == nullptr) {
        return S_OK;
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, raw, -1, nullptr, 0, nullptr, nullptr);
    std::string json(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) {
        WideCharToMultiByte(CP_UTF8, 0, raw, -1, json.data(), n, nullptr, nullptr);
    }
    CoTaskMemFree(raw);
    const bool ready = json.find("\"ready\":true") != std::string::npos ||
                       json.find("\"ready\": true") != std::string::npos;
    std::string status;
    const auto key = json.find("\"status\":\"");
    if (key != std::string::npos) {
        const auto from = key + 10;
        const auto to = json.find('"', from);
        if (to != std::string::npos) {
            status = json.substr(from, to - from);
        }
    }
    std::lock_guard<std::mutex> lock(gMu);
    gJsReady = ready;
    if (!status.empty()) {
        setStatusLocked(std::move(status));
    } else if (ready) {
        setStatusLocked("OSM 3D");
    }
    return S_OK;
}

void finishController(ICoreWebView2Controller* controller) {
    ComPtr<ICoreWebView2> webview;
    if (controller == nullptr || FAILED(controller->get_CoreWebView2(&webview)) || webview == nullptr) {
        std::lock_guard<std::mutex> lock(gMu);
        setStatusLocked("OSM 3D: WebView2 controller failed");
        gCreating = false;
        return;
    }
    ComPtr<ICoreWebView2Controller2> ctrl2;
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&ctrl2))) && ctrl2 != nullptr) {
        COREWEBVIEW2_COLOR bg{255, 0x8E, 0xB4, 0xD4};
        ctrl2->put_DefaultBackgroundColor(bg);
    }
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview->get_Settings(&settings)) && settings != nullptr) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    }
    ComPtr<ICoreWebView2_3> web3;
    if (SUCCEEDED(webview.As(&web3)) && web3 != nullptr && !gAssets.empty()) {
        const HRESULT mapHr = web3->SetVirtualHostNameToFolderMapping(
            L"photon.assets", gAssets.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        if (FAILED(mapHr)) {
            std::lock_guard<std::mutex> lock(gMu);
            setStatusLocked("OSM 3D: asset host mapping failed");
        }
    }
    webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                return onWebMessage(sender, args);
            })
            .Get(),
        nullptr);
    webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                BOOL ok = FALSE;
                if (args != nullptr) {
                    args->get_IsSuccess(&ok);
                }
                std::lock_guard<std::mutex> lock(gMu);
                gNavOk = ok == TRUE;
                if (!gNavOk) {
                    setStatusLocked("OSM 3D: maps3d.html failed to load");
                    if (!gTriedFile && gWebView != nullptr && !gAssets.empty()) {
                        gTriedFile = true;
                        std::wstring file = L"file:///" + gAssets;
                        for (wchar_t& c : file) {
                            if (c == L'\\') {
                                c = L'/';
                            }
                        }
                        if (!file.empty() && file.back() != L'/') {
                            file += L'/';
                        }
                        file += L"maps3d.html";
                        gWebView->Navigate(file.c_str());
                    }
                    return S_OK;
                }
                setStatusLocked("OSM 3D: loading tiles…");
                flushJsLocked();
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
        applyBoundsLocked();
    }
    webview->Navigate(L"https://photon.assets/maps3d.html");
}

void ensureCreated() {
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(gMu);
        if (gController != nullptr || gCreating || gHwnd == nullptr) {
            return;
        }
        if (ensureHostWindowLocked() == nullptr) {
            setStatusLocked("OSM 3D: host window failed");
            return;
        }
        gCreating = true;
        hwnd = gHostWnd;
        setStatusLocked("OSM 3D: creating WebView2");
        std::fprintf(stderr, "[maps3d] assets=%ls host=%p\n", gAssets.c_str(), static_cast<void*>(gHostWnd));
    }
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, gUserData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT envHr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(envHr) || env == nullptr) {
                    std::lock_guard<std::mutex> lock(gMu);
                    gCreating = false;
                    setStatusLocked("OSM 3D: install the WebView2 runtime");
                    return S_OK;
                }
                env->CreateCoreWebView2Controller(
                    hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                              [](HRESULT ctrlHr, ICoreWebView2Controller* controller) -> HRESULT {
                                  if (FAILED(ctrlHr) || controller == nullptr) {
                                      std::lock_guard<std::mutex> lock(gMu);
                                      gCreating = false;
                                      setStatusLocked("OSM 3D: WebView2 controller failed");
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
        setStatusLocked("OSM 3D: WebView2 runtime missing");
    }
}

}  // namespace

namespace photon::ui {

void nativeHostAttach(void* hwnd) {
    std::lock_guard<std::mutex> lock(gMu);
    gHwnd = static_cast<HWND>(hwnd);
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    gUserData = std::wstring(tmp) + L"photon-ui-webview";
    gAssets = utf16(assetRoot());
}

void nativeHostShutdown() {
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
    gNavOk = false;
    gJsReady = false;
    gCreating = false;
    gTriedFile = false;
    gPendingJs.clear();
}

}  // namespace photon::ui

extern "C" {

void photon_maps3d_show(int on) {
    ensureCreated();
    std::lock_guard<std::mutex> lock(gMu);
    gWantShow = on != 0;
    applyBoundsLocked();
    execLocked(on ? L"window.photonMaps3d&&photonMaps3d.show(true)"
                  : L"window.photonMaps3d&&photonMaps3d.show(false)");
}

void photon_maps3d_layout(float x, float y, float w, float h) {
    ensureCreated();
    std::lock_guard<std::mutex> lock(gMu);
    gLogical[0] = static_cast<int>(x);
    gLogical[1] = static_cast<int>(y);
    gLogical[2] = static_cast<int>(w);
    gLogical[3] = static_cast<int>(h);
    applyBoundsLocked();
}

void photon_maps3d_start(double lat, double lng, const char* url) {
    ensureCreated();
    char js[512];
    std::snprintf(js, sizeof(js),
                  "window.photonMaps3d&&photonMaps3d.start(%0.8f,%0.8f,'%s')", lat, lng,
                  jsEscape(url != nullptr ? url : "models/newCar.glb").c_str());
    std::fprintf(stderr, "[maps3d] start lat=%.6f lng=%.6f nav=%d ready=%d\n", lat, lng, gNavOk ? 1 : 0,
                 gJsReady ? 1 : 0);
    std::fflush(stderr);
    std::lock_guard<std::mutex> lock(gMu);
    setStatusLocked("OSM 3D: loading tiles…");
    execLocked(utf16(js));
}

void photon_maps3d_pose(double lat, double lng, double heading, double scale, double range, double tilt,
                        int follow) {
    char js[384];
    std::snprintf(js, sizeof(js),
                  "window.photonMaps3d&&photonMaps3d.pose(%0.8f,%0.8f,%0.4f,%0.4f,%0.3f,%0.3f,%d)", lat, lng,
                  heading, scale, range, tilt, follow ? 1 : 0);
    static double sLat = 1e9;
    static double sLng = 1e9;
    if (std::fabs(lat - sLat) > 1e-4 || std::fabs(lng - sLng) > 1e-4) {
        sLat = lat;
        sLng = lng;
        std::fprintf(stderr, "[maps3d] pose lat=%.6f lng=%.6f ready=%d\n", lat, lng, gJsReady ? 1 : 0);
        std::fflush(stderr);
    }
    std::lock_guard<std::mutex> lock(gMu);
    execLocked(utf16(js));
}

int photon_maps3d_ready() {
    std::lock_guard<std::mutex> lock(gMu);
    return gJsReady ? 1 : 0;
}

int photon_maps3d_hosted() {
    std::lock_guard<std::mutex> lock(gMu);
    return (gNavOk || gJsReady) ? 1 : 0;
}

void photon_maps3d_status(char* buf, int n) {
    if (buf == nullptr || n <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(gMu);
    std::snprintf(buf, static_cast<std::size_t>(n), "%s", gStatus.c_str());
}

void photon_maps3d_reset_cam() {
    std::lock_guard<std::mutex> lock(gMu);
    execLocked(L"window.photonMaps3d&&photonMaps3d.resetCam()");
}

void photon_maps3d_interactive(int on) {
    char js[96];
    std::snprintf(js, sizeof(js), "window.photonMaps3d&&photonMaps3d.setInteractive(%d)", on ? 1 : 0);
    std::lock_guard<std::mutex> lock(gMu);
    execLocked(utf16(js));
}

}  // extern "C"

#endif
