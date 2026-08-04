#pragma once

#include "../Common/FluentShell.h"

#include <functional>
#include <memory>
#include <string>

namespace FluentShell::Island {

enum class DialogIcon {
    None,
    Information,
    Warning,
    Error,
    Question,
};

struct DialogModel {
    std::wstring title;
    std::wstring mainInstruction;
    std::wstring content;
    std::wstring footer;
    std::wstring verificationText;
    std::vector<std::wstring> buttons;
    int defaultButtonIndex = 0;
    int cancelButtonIndex = -1;
    DialogIcon icon = DialogIcon::None;
    bool verificationChecked = false;
    bool dark = true;
};

struct DialogResult {
    int buttonIndex = -1;
    bool verificationChecked = false;
};

enum class IslandAttachMode {
    // Preserves the established path: DesktopWindowXamlSource, then a child ContentIsland.
    Auto,
    DesktopWindowXamlSource,
    ContentIslandChild,
    // Uses a same-XAML-thread owned popup aligned to an externally owned HWND.
    // This is the viable replacement mode when the target HWND belongs to another thread.
    ContentIslandOverlay,
    // Connects a XamlIsland ContentIsland directly to an existing HWND. This is the
    // zero-intermediate-HWND path on Windows App SDK versions that support it.
    ContentIslandAttached,
};

struct IslandAttachOptions {
    IslandAttachMode mode = IslandAttachMode::Auto;
    bool allowFallback = true;
};

// Process-wide WinUI / ContentIsland runtime (dedicated STA + DispatcherQueue + Application).
// All XAML work is marshaled onto this thread — required for stable element rendering.
class XamlRuntime {
public:
    static XamlRuntime& Instance();

    bool EnsureStarted();
    void Shutdown();

    bool IsRunning() const;
    DWORD XamlThreadId() const;

    // Run work on the XAML STA thread. Blocks caller until complete (unless already on XAML thread).
    bool RunSync(const std::function<void()>& work);
    bool RunAsync(std::function<void()> work);

private:
    XamlRuntime() = default;
    ~XamlRuntime() = default;
    XamlRuntime(const XamlRuntime&) = delete;
    XamlRuntime& operator=(const XamlRuntime&) = delete;

    struct State;
    std::unique_ptr<State> state_;
};

// Hosts WinUI content through DesktopWindowXamlSource or XamlIsland/ContentIsland site bridges.
// Instances must be created/used via XamlRuntime::RunSync (same STA).
class XamlIslandHost {
public:
    XamlIslandHost();
    ~XamlIslandHost();

    XamlIslandHost(const XamlIslandHost&) = delete;
    XamlIslandHost& operator=(const XamlIslandHost&) = delete;

    // Preferred path for in-process demos that already run on the XAML thread.
    bool InitializeOnCurrentThread();
    void Shutdown();

    bool AttachToWindow(HWND parent);
    bool AttachToWindow(HWND parent, const IslandAttachOptions& options);
    bool SetDialogContent(const DialogModel& model, std::function<void(DialogResult)> onComplete);
    bool SetSmokeTestContent(std::wstring_view label);

    // Replaces a fully-created, simple same-process #32770/TaskDialog client area.
    // The adapter mirrors static text, push buttons, and one verification checkbox,
    // hides the legacy child HWNDs, and forwards XAML actions to their original IDs.
    // Complex/custom controls are rejected without changing the target window.
    bool ReplaceExistingDialog(HWND dialog, bool dark = true);

    void ResizeToParent();
    void Resize(int x, int y, int width, int height);

    HWND IslandHwnd() const;
    bool IsReady() const { return ready_; }
    bool IsReplacingExistingContent() const;

    // Diagnostics: which backend is active.
    const wchar_t* BackendName() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ready_ = false;
};

bool BootstrapWindowsAppSdk();
void ShutdownWindowsAppSdk();

// Always safe: marshals to XAML STA, builds modal Fluent dialog, returns result.
DialogResult ShowFluentDialogModal(HWND owner, const DialogModel& model);

// Non-blocking no-source replacement entry points for dialog hooks/subclasses.
// Hosts are owned by the XAML-thread registry until ReleaseExistingDialogAsync
// (normally from WM_NCDESTROY) or automatic target-loss cleanup.
bool TryReplaceExistingDialogAsync(HWND dialog);
void ReleaseExistingDialogAsync(HWND dialog);

// Self-test: create top-level host on XAML thread and show smoke UI for N ms (debug).
bool RunIslandSmokeTest(unsigned keepAliveMs = 3000);

} // namespace FluentShell::Island
