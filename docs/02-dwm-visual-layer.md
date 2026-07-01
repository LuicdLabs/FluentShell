# 02 — DWM 視覺層現代化

`DwmSetWindowAttribute` 是把舊視窗「一鍵現代化外框」最快的手段：不需自己畫非客戶區，直接請 DWM 給深色標題列、圓角、Mica。以下所有屬性在本機（build 26200）都實測回傳 `S_OK`。

## 屬性表（`DWMWINDOWATTRIBUTE`）

| 屬性 | 值 | 用途 | 最低 build |
| :--- | :---: | :--- | :---: |
| `DWMWA_USE_IMMERSIVE_DARK_MODE` | 20 | 深色標題列 / 非客戶區 | 22000（Win10 部分可用） |
| `DWMWA_WINDOW_CORNER_PREFERENCE` | 33 | 圓角（`DWMWCP_ROUND=2`、`DWMWCP_ROUNDSMALL=3`、`DWMWCP_DONOTROUND=1`） | 22000 |
| `DWMWA_BORDER_COLOR` | 34 | 邊框顏色（`COLORREF`） | 22000 |
| `DWMWA_CAPTION_COLOR` | 35 | 標題列顏色 | 22000 |
| `DWMWA_TEXT_COLOR` | 36 | 標題文字色 | 22000 |
| `DWMWA_SYSTEMBACKDROP_TYPE` | 38 | 系統背景材質 | 22621 |

`DWM_SYSTEMBACKDROP_TYPE`：`DWMSBT_AUTO=0`、`DWMSBT_NONE=1`、`DWMSBT_MAINWINDOW=2`（Mica）、`DWMSBT_TRANSIENTWINDOW=3`（Acrylic）、`DWMSBT_TABBEDWINDOW=4`（Mica Alt）。

## 本專案封裝（`src/common/FluentCore.cpp` → `namespace dwm`）

```cpp
void EnableDarkTitleBar(HWND h, bool dark) {
    BOOL b = dark; DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &b, sizeof(b));
}
void EnableRoundedCorners(HWND h) {
    DWM_WINDOW_CORNER_PREFERENCE p = DWMWCP_ROUND;
    DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &p, sizeof(p));
}
bool SetMicaBackdrop(HWND h) {                 // 先擋 build，再套用
    if (BuildNumber() < 22621) return false;
    DWM_SYSTEMBACKDROP_TYPE bt = DWMSBT_MAINWINDOW;
    return SUCCEEDED(DwmSetWindowAttribute(h, DWMWA_SYSTEMBACKDROP_TYPE, &bt, sizeof(bt)));
}
```

`BuildNumber()` 用 `ntdll!RtlGetVersion`（不受相容性外殼欺騙）取得真實 build 號來守門。

### Accent 取色

主按鈕的強調色直接取系統色：`DwmGetColorizationColor(&argb, &opaque)`（回傳 `0xAARRGGBB`），失敗才退回 Fluent 藍 `#0078D4`。並依亮度決定字色（黑/白）以保證對比。

## Mica 透出的「空域/不透明方塊」問題

`dwm method 1.txt` 正確指出：把視窗背景設成 Mica，但按鈕若是原本的 GDI Button，會帶不透明方塊背景擋住 Mica。要透出需：

1. `DwmExtendFrameIntoClientArea(hwnd, {-1,-1,-1,-1})`（sheet of glass），且
2. 攔 `WM_ERASEBKGND` / `WM_CTLCOLORDLG` 讓客戶區**不要**用 `COLOR_BTNFACE` 填成不透明，並讓需要透出的控件以 alpha 合成。

**本專案的務實取捨**：目標圖片 `Untitled.png` 右側其實是**不透明深色**（並非半透明看到桌面），因此預設採「純深色背景 + 深色標題列 + 圓角 + Fluent 重繪控件」，用不透明的 `ID2D1DCRenderTarget`（alpha ignore）重繪控件——**可靠、無閃爍、且精準對齊目標圖**。Mica 透出列為進階選項（`SetMicaBackdrop`）：對簡單對話框可開，但要讓 GDI 控件區真正透出需要把每個控件都改成有 alpha 的合成表面，屬於路線圖項目（見 [06](06-risks-limitations-roadmap.md)）。

## 深色標準控件（未公開 uxtheme）

DWM 只管外框；客戶區裡的捲動軸、右鍵選單、Edit 邊框要變深色，得靠 `uxtheme.dll` 的**未公開序號**（`ysc3839/win32-darkmode` 為公認參考）：

| 函式 | 序號 | 簽章 |
| :--- | :---: | :--- |
| `RefreshImmersiveColorPolicyState` | 104 | `void()` |
| `ShouldAppsUseDarkMode` | 132 | `bool()` |
| `AllowDarkModeForWindow` | 133 | `bool(HWND, bool)` |
| `SetPreferredAppMode`（新）/`AllowDarkModeForApp`（舊） | 135 | `PreferredAppMode(PreferredAppMode)` |
| `FlushMenuThemes` | 136 | `void()` |

`enum PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };`

用法（`namespace darkmode`）：以 `GetProcAddress(uxtheme, MAKEINTRESOURCEA(ord))` 取得後，`SetPreferredAppMode(AllowDark)` → `AllowDarkModeForWindow(hwnd,true)` → `RefreshImmersiveColorPolicyState()` → `FlushMenuThemes()`，再對窗發 `WM_THEMECHANGED`。標準控件另可 `SetWindowTheme(h, L"DarkMode_Explorer", nullptr)` 取得深色捲動軸。

> ⚠️ 這些是未公開 API，序號可能隨 Windows 版本變動，正式產品需做版本/序號防護與退場。

## 出處

- [DWMWINDOWATTRIBUTE (dwmapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute)
- [DWM_SYSTEMBACKDROP_TYPE](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type)
- [win32-darkmode（未公開 uxtheme 序號）](https://github.com/ysc3839/win32-darkmode/blob/master/win32-darkmode/DarkMode.h)
- [DwmGetColorizationColor](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcolorizationcolor)
