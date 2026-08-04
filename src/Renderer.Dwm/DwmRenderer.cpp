#include "DwmRenderer.h"
#include "../Common/ProcessPolicy.h"

#include <uxtheme.h>

#pragma comment(lib, "uxtheme.lib")

namespace FluentShell::Dwm {
namespace {

// DWM attribute constants (SDK may lag newer values on older kits).
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

#ifndef DWMWCP_DEFAULT
#define DWMWCP_DEFAULT 0
#define DWMWCP_DONOTROUND 1
#define DWMWCP_ROUND 2
#define DWMWCP_ROUNDSMALL 3
#endif

#ifndef DWMSBT_AUTO
#define DWMSBT_AUTO 0
#define DWMSBT_NONE 1
#define DWMSBT_MAINWINDOW 2
#define DWMSBT_TRANSIENTWINDOW 3
#define DWMSBT_TABBEDWINDOW 4
#endif

// Undocumented accent policy fallback for Win10 acrylic.
struct ACCENT_POLICY {
    int nAccentState;
    int nFlags;
    unsigned int nGradientColor;
    int nAnimationId;
};

struct WINDOWCOMPOSITIONATTRIBDATA {
    int nAttribute;
    void* pData;
    size_t ulDataSize;
};

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

constexpr int WCA_ACCENT_POLICY = 19;
constexpr int ACCENT_ENABLE_ACRYLICBLURBEHIND = 4;
constexpr int ACCENT_ENABLE_BLURBEHIND = 3;

bool SetAccentAcrylic(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;
    auto fn = reinterpret_cast<SetWindowCompositionAttributeFn>(
        GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (!fn) return false;

    ACCENT_POLICY policy{};
    policy.nAccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    // ABGR: alpha + tint (Fluent dark-ish)
    policy.nGradientColor = 0xCC202020;
    policy.nFlags = 2;

    WINDOWCOMPOSITIONATTRIBDATA data{};
    data.nAttribute = WCA_ACCENT_POLICY;
    data.pData = &policy;
    data.ulDataSize = sizeof(policy);
    return fn(hwnd, &data) == TRUE;
}

bool IsTopLevel(HWND hwnd) {
    return GetAncestor(hwnd, GA_ROOT) == hwnd && IsWindowVisible(hwnd);
}

bool MatchWindow(HWND hwnd, const MatchRule& rule) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    // Never touch denied processes (explorer / Start / Quick Settings / …).
    if (IsProcessDeniedByPid(pid)) {
        return false;
    }

    wchar_t cls[256]{};
    if (!GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)))) {
        return false;
    }
    if (IsShellOrXamlWindowClass(cls)) {
        return false;
    }

    if (!rule.className.empty()) {
        if (!EqualsIgnoreCase(cls, rule.className)) return false;
    }
    if (!rule.title.empty()) {
        wchar_t title[512]{};
        GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        if (!ContainsIgnoreCase(title, rule.title)) return false;
    }
    if (!rule.exe.empty()) {
        auto path = GetProcessImagePath(pid);
        auto name = FileNameOf(path);
        if (!EqualsIgnoreCase(name, rule.exe)) return false;
    }
    // Class-only rules (#32770) still require a non-denied process (checked above).
    return true;
}

} // namespace

bool ApplyDarkTitleBar(HWND hwnd, bool dark) {
    BOOL useDark = dark ? TRUE : FALSE;
    // Try modern attribute first (Win10 1903+ / Win11).
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
    if (FAILED(hr)) {
        // Pre-20H1 used attribute 19.
        constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 = 19;
        hr = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1, &useDark, sizeof(useDark));
    }
    return SUCCEEDED(hr);
}

