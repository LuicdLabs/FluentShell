#include "IslandHost.h"
#include "../Renderer.Dwm/DwmRenderer.h"

#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#if !defined(FLUENTSHELL_WASDK_SELF_CONTAINED)
#include <MddBootstrap.h>
#include <WindowsAppSDK-VersionInfo.h>
#endif

// Storyboard::GetCurrentTime collides with winbase.h macro.
#undef GetCurrentTime

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Composition.h>

#include <Microsoft.UI.Dispatching.Interop.h>

#if !defined(FLUENTSHELL_WASDK_SELF_CONTAINED)
#pragma comment(lib, "Microsoft.WindowsAppRuntime.Bootstrap.lib")
#endif

namespace FluentShell::Island {
namespace {

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxh = winrt::Microsoft::UI::Xaml::Hosting;
namespace muc = winrt::Microsoft::UI::Content;
namespace mud = winrt::Microsoft::UI::Dispatching;
namespace mui = winrt::Microsoft::UI;

std::atomic<int> g_bootstrapRef{ 0 };
std::mutex g_bootstrapMutex;
std::mutex g_runtimeStartMutex;

winrt::Windows::UI::Color Argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return { a, r, g, b };
}

void LogHr(const wchar_t* where, const winrt::hresult_error& e) {
    Log(std::wstring(where) + L" hr=0x" +
        [&] {
            wchar_t buf[16]{};
            swprintf_s(buf, L"%08X", static_cast<unsigned>(e.code().value));
            return std::wstring(buf);
        }() +
        L" " + std::wstring(e.message()));
}

class ScopedModuleActivationContext {
public:
    ScopedModuleActivationContext() = default;
    ~ScopedModuleActivationContext() {
        if (active_) DeactivateActCtx(0, cookie_);
        if (handle_ != INVALID_HANDLE_VALUE) ReleaseActCtx(handle_);
    }

    ScopedModuleActivationContext(const ScopedModuleActivationContext&) = delete;
    ScopedModuleActivationContext& operator=(const ScopedModuleActivationContext&) = delete;

    bool Activate() {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ScopedModuleActivationContext::AddressAnchor),
                &module) || !module) {
            Log(L"Activation context: owning module not found");
            return false;
        }

        // Executables embed their manifest as resource #1; DLLs use #2.
        WORD resourceId = 0;
        if (FindResourceW(module, MAKEINTRESOURCEW(2), RT_MANIFEST)) resourceId = 2;
        else if (FindResourceW(module, MAKEINTRESOURCEW(1), RT_MANIFEST)) resourceId = 1;
        if (!resourceId) {
            Log(L"Activation context: embedded manifest not found");
            return false;
        }

        ACTCTXW context{ sizeof(context) };
        context.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_HMODULE_VALID;
        context.lpResourceName = MAKEINTRESOURCEW(resourceId);
        context.hModule = module;
        handle_ = CreateActCtxW(&context);
        if (handle_ == INVALID_HANDLE_VALUE) {
            Log(L"Activation context: CreateActCtx failed error=" +
                std::to_wstring(GetLastError()));
            return false;
        }
        if (!ActivateActCtx(handle_, &cookie_)) {
            Log(L"Activation context: ActivateActCtx failed error=" +
                std::to_wstring(GetLastError()));
            ReleaseActCtx(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            return false;
        }
        active_ = true;
        Log(std::wstring(L"WinAppSDK activation context active (manifest #") +
            std::to_wstring(resourceId) + L")");
        return true;
    }

private:
    static void AddressAnchor() {}

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    ULONG_PTR cookie_ = 0;
    bool active_ = false;
};

// ---------- Application + control styles (critical for stable control rendering) ----------

struct XamlAppHolder {
    mux::Application app{ nullptr };
    muxh::WindowsXamlManager manager{ nullptr };
};

// Keep one Application per process (WinUI expects a singleton Application.Current).
XamlAppHolder& AppHolder() {
    static XamlAppHolder holder;
    return holder;
}

bool EnsureXamlApplication(bool dark) {
    auto& h = AppHolder();
    try {
        if (!h.manager) {
            h.manager = muxh::WindowsXamlManager::InitializeForCurrentThread();
            Log(L"WindowsXamlManager initialized");
        }

        if (mux::Application::Current()) {
            h.app = mux::Application::Current();
            Log(L"Reusing existing Application.Current");
        } else if (!h.app) {
            // Construct Application AFTER WindowsXamlManager (island hosting requirement).
            h.app = mux::Application{};
            Log(L"Application created");
        }

        // Theme dictionary is optional. Unpackaged self-contained often cannot resolve
        // ms-appx:///Microsoft.UI.Xaml/Themes/themeresources.xaml - we paint with
        // explicit brushes instead, so this failure is non-fatal.
        try {
            auto resources = h.app.Resources();
            if (!resources) {
                resources = mux::ResourceDictionary{};
                h.app.Resources(resources);
            }
            bool hasControls = false;
            for (auto const& merged : resources.MergedDictionaries()) {
                if (merged.try_as<muxc::XamlControlsResources>()) {
                    hasControls = true;
                    break;
                }
            }
            if (!hasControls) {
                resources.MergedDictionaries().Append(muxc::XamlControlsResources{});
                Log(L"XamlControlsResources merged");
            }
        } catch (const winrt::hresult_error& e) {
            LogHr(L"XamlControlsResources unavailable - using explicit brushes", e);
        }

        try {
            h.app.RequestedTheme(dark ? mux::ApplicationTheme::Dark : mux::ApplicationTheme::Light);
        } catch (...) {
        }
        return true;
    } catch (const winrt::hresult_error& e) {
        LogHr(L"EnsureXamlApplication", e);
        return false;
    } catch (...) {
        Log(L"EnsureXamlApplication unknown failure");
        return false;
    }
}

