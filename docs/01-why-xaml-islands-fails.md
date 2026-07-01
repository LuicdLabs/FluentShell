# 01 — 為什麼 XAML Islands 全域注入行不通

`xaml islands.txt` 的核心論點正確：**XAML Islands 是給開發者「重構自己的程式」用的，不是給工具「全域換膚黑盒程式」用的。** 本節整理技術根據，並補上查證出處。

## 1. 繪製與控制項的不可逆性

傳統 Win32/GDI 程式的邏輯是：算出按鈕狀態 → 呼叫 `DrawText`/`FillRect` 畫到螢幕。XAML 控件的邏輯是：建物件樹 → 綁事件 → 交給 Composition 引擎渲染。

若用注入攔截 `DrawText`，你拿到的只是「已算好的字串與座標」，**無法逆推出「這是一個按鈕、需要綁 Click 事件」**。硬在該座標蓋一個透明 WinUI 按鈕，底層 Win32 視窗仍會收到 `WM_PAINT` 把舊 GDI 按鈕畫在你的按鈕上下方，造成閃爍與破圖。這也是本專案**不覆蓋、而是攔截 `WM_PAINT` 直接重繪原控件**的原因（見 [04](04-control-repaint-direct2d.md)）。

## 2. Airspace（空域）限制

XAML Islands 依賴獨立的 HWND 子視窗（`Windows.UI.Composition` / `DesktopWindowXamlSource`）承載視覺樹，因此有一塊實體「矩形空域」。在該空域內，底層 GDI 繪製會被完全遮蔽，無法和周圍 GDI 內容像素級混排。對「不能重新編譯」的舊程式，這是無解的排版災難——你不能把一個 `DataGridView` 的捲動軸單獨換成 WinUI 捲動軸而保留其餘 GDI 內容。

> Airspace 是 WPF/WinForms 互操作年代就有的老問題；XAML Islands 的 host 是獨立合成表面，沿用同樣的矩形邊界限制。

## 3. WinUI 2 vs WinUI 3 的混淆與承載成本

- **WinUI 2（系統 XAML，`Windows.UI.Xaml`）** 還能靠 `WindowsXamlManager` 在既有進程勉強偷渡 UWP 上下文。
- **WinUI 3（Windows App SDK，`Microsoft.UI.Xaml`）** 是完全解耦、龐大的原生執行時。要在**不改原始碼**下注入 DLL 去引導 WinUI 3 執行時、初始化 XAML 元素樹、接管現代化訊息迴圈，極易記憶體洩漏與崩潰。微軟在 Windows App SDK 1.4 才把 XAML Islands（`Microsoft.UI.Xaml.Hosting`）做穩定，正說明這套機制本身脆弱。

把兩者混為一談、以為能「全域自動注入 WinUI 3」，是不切實際的。

## 4. 微軟自己怎麼做？

Windows 11 的 `explorer.exe` 確實用 XAML Islands 換掉了檔案總管部分 UI——但那是**微軟改自己的程式碼**，他們知道確切的 HWND 結構與訊息流。外部工具面對任意黑盒程式沒有這種先驗知識。

## 結論：改走視覺層攔截與重繪

既然控件級替換行不通，務實做法是**捨棄「控制項替換」，改做「視覺層攔截與重繪」**：

- **DWM 屬性**改視窗外框（深色標題列、圓角、Mica）——見 [02](02-dwm-visual-layer.md)。
- **未公開 uxtheme 深色**讓標準控件（捲動軸、選單、Edit）免費變深色——見 [02](02-dwm-visual-layer.md)。
- **Direct2D/DirectWrite** 攔截 `WM_PAINT` 重繪按鈕、文字、圖示——見 [04](04-control-repaint-direct2d.md)。
- **使用者態 Hook/注入**把上述能力送進黑盒程式——見 [03](03-injection-and-hooking.md)。

本專案的實測（[05](05-implementation-and-test-results.md)）證明這條路可行：一個 `#32770` 對話框可在完全沒有原始碼、沒有 XAML Islands 的情況下被轉成 Fluent Design。

## 出處

- Airspace / XAML Islands host 限制、Windows App SDK 1.4 穩定化：[Microsoft Learn — XAML Islands](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/xaml-islands)
- WinUI 版本差異：[Microsoft Learn — WinUI](https://learn.microsoft.com/en-us/windows/apps/winui/)
- 系統級美化的視覺層做法參考：[valinet/ExplorerPatcher（DeepWiki）](https://deepwiki.com/valinet/ExplorerPatcher)
