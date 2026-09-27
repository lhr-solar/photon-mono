#ifndef __EMSCRIPTEN__

#include "photon_ui/overlay_host.hpp"

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
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace photon::ui {

struct OverlayPage::Impl {
    std::wstring windowClass;
    std::wstring profileLeaf;
    std::string label;
    bool devTools = false;

    std::mutex mu;
    HWND parent = nullptr;
    HWND host = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
    bool creating = false;
    bool classReady = false;
    bool wantShow = false;
    bool claimed = false;
    bool navOk = false;
    int logical[4] = {0, 0, 8, 8};
    std::wstring userData;
    std::wstring url;
    std::wstring loaded;
    std::string status;
    std::vector<std::wstring> pendingJs;
    PrepareFn prepare = nullptr;
    void* prepareUser = nullptr;
    NavigatedFn navigated = nullptr;
    void* navigatedUser = nullptr;

    void setStatusLocked(std::string text) { status = std::move(text); }

    HWND ensureHostLocked() {
        if (host != nullptr || parent == nullptr) {
            return host;
        }
        if (!classReady) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = windowClass.c_str();
            RegisterClassW(&wc);
            classReady = true;
        }
        // Owned popup, not a child of the SDL/OpenGL HWND. A child is painted
        // over by the GL swapchain.
        host = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass.c_str(), L"",
                               WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 8, 8, parent, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
        return host;
    }

    void applyBoundsLocked() {
        if (parent == nullptr || ensureHostLocked() == nullptr) {
            return;
        }
        const UINT dpi = GetDpiForWindow(parent);
        const float scale = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
        const int w = static_cast<int>(std::lround(static_cast<double>(logical[2]) * scale));
        const int h = static_cast<int>(std::lround(static_cast<double>(logical[3]) * scale));
        POINT origin{static_cast<LONG>(std::lround(static_cast<double>(logical[0]) * scale)),
                     static_cast<LONG>(std::lround(static_cast<double>(logical[1]) * scale))};
        ClientToScreen(parent, &origin);
        if (!wantShow || w < 8 || h < 8) {
            ShowWindow(host, SW_HIDE);
            if (controller != nullptr) {
                controller->put_IsVisible(FALSE);
            }
            return;
        }
        SetWindowPos(host, HWND_TOP, origin.x, origin.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(host, SW_SHOWNA);
        if (controller != nullptr) {
            RECT fill{0, 0, w, h};
            controller->put_Bounds(fill);
            controller->put_IsVisible(TRUE);
        }
    }

    // Caller holds mu. Returns the URL to Navigate after the lock is released.
    std::wstring takeNavigateLocked() {
        if (web == nullptr || url.empty() || url == loaded) {
            return {};
        }
        loaded = url;
        navOk = false;
        setStatusLocked(label + ": loading");
        return url;
    }

    void flushJsLocked() {
        if (web == nullptr || !navOk) {
            return;
        }
        for (const std::wstring& js : pendingJs) {
            web->ExecuteScript(js.c_str(), nullptr);
        }
        pendingJs.clear();
    }

    void finish(ICoreWebView2Controller* controllerIn) {
        ComPtr<ICoreWebView2> webview;
        if (controllerIn == nullptr || FAILED(controllerIn->get_CoreWebView2(&webview)) || webview == nullptr) {
            std::lock_guard<std::mutex> lock(mu);
            creating = false;
            setStatusLocked(label + ": WebView2 controller failed");
            return;
        }
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(webview->get_Settings(&settings)) && settings != nullptr) {
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_AreDevToolsEnabled(devTools ? TRUE : FALSE);
        }
        PrepareFn prep = nullptr;
        void* prepUser = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu);
            controller = controllerIn;
            web = webview;
            creating = false;
            prep = prepare;
            prepUser = prepareUser;
        }
        if (prep != nullptr) {
            prep(webview.Get(), controllerIn, prepUser);
        }
        webview->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    BOOL ok = FALSE;
                    if (args != nullptr) {
                        args->get_IsSuccess(&ok);
                    }
                    NavigatedFn fn = nullptr;
                    void* user = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(mu);
                        navOk = ok == TRUE;
                        if (navOk) {
                            flushJsLocked();
                        } else {
                            setStatusLocked(label + ": page failed to load");
                        }
                        fn = navigated;
                        user = navigatedUser;
                        applyBoundsLocked();
                    }
                    if (fn != nullptr) {
                        fn(sender, ok == TRUE, user);
                    }
                    return S_OK;
                })
                .Get(),
            nullptr);
        std::wstring open;
        ICoreWebView2* page = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu);
            page = web.Get();
            open = takeNavigateLocked();
            applyBoundsLocked();
        }
        if (page != nullptr && !open.empty()) {
            page->Navigate(open.c_str());
        }
    }

    void ensureCreated() {
        HWND hwnd = nullptr;
        std::wstring profile;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (controller != nullptr || creating || parent == nullptr) {
                return;
            }
            if (ensureHostLocked() == nullptr) {
                setStatusLocked(label + ": host window failed");
                return;
            }
            creating = true;
            hwnd = host;
            profile = userData;
            setStatusLocked(label + ": creating WebView2");
        }
        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, profile.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this, hwnd](HRESULT envHr, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(envHr) || env == nullptr) {
                        std::lock_guard<std::mutex> lock(mu);
                        creating = false;
                        setStatusLocked(label + ": install the WebView2 runtime");
                        return S_OK;
                    }
                    env->CreateCoreWebView2Controller(
                        hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                  [this](HRESULT ctrlHr, ICoreWebView2Controller* created) -> HRESULT {
                                      if (FAILED(ctrlHr) || created == nullptr) {
                                          std::lock_guard<std::mutex> lock(mu);
                                          creating = false;
                                          setStatusLocked(label + ": WebView2 controller failed");
                                          return S_OK;
                                      }
                                      finish(created);
                                      return S_OK;
                                  })
                                  .Get());
                    return S_OK;
                })
                .Get());
        if (FAILED(hr)) {
            std::lock_guard<std::mutex> lock(mu);
            creating = false;
            setStatusLocked(label + ": WebView2 runtime missing");
        }
    }
};