muxc::Border BuildDialogVisual(const DialogModel& model, std::function<void(DialogResult)> onComplete) {
    // Outer border gives solid fill + rounded clip that always paints even if control templates fail.
    muxc::Border chrome;
    chrome.CornerRadius({ 8, 8, 8, 8 });
    chrome.Background(mux::Media::SolidColorBrush(model.dark ? Argb(255, 32, 32, 32) : Argb(255, 243, 243, 243)));
    chrome.BorderBrush(mux::Media::SolidColorBrush(model.dark ? Argb(255, 64, 64, 64) : Argb(255, 200, 200, 200)));
    chrome.BorderThickness({ 1, 1, 1, 1 });
    chrome.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    chrome.VerticalAlignment(mux::VerticalAlignment::Stretch);

    muxc::Grid root;
    // Row0 content, Row1 buttons
    muxc::RowDefinition row0;
    row0.Height({ 1, mux::GridUnitType::Star });
    muxc::RowDefinition row1;
    row1.Height({ 1, mux::GridUnitType::Auto });
    root.RowDefinitions().Append(row0);
    root.RowDefinitions().Append(row1);

    muxc::StackPanel body;
    body.Spacing(12.0);
    body.Padding({ 24, 24, 24, 12 });
    body.HorizontalAlignment(mux::HorizontalAlignment::Stretch);

    auto fg = mux::Media::SolidColorBrush(model.dark ? Argb(255, 255, 255, 255) : Argb(255, 0, 0, 0));
    auto secondary = mux::Media::SolidColorBrush(model.dark ? Argb(255, 200, 200, 200) : Argb(255, 80, 80, 80));

    if (model.icon != DialogIcon::None) {
        muxc::FontIcon icon;
        icon.FontSize(30.0);
        icon.HorizontalAlignment(mux::HorizontalAlignment::Left);
        switch (model.icon) {
        case DialogIcon::Information:
            icon.Glyph(L"\xE946");
            icon.Foreground(mux::Media::SolidColorBrush(Argb(255, 0, 120, 212)));
            break;
        case DialogIcon::Warning:
            icon.Glyph(L"\xE7BA");
            icon.Foreground(mux::Media::SolidColorBrush(Argb(255, 255, 185, 0)));
            break;
        case DialogIcon::Error:
            icon.Glyph(L"\xEA39");
            icon.Foreground(mux::Media::SolidColorBrush(Argb(255, 232, 17, 35)));
            break;
        case DialogIcon::Question:
            icon.Glyph(L"\xE897");
            icon.Foreground(mux::Media::SolidColorBrush(Argb(255, 0, 120, 212)));
            break;
        default:
            break;
        }
        body.Children().Append(icon);
    }

    if (!model.mainInstruction.empty()) {
        muxc::TextBlock title;
        title.Text(model.mainInstruction);
        title.FontSize(20.0);
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        title.Foreground(fg);
        title.TextWrapping(mux::TextWrapping::Wrap);
        body.Children().Append(title);
    }

    if (!model.content.empty()) {
        muxc::TextBlock content;
        content.Text(model.content);
        content.FontSize(14.0);
        content.Foreground(secondary);
        content.TextWrapping(mux::TextWrapping::Wrap);
        body.Children().Append(content);
    }

    muxc::CheckBox verify;
    const bool hasVerify = !model.verificationText.empty();
    if (hasVerify) {
        verify.Content(winrt::box_value(model.verificationText));
        verify.Foreground(fg);
        verify.IsChecked(model.verificationChecked);
        body.Children().Append(verify);
    }

    if (!model.footer.empty()) {
        muxc::TextBlock footer;
        footer.Text(model.footer);
        footer.FontSize(12.0);
        footer.Foreground(secondary);
        footer.TextWrapping(mux::TextWrapping::Wrap);
        body.Children().Append(footer);
    }

    muxc::Grid::SetRow(body, 0);
    root.Children().Append(body);

    muxc::StackPanel buttons;
    buttons.Orientation(muxc::Orientation::Horizontal);
    buttons.Spacing(8.0);
    buttons.HorizontalAlignment(mux::HorizontalAlignment::Right);
    buttons.Padding({ 24, 8, 24, 24 });

    auto complete = std::make_shared<std::function<void(DialogResult)>>(std::move(onComplete));

    for (int i = 0; i < static_cast<int>(model.buttons.size()); ++i) {
        muxc::Button btn;
        btn.Content(winrt::box_value(model.buttons[static_cast<size_t>(i)]));
        btn.MinWidth(88.0);
        btn.MinHeight(32.0);
        btn.CornerRadius({ 4, 4, 4, 4 });
        btn.Padding({ 12, 6, 12, 6 });

        if (i == model.defaultButtonIndex) {
            btn.Background(mux::Media::SolidColorBrush(Argb(255, 0, 120, 212)));
            btn.Foreground(mux::Media::SolidColorBrush(Argb(255, 255, 255, 255)));
        } else {
            btn.Background(mux::Media::SolidColorBrush(model.dark ? Argb(255, 45, 45, 45) : Argb(255, 225, 225, 225)));
            btn.Foreground(fg);
        }

        const int index = i;
        btn.Click([complete, index, verify, hasVerify](auto const&, auto const&) {
            DialogResult r;
            r.buttonIndex = index;
            if (hasVerify) {
                auto st = verify.IsChecked();
                r.verificationChecked = st && st.Value();
            }
            if (*complete) {
                (*complete)(r);
            }
        });
        buttons.Children().Append(btn);
    }

    muxc::Grid::SetRow(buttons, 1);
    root.Children().Append(buttons);
    chrome.Child(root);
    return chrome;
}

muxc::Grid BuildSmokeVisual(std::wstring_view label) {
    // Full-bleed grid + rectangle guarantees paint even if control templates fail.
    muxc::Grid root;
    root.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    root.VerticalAlignment(mux::VerticalAlignment::Stretch);

    muxc::Border fill;
    fill.Background(mux::Media::SolidColorBrush(Argb(255, 28, 28, 28)));
    fill.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    fill.VerticalAlignment(mux::VerticalAlignment::Stretch);
    root.Children().Append(fill);

    muxc::StackPanel panel;
    panel.Spacing(16.0);
    panel.Padding({ 24, 24, 24, 24 });
    panel.HorizontalAlignment(mux::HorizontalAlignment::Center);
    panel.VerticalAlignment(mux::VerticalAlignment::Center);
    panel.Background(mux::Media::SolidColorBrush(Argb(255, 40, 40, 40)));

    muxc::TextBlock tb;
    tb.Text(std::wstring(label));
    tb.Foreground(mux::Media::SolidColorBrush(Argb(255, 255, 255, 255)));
    tb.FontSize(20.0);
    tb.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    tb.TextWrapping(mux::TextWrapping::Wrap);
    tb.HorizontalAlignment(mux::HorizontalAlignment::Center);
    panel.Children().Append(tb);

    muxc::TextBlock sub;
    sub.Text(L"If you can read this, XAML composition is painting.");
    sub.Foreground(mux::Media::SolidColorBrush(Argb(255, 200, 200, 200)));
    sub.FontSize(14.0);
    sub.HorizontalAlignment(mux::HorizontalAlignment::Center);
    panel.Children().Append(sub);

    // Prefer Border-as-button look so we don't depend on Button control template.
    muxc::Border btn;
    btn.Background(mux::Media::SolidColorBrush(Argb(255, 0, 120, 212)));
    btn.CornerRadius({ 4, 4, 4, 4 });
    btn.Padding({ 16, 10, 16, 10 });
    btn.HorizontalAlignment(mux::HorizontalAlignment::Center);
    muxc::TextBlock btnText;
    btnText.Text(L"Fluent Button");
    btnText.Foreground(mux::Media::SolidColorBrush(Argb(255, 255, 255, 255)));
    btnText.FontSize(14.0);
    btn.Child(btnText);
    btn.Tapped([](auto&&, auto&&) { Log(L"Smoke-test button tapped"); });
    panel.Children().Append(btn);

    root.Children().Append(panel);
    return root;
}

} // namespace

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

std::wstring ModuleDirectory() {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ModuleDirectory),
            &mod)) {
        mod = GetModuleHandleW(nullptr);
    }
    wchar_t path[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(mod, path, static_cast<DWORD>(std::size(path)));
    if (!n) return {};
    std::wstring full(path, n);
    const auto slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? full : full.substr(0, slash);
}

bool BootstrapWindowsAppSdk() {
    std::lock_guard lock(g_bootstrapMutex);
    if (g_bootstrapRef > 0) {
        ++g_bootstrapRef;
        return true;
    }

#if defined(FLUENTSHELL_WASDK_SELF_CONTAINED)
    // Self-contained: use an absolute path and a call-scoped dependency search.
    // Never mutate the injected process's global DLL search policy.
    const auto dir = ModuleDirectory();
    std::wstring xamlPath = dir + L"\\Microsoft.UI.Xaml.dll";
    HMODULE xaml = LoadLibraryExW(
        xamlPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!xaml) {
        Log(L"Self-contained Microsoft.UI.Xaml.dll not found next to module");
        return false;
    }
    g_bootstrapRef = 1;
    Log(L"Self-contained WinUI ready (no MddBootstrap)");
    return true;
#else
    const PACKAGE_VERSION minVersion{};
    HRESULT hr = MddBootstrapInitialize2(
        WINDOWSAPPSDK_RELEASE_MAJORMINOR,
        WINDOWSAPPSDK_RELEASE_VERSION_TAG_W,
        minVersion,
        MddBootstrapInitializeOptions_None);
    if (FAILED(hr)) {
        hr = MddBootstrapInitialize(
            WINDOWSAPPSDK_RELEASE_MAJORMINOR,
            WINDOWSAPPSDK_RELEASE_VERSION_TAG_W,
            minVersion);
    }
    if (FAILED(hr)) {
        wchar_t buf[64]{};
        swprintf_s(buf, L"MddBootstrapInitialize failed hr=0x%08X", static_cast<unsigned>(hr));
        Log(buf);
        return false;
    }
    g_bootstrapRef = 1;
    Log(L"WinAppSDK framework bootstrap OK");
    return true;
#endif
}

void ShutdownWindowsAppSdk() {
    std::lock_guard lock(g_bootstrapMutex);
    if (g_bootstrapRef <= 0) {
        return;
    }
    if (--g_bootstrapRef == 0) {
#if !defined(FLUENTSHELL_WASDK_SELF_CONTAINED)
        MddBootstrapShutdown();
        Log(L"WinAppSDK bootstrap shutdown");
#else
        Log(L"Self-contained WinUI shutdown");
#endif
    }
}

// ---------------------------------------------------------------------------
// Process-wide XAML runtime (dedicated STA)
// ---------------------------------------------------------------------------