bool ApplyBackdrop(HWND hwnd, Backdrop backdrop) {
    DWORD type = DWMSBT_AUTO;
    switch (backdrop) {
    case Backdrop::None: type = DWMSBT_NONE; break;
    case Backdrop::Mica: type = DWMSBT_MAINWINDOW; break;
    case Backdrop::MicaAlt: type = DWMSBT_TABBEDWINDOW; break;
    case Backdrop::Acrylic: type = DWMSBT_TRANSIENTWINDOW; break;
    case Backdrop::Auto:
    default: type = DWMSBT_AUTO; break;
    }

    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &type, sizeof(type));
    if (SUCCEEDED(hr)) return true;

    // Fallback: undocumented acrylic on older builds when Acrylic requested.
    if (backdrop == Backdrop::Acrylic || backdrop == Backdrop::Mica) {
        return SetAccentAcrylic(hwnd);
    }
    return false;
}

bool ApplyRoundCorners(HWND hwnd, bool round) {
    DWORD pref = round ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    return SUCCEEDED(hr);
}

bool ApplyCaptionColors(HWND hwnd, bool dark) {
    // COLORREF as RGB — DWM uses COLORREF for these attrs on Win11.
    const COLORREF caption = dark ? RGB(32, 32, 32) : RGB(243, 243, 243);
    const COLORREF text = dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
    const COLORREF border = dark ? RGB(32, 32, 32) : RGB(229, 229, 229);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof(text));
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    return true;
}

bool ApplyToWindow(HWND hwnd, const DwmStyle& style) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    bool ok = ApplyDarkTitleBar(hwnd, style.darkMode);
    ok = ApplyCaptionColors(hwnd, style.darkMode) || ok;
    ok = ApplyRoundCorners(hwnd, style.roundCorners) || ok;
    ok = ApplyBackdrop(hwnd, style.backdrop) || ok;

    if (style.extendFrame) {
        MARGINS margins{ -1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);
    }

    // Nudge non-client repaint so title bar updates immediately.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    return ok;
}

bool ApplyMatchingRules(HWND hwnd, const std::vector<MatchRule>& rules) {
    for (const auto& rule : rules) {
        if (MatchWindow(hwnd, rule)) {
            return ApplyToWindow(hwnd, rule.dwm);
        }
    }
    return false;
}

void EnumTopLevelWindows(const std::vector<MatchRule>& rules, EnumCallback cb, void* ctx) {
    struct State {
        const std::vector<MatchRule>* rules;
        EnumCallback cb;
        void* ctx;
    } state{ &rules, cb, ctx };

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* s = reinterpret_cast<State*>(lp);
        if (!IsTopLevel(hwnd)) return TRUE;
        for (const auto& rule : *s->rules) {
            if (MatchWindow(hwnd, rule)) {
                if (s->cb && !s->cb(hwnd, rule, s->ctx)) {
                    return FALSE;
                }
                break;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
}

std::vector<MatchRule> DefaultRules() {
    DwmStyle darkMica{};
    darkMica.darkMode = true;
    darkMica.backdrop = Backdrop::Mica;
    darkMica.roundCorners = true;

    DwmStyle darkAcrylic = darkMica;
    darkAcrylic.backdrop = Backdrop::Acrylic;

    // IMPORTANT: Prefer exe-scoped rules. Class-only (#32770) is dangerous
    // because many shell/XAML hosts also use classic dialog classes.
    // MatchWindow() already denies explorer/shell/XAML processes.
    return {
        MatchRule{ L"LegacyDialogHost.exe", L"", L"", darkAcrylic },
        MatchRule{ L"notepad.exe", L"", L"", darkMica },
        MatchRule{ L"mmc.exe", L"", L"", darkMica },
        MatchRule{ L"regedit.exe", L"", L"", darkMica },
        MatchRule{ L"eventvwr.exe", L"", L"", darkMica },
        // Dialog classes only for non-denied processes (see MatchWindow).
        MatchRule{ L"", L"#32770", L"", darkAcrylic },
        MatchRule{ L"", L"TaskDialogTheme", L"", darkAcrylic },
    };
}

} // namespace FluentShell::Dwm