OverlayPage::OverlayPage(OverlayConfig cfg) : impl_(new Impl) {
    impl_->windowClass = cfg.windowClass != nullptr ? cfg.windowClass : L"PhotonOverlay";
    impl_->profileLeaf = cfg.profileLeaf != nullptr ? cfg.profileLeaf : L"photon-ui-overlay";
    impl_->label = cfg.label != nullptr ? cfg.label : "page";
    impl_->devTools = cfg.devTools;
    impl_->status = impl_->label + ": starting";
}

OverlayPage::~OverlayPage() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

void OverlayPage::attach(void* parentHwnd) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->parent = static_cast<HWND>(parentHwnd);
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    impl_->userData = std::wstring(tmp) + impl_->profileLeaf;
}

void OverlayPage::shutdown() {
    if (impl_ == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->controller != nullptr) {
        impl_->controller->Close();
    }
    impl_->web.Reset();
    impl_->controller.Reset();
    if (impl_->host != nullptr) {
        DestroyWindow(impl_->host);
        impl_->host = nullptr;
    }
    impl_->parent = nullptr;
    impl_->creating = false;
    impl_->wantShow = false;
    impl_->claimed = false;
    impl_->navOk = false;
    impl_->loaded.clear();
    impl_->pendingJs.clear();
}

void OverlayPage::show(bool on) {
    impl_->ensureCreated();
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->claimed = on;
    impl_->wantShow = on;
    impl_->applyBoundsLocked();
}

void OverlayPage::layout(float x, float y, float w, float h) {
    impl_->ensureCreated();
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->logical[0] = static_cast<int>(std::lround(x));
    impl_->logical[1] = static_cast<int>(std::lround(y));
    impl_->logical[2] = static_cast<int>(std::lround(w));
    impl_->logical[3] = static_cast<int>(std::lround(h));
    impl_->applyBoundsLocked();
}

void OverlayPage::navigate(const wchar_t* url) {
    impl_->ensureCreated();
    std::wstring open;
    ICoreWebView2* page = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        const std::wstring next = url != nullptr ? url : L"";
        if (next == impl_->url && next == impl_->loaded) {
            return;
        }
        impl_->url = next;
        impl_->loaded.clear();
        page = impl_->web.Get();
        open = impl_->takeNavigateLocked();
    }
    if (page != nullptr && !open.empty()) {
        page->Navigate(open.c_str());
    }
}

void OverlayPage::execute(const wchar_t* js) {
    if (js == nullptr || js[0] == L'\0') {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->web != nullptr && impl_->navOk) {
        impl_->web->ExecuteScript(js, nullptr);
        return;
    }
    impl_->pendingJs.emplace_back(js);
}

void OverlayPage::copyStatus(char* buf, int n) {
    if (buf == nullptr || n <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::snprintf(buf, static_cast<std::size_t>(n), "%s", impl_->status.c_str());
}

void OverlayPage::setStatus(const char* text) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->status = text != nullptr ? text : "";
}

bool OverlayPage::navigationOk() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->navOk;
}

void OverlayPage::beginFrame() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->claimed = false;
}

void OverlayPage::endFrame() {
    bool hide = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        hide = !impl_->claimed && impl_->wantShow;
    }
    if (hide) {
        show(false);
    }
}

void OverlayPage::setPrepare(PrepareFn fn, void* user) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->prepare = fn;
    impl_->prepareUser = user;
}

void OverlayPage::setNavigated(NavigatedFn fn, void* user) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->navigated = fn;
    impl_->navigatedUser = user;
}

}  // namespace photon::ui

#endif