struct XamlRuntime::State {
    std::mutex mu;
    std::condition_variable cv;
    std::thread thread;
    DWORD threadId = 0;
    bool started = false;
    bool startOk = false;
    bool stop = false;

    mud::DispatcherQueueController dqController{ nullptr };
    mud::DispatcherQueue dispatcher{ nullptr };
};

XamlRuntime& XamlRuntime::Instance() {
    static XamlRuntime rt;
    return rt;
}

bool XamlRuntime::IsRunning() const {
    return state_ && state_->started && state_->startOk;
}

DWORD XamlRuntime::XamlThreadId() const {
    return state_ ? state_->threadId : 0;
}

bool XamlRuntime::EnsureStarted() {
    // Bridge prewarming and a newly-created dialog may race to start the STA.
    // Serialize the whole one-time startup, including the readiness wait.
    std::unique_lock startLock(g_runtimeStartMutex);
    if (IsRunning()) {
        return true;
    }

    if (!state_) {
        state_ = std::make_unique<State>();
    }

    {
        std::unique_lock lock(state_->mu);
        if (state_->started) {
            return state_->startOk;
        }

        state_->thread = std::thread([this] {
            state_->threadId = GetCurrentThreadId();
            ScopedModuleActivationContext activationContext;

            // Dedicated message-only window keeps GetMessage alive if needed.
            try {
                if (!activationContext.Activate()) {
                    std::lock_guard g(state_->mu);
                    state_->started = true;
                    state_->startOk = false;
                    state_->cv.notify_all();
                    return;
                }
                if (!BootstrapWindowsAppSdk()) {
                    std::lock_guard g(state_->mu);
                    state_->started = true;
                    state_->startOk = false;
                    state_->cv.notify_all();
                    return;
                }

                winrt::init_apartment(winrt::apartment_type::single_threaded);
                state_->dqController = mud::DispatcherQueueController::CreateOnCurrentThread();
                state_->dispatcher = state_->dqController.DispatcherQueue();

                if (!EnsureXamlApplication(true)) {
                    std::lock_guard g(state_->mu);
                    state_->started = true;
                    state_->startOk = false;
                    state_->cv.notify_all();
                    return;
                }

                {
                    std::lock_guard g(state_->mu);
                    state_->started = true;
                    state_->startOk = true;
                }
                state_->cv.notify_all();
                Log(L"XamlRuntime STA thread ready");

                // Pump: ContentPreTranslateMessage is mandatory for island input/focus.
                MSG msg{};
                while (!state_->stop) {
                    const BOOL gm = GetMessageW(&msg, nullptr, 0, 0);
                    if (gm <= 0) {
                        break;
                    }
                    if (ContentPreTranslateMessage(&msg)) {
                        continue;
                    }
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                Log(L"XamlRuntime STA thread exiting");
            } catch (const winrt::hresult_error& e) {
                LogHr(L"XamlRuntime thread", e);
                std::lock_guard g(state_->mu);
                state_->started = true;
                state_->startOk = false;
                state_->cv.notify_all();
            } catch (...) {
                Log(L"XamlRuntime thread unknown failure");
                std::lock_guard g(state_->mu);
                state_->started = true;
                state_->startOk = false;
                state_->cv.notify_all();
            }
        });
    }

    std::unique_lock lock(state_->mu);
    state_->cv.wait(lock, [&] { return state_->started; });
    return state_->startOk;
}

bool XamlRuntime::RunSync(const std::function<void()>& work) {
    if (!EnsureStarted() || !work) {
        return false;
    }

    if (GetCurrentThreadId() == state_->threadId) {
        try {
            work();
            return true;
        } catch (const winrt::hresult_error& e) {
            LogHr(L"RunSync inline", e);
            return false;
        }
    }

    // CRITICAL: caller may be a UI thread (MessageBox/TaskDialog hook).
    // Blocking without pumping causes "Not Responding" and hangs the target.
    std::atomic<bool> done{ false };
    std::atomic<bool> ok{ false };

    const bool enqueued = state_->dispatcher.TryEnqueue(
        mud::DispatcherQueuePriority::Normal,
        [&] {
            try {
                work();
                ok.store(true);
            } catch (const winrt::hresult_error& e) {
                LogHr(L"RunSync queued", e);
                ok.store(false);
            } catch (...) {
                Log(L"RunSync queued unknown failure");
                ok.store(false);
            }
            done.store(true);
        });

    if (!enqueued) {
        Log(L"DispatcherQueue.TryEnqueue failed");
        return false;
    }

    // Pump THIS thread while waiting for XAML STA work (avoids freeze).
    const ULONGLONG deadline = GetTickCount64() + 120000; // 2 min safety
    MSG msg{};
    while (!done.load()) {
        if (GetTickCount64() > deadline) {
            Log(L"RunSync timed out");
            return false;
        }
        // Process any messages for the calling UI thread.
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                // Don't swallow quit permanently - re-post after dialog.
                PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (done.load()) {
            break;
        }
        // Wake when any input/paint arrives, or after 16ms.
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
    }
    return ok.load();
}

bool XamlRuntime::RunAsync(std::function<void()> work) {
    if (!EnsureStarted() || !work) {
        return false;
    }
    return state_->dispatcher.TryEnqueue(mud::DispatcherQueuePriority::Normal, [w = std::move(work)] {
        try {
            w();
        } catch (const winrt::hresult_error& e) {
            LogHr(L"RunAsync", e);
        } catch (...) {
            Log(L"RunAsync unknown failure");
        }
    });
}

void XamlRuntime::Shutdown() {
    if (!state_) {
        return;
    }
    state_->stop = true;
    if (state_->dispatcher) {
        // Wake the pump.
        PostThreadMessageW(state_->threadId, WM_QUIT, 0, 0);
    }
    if (state_->thread.joinable()) {
        state_->thread.join();
    }
    try {
        if (state_->dqController) {
            state_->dqController.ShutdownQueue();
            state_->dqController = nullptr;
        }
    } catch (...) {
    }
    state_.reset();
    ShutdownWindowsAppSdk();
}

// ---------------------------------------------------------------------------
// Island host
// ---------------------------------------------------------------------------

namespace {

constexpr UINT kDialogProbeTimeoutMs = 250;

struct LegacyButtonTarget {
    HWND hwnd = nullptr;
    int id = 0;
    RECT bounds{};
};

struct LegacyChildState {
    HWND hwnd = nullptr;
    bool visible = false;
};

struct LegacyDialogState {
    HWND dialog = nullptr;
    std::vector<LegacyButtonTarget> buttons;
    HWND verification = nullptr;
    int verificationId = 0;
    bool verificationChecked = false;
    std::vector<LegacyChildState> children;
};

struct CapturedDialog {
    DialogModel model;
    std::shared_ptr<LegacyDialogState> state;
    bool supported = true;
    std::wstring unsupportedClass;
};

bool TrySendMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, DWORD_PTR& result) {
    result = 0;
    return hwnd && SendMessageTimeoutW(
        hwnd, message, wParam, lParam,
        SMTO_ABORTIFHUNG | SMTO_BLOCK, kDialogProbeTimeoutMs, &result) != 0;
}

std::wstring ReadWindowText(HWND hwnd) {
    DWORD_PTR rawLength = 0;
    if (!TrySendMessage(hwnd, WM_GETTEXTLENGTH, 0, 0, rawLength) || rawLength > 32768) {
        return {};
    }

    std::wstring text(static_cast<size_t>(rawLength) + 1, L'\0');
    DWORD_PTR copied = 0;
    if (!TrySendMessage(
            hwnd, WM_GETTEXT, static_cast<WPARAM>(text.size()),
            reinterpret_cast<LPARAM>(text.data()), copied)) {
        return {};
    }
    text.resize(std::min(static_cast<size_t>(copied), text.size() - 1));
    return text;
}

std::wstring WindowClass(HWND hwnd) {
    wchar_t name[128]{};
    const int length = GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
    return length > 0 ? std::wstring(name, static_cast<size_t>(length)) : std::wstring{};
}

std::wstring NormalizeControlText(std::wstring text) {
    std::wstring normalized;
    normalized.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != L'&') {
            normalized.push_back(text[i]);
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == L'&') {
            normalized.push_back(L'&');
            ++i;
        }
    }

    const auto first = std::find_if_not(normalized.begin(), normalized.end(), iswspace);
    const auto last = std::find_if_not(normalized.rbegin(), normalized.rend(), iswspace).base();
    if (first >= last) {
        return {};
    }
    return std::wstring(first, last);
}

