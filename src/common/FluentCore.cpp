// AnyFluent — shared Fluent modernization core (implementation).
#include "FluentCore.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <commctrl.h>
#include <string>
#include <map>
#include <cmath>

// ID2D1RenderTarget::DrawText collides with the windows.h DrawText macro.
#ifdef DrawText
#undef DrawText
#endif

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "comctl32.lib")

namespace fluent {

// ---------------------------------------------------------------- color utils
static D2D1_COLOR_F RGBAf(int r, int g, int b, float a = 1.f) {
    return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a);
}
static float clampf(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
static D2D1_COLOR_F lerp(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
static D2D1_COLOR_F mul(const D2D1_COLOR_F& c, float f) {
    return D2D1::ColorF(clampf(c.r * f), clampf(c.g * f), clampf(c.b * f), c.a);
}

// ------------------------------------------------------------------- theme
const Theme& DarkTheme() {
    static Theme t = [] {
        Theme x{};
        x.windowBg      = RGBAf(32, 32, 32);
        x.textPrimary   = RGBAf(255, 255, 255);
        x.textSecondary = RGBAf(205, 205, 205);
        x.btnFace       = RGBAf(45, 45, 45);
        x.btnFaceHover  = RGBAf(60, 60, 60);
        x.btnFacePressed= RGBAf(38, 38, 38);
        x.btnBorder     = RGBAf(72, 72, 72);
        // Pull the live system accent from DWM; fall back to Fluent blue.
        D2D1_COLOR_F accent = RGBAf(0, 120, 215);
        DWORD col = 0; BOOL opaque = FALSE;
        if (SUCCEEDED(DwmGetColorizationColor(&col, &opaque))) {
            accent = RGBAf((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
        }
        x.accent      = accent;
        x.accentHover = D2D1::ColorF(clampf(accent.r * 1.12f + 0.04f),
                                     clampf(accent.g * 1.12f + 0.04f),
                                     clampf(accent.b * 1.12f + 0.04f));
        float lum = 0.299f * accent.r + 0.587f * accent.g + 0.114f * accent.b;
        x.accentText  = (lum > 0.6f) ? RGBAf(0, 0, 0) : RGBAf(255, 255, 255);
        x.warning     = RGBAf(255, 185, 0);
        x.windowBgRef = RGB(32, 32, 32);
        return x;
    }();
    return t;
}

// -------------------------------------------------------------------- DWM
namespace dwm {
unsigned long BuildNumber() {
    static unsigned long build = [] () -> unsigned long {
        typedef struct { DWORD sz, maj, min, build, plat; WCHAR csd[128]; } OSVER;
        typedef LONG (WINAPI *RtlGetVersionFn)(OSVER*);
        HMODULE h = GetModuleHandleW(L"ntdll.dll");
        if (!h) return 0;
        auto p = (RtlGetVersionFn)GetProcAddress(h, "RtlGetVersion");
        if (!p) return 0;
        OSVER v{}; v.sz = sizeof(v);
        if (p(&v) != 0) return 0;
        return v.build;
    }();
    return build;
}
bool IsWin11() { return BuildNumber() >= 22000; }

void EnableDarkTitleBar(HWND h, bool dark) {
    BOOL b = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &b, sizeof(b));
}
void EnableRoundedCorners(HWND h) {
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
}
void SetCaptionColor(HWND h, COLORREF c) {
    DwmSetWindowAttribute(h, DWMWA_CAPTION_COLOR, &c, sizeof(c));
}
void SetBorderColor(HWND h, COLORREF c) {
    DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &c, sizeof(c));
}
bool SetMicaBackdrop(HWND h) {
    if (BuildNumber() < 22621) return false;
    DWM_SYSTEMBACKDROP_TYPE bt = DWMSBT_MAINWINDOW;
    return SUCCEEDED(DwmSetWindowAttribute(h, DWMWA_SYSTEMBACKDROP_TYPE, &bt, sizeof(bt)));
}
} // namespace dwm

// -------------------------------------------------------------- dark mode
namespace darkmode {
enum PreferredAppMode { PAM_Default, PAM_AllowDark, PAM_ForceDark, PAM_ForceLight, PAM_Max };
typedef PreferredAppMode (WINAPI *fnSetPreferredAppMode)(PreferredAppMode);
typedef bool  (WINAPI *fnAllowDarkModeForWindow)(HWND, bool);
typedef void  (WINAPI *fnRefreshImmersiveColorPolicyState)();
typedef void  (WINAPI *fnFlushMenuThemes)();

static fnSetPreferredAppMode              pSetPreferredAppMode = nullptr;      // ord 135
static fnAllowDarkModeForWindow           pAllowDarkModeForWindow = nullptr;   // ord 133
static fnRefreshImmersiveColorPolicyState pRefreshImmersive = nullptr;         // ord 104
static fnFlushMenuThemes                  pFlushMenuThemes = nullptr;          // ord 136
static bool g_init = false;

void Init() {
    if (g_init) return;
    g_init = true;
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    pSetPreferredAppMode    = (fnSetPreferredAppMode)              GetProcAddress(ux, MAKEINTRESOURCEA(135));
    pAllowDarkModeForWindow = (fnAllowDarkModeForWindow)           GetProcAddress(ux, MAKEINTRESOURCEA(133));
    pRefreshImmersive       = (fnRefreshImmersiveColorPolicyState) GetProcAddress(ux, MAKEINTRESOURCEA(104));
    pFlushMenuThemes        = (fnFlushMenuThemes)                  GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (pSetPreferredAppMode) pSetPreferredAppMode(PAM_AllowDark);
    if (pRefreshImmersive)    pRefreshImmersive();
    if (pFlushMenuThemes)     pFlushMenuThemes();
}
void ForceDark() {
    if (pSetPreferredAppMode) pSetPreferredAppMode(PAM_ForceDark);
    if (pRefreshImmersive)    pRefreshImmersive();
}
void AllowForWindow(HWND h, bool allow) {
    if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(h, allow);
}
void EnableDarkScrollBars(HWND h) { SetWindowTheme(h, L"DarkMode_Explorer", nullptr); }
} // namespace darkmode

// ----------------------------------------------------------- Direct2D core
namespace d2d {
static ID2D1Factory*        g_factory = nullptr;
static IDWriteFactory*      g_dwrite  = nullptr;
static ID2D1DCRenderTarget* g_dcRT    = nullptr;

bool EnsureInit() {
    if (g_factory && g_dwrite && g_dcRT) return true;
    if (!g_factory &&
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_factory)))
        return false;
    if (!g_dwrite &&
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&g_dwrite))))
        return false;
    if (!g_dcRT) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
        if (FAILED(g_factory->CreateDCRenderTarget(&props, &g_dcRT))) return false;
    }
    return true;
}
ID2D1DCRenderTarget* DcTarget() { return g_dcRT; }

