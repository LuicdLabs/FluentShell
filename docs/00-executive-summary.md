# 00 — 執行摘要

## 目標

把圖中（`Untitled.png`）舊版 Windows「System Protection」對話框這類 Win32 `#32770` UI，透過一個 C++ 引擎在**執行期**徹底轉換成現代化的**全域 Fluent Design**（深色、圓角、Mica、Segoe Fluent Icons、Segoe UI Variable、Accent 主按鈕、懸停動畫），並在**本機實際編譯與測試**。

![before/after](images/tier1_before_after.png)

## 核心結論

1. **`xaml islands.txt` 與 `dwm method 1.txt` 的方向是對的。** 用 XAML Islands 對黑盒程式做全域「換膚」不可行（見 [01](01-why-xaml-islands-fails.md)）；唯一務實且不會藍屏的路徑是 **DWM 屬性 + 使用者態 Hook/注入 + Direct2D/DirectWrite 重繪**，這正是 WindowBlinds / ExplorerPatcher / Rectify11 的底層原理。

2. **本機完全可行且已實測。** Windows 11 25H2（build **26200**）支援全部現代 DWM 效果；Visual Studio 2026（MSVC 14.51）+ Windows SDK 10.0.26100 可編譯 DWM/Direct2D/DirectWrite/DirectComposition。五個 C++ 產物全部建置成功並通過測試。

3. **三階遞進全部驗證通過：**

| Tier | 驗證內容 | 結果 |
| :--- | :--- | :--- |
| 1 | 自包含 before/after（重現圖片） | ✅ 完整轉換 |
| 2 | 目標式 DLL 注入到獨立黑盒程式 | ✅ classic `#32770` 完整轉換；TaskDialog 邊框+真實按鈕轉換 |
| 3 | 全域 `WH_CBT` 勾（未直接注入的程式） | ✅ 自動轉換 |
| 3 | **真實系統程序** `SystemPropertiesProtection.exe` | ✅ 注入成功，按鈕以注入的 Direct2D 重繪 |

## 關鍵技術發現（可直接落地）

- **DWM 屬性值**（build 26200 全可用）：`DWMWA_USE_IMMERSIVE_DARK_MODE=20`、`DWMWA_WINDOW_CORNER_PREFERENCE=33`（`DWMWCP_ROUND=2`）、`DWMWA_CAPTION_COLOR=35`、`DWMWA_BORDER_COLOR=34`、`DWMWA_SYSTEMBACKDROP_TYPE=38`（`DWMSBT_MAINWINDOW=2`=Mica）。
- **未公開 uxtheme 深色序號**：`RefreshImmersiveColorPolicyState`(104)、`ShouldAppsUseDarkMode`(132)、`AllowDarkModeForWindow`(133)、`SetPreferredAppMode`(135)、`FlushMenuThemes`(136)。
- **Segoe Fluent Icons 警告三角形 = `U+E7BA`**（本專案實測確認；`U+EA84` 是細的驚嘆號，非三角形）。
- **注入的致命細節**：WinEvent/Windows Hook 的生命週期綁在**安裝它的執行緒**上；不能在 `CreateRemoteThread` 跑的 `DllMain` 暫態執行緒裡安裝，否則執行緒一結束勾就被移除——必須改用常駐執行緒（本專案踩過並修正）。
- **目標對話框結構**：圖中是 **TaskDialog**（頂層 `#32770` 內含 `DirectUIHWND`）。它的**按鈕是真的 `Button` HWND**（可子類化重繪），但圖示與文字由 DirectUI 內部繪製（標準 `Static` 子類化到不了）——這決定了 classic 與 TaskDialog 兩種轉換策略與各自的完成度。

## 安全與可還原

全程**使用者態、可還原、不持久化、不做破壞性動作**：不改 `win32k.sys`（避開 PatchGuard）、不改系統檔（避開 SFC）、不動 `AppInit_DLLs`。Tier 3 測試對真實系統程序只「看樣式」，破壞性的刪除還原點確認框一律 Cancel；測試後確認**還原點完好無損**。

各細節見 [02](02-dwm-visual-layer.md)–[06](06-risks-limitations-roadmap.md) 與 [實測結果](05-implementation-and-test-results.md)。