DialogIcon IdentifyDialogIcon(HWND iconControl) {
    DWORD_PTR rawIcon = 0;
    if (!TrySendMessage(iconControl, STM_GETICON, 0, 0, rawIcon) || !rawIcon) {
        return DialogIcon::None;
    }

    const auto icon = reinterpret_cast<HICON>(rawIcon);
    if (icon == LoadIconW(nullptr, IDI_INFORMATION)) return DialogIcon::Information;
    if (icon == LoadIconW(nullptr, IDI_WARNING)) return DialogIcon::Warning;
    if (icon == LoadIconW(nullptr, IDI_ERROR)) return DialogIcon::Error;
    if (icon == LoadIconW(nullptr, IDI_QUESTION)) return DialogIcon::Question;
    return DialogIcon::None;
}

struct CaptureContext {
    HWND dialog = nullptr;
    CapturedDialog* capture = nullptr;
    std::vector<std::pair<RECT, std::wstring>> text;
};

BOOL CALLBACK CaptureDialogChild(HWND child, LPARAM contextValue) {
    auto& context = *reinterpret_cast<CaptureContext*>(contextValue);
    if (GetParent(child) != context.dialog || !IsWindowVisible(child)) {
        return TRUE;
    }

    auto& capture = *context.capture;
    capture.state->children.push_back({ child, true });

    const auto className = WindowClass(child);
    const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
    RECT bounds{};
    GetWindowRect(child, &bounds);
    MapWindowPoints(HWND_DESKTOP, context.dialog, reinterpret_cast<POINT*>(&bounds), 2);

    if (EqualsIgnoreCase(className, L"Static")) {
        const auto type = static_cast<DWORD>(style) & SS_TYPEMASK;
        if (type == SS_ICON) {
            const auto icon = IdentifyDialogIcon(child);
            if (icon != DialogIcon::None) capture.model.icon = icon;
            return TRUE;
        }
        if (type == SS_BITMAP || type == SS_OWNERDRAW) {
            capture.supported = false;
            capture.unsupportedClass = L"Static(image/owner-draw)";
            return FALSE;
        }

        auto text = NormalizeControlText(ReadWindowText(child));
        if (!text.empty()) context.text.emplace_back(bounds, std::move(text));
        return TRUE;
    }

    if (EqualsIgnoreCase(className, L"Button")) {
        const auto type = static_cast<DWORD>(style) & BS_TYPEMASK;
        const bool pushButton = type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON ||
            type == BS_SPLITBUTTON || type == BS_DEFSPLITBUTTON ||
            type == BS_COMMANDLINK || type == BS_DEFCOMMANDLINK;
        const bool checkBox = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX ||
            type == BS_3STATE || type == BS_AUTO3STATE;

        if (pushButton) {
            auto text = NormalizeControlText(ReadWindowText(child));
            if (text.empty()) {
                capture.supported = false;
                capture.unsupportedClass = L"Button(without text)";
                return FALSE;
            }
            capture.state->buttons.push_back({ child, GetDlgCtrlID(child), bounds });
            capture.model.buttons.push_back(std::move(text));
            return TRUE;
        }

        if (checkBox && !capture.state->verification) {
            capture.state->verification = child;
            capture.state->verificationId = GetDlgCtrlID(child);
            capture.model.verificationText = NormalizeControlText(ReadWindowText(child));
            DWORD_PTR checked = BST_UNCHECKED;
            if (TrySendMessage(child, BM_GETCHECK, 0, 0, checked)) {
                capture.model.verificationChecked = checked == BST_CHECKED;
                capture.state->verificationChecked = capture.model.verificationChecked;
            }
            return TRUE;
        }

        capture.supported = false;
        capture.unsupportedClass = L"Button(custom/radio/group)";
        return FALSE;
    }

    // Empty framework helper windows do not affect the dialog contract. Any visible
    // control carrying content or input must remain native until an adapter exists.
    if (ReadWindowText(child).empty() && !IsWindowEnabled(child)) {
        return TRUE;
    }
    capture.supported = false;
    capture.unsupportedClass = className.empty() ? L"unknown" : className;
    return FALSE;
}

