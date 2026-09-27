#ifndef __EMSCRIPTEN__

#include "photon_ui/overlay_host.hpp"
#include "photon_ui/platform.hpp"

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

using Microsoft::WRL::ComPtr;

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using Microsoft::WRL::Callback;

namespace {
photon::ui::OverlayPage gPage({L"PhotonMaps3dHost", L"photon-ui-webview", "OSM 3D", false});
std::wstring gAssets;
std::atomic<bool> gJsReady{false};
bool gTriedFile = false;

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

void prepareMaps(void* webviewPtr, void* controllerPtr, void*) {
    auto* webview = static_cast<ICoreWebView2*>(webviewPtr);
    auto* controller = static_cast<ICoreWebView2Controller*>(controllerPtr);
    if (webview == nullptr || controller == nullptr) {
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
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
    }
    ComPtr<ICoreWebView2_3> web3;
    if (SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&web3))) && web3 != nullptr && !gAssets.empty()) {
        const HRESULT mapHr = web3->SetVirtualHostNameToFolderMapping(
            L"photon.assets", gAssets.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        if (FAILED(mapHr)) {
            gPage.setStatus("OSM 3D: asset host mapping failed");
        }
    }
    webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
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
                gJsReady.store(ready);
                if (!status.empty()) {
                    gPage.setStatus(status.c_str());
                } else if (ready) {
                    gPage.setStatus("OSM 3D");
                }
                return S_OK;
            })
            .Get(),
        nullptr);
    std::fprintf(stderr, "[maps3d] assets=%ls\n", gAssets.c_str());
}

void mapsNavigated(void* webviewPtr, bool ok, void*) {
    if (ok) {
        gPage.setStatus("OSM 3D: loading tiles…");
        return;
    }
    gPage.setStatus("OSM 3D: maps3d.html failed to load");
    auto* webview = static_cast<ICoreWebView2*>(webviewPtr);
    if (gTriedFile || gAssets.empty() || webview == nullptr) {
        return;
    }
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
    gPage.navigate(file.c_str());
}

}  // namespace

namespace photon::ui {

void nativeHostAttach(void* hwnd) {
    gPage.setPrepare(prepareMaps, nullptr);
    gPage.setNavigated(mapsNavigated, nullptr);
    gPage.attach(hwnd);
    gAssets = utf16(assetRoot());
    gPage.navigate(L"https://photon.assets/maps3d.html");
}

void nativeHostShutdown() {
    gPage.shutdown();
    gJsReady.store(false);
    gTriedFile = false;
}

}  // namespace photon::ui

extern "C" {

void photon_maps3d_show(int on) {
    gPage.show(on != 0);
    gPage.execute(on ? L"window.photonMaps3d&&photonMaps3d.show(true)"
                     : L"window.photonMaps3d&&photonMaps3d.show(false)");
}

void photon_maps3d_layout(float x, float y, float w, float h) { gPage.layout(x, y, w, h); }

void photon_maps3d_start(double lat, double lng, const char* url) {
    char js[512];
    std::snprintf(js, sizeof(js), "window.photonMaps3d&&photonMaps3d.start(%0.8f,%0.8f,'%s')", lat, lng,
                  jsEscape(url != nullptr ? url : "models/newCar.glb").c_str());
    std::fprintf(stderr, "[maps3d] start lat=%.6f lng=%.6f nav=%d ready=%d\n", lat, lng,
                 gPage.navigationOk() ? 1 : 0, gJsReady.load() ? 1 : 0);
    std::fflush(stderr);
    gPage.setStatus("OSM 3D: loading tiles…");
    gPage.execute(utf16(js).c_str());
}

void photon_maps3d_pose(double lat, double lng, double heading, double scale, double range, double tilt,
                        int follow) {
    char js[384];
    std::snprintf(js, sizeof(js),
                  "window.photonMaps3d&&photonMaps3d.pose(%0.8f,%0.8f,%0.4f,%0.4f,%0.3f,%0.3f,%d)", lat, lng, heading,
                  scale, range, tilt, follow ? 1 : 0);
    static double sLat = 1e9;
    static double sLng = 1e9;
    if (std::fabs(lat - sLat) > 1e-4 || std::fabs(lng - sLng) > 1e-4) {
        sLat = lat;
        sLng = lng;
        std::fprintf(stderr, "[maps3d] pose lat=%.6f lng=%.6f ready=%d\n", lat, lng, gJsReady.load() ? 1 : 0);
        std::fflush(stderr);
    }
    gPage.execute(utf16(js).c_str());
}

int photon_maps3d_ready() { return gJsReady.load() ? 1 : 0; }

int photon_maps3d_hosted() { return (gPage.navigationOk() || gJsReady.load()) ? 1 : 0; }

void photon_maps3d_status(char* buf, int n) { gPage.copyStatus(buf, n); }

void photon_maps3d_reset_cam() { gPage.execute(L"window.photonMaps3d&&photonMaps3d.resetCam()"); }

void photon_maps3d_interactive(int on) {
    char js[96];
    std::snprintf(js, sizeof(js), "window.photonMaps3d&&photonMaps3d.setInteractive(%d)", on ? 1 : 0);
    gPage.execute(utf16(js).c_str());
}

}  // extern "C"

#endif