IDWriteTextFormat* TextFormat(const wchar_t* family, float sizePx, DWRITE_FONT_WEIGHT weight) {
    static std::map<std::wstring, IDWriteTextFormat*> cache;
    std::wstring key = std::wstring(family) + L"|" +
                       std::to_wstring((int)(sizePx * 10)) + L"|" +
                       std::to_wstring((int)weight);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    IDWriteTextFormat* fmt = nullptr;
    HRESULT hr = g_dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, sizePx, L"en-us", &fmt);
    if (FAILED(hr))
        g_dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, sizePx, L"en-us", &fmt);
    cache[key] = fmt;
    return fmt;
}
} // namespace d2d

// ------------------------------------------------------------- painters
static float DpiScale(HWND h) {
    UINT dpi = GetDpiForWindow(h);
    float s = (dpi ? dpi : 96) / 96.f;
    return s <= 0.f ? 1.f : s;
}

static void PaintButton(HWND h, bool primary, float hover) {
    const Theme& T = DarkTheme();
    RECT rc; GetClientRect(h, &rc);
    float scale = DpiScale(h);
    PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
    if (!d2d::EnsureInit()) { EndPaint(h, &ps); return; }
    ID2D1DCRenderTarget* rt = d2d::DcTarget();
    if (FAILED(rt->BindDC(hdc, &rc))) { EndPaint(h, &ps); return; }
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(T.windowBg);

    LRESULT bst = SendMessageW(h, BM_GETSTATE, 0, 0);
    bool pressed  = (bst & BST_PUSHED) != 0;
    bool focused  = (bst & BST_FOCUS)  != 0;
    bool disabled = !IsWindowEnabled(h);

    float inset = 1.f * scale;
    D2D1_RECT_F r = D2D1::RectF(inset, inset, rc.right - inset, rc.bottom - inset);
    float radius = 6.f * scale;
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, radius, radius);

    D2D1_COLOR_F fill, txtc;
    if (primary) {
        fill = lerp(T.accent, T.accentHover, hover);
        if (pressed) fill = mul(fill, 0.85f);
        txtc = T.accentText;
    } else {
        fill = pressed ? T.btnFacePressed : lerp(T.btnFace, T.btnFaceHover, hover);
        txtc = T.textPrimary;
    }
    if (disabled) { fill = mul(fill, 0.55f); txtc = mul(txtc, 0.55f); }

    ID2D1SolidColorBrush* b = nullptr;
    rt->CreateSolidColorBrush(fill, &b);
    if (b) rt->FillRoundedRectangle(rr, b);
    if (!primary) {
        ID2D1SolidColorBrush* bb = nullptr;
        rt->CreateSolidColorBrush(T.btnBorder, &bb);
        if (bb) { rt->DrawRoundedRectangle(rr, bb, 1.f * scale); bb->Release(); }
    }
    if (focused) {
        ID2D1SolidColorBrush* fb = nullptr;
        rt->CreateSolidColorBrush(primary ? T.accentText : T.accent, &fb);
        if (fb) {
            float fi = 2.5f * scale;
            D2D1_ROUNDED_RECT fr = D2D1::RoundedRect(
                D2D1::RectF(r.left + fi, r.top + fi, r.right - fi, r.bottom - fi),
                radius - fi, radius - fi);
            rt->DrawRoundedRectangle(fr, fb, 1.5f * scale);
            fb->Release();
        }
    }
    if (b) b->Release();

    wchar_t txt[256] = L"";
    GetWindowTextW(h, txt, 256);
    IDWriteTextFormat* fmt = d2d::TextFormat(L"Segoe UI Variable", 14.f * scale, DWRITE_FONT_WEIGHT_NORMAL);
    if (fmt) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ID2D1SolidColorBrush* tb = nullptr;
        rt->CreateSolidColorBrush(txtc, &tb);
        if (tb) { rt->DrawText(txt, (UINT32)lstrlenW(txt), fmt, r, tb); tb->Release(); }
    }
    rt->EndDraw();
    EndPaint(h, &ps);
}