CapturedDialog CaptureSimpleDialog(HWND dialog, bool dark) {
    CapturedDialog capture;
    capture.state = std::make_shared<LegacyDialogState>();
    capture.state->dialog = dialog;
    capture.model.dark = dark;
    capture.model.title = NormalizeControlText(ReadWindowText(dialog));

    CaptureContext context{ dialog, &capture };
    EnumChildWindows(dialog, CaptureDialogChild, reinterpret_cast<LPARAM>(&context));
    if (!capture.supported) return capture;

    const bool rtl = (GetWindowLongPtrW(dialog, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;
    std::sort(context.text.begin(), context.text.end(), [rtl](auto const& left, auto const& right) {
        if (std::abs(left.first.top - right.first.top) > 6) return left.first.top < right.first.top;
        return rtl ? left.first.left > right.first.left : left.first.left < right.first.left;
    });
    std::sort(capture.state->buttons.begin(), capture.state->buttons.end(), [rtl](auto const& left, auto const& right) {
        if (std::abs(left.bounds.top - right.bounds.top) > 6) return left.bounds.top < right.bounds.top;
        return rtl ? left.bounds.left > right.bounds.left : left.bounds.left < right.bounds.left;
    });

    // Keep labels aligned with the sorted native button targets.
    capture.model.buttons.clear();
    for (auto const& button : capture.state->buttons) {
        capture.model.buttons.push_back(NormalizeControlText(ReadWindowText(button.hwnd)));
    }

    if (!context.text.empty()) {
        capture.model.mainInstruction = context.text.front().second;
        for (size_t i = 1; i < context.text.size(); ++i) {
            if (!capture.model.content.empty()) capture.model.content += L"\n\n";
            capture.model.content += context.text[i].second;
        }
    }

    DWORD_PTR defaultIdResult = 0;
    int defaultId = 0;
    if (TrySendMessage(dialog, DM_GETDEFID, 0, 0, defaultIdResult) &&
        HIWORD(defaultIdResult) == DC_HASDEFID) {
        defaultId = LOWORD(defaultIdResult);
    }

    capture.model.defaultButtonIndex = 0;
    capture.model.cancelButtonIndex = -1;
    for (size_t i = 0; i < capture.state->buttons.size(); ++i) {
        const auto& button = capture.state->buttons[i];
        const auto type = static_cast<DWORD>(GetWindowLongPtrW(button.hwnd, GWL_STYLE)) & BS_TYPEMASK;
        if ((defaultId && button.id == defaultId) || (!defaultId && type == BS_DEFPUSHBUTTON)) {
            capture.model.defaultButtonIndex = static_cast<int>(i);
        }
        if (button.id == IDCANCEL) {
            capture.model.cancelButtonIndex = static_cast<int>(i);
        }
    }

    if (capture.state->buttons.empty()) {
        capture.supported = false;
        capture.unsupportedClass = L"dialog without push buttons";
    }
    return capture;
}

void SetLegacyChildrenVisible(const std::shared_ptr<LegacyDialogState>& state, bool visible) {
    if (!state) return;
    for (const auto& child : state->children) {
        if (!child.hwnd || !IsWindow(child.hwnd) || GetParent(child.hwnd) != state->dialog) continue;
        const UINT showFlag = visible && child.visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
        UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | showFlag;
        DWORD ownerProcess = 0;
        const DWORD ownerThread = GetWindowThreadProcessId(child.hwnd, &ownerProcess);
        if (ownerThread && ownerThread != GetCurrentThreadId()) flags |= SWP_ASYNCWINDOWPOS;
        SetWindowPos(child.hwnd, nullptr, 0, 0, 0, 0, flags);
    }
}

struct OverlayWindowState {
    HWND target = nullptr;
};

constexpr UINT_PTR kOverlayTimer = 0x5F17;

void SyncOverlayWindow(HWND overlay, OverlayWindowState* state) {
    if (!overlay || !state) return;
    if (!state->target || !IsWindow(state->target)) {
        KillTimer(overlay, kOverlayTimer);
        ShowWindow(overlay, SW_HIDE);
        ReleaseExistingDialogAsync(state->target);
        return;
    }

    if (!IsWindowVisible(state->target) || IsIconic(state->target)) {
        ShowWindow(overlay, SW_HIDE);
        return;
    }

    RECT client{};
    GetClientRect(state->target, &client);
    POINT points[2] = { { client.left, client.top }, { client.right, client.bottom } };
    MapWindowPoints(state->target, HWND_DESKTOP, points, 2);
    const int width = points[1].x - points[0].x;
    const int height = points[1].y - points[0].y;
    if (width <= 0 || height <= 0) {
        ShowWindow(overlay, SW_HIDE);
        return;
    }

    UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW;
    HWND foreground = GetForegroundWindow();
    HWND foregroundOwner = foreground ? GetAncestor(foreground, GA_ROOTOWNER) : nullptr;
    HWND targetOwner = GetAncestor(state->target, GA_ROOTOWNER);
    if (foreground != state->target && foregroundOwner != targetOwner) {
        flags |= SWP_NOZORDER;
    }
    SetWindowPos(overlay, HWND_TOP, points[0].x, points[0].y, width, height, flags);
}

LRESULT CALLBACK ReplacementOverlayWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<OverlayWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<OverlayWindowState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    switch (message) {
    case WM_CREATE:
        SetTimer(hwnd, kOverlayTimer, 33, nullptr);
        SyncOverlayWindow(hwnd, state);
        return 0;
    case WM_TIMER:
        if (wParam == kOverlayTimer) SyncOverlayWindow(hwnd, state);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_NCDESTROY:
        KillTimer(hwnd, kOverlayTimer);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsureReplacementOverlayClass() {
    static std::once_flag once;
    static bool registered = false;
    std::call_once(once, [] {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = ReplacementOverlayWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"FluentShell.ContentIslandReplacementOverlay";
        registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    });
    return registered;
}

} // namespace

struct XamlIslandHost::Impl {
    mud::DispatcherQueueController localDq{ nullptr }; // only if InitializeOnCurrentThread without runtime
    muxh::DesktopWindowXamlSource desktopSource{ nullptr };
    mux::XamlIsland xamlIsland{ nullptr };
    muc::DesktopAttachedSiteBridge attachedBridge{ nullptr };
    muc::DesktopChildSiteBridge childBridge{ nullptr };
    HWND parent{ nullptr };
    HWND overlayHwnd{ nullptr };
    HWND islandHwnd{ nullptr };
    enum class Backend { None, DesktopSource, AttachedXamlIsland, ChildXamlIsland, OverlayXamlIsland } backend = Backend::None;
    std::function<void(DialogResult)> onComplete;
    std::shared_ptr<LegacyDialogState> legacyDialog;
    std::unique_ptr<OverlayWindowState> overlayState;
    bool ownsLocalRuntime = false;
};

XamlIslandHost::XamlIslandHost() : impl_(std::make_unique<Impl>()) {}
XamlIslandHost::~XamlIslandHost() { Shutdown(); }

const wchar_t* XamlIslandHost::BackendName() const {
    if (!impl_) return L"none";
    switch (impl_->backend) {
    case Impl::Backend::DesktopSource: return L"DesktopWindowXamlSource";
    case Impl::Backend::AttachedXamlIsland: return L"XamlIsland+DesktopAttachedSiteBridge";
    case Impl::Backend::ChildXamlIsland: return L"XamlIsland+DesktopChildSiteBridge";
    case Impl::Backend::OverlayXamlIsland: return L"XamlIsland+owned HWND overlay";
    default: return L"none";
    }
}

bool XamlIslandHost::InitializeOnCurrentThread() {
    if (ready_) {
        return true;
    }

    // Prefer process runtime if already up and we are on its thread.
    auto& rt = XamlRuntime::Instance();
    if (rt.IsRunning() && GetCurrentThreadId() == rt.XamlThreadId()) {
        ready_ = true;
        Log(L"IslandHost using process XamlRuntime");
        return true;
    }

    // Standalone path (IslandDemo main thread).
    if (!BootstrapWindowsAppSdk()) {
        return false;
    }
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (...) {
    }

    try {
        impl_->localDq = mud::DispatcherQueueController::CreateOnCurrentThread();
        if (!EnsureXamlApplication(true)) {
            return false;
        }
        impl_->ownsLocalRuntime = true;
        ready_ = true;
        Log(L"IslandHost local STA init OK");
        return true;
    } catch (const winrt::hresult_error& e) {
        LogHr(L"InitializeOnCurrentThread", e);
        return false;
    }
}

void XamlIslandHost::Shutdown() {
    if (!impl_) {
        return;
    }
    try {
        if (impl_->desktopSource) {
            impl_->desktopSource.Content(nullptr);
            impl_->desktopSource.Close();
            impl_->desktopSource = nullptr;
        }
        if (impl_->xamlIsland) {
            impl_->xamlIsland.Content(nullptr);
            impl_->xamlIsland.Close();
            impl_->xamlIsland = nullptr;
        }
        if (impl_->attachedBridge) {
            impl_->attachedBridge = nullptr;
        }
        if (impl_->childBridge) {
            impl_->childBridge.Close();
            impl_->childBridge = nullptr;
        }
        // Do NOT close process-wide WindowsXamlManager / Application here.
        if (impl_->ownsLocalRuntime && impl_->localDq) {
            impl_->localDq.ShutdownQueue();
            impl_->localDq = nullptr;
            ShutdownWindowsAppSdk();
        }
    } catch (...) {
    }
    if (impl_->overlayHwnd && IsWindow(impl_->overlayHwnd)) {
        DestroyWindow(impl_->overlayHwnd);
    }
    impl_->overlayHwnd = nullptr;
    impl_->overlayState.reset();
    SetLegacyChildrenVisible(impl_->legacyDialog, true);
    impl_->legacyDialog.reset();
    impl_->onComplete = {};
    impl_->backend = Impl::Backend::None;
    impl_->parent = nullptr;
    impl_->islandHwnd = nullptr;
    ready_ = false;
}

bool XamlIslandHost::AttachToWindow(HWND parent) {
    return AttachToWindow(parent, {});
}

bool XamlIslandHost::AttachToWindow(HWND parent, const IslandAttachOptions& options) {
    if (!ready_ || !parent || !IsWindow(parent)) {
        Log(L"AttachToWindow: not ready or invalid parent");
        return false;
    }
    if (impl_->backend != Impl::Backend::None) {
        Log(L"AttachToWindow: host is already attached");
        return false;
    }

    DWORD parentProcess = 0;
    GetWindowThreadProcessId(parent, &parentProcess);
    if (parentProcess != GetCurrentProcessId()) {
        Log(L"AttachToWindow: target HWND belongs to another process; inject or use an out-of-process bridge");
        return false;
    }
    impl_->parent = parent;

    const auto windowId = mui::GetWindowIdFromWindow(parent);

    auto clearDesktopSource = [&] {
        try {
            if (impl_->desktopSource) impl_->desktopSource.Close();
        } catch (...) {
        }
        impl_->desktopSource = nullptr;
        impl_->islandHwnd = nullptr;
    };

    auto clearContentIsland = [&] {
        try {
            if (impl_->xamlIsland) {
                impl_->xamlIsland.Content(nullptr);
                impl_->xamlIsland.Close();
            }
        } catch (...) {
        }
        try {
            if (impl_->childBridge) impl_->childBridge.Close();
        } catch (...) {
        }
        impl_->attachedBridge = nullptr;
        impl_->childBridge = nullptr;
        impl_->xamlIsland = nullptr;
        impl_->islandHwnd = nullptr;
    };

    auto tryDesktopSource = [&]() -> bool {
        try {
            impl_->desktopSource = muxh::DesktopWindowXamlSource{};
            impl_->desktopSource.Initialize(windowId);

            auto site = impl_->desktopSource.SiteBridge();
            try {
                site.ResizePolicy(muc::ContentSizePolicy::ResizeContentToParentWindow);
            } catch (...) {
                // Older projections may require the explicit initial size below.
            }
            RECT rc{};
            GetClientRect(parent, &rc);
            site.MoveAndResize({ 0, 0, rc.right - rc.left, rc.bottom - rc.top });
            site.Show();

            impl_->islandHwnd = mui::GetWindowFromWindowId(site.WindowId());
            if (impl_->islandHwnd) {
                SetWindowLongPtrW(impl_->islandHwnd, GWL_STYLE,
                    WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
                SetWindowPos(impl_->islandHwnd, HWND_TOP, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }

            impl_->backend = Impl::Backend::DesktopSource;
            Log(L"Attached via DesktopWindowXamlSource");
            return true;
        } catch (const winrt::hresult_error& e) {
            LogHr(L"DesktopWindowXamlSource attach failed", e);
            clearDesktopSource();
            return false;
        } catch (...) {
            Log(L"DesktopWindowXamlSource attach failed with unknown exception");
            clearDesktopSource();
            return false;
        }
    };

    auto tryAttachedContentIsland = [&]() -> bool {
        try {
            const auto dq = mud::DispatcherQueue::GetForCurrentThread();
            if (!dq || !muc::DesktopSiteBridge::IsSupported()) {
                Log(L"DesktopAttachedSiteBridge is not supported on this runtime");
                return false;
            }

            impl_->xamlIsland = mux::XamlIsland{};
            impl_->attachedBridge = muc::DesktopAttachedSiteBridge::CreateFromWindowId(dq, windowId);
            impl_->attachedBridge.Connect(impl_->xamlIsland.ContentIsland());
            impl_->backend = Impl::Backend::AttachedXamlIsland;
            Log(L"Attached via XamlIsland + DesktopAttachedSiteBridge");
            return true;
        } catch (const winrt::hresult_error& e) {
            LogHr(L"DesktopAttachedSiteBridge attach failed", e);
            clearContentIsland();
            return false;
        } catch (...) {
            Log(L"DesktopAttachedSiteBridge attach failed with unknown exception");
            clearContentIsland();
            return false;
        }
    };

    auto tryChildContentIsland = [&](HWND bridgeParent) -> bool {
        try {
            impl_->xamlIsland = mux::XamlIsland{};
            auto contentIsland = impl_->xamlIsland.ContentIsland();
            const auto bridgeWindowId = mui::GetWindowIdFromWindow(bridgeParent);

            auto dq = mud::DispatcherQueue::GetForCurrentThread();
            if (dq) {
                impl_->childBridge = muc::DesktopChildSiteBridge::CreateWithDispatcherQueue(dq, bridgeWindowId);
            } else {
                impl_->childBridge = muc::DesktopChildSiteBridge::Create(
                    winrt::Microsoft::UI::Composition::Compositor{}, bridgeWindowId);
            }

            impl_->childBridge.ResizePolicy(muc::ContentSizePolicy::ResizeContentToParentWindow);
            RECT rc{};
            GetClientRect(bridgeParent, &rc);
            impl_->childBridge.MoveAndResize({ 0, 0, rc.right - rc.left, rc.bottom - rc.top });
            impl_->childBridge.Connect(contentIsland);
            impl_->childBridge.Show();

            impl_->islandHwnd = mui::GetWindowFromWindowId(impl_->childBridge.WindowId());
            impl_->backend = Impl::Backend::ChildXamlIsland;
            Log(L"Attached via XamlIsland + DesktopChildSiteBridge");
            return true;
        } catch (const winrt::hresult_error& e) {
            LogHr(L"DesktopChildSiteBridge attach failed", e);
            clearContentIsland();
            return false;
        } catch (...) {
            Log(L"DesktopChildSiteBridge attach failed with unknown exception");
            clearContentIsland();
            return false;
        }
    };

    auto tryOverlayContentIsland = [&]() -> bool {
        if (!EnsureReplacementOverlayClass()) {
            Log(L"Replacement overlay class registration failed");
            return false;
        }

        RECT client{};
        GetClientRect(parent, &client);
        POINT points[2] = { { client.left, client.top }, { client.right, client.bottom } };
        MapWindowPoints(parent, HWND_DESKTOP, points, 2);

        auto overlayState = std::make_unique<OverlayWindowState>();
        overlayState->target = parent;
        HWND overlay = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            L"FluentShell.ContentIslandReplacementOverlay", L"",
            WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            points[0].x, points[0].y,
            std::max(1, static_cast<int>(points[1].x - points[0].x)),
            std::max(1, static_cast<int>(points[1].y - points[0].y)),
            parent, nullptr, GetModuleHandleW(nullptr), overlayState.get());
        if (!overlay) {
            Log(L"Replacement overlay HWND creation failed");
            return false;
        }

        impl_->overlayHwnd = overlay;
        impl_->overlayState = std::move(overlayState);
        impl_->parent = overlay;
        if (!tryChildContentIsland(overlay)) {
            if (IsWindow(overlay)) DestroyWindow(overlay);
            impl_->overlayHwnd = nullptr;
            impl_->overlayState.reset();
            impl_->parent = parent;
            return false;
        }

        impl_->backend = Impl::Backend::OverlayXamlIsland;
        ShowWindow(overlay, SW_SHOWNOACTIVATE);
        SyncOverlayWindow(overlay, impl_->overlayState.get());
        Log(L"Attached via XamlIsland + owned replacement overlay");
        return true;
    };

    bool attached = false;
    switch (options.mode) {
    case IslandAttachMode::DesktopWindowXamlSource:
        attached = tryDesktopSource();
        if (!attached && options.allowFallback) attached = tryChildContentIsland(parent);
        if (!attached && options.allowFallback) attached = tryAttachedContentIsland();
        break;
    case IslandAttachMode::ContentIslandChild:
        attached = tryChildContentIsland(parent);
        if (!attached && options.allowFallback) attached = tryAttachedContentIsland();
        if (!attached && options.allowFallback) attached = tryDesktopSource();
        break;
    case IslandAttachMode::ContentIslandOverlay:
        attached = tryOverlayContentIsland();
        break;
    case IslandAttachMode::ContentIslandAttached:
        attached = tryAttachedContentIsland();
        if (!attached && options.allowFallback) attached = tryChildContentIsland(parent);
        if (!attached && options.allowFallback) attached = tryDesktopSource();
        break;
    case IslandAttachMode::Auto:
    default:
        attached = tryDesktopSource();
        if (!attached) attached = tryChildContentIsland(parent);
        break;
    }

    if (!attached) impl_->parent = nullptr;
    return attached;
}

bool XamlIslandHost::SetSmokeTestContent(std::wstring_view label) {
    if (!ready_) return false;
    try {
        auto visual = BuildSmokeVisual(label);
        if (impl_->backend == Impl::Backend::DesktopSource && impl_->desktopSource) {
            impl_->desktopSource.Content(visual);
        } else if ((impl_->backend == Impl::Backend::AttachedXamlIsland ||
                    impl_->backend == Impl::Backend::ChildXamlIsland ||
                    impl_->backend == Impl::Backend::OverlayXamlIsland) && impl_->xamlIsland) {
            impl_->xamlIsland.Content(visual);
        } else {
            return false;
        }
        // Force layout pass after content set.
        ResizeToParent();
        Log(L"Smoke content set");
        return true;
    } catch (const winrt::hresult_error& e) {
        LogHr(L"SetSmokeTestContent", e);
        return false;
    }
}

bool XamlIslandHost::SetDialogContent(const DialogModel& model, std::function<void(DialogResult)> onComplete) {
    if (!ready_) return false;
    impl_->onComplete = std::move(onComplete);
    try {
        auto visual = BuildDialogVisual(model, impl_->onComplete);
        if (impl_->backend == Impl::Backend::DesktopSource && impl_->desktopSource) {
            impl_->desktopSource.Content(visual);
        } else if ((impl_->backend == Impl::Backend::AttachedXamlIsland ||
                    impl_->backend == Impl::Backend::ChildXamlIsland ||
                    impl_->backend == Impl::Backend::OverlayXamlIsland) && impl_->xamlIsland) {
            impl_->xamlIsland.Content(visual);
        } else {
            return false;
        }
        ResizeToParent();
        return true;
    } catch (const winrt::hresult_error& e) {
        LogHr(L"SetDialogContent", e);
        return false;
    }
}

bool XamlIslandHost::ReplaceExistingDialog(HWND dialog, bool dark) {
    if (!ready_ || !dialog || !IsWindow(dialog) || impl_->backend != Impl::Backend::None) {
        Log(L"ReplaceExistingDialog: host is not ready, target is invalid, or host is already attached");
        return false;
    }

    DWORD processId = 0;
    const DWORD dialogThreadId = GetWindowThreadProcessId(dialog, &processId);
    const auto className = WindowClass(dialog);
    if (processId != GetCurrentProcessId() ||
        (!EqualsIgnoreCase(className, L"#32770") && !ContainsIgnoreCase(className, L"TaskDialog"))) {
        Log(L"ReplaceExistingDialog: target must be a same-process #32770/TaskDialog window");
        return false;
    }

    auto capture = CaptureSimpleDialog(dialog, dark);
    if (!capture.supported) {
        Log(std::wstring(L"ReplaceExistingDialog: pass-through required for ") + capture.unsupportedClass);
        return false;
    }

    IslandAttachOptions attachOptions;
    const bool sameThread = dialogThreadId == GetCurrentThreadId();
    attachOptions.mode = sameThread
        ? IslandAttachMode::ContentIslandAttached
        : IslandAttachMode::ContentIslandOverlay;
    attachOptions.allowFallback = sameThread;
    if (!AttachToWindow(dialog, attachOptions)) {
        Log(L"ReplaceExistingDialog: no island backend could attach");
        return false;
    }

    const auto state = capture.state;
    if (!SetDialogContent(capture.model, [state](DialogResult result) {
            if (!state || !IsWindow(state->dialog) || result.buttonIndex < 0 ||
                result.buttonIndex >= static_cast<int>(state->buttons.size())) {
                return;
            }

            if (state->verification && IsWindow(state->verification) &&
                state->verificationChecked != result.verificationChecked) {
                // BM_CLICK preserves the original dialog's verification notification contract.
                PostMessageW(state->verification, BM_CLICK, 0, 0);
                state->verificationChecked = result.verificationChecked;
            }

            const auto& target = state->buttons[static_cast<size_t>(result.buttonIndex)];
            if (!target.hwnd || !IsWindow(target.hwnd) || !PostMessageW(target.hwnd, BM_CLICK, 0, 0)) {
                PostMessageW(state->dialog, WM_COMMAND,
                    MAKEWPARAM(static_cast<WORD>(target.id), BN_CLICKED),
                    reinterpret_cast<LPARAM>(target.hwnd));
            }
        })) {
        Log(L"ReplaceExistingDialog: XAML content creation failed");
        Shutdown();
        return false;
    }

    impl_->legacyDialog = state;
    SetLegacyChildrenVisible(state, false);
    Dwm::ApplyToWindow(dialog, DwmStyle{ dark, Backdrop::Acrylic, true, false });
    ResizeToParent();
    if (impl_->islandHwnd) {
        SetWindowPos(impl_->islandHwnd, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    Log(std::wstring(L"Replaced existing dialog through ") + BackendName());
    return true;
}

void XamlIslandHost::ResizeToParent() {
    if (!impl_->parent) return;
    RECT rc{};
    GetClientRect(impl_->parent, &rc);
    Resize(0, 0, rc.right - rc.left, rc.bottom - rc.top);
}

void XamlIslandHost::Resize(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return;
    try {
        if (impl_->desktopSource) {
            impl_->desktopSource.SiteBridge().MoveAndResize({ x, y, width, height });
        } else if (impl_->childBridge) {
            impl_->childBridge.MoveAndResize({ x, y, width, height });
        } else if (impl_->islandHwnd) {
            SetWindowPos(impl_->islandHwnd, nullptr, x, y, width, height, SWP_NOZORDER | SWP_SHOWWINDOW);
        }
    } catch (...) {
    }
}

HWND XamlIslandHost::IslandHwnd() const {
    return impl_ ? impl_->islandHwnd : nullptr;
}

bool XamlIslandHost::IsReplacingExistingContent() const {
    return impl_ && impl_->legacyDialog != nullptr;
}

// ---------------------------------------------------------------------------
// Modal dialog (always on XAML STA)
// ---------------------------------------------------------------------------

namespace {

struct ModalState {
    DialogResult result{};
    bool done = false;
    XamlIslandHost* host = nullptr;
    HWND hwnd = nullptr;
};

LRESULT CALLBACK ModalWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ModalState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (msg) {
    case WM_SIZE:
        if (state && state->host) {
            state->host->ResizeToParent();
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hWnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        if (state && state->host) {
            state->host->ResizeToParent();
        }
        return 0;
    }
    case WM_CLOSE:
        if (state) {
            state->done = true;
            state->result.buttonIndex = -1;
        }
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

void EnsureModalClass() {
    static std::once_flag once;
    std::call_once(once, [] {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ModalWndProc),
                &mod) && mod) {
            inst = reinterpret_cast<HINSTANCE>(mod);
        }
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = ModalWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"FluentShell.ModalHost";
        wc.style = CS_HREDRAW | CS_VREDRAW;
        RegisterClassExW(&wc);
    });
}

DialogResult ShowFluentDialogModalOnXamlThread(HWND owner, const DialogModel& model) {
    DialogResult fallback{ -1, false };
    EnsureModalClass();

    if (owner && IsWindow(owner)) {
        Dwm::ApplyToWindow(owner, DwmStyle{});
    }

    const int width = 480;
    const int height = 320;
    RECT ownerRc{ 200, 200, 200 + width, 200 + height };
    if (owner && IsWindow(owner)) {
        GetWindowRect(owner, &ownerRc);
    }
    const int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - width) / 2;
    const int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - height) / 2;

    // Create host window ON the XAML STA thread (critical).
    // Use Bridge/Island module for HINSTANCE when injected (not host EXE).
    HINSTANCE inst = GetModuleHandleW(nullptr);
    {
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ShowFluentDialogModalOnXamlThread),
                &mod) && mod) {
            inst = reinterpret_cast<HINSTANCE>(mod);
        }
    }

    HWND hostWnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
        L"FluentShell.ModalHost",
        model.title.empty() ? model.mainInstruction.c_str() : model.title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        x, y, width, height,
        owner, nullptr, inst, nullptr);
    if (!hostWnd) {
        Log(L"Modal host CreateWindow failed");
        return fallback;
    }

    Dwm::ApplyToWindow(hostWnd, DwmStyle{ true, Backdrop::Acrylic, true, false });

    ModalState state{};
    state.hwnd = hostWnd;
    XamlIslandHost host;

    Log(L"Modal: init...");
    if (!host.InitializeOnCurrentThread()) {
        Log(L"Modal host XAML init failed");
        DestroyWindow(hostWnd);
        return fallback;
    }

    ShowWindow(hostWnd, SW_SHOW);
    UpdateWindow(hostWnd);

    Log(L"Modal: attach...");
    if (!host.AttachToWindow(hostWnd, { IslandAttachMode::ContentIslandAttached, true })) {
        Log(L"Modal host attach failed - caller should pass through");
        DestroyWindow(hostWnd);
        host.Shutdown();
        return fallback;
    }

    state.host = &host;
    SetWindowLongPtrW(hostWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));

    Log(L"Modal: set content...");
    if (!host.SetDialogContent(model, [&](DialogResult r) {
            state.result = r;
            state.done = true;
            if (IsWindow(hostWnd)) {
                DestroyWindow(hostWnd);
            }
        })) {
        Log(L"Modal SetDialogContent failed");
        DestroyWindow(hostWnd);
        host.Shutdown();
        return fallback;
    }

    host.ResizeToParent();
    SetWindowPos(hostWnd, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hostWnd);
    Log(L"Modal: pump start");

    if (owner && IsWindow(owner)) {
        EnableWindow(owner, FALSE);
    }

    // Nested pump on XAML thread - ContentPreTranslateMessage keeps island input alive.
    MSG msg{};
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (ContentPreTranslateMessage(&msg)) {
            continue;
        }
        if (!IsWindow(hostWnd)) {
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (owner && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }

    host.Shutdown();
    if (IsWindow(hostWnd)) {
        DestroyWindow(hostWnd);
    }
    return state.result;
}

} // namespace

