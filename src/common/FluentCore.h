// AnyFluent — shared Fluent modernization core.
// Turns a legacy Win32 dialog (#32770 / TaskDialog) into a dark Fluent Design
// window: DWM dark frame + rounded corners + Mica, undocumented uxtheme dark
// mode for standard controls, and Direct2D/DirectWrite repaint of buttons,
// static text and the caution icon (Segoe Fluent Icons).
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
// windows.h defines DrawText -> DrawTextW as a macro; if it is active when
// d2d1.h is parsed it renames ID2D1RenderTarget::DrawText. Undef it first.
#ifdef DrawText
#undef DrawText
#endif
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>

namespace fluent {

struct Theme {
    D2D1_COLOR_F windowBg;
    D2D1_COLOR_F textPrimary;
    D2D1_COLOR_F textSecondary;
    D2D1_COLOR_F btnFace;
    D2D1_COLOR_F btnFaceHover;
    D2D1_COLOR_F btnFacePressed;
    D2D1_COLOR_F btnBorder;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F accentHover;
    D2D1_COLOR_F accentText;
    D2D1_COLOR_F warning;
    COLORREF     windowBgRef;   // GDI equivalent of windowBg (for brushes / ctlcolor)
};
const Theme& DarkTheme();

namespace dwm {
    bool IsWin11();                                  // build >= 22000
    unsigned long BuildNumber();
    void EnableDarkTitleBar(HWND, bool dark = true); // DWMWA_USE_IMMERSIVE_DARK_MODE
    void EnableRoundedCorners(HWND);                 // DWMWA_WINDOW_CORNER_PREFERENCE
    void SetCaptionColor(HWND, COLORREF);            // DWMWA_CAPTION_COLOR
    void SetBorderColor(HWND, COLORREF);             // DWMWA_BORDER_COLOR
    bool SetMicaBackdrop(HWND);                      // DWMWA_SYSTEMBACKDROP_TYPE (build >= 22621)
}

namespace darkmode {
    void Init();                                     // load undocumented uxtheme ordinals once
    void ForceDark();                                // SetPreferredAppMode(ForceDark) — for DirectUI/TaskDialog
    void AllowForWindow(HWND, bool allow = true);    // ordinal 133
    void EnableDarkScrollBars(HWND);                 // SetWindowTheme("DarkMode_Explorer")
}

namespace d2d {
    bool EnsureInit();
    ID2D1DCRenderTarget* DcTarget();
    IDWriteTextFormat* TextFormat(const wchar_t* family, float sizePx, DWRITE_FONT_WEIGHT weight);
}

// High-level entry: modernize a whole dialog (frame + dark mode + control repaint).
void ModernizeDialog(HWND hDlg, bool isTaskDialog = false);

// Fine-grained subclassers (used by the demo and by the injected hook).
void SubclassButton(HWND, bool primary);
void SubclassStaticText(HWND);
void SubclassStaticIcon(HWND);
void SubclassDialogChrome(HWND hDlg);

} // namespace fluent