static void PaintStaticText(HWND h) {
    const Theme& T = DarkTheme();
    RECT rc; GetClientRect(h, &rc);
    float scale = DpiScale(h);

    bool instruction = false;
    if (HFONT hf = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0)) {
        LOGFONTW lf{};
        if (GetObjectW(hf, sizeof(lf), &lf) && lf.lfWeight >= FW_SEMIBOLD) instruction = true;
    }

    PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
    if (!d2d::EnsureInit()) { EndPaint(h, &ps); return; }
    ID2D1DCRenderTarget* rt = d2d::DcTarget();
    if (FAILED(rt->BindDC(hdc, &rc))) { EndPaint(h, &ps); return; }
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(T.windowBg);

    wchar_t txt[1024] = L"";
    GetWindowTextW(h, txt, 1024);
    float size          = instruction ? 15.f * scale : 13.f * scale;
    DWRITE_FONT_WEIGHT w = instruction ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
    const wchar_t* fam   = instruction ? L"Segoe UI Variable Display" : L"Segoe UI Variable Text";
    IDWriteTextFormat* fmt = d2d::TextFormat(fam, size, w);
    D2D1_COLOR_F col = instruction ? T.textPrimary : T.textSecondary;
    if (fmt) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        ID2D1SolidColorBrush* tb = nullptr;
        rt->CreateSolidColorBrush(col, &tb);
        if (tb) {
            D2D1_RECT_F r = D2D1::RectF(0, 0, (float)rc.right, (float)rc.bottom);
            rt->DrawText(txt, (UINT32)lstrlenW(txt), fmt, r, tb);
            tb->Release();
        }
    }
    rt->EndDraw();
    EndPaint(h, &ps);
}