DialogResult ShowFluentDialogModal(HWND owner, const DialogModel& model) {
    DialogResult result{ -1, false };

    auto& rt = XamlRuntime::Instance();
    if (!rt.EnsureStarted()) {
        // Do NOT call MessageBoxW here when invoked from a MessageBox hook -
        // re-entrancy is handled by the caller's fail-safe pass-through.
        Log(L"ShowFluentDialogModal: XamlRuntime not ready");
        return result;
    }

    // Entire modal (HWND create + island + nested pump) on XAML STA.
    // Caller's thread is pumped by RunSync so UI doesn't freeze.
    const bool ok = rt.RunSync([&] {
        result = ShowFluentDialogModalOnXamlThread(owner, model);
    });

    if (!ok) {
        Log(L"ShowFluentDialogModal RunSync failed");
        result.buttonIndex = -1;
    }
    return result;
}

namespace {

struct ExistingDialogRegistry {
    std::mutex mutex;
    std::unordered_set<HWND> pending;
    std::unordered_set<HWND> cancelled;
    std::unordered_map<HWND, std::unique_ptr<XamlIslandHost>> active;
};

ExistingDialogRegistry& DialogRegistry() {
    // Process lifetime by design: active hosts are released on the XAML thread, never
    // from a static destructor running under the loader lock.
    static auto* registry = new ExistingDialogRegistry();
    return *registry;
}

bool IsHighContrastEnabled() {
    HIGHCONTRASTW highContrast{ sizeof(highContrast) };
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast), &highContrast, 0) &&
        (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

void RemovePendingDialog(HWND dialog) {
    auto& registry = DialogRegistry();
    std::lock_guard lock(registry.mutex);
    registry.pending.erase(dialog);
    registry.cancelled.erase(dialog);
}

} // namespace

