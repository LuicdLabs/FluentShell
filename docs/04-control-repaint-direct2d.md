# 04 — Direct2D/DirectWrite 控件重繪

DWM 管外框、uxtheme 管標準控件深色；真正把按鈕/文字/圖示變成 Fluent 樣式的「破綻消除」，靠**子類化 + Direct2D/DirectWrite 攔截 `WM_PAINT` 重繪**。

## 子類化（`SetWindowSubclass`）

用 `comctl32` 的 `SetWindowSubclass`（可帶每實例 ref data，於 `WM_NCDESTROY` 清理）而非 `SetWindowLongPtr`，因為前者可安全鏈式、易還原。每個目標控件裝一個子類化 WndProc，攔 `WM_PAINT`（完全接管繪製）與 `WM_ERASEBKGND`（回 1 防閃），其餘訊息 `DefSubclassProc` 放行——**只覆寫繪製，保留原本點擊/預設鍵/焦點行為**。

## Direct2D DC render target（適合子類化的關鍵選擇）

子類化任意控件時，最方便的是把 Direct2D 綁到 `BeginPaint` 給的 `HDC`：

```cpp
D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
    D2D1_RENDER_TARGET_TYPE_DEFAULT,
    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
    0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
factory->CreateDCRenderTarget(&props, &g_dcRT);
// 每次繪製：
g_dcRT->BindDC(hdc, &rc); g_dcRT->BeginDraw(); ... g_dcRT->EndDraw();
```

`ID2D1Factory` / `IDWriteFactory` / `IDWriteTextFormat` / 筆刷全在模組層建立一次共用；`ID2D1DCRenderTarget` 也共用、每次 `BindDC` 重綁。`alpha=IGNORE`（不透明）搭配純深色背景，可靠且對齊目標圖。

> ⚠️ **編譯陷阱**：`windows.h` 把 `DrawText` 定義成 `DrawText→DrawTextW` 巨集；若在它生效時解析 `d2d1.h`，會把 `ID2D1RenderTarget::DrawText` 改名成 `DrawTextW`，導致 `rt->DrawText(...)` 報「不是成員」。解法：在 `#include <d2d1.h>` **之前** `#undef DrawText`（本專案已在 `FluentCore.h` 處理）。

## 按鈕（classic 與 TaskDialog 共用）

`WM_PAINT` → `ID2D1DCRenderTarget`：

- 背景圓角矩形（半徑 `6*scale`）。**主按鈕**（`BS_DEFPUSHBUTTON`）填 Accent；**次按鈕**填深灰 + 邊框。
- 狀態：`BM_GETSTATE` 讀 `BST_PUSHED`（按下變暗）、`BST_FOCUS`（畫焦點環）；`IsWindowEnabled` 為禁用變暗。
- **懸停動畫**：`WM_MOUSEMOVE` 起 `TrackMouseEvent(TME_LEAVE)`，設 15ms `SetTimer`，把 `hover` 值以緩動趨近 0/1，`InvalidateRect` 重繪，穩定後 `KillTimer`；填色在 base 與 hover 色間 `lerp`。
- 文字：`GetWindowText` → DirectWrite `Segoe UI Variable` 置中。

## 靜態文字 / 圖示（classic 路徑）

- **文字**：以 `WM_GETFONT` 讀 `LOGFONT.lfWeight`，`>= FW_SEMIBOLD` 視為「主指示」（`Segoe UI Variable Display`、較大、亮白）；否則為「內文」（`Segoe UI Variable Text`、次要灰）。左對齊、自動換行、深色底。
- **圖示**（`Static` + `SS_ICON`）：丟棄舊 32×32 ICO，改用 **Segoe Fluent Icons** 字型繪製警告三角形。

### 實測校正：警告三角形是 `U+E7BA`

初版用資料表宣稱的 `U+EA84`，實測**渲染成細的驚嘆號、非三角形**；改用 `U+E7BA` 後正確得到 Fluent 警告三角形（本專案螢幕實測確認）。其他常用碼位：Info `U+E946`、Error `U+E783`、Close `U+E8BB`。

## 對話框底色（`SubclassDialogChrome`）

子類化頂層 `#32770`，攔 `WM_ERASEBKGND`（用深色 brush 填滿）、`WM_CTLCOLORDLG` / `WM_CTLCOLORSTATIC` / `WM_CTLCOLORBTN`（回深色 brush + `SetTextColor` 亮色 + `SetBkMode(TRANSPARENT)`）。這讓「沒被逐一重繪的」控件也有深色底、文字可讀，是低風險的深色基線；Direct2D 重繪再疊上「有 wow」的按鈕/圖示。

## TaskDialog 的限制（實測）

TaskDialog 頂層是 `#32770`，內含 `DirectUIHWND`：

- **按鈕是真的 `Button` HWND**（`EnumChildWindows` 會遞迴找到，含巢狀）→ **可子類化重繪，成功**。
- **圖示與主指示/內文是 DirectUI 內部繪製的 label**，不是 `Static` 子視窗 → 標準子類化到不了。
- 實測：即使注入後 `SetPreferredAppMode(ForceDark)` + 對 `DirectUIHWND` `SetWindowTheme(DarkMode_Explorer)` + 廣播 `WM_THEMECHANGED`，**已建立的** TaskDialog 內文仍維持亮色（File Explorer 的深色 TaskDialog 之所以成立，是因為進程一開始就處於深色模式、對話框建立時即套用）。

結論：**classic `#32770` 可完整轉換；TaskDialog 得到深色邊框 + 現代化真實按鈕，DirectUI 內文為已知限制**（見 [05](05-implementation-and-test-results.md) 截圖與 [06](06-risks-limitations-roadmap.md) 路線圖）。

## DPI

各繪製以 `GetDpiForWindow(h)/96` 取 `scale`，半徑、字級、內距皆乘 `scale`（render target 維持 96 DPI，座標用實體像素）。舊程式多為 System-DPI-aware，強改字級需嚴謹 DPI 數學以免裁切。

## 出處

- [Create a simple Direct2D application](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-quickstart)
- [SetWindowSubclass](https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-setwindowsubclass)
- [Segoe Fluent Icons font](https://learn.microsoft.com/en-us/windows/apps/design/iconography/segoe-fluent-icons-font)
- [TaskDialogIndirect](https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-taskdialogindirect)、[DirectUIHWND 說明（pywinauto #787）](https://github.com/pywinauto/pywinauto/issues/787)