static void PaintStaticIcon(HWND h) {
    const Theme& T = DarkTheme();
    RECT rc; GetClientRect(h, &rc);
    PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
    if (!d2d::EnsureInit()) { EndPaint(h, &ps); return; }
    ID2D1DCRenderTarget* rt = d2d::DcTarget();
    if (FAILED(rt->BindDC(hdc, &rc))) { EndPaint(h, &ps); return; }
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->Clear(T.windowBg);

    float box = (float)((rc.right < rc.bottom) ? rc.right : rc.bottom);
    IDWriteTextFormat* fmt = d2d::TextFormat(L"Segoe Fluent Icons", box * 0.92f, DWRITE_FONT_WEIGHT_NORMAL);
    if (fmt) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ID2D1SolidColorBrush* tb = nullptr;
        rt->CreateSolidColorBrush(T.warning, &tb);
        if (tb) {
            const wchar_t glyph[2] = { 0xE7BA, 0 };   // Segoe Fluent Icons: Warning (triangle)
            D2D1_RECT_F r = D2D1::RectF(0, 0, (float)rc.right, (float)rc.bottom);
            rt->DrawText(glyph, 1, fmt, r, tb);
            tb->Release();
        }
    }
    rt->EndDraw();
    EndPaint(h, &ps);
}

// ---------------------------------------------------------- subclass procs
struct BtnState  { float hover = 0.f, target = 0.f; bool primary = false; bool tracking = false; };
struct DlgState  { HBRUSH bg = nullptr; };

static const UINT_PTR ID_BTN   = 0xB01;
static const UINT_PTR ID_TEXT  = 0xB02;
static const UINT_PTR ID_ICON  = 0xB03;
static const UINT_PTR ID_DLG   = 0xB04;
static const UINT_PTR ANIM_TMR = 0xA01;

static LRESULT CALLBACK ButtonProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    BtnState* s = reinterpret_cast<BtnState*>(ref);
    switch (m) {
    case WM_NCDESTROY: RemoveWindowSubclass(h, ButtonProc, id); delete s; break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintButton(h, s->primary, s->hover); return 0;
    case WM_MOUSEMOVE:
        if (!s->tracking) {
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            s->tracking = true; s->target = 1.f;
            SetTimer(h, ANIM_TMR, 15, nullptr);
        }
        break;
    case WM_MOUSELEAVE:
        s->tracking = false; s->target = 0.f;
        SetTimer(h, ANIM_TMR, 15, nullptr);
        break;
    case WM_TIMER:
        if (w == ANIM_TMR) {
            float d = s->target - s->hover, step = 0.18f;
            if (fabsf(d) <= step) { s->hover = s->target; KillTimer(h, ANIM_TMR); }
            else s->hover += (d > 0 ? step : -step);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_SETFOCUS:    case WM_KILLFOCUS:
        InvalidateRect(h, nullptr, FALSE);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK StaticTextProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    switch (m) {
    case WM_NCDESTROY: RemoveWindowSubclass(h, StaticTextProc, id); break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintStaticText(h); return 0;
    }
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK StaticIconProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    switch (m) {
    case WM_NCDESTROY: RemoveWindowSubclass(h, StaticIconProc, id); break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintStaticIcon(h); return 0;
    }
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK DialogChromeProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
    DlgState* s = reinterpret_cast<DlgState*>(ref);
    switch (m) {
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, DialogChromeProc, id);
        if (s->bg) DeleteObject(s->bg);
        delete s;
        break;
    case WM_ERASEBKGND: {
        HDC dc = (HDC)w; RECT rc; GetClientRect(h, &rc);
        FillRect(dc, &rc, s->bg);
        return 1;
    }
    case WM_CTLCOLORDLG:
        return (LRESULT)s->bg;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)w;
        SetTextColor(dc, RGB(240, 240, 240));
        SetBkColor(dc, DarkTheme().windowBgRef);
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)s->bg;
    }
    }
    return DefSubclassProc(h, m, w, l);
}