bool TryReplaceExistingDialogAsync(HWND dialog) {
    if (!dialog || !IsWindow(dialog)) return false;
    if (IsHighContrastEnabled()) {
        Log(L"Dialog island replacement skipped while high contrast is active");
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(dialog, &processId);
    if (processId != GetCurrentProcessId()) return false;

    auto& registry = DialogRegistry();
    {
        std::lock_guard lock(registry.mutex);
        if (registry.pending.contains(dialog) || registry.active.contains(dialog)) return true;
        registry.cancelled.erase(dialog);
        registry.pending.insert(dialog);
    }

    auto& runtime = XamlRuntime::Instance();
    if (!runtime.EnsureStarted()) {
        RemovePendingDialog(dialog);
        return false;
    }

    const bool queued = runtime.RunAsync([dialog] {
        auto& registry = DialogRegistry();
        {
            std::lock_guard lock(registry.mutex);
            if (!registry.pending.contains(dialog) || registry.cancelled.erase(dialog) != 0) {
                registry.pending.erase(dialog);
                return;
            }
        }

        if (!IsWindow(dialog)) {
            RemovePendingDialog(dialog);
            return;
        }

        auto host = std::make_unique<XamlIslandHost>();
        const bool replaced = host->InitializeOnCurrentThread() && host->ReplaceExistingDialog(dialog, true);
        bool cancelled = false;
        {
            std::lock_guard lock(registry.mutex);
            registry.pending.erase(dialog);
            cancelled = registry.cancelled.erase(dialog) != 0;
            if (replaced && !cancelled && IsWindow(dialog)) {
                registry.active.emplace(dialog, std::move(host));
            }
        }

        if (host) {
            host->Shutdown();
        }
        if (!replaced) {
            Log(L"Async dialog island replacement failed; target remains eligible for retry");
        }
    });

    if (!queued) RemovePendingDialog(dialog);
    return queued;
}

void ReleaseExistingDialogAsync(HWND dialog) {
    if (!dialog) return;
    auto& registry = DialogRegistry();
    {
        std::lock_guard lock(registry.mutex);
        if (!registry.pending.contains(dialog) && !registry.active.contains(dialog)) return;
        registry.cancelled.insert(dialog);
    }

    auto& runtime = XamlRuntime::Instance();
    if (!runtime.IsRunning()) return;
    runtime.RunAsync([dialog] {
        std::unique_ptr<XamlIslandHost> host;
        auto& registry = DialogRegistry();
        {
            std::lock_guard lock(registry.mutex);
            registry.pending.erase(dialog);
            registry.cancelled.erase(dialog);
            auto active = registry.active.find(dialog);
            if (active != registry.active.end()) {
                host = std::move(active->second);
                registry.active.erase(active);
            }
        }
        if (host) host->Shutdown();
    });
}

bool RunIslandSmokeTest(unsigned keepAliveMs) {
    auto& rt = XamlRuntime::Instance();
    if (!rt.EnsureStarted()) {
        return false;
    }

    constexpr UINT kCloseSmokeDialog = WM_APP + 0x415;
    std::mutex dialogMutex;
    std::condition_variable dialogReady;
    HWND dialog = nullptr;
    DWORD dialogThreadId = 0;
    bool dialogCreated = false;
    bool dialogInitialized = false;

    // The target lives on a different STA/message loop, matching an injected legacy
    // application instead of the easier same-thread demo case.
    std::thread dialogThread([&] {
        const DWORD currentDialogThreadId = GetCurrentThreadId();
        MSG queueProbe{};
        PeekMessageW(&queueProbe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        HWND hwnd = CreateWindowExW(
            WS_EX_DLGMODALFRAME, L"#32770", L"FluentShell Dialog Replacement Smoke",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            100, 100, 640, 360, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (hwnd) {
            const auto font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            HWND instruction = CreateWindowExW(
                0, L"Static", L"This legacy #32770 client is rendered by a XamlIsland.",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                28, 28, 560, 44, hwnd, reinterpret_cast<HMENU>(1001), GetModuleHandleW(nullptr), nullptr);
            HWND content = CreateWindowExW(
                0, L"Static", L"The native controls remain the command and state endpoints.",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                28, 82, 560, 32, hwnd, reinterpret_cast<HMENU>(1002), GetModuleHandleW(nullptr), nullptr);
            HWND verification = CreateWindowExW(
                0, L"Button", L"Remember this choice",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                28, 132, 220, 28, hwnd, reinterpret_cast<HMENU>(1003), GetModuleHandleW(nullptr), nullptr);
            HWND okButton = CreateWindowExW(
                0, L"Button", L"OK",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                390, 230, 90, 32, hwnd, reinterpret_cast<HMENU>(IDOK), GetModuleHandleW(nullptr), nullptr);
            HWND cancelButton = CreateWindowExW(
                0, L"Button", L"Cancel",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                490, 230, 90, 32, hwnd, reinterpret_cast<HMENU>(IDCANCEL), GetModuleHandleW(nullptr), nullptr);
            for (HWND child : { instruction, content, verification, okButton, cancelButton }) {
                if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            }
            SendMessageW(hwnd, DM_SETDEFID, IDOK, 0);
        }

        {
            std::lock_guard lock(dialogMutex);
            dialog = hwnd;
            dialogThreadId = currentDialogThreadId;
            dialogCreated = hwnd != nullptr;
            dialogInitialized = true;
        }
        dialogReady.notify_one();

        if (!hwnd) return;
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == kCloseSmokeDialog) {
                DestroyWindow(hwnd);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    });

    {
        std::unique_lock lock(dialogMutex);
        dialogReady.wait(lock, [&] { return dialogInitialized; });
    }

    bool ok = false;
    if (dialogCreated) rt.RunSync([&] {
        XamlIslandHost host;
        if (!host.InitializeOnCurrentThread() || !host.ReplaceExistingDialog(dialog, true)) return;
        Log(std::wstring(L"Dialog replacement smoke backend: ") + host.BackendName());

        // Pump for keepAliveMs with PeekMessage - GetMessage can hang when this
        // runs nested inside DispatcherQueue.TryEnqueue on the same STA thread.
        const ULONGLONG end = GetTickCount64() + keepAliveMs;
        bool quit = false;
        MSG msg{};
        while (!quit && GetTickCount64() < end && IsWindow(dialog)) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    quit = true;
                    break;
                }
                if (!ContentPreTranslateMessage(&msg)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
            Sleep(10);
        }
        Log(L"Smoke pump finished");
        host.Shutdown();
        ok = true;
    });

    if (dialogThreadId) PostThreadMessageW(dialogThreadId, kCloseSmokeDialog, 0, 0);
    if (dialogThread.joinable()) dialogThread.join();
    return ok;
}

} // namespace FluentShell::Island