// ------------------------------------------------------------ public API
void SubclassButton(HWND h, bool primary) {
    BtnState* s = new BtnState();
    s->primary = primary;
    SetWindowSubclass(h, ButtonProc, ID_BTN, (DWORD_PTR)s);
    InvalidateRect(h, nullptr, TRUE);
}
void SubclassStaticText(HWND h) {
    SetWindowSubclass(h, StaticTextProc, ID_TEXT, 0);
    InvalidateRect(h, nullptr, TRUE);
}
void SubclassStaticIcon(HWND h) {
    SetWindowSubclass(h, StaticIconProc, ID_ICON, 0);
    InvalidateRect(h, nullptr, TRUE);
}
void SubclassDialogChrome(HWND h) {
    DlgState* s = new DlgState();
    s->bg = CreateSolidBrush(DarkTheme().windowBgRef);
    SetWindowSubclass(h, DialogChromeProc, ID_DLG, (DWORD_PTR)s);
}

static BOOL CALLBACK EnumChildProc(HWND c, LPARAM) {
    wchar_t cls[64] = L"";
    GetClassNameW(c, cls, 64);
    LONG style = GetWindowLongW(c, GWL_STYLE);
    if (_wcsicmp(cls, L"Button") == 0) {
        LONG type = style & 0x0F;   // BS_* base type
        if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON)
            SubclassButton(c, type == BS_DEFPUSHBUTTON);
        else
            darkmode::AllowForWindow(c, true);   // checkbox/radio/group -> dark theme
    } else if (_wcsicmp(cls, L"Static") == 0) {
        if ((style & 0x1F) == SS_ICON)
            SubclassStaticIcon(c);
        else
            SubclassStaticText(c);
    } else {
        darkmode::AllowForWindow(c, true);
        darkmode::EnableDarkScrollBars(c);
    }
    return TRUE;
}

void ModernizeDialog(HWND hDlg, bool isTaskDialog) {
    darkmode::Init();
    darkmode::AllowForWindow(hDlg, true);

    dwm::EnableDarkTitleBar(hDlg, true);
    dwm::EnableRoundedCorners(hDlg);
    dwm::SetCaptionColor(hDlg, DarkTheme().windowBgRef);
    dwm::SetBorderColor(hDlg, DarkTheme().windowBgRef);

    SubclassDialogChrome(hDlg);
    EnumChildWindows(hDlg, EnumChildProc, 0);

    // TaskDialog content lives in a DirectUIHWND that paints itself; standard
    // Static subclassing can't reach it. Force process dark mode and re-theme
    // the DirectUI host so comctl32 repaints its background/text dark.
    if (isTaskDialog) {
        darkmode::ForceDark();
        if (HWND dui = FindWindowExW(hDlg, nullptr, L"DirectUIHWND", nullptr)) {
            darkmode::AllowForWindow(dui, true);
            SetWindowTheme(dui, L"DarkMode_Explorer", nullptr);
            SendMessageW(dui, WM_THEMECHANGED, 0, 0);
        }
        SendMessageW(hDlg, WM_THEMECHANGED, 0, 0);
    }

    SetWindowPos(hDlg, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    RedrawWindow(hDlg, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASE);
}

} // namespace fluent
