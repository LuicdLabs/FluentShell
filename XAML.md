# 以 XAML Islands / WinUI 現代化舊版 Windows 程式與系統組件
## 可行性研究與完整實施方案

> **歷史研究文件。** 本文記錄已放棄的 in-process Island/overlay 方向，並非目前
> production 架構或操作指南。現行實作以 `README.md` 與
> `docs/goals/win32-to-winui-translation/PLAN.md` 為準：native HWND 是 truth owner，
> WinUI 3 僅在獨立的 `FluentShell.Renderer.exe` process 中執行。

> 版本：v1.0 ｜ 日期：2026-07-17
> 目標平台：Windows 10 1903 (18362) ～ Windows 11 24H2/25H2，x64 / ARM64
> 本文用於回應「直接注入 XAML Islands 用 WinUI 行不行」這個問題，並據此制定一整套「外觀轉換器」的完整方案。

---

## 一、執行摘要（先講結論）

**「直接注入 XAML Islands 用 WinUI」這句話需要拆成四個層次回答：**

| 層級 | 做法 | 可行性 | 一句話結論 |
|---|---|---|---|
| L0 | **不注入**：跨進程設定 DWM 屬性（深色標題列、Mica/壓克力、圓角）＋ 進程外覆蓋層 | ✅ 成熟可行 | 成本低、風險低，能完成截圖中 60% 的視覺效果 |
| L1 | **注入但只攔截繪製**：Detours 攔 TaskDialog/uxtheme/comctl32，自繪 Fluent 風格 | ✅ 可行（有先例） | 相容性最好，是轉換器套件的主力方案 |
| L2 | **注入 + 內嵌 XAML Island / Composition Island**：在目標進程內真的跑 XAML | ⚠️ 有條件可行 | 「直接注入」官方不支援，但工程上可繞過；本計畫核心 |
| L3 | **修改系統核心組件**（comctl32、shell32、dwm 等檔案級修補） | ❌ 不建議直接改檔 | 受 WRP/TrustedInstaller/每月累積更新夾殺；應改以「主題包 + 記憶體修補 + 注入平台」實現同等效果 |

**對原問題的精確回答：**

1. **「XAML Islands + WinUI 2（系統 XAML）」**——在你自己擁有的進程裡是官方支援的；注入到**第三方/系統進程**屬於非支援情境，但技術上可以透過 Dynamic Dependency API 取得套件身分後載入 Microsoft.UI.Xaml，能跑起來。障礙在執行緒模型、套件身分、XAML 框架版本共存與生命週期管理，全部有解但工程量大。
2. **「WinUI 3 的 XAML Islands」**——穩定通道長期不支援；微軟的替代路線是 **Content Islands**（`Microsoft.UI.Content.ContentIsland` + `DesktopChildSiteBridge`），且在 Windows App SDK 2.0 實驗通道中已出現正式的 `Microsoft.UI.Xaml.XamlIsland` 控制項、`ContentIsland.CreateForSystemVisual`、`DesktopPopupSiteBridge`、`SystemBackdropHost` 等 API[^34^]。也就是說「WinUI 3 版 XAML Island」正在成形的路上，但現在（穩定通道為 1.8.x[^25^]）不能當作生產依賴。
3. 因此本方案採取**混合架構**：L0/L1 打底保證覆蓋率與穩定性，L2 作為視覺上限（旗艦效果），L3 改用安全等效路線（msstyles 主題 + uxtheme 記憶體修補 + Windhawk 式注入平台），**不做永久性系統檔案替換**。

---

## 二、背景、目標與範圍

### 2.1 目標

打造一套**外觀轉換器套件（Converter Suite）**，讓以下類型的程式在使用者不修改原始碼的前提下，呈現 Fluent Design（深色模式、Mica/壓克力材質、圓角、現代控制項樣式、Segoe UI Variable 字體）：

- 純 Win32 / comctl32 v6 程式（含系統內建：`mmc.exe`、`eventvwr`、`regedit`、控制台 `.cpl`、`#32770` 對話框、Run 對話框）
- GDI/GDI+ 自繪程式
- MFC 程式（含 MFC Feature Pack 視覺管理員）
- Windows Forms（.NET Framework 2.x–4.8、.NET 6–9）
- WPF（.NET Framework / .NET Core+）
- 系統對話框：`TaskDialog` / `MessageBox` / 檔案通用對話框（comdlg32）

效果參考（即本專案附圖）：左側為傳統淺色對話框，右側為深色 Fluent 化後——標題列深色化、按鈕圓角、主體深色、強調色按鈕。這個效果**正好落在 L0+L1 可完整達成的範圍內**（這三個對話框本質上都是 comctl32 TaskDialog 或 `#32770` 對話框，是攔截重繪的最佳目標）。

### 2.2 非目標（明確排除）

- 不修改開機程序、不動核心（無 PatchGuard 問題）、不需要自製驅動
- 不處理 Direct3D/DirectDraw 全螢幕獨佔程式（遊戲）
- 不保證對「自備整套自繪引擎且不走 comctl32/uxtheme」的程式（如 Chrome、Electron、Qt 自繪）有完美效果——這類只給 L0 級（標題列 + 材質）

---

## 三、可行性論證

### 3.1 XAML Islands 運作原理（精確拆解）

XAML Islands（Windows 10 1903+）的官方架構由兩個物件構成：

1. **`WindowsXamlManager`**——每個**執行緒**一個，負責初始化該執行緒的 XAML 框架（`DispatcherQueue`、資源系統、輸入來源）。透過 `WindowsXamlManager::InitializeForCurrentThread()` 建立。
2. **`DesktopWindowXamlSource`**——一個 HWND 級的「XAML 島」。內部會建立自己的中繼 HWND（透過 `IDesktopWindowXamlSourceNative::AttachToWindow` 掛到你提供的父 HWND），並在系統合成器中掛上對應的 Composition 子樹。你把任何 `Windows.UI.Xaml.UIElement` 設給它的 `Content` 即可。

由此可推出對本計畫至關重要的幾個性質：

- **執行緒親和性**：島的內容只能在建立它的執行緒上操作；一個進程可以有多個島、多個 XAML 執行緒，但每個都要自己的 `WindowsXamlManager`。注入他人進程時，你不能假設目標 UI 執行緒適合直接初始化 XAML（例如它可能沒有 `DispatcherQueue`、或是 console 執行緒）——**解法：由注入 DLL 自建一個專用 UI 執行緒跑 XAML，再與目標 HWND 做父子/兄弟掛接**。
- **HWND 本質**：島本質上仍是 HWND 樹＋Composition 視覺樹，所以它能與 Win32 混排，但受 **airspace 規則**約束：重疊區域永遠是「HWND 對 HWND」的 z-order，XAML 島內的內容無法與島外的 GDI 內容交疊混合（彈出視窗/浮動控制項會開獨立頂層 HWND 來繞過）。這決定了我們「整體置換」策略的視覺一致性極佳、但「局部半透明疊加在原控制項上」不可行。
- **套件身分（Package Identity）**：載入 `Microsoft.UI.Xaml`（WinUI 2）需要 framework package。已封裝（MSIX）進程自動有；**未封裝進程**需要 Dynamic Dependency API——Windows App SDK 的 Bootstrapper（`MddBootstrapInitialize`）適用 Win10 1809+，Windows 11 另外提供通用的 `AddPackageDependency`。**注入第三方進程時，由我們的注入 DLL 在目標進程內呼叫這些 API 取得身分，即可合法載入 XAML 框架**——這是 L2 成立的最關鍵一塊拼圖。
- **版本共存**：同一進程混用系統 XAML（`Windows.UI.Xaml`，含 WinUI 2）與 WinUI 3（`Microsoft.UI.Xaml`）有已知衝突；若目標程式本身已載入任一 XAML 框架（例如新版記事本、部分系統殼層元件），必須偵測並改走「同框架注入」或退到 L1。

### 3.2 WinUI 版本現實

| 項目 | 系統 XAML + WinUI 2 | WinUI 3 |
|---|---|---|
| Islands 支援 | ✅ 官方支援（1903+） | ❌ 穩定通道不支援；2.0 實驗通道已出現 `XamlIsland`[^34^] |
| 注入第三方進程 | 非支援但可行（§3.1） | 需走 Content Islands 或自重，風險更高 |
| Fluent 控制項完整度 | 完整（WinUI 2.8） | 完整且最新 |
| 執行期部署 | Microsoft.UI.Xaml framework package | WinAppSDK runtime（或 self-contained，體積大） |
| 本方案角色 | **L2 主力（島內 XAML）** | **觀察項 / 實驗分支** |

結論：現階段「注入 XAML Islands 用 WinUI」**可行的是 WinUI 2**，不是 WinUI 3；而 WinUI 2 的控制項集（NavigationView、TeachingTip、AcrylicBrush、ThemeShadow…）對本計畫的「對話框/控制項現代化」已完全夠用。Content Islands（`ContentIsland`、`DesktopSiteBridge`、`DesktopChildSiteBridge`、以及 2.0 實驗版的 `ContentIsland.CreateForSystemVisual`——把**現有 HWND 的系統視覺**包成 Composition 島再掛 WinUI 內容[^34^]）是微軟欽定的下一代路線，應在架構上預留介面。

### 3.3 DWM / Composition 渲染機制（L0/L1 的技術地基）

要「不換控制項就改外觀」，靠的是 DWM 這一層。關鍵事實：

1. **重導向表面模型**：DWM 開啟時，每個視窗的 GDI 內容畫進離屏 redirection surface，由 DWM 統一合成。**這代表我們改不了別人 surface 裡的像素，但可以控制合成階段的屬性**——這正是 L0 全部手段的理論基礎。
2. **跨進程可設的公開屬性**（有足夠權限時可對他人視窗呼叫 `DwmSetWindowAttribute`）：
   - `DWMWA_USE_IMMERSIVE_DARK_MODE` (20)：深色標題列
   - `DWMWA_WINDOW_CORNER_PREFERENCE` (33)：圓角（Win11 預設已圓）
   - `DWMWA_SYSTEMBACKDROP_TYPE` (38)：整窗 Mica（`DWMSBT_MAINWINDOW`）、Mica Alt（`DWMSBT_TABBEDWINDOW`）、Backdrop Acrylic（`DWMSBT_TRANSIENTWINDOW`）[^35^]——Win11 22H2+
   - `DWMWA_BORDER_COLOR` / `DWMWA_TEXT_COLOR` / `DWMWA_CAPTION_COLOR`
3. **未公開但廣泛使用**：user32 `SetWindowCompositionAttribute` + `ACCENT_POLICY`（`ACCENT_ENABLE_ACRYLICBLURBEHIND`）——TranslucentTB 等大量軟體使用，Win10/11 皆可用。
4. **System Composition Interop**：`Windows.UI.Composition` 可透過 `ICompositorInterop`/`CreateDesktopWindowTarget` 把視覺樹掛到自有 HWND；對**他人視窗**沒有支援的掛接方式，但 2.0 實驗通道的 `ContentIsland.CreateForSystemVisual` 開啟了「包覆現有系統視覺」的官方方向[^34^]。
5. **視覺置換的經典招式**：`DwmRegisterThumbnail`（唯讀投影）、`DwmExtendFrameIntoClientArea`（玻璃延伸到客戶區）、Layered Window（`WS_EX_LAYERED` + `UpdateLayeredWindow`，可強行給他人視窗加透明通道——會破壞其 GDI 混合路徑，慎用）、`DWMWA_CLOAK`（隱藏原窗，配合替身視窗）。
6. **Win11 免費送的部分**：圓角、部分 snap 視覺已是系統預設，L0 要補的主要是深色標題列 + 材質。

**重要推論**：截圖中那三個對話框的右側效果，其標題列深色（`DWMWA_USE_IMMERSIVE_DARK_MODE`）、圓角（系統預設）、按鈕圓角與深色主體（TaskDialog 自繪或置換）可分別由 L0 與 L1 獨立達成，**完全不需要真的把 XAML 跑進 mmc.exe**。

### 3.4 注入第三方進程的可行性矩陣

| 技術 | 說明 | 適用 | 限制 |
|---|---|---|---|
| `SetWindowsHookEx`（WH_CALLWNDPROC / WH_GETMESSAGE / 殼層掛鉤） | 載入同位元 DLL 到所有 GUI 進程 | 全域視窗偵測 + 注入 | 位元數需匹配（x64/ARM64/x86 各編一份）；UIPI 擋低完整性→高完整性 |
| `CreateRemoteThread` + `LoadLibrary` | 經典遠程注入 | 定點目標（mmc、eventvwr） | 需相符權限；EDR 高度關注此行為 |
| IFEO（`Image File Execution Options`） | 對指定 exe 自動掛 VerifierDlls / 除錯器 | 系統組件定點改造 | 需系統管理員；對受保護組件部分受限 |
| App Compat Shim（自訂 Fix，`InjectDLL`） | 微軟官方相容性基礎設施 | 對特定 exe 的「合法」注入 | 需安裝自訂 SDB；對系統關鍵進程受限 |
| AppInit_DLLs | — | **不可用** | Secure Boot 開啟即失效，早已廢棄 |
| UIA / MSAA（不注入） | 讀取 UI 樹、輔助定位與對映 | 轉換器的「視覺分析」階段 | 不能改繪製，只能讀與模擬輸入 |

結論：**注入本身是成熟技術**（Windhawk、ExplorerPatcher、StartAllBack 皆為先例），真正的難點在「注入進去之後做什麼」——也就是 §3.1 的 XAML 環境搭建與 §3.5 的控制項接管。

### 3.5 在他人進程內接管外觀的三種策略（核心設計抉擇）

- **策略 A｜原地重繪（In-place repaint）**：`SetWindowSubclass` 子類化目標控制項，攔 `WM_PAINT`/`WM_ERASEBKGND`/`WM_CTLCOLOR*`/`NM_CUSTOMDRAW`，用 Direct2D/DirectWrite 或 GDI+ 以 Fluent 規範自繪；配合 uxtheme 未公開深色 API（`SetPreferredAppMode`、`AllowDarkModeForWindow`、`FlushMenuThemes` 等序數，win32-darkmode 先例）讓 comctl32 自己畫深色。**優點**：控制項行為、輸入、無障礙 100% 保留；**缺點**：每個控制項類別都要寫繪製器。
- **策略 B｜混合嫁接（Hybrid grafting）**：保留原 HWND 控制項做輸入與邏輯，外層用 XAML Island/Composition 視覺（陰影、壓克力底板、圓角裁剪）包覆。**優點**：視覺上限高；**缺點**：airspace 與裁切同步複雜。
- **策略 C｜整體置換（Full replacement）**：Detours 攔 `TaskDialogIndirect`/`DialogBoxIndirectParam`/`MessageBox`，解析參數後**用 XAML Island 重建整個 Fluent 對話框**，結果回傳原呼叫者。**優點**：效果最完美（截圖等級）；**缺點**：資料繫結、自訂控制項嵌入（`TaskDialog` 的 progress bar/footer/hyperlink）、訊息迴圈巢狀（modal loop）都要重建。

**方案取捨**：對話框類（TaskDialog/MessageBox/#32770）走策略 C；一般應用程式主視窗走 A，重點控制項（Toolbar/ListView/TreeView/Tab）走 A+B。XAML Island（L2）只在策略 B/C 中出現——**這回答了「直接注入 XAML Islands」的定位：它是武器庫裡的重砲，不是步兵**。

### 3.6 修改系統核心組件的可行性（L3 紅線分析）

直接改 `comctl32.dll`、`shell32.dll`、`dwm.exe`、`udwm.dll` 的現實：

1. **Windows Resource Protection (WRP)**：系統檔由 TrustedInstaller 持有，直接覆寫需 takeown/icacls 提權；`sfc /scannow`、DISM、甚至累積更新都會**自動還原或造成更新失敗**。
2. ** servicing（CBS/WinSxS）**：系統檔多為 WinSxS 硬連結，原地修改會污染元件存放區；每月 LC U 改版後你的二進位修補**位元級失效**（位移全變），需要逐版本維護 patch offset 資料庫。
3. **簽章與信任**：關鍵進程載入時驗簽（catalog signature），改過的 DLL 會被拒載或觸發 TPM/安全開機相關的完整性警報；`dwm.exe` 當掉＝整個畫面黑掉。
4. **先例的啟示**：ExplorerPatcher（以 `dxgi.dll` 側載代理注入 explorer，**不改任何系統檔**）、StartAllBack（注入 + 記憶體修補）、SecureUXTheme（記憶體修補 uxtheme 的簽章檢查以載入第三方 msstyles）、DWMBlurGlass（注入 dwm 掛 udwm 函式——效果炫但每月追版本）——**業界共識是「記憶體修補 + 注入平台」，不是「磁碟改檔」**。

**因此 L3 的落地形式定為**：
- 自製 **msstyles 主題包**（全域 comctl32 視覺樣式，配合 SecureUXTheme 式載入）→ 一次覆蓋所有走 uxtheme 的舊程式；
- **系統組件注入描述檔**（mmc、eventvwr、control、run dialog 的 L1/L2 規則）；
- 對極少數無法注入的受保護情境，只做 L0。

### 3.7 先行案例對照（證明每一層都有人走通）

| 專案 | 它證明了什麼 |
|---|---|
| Mica For Everyone | 跨進程 `DwmSetWindowAttribute`/`SetWindowCompositionAttribute` 全局材質可行（L0） |
| win32-darkmode（ysc3839） | uxtheme 未公開深色序數 + 子類化可讓任意 Win32 程式深色化（L1） |
| Windhawk | 「注入 + 模組市場」的工程與分發模式可行（平台層） |
| ExplorerPatcher / StartAllBack | 對 explorer/系統殼層的長期記憶體修補可維運（L3 等效） |
| SecureUXTheme | msstyles 全域主題繞過簽章檢查可行（L3 等效） |
| DWMBlurGlass | 連 dwm 內部都能掛鉤——同時也示範了這條路的維護成本（紅線警示） |
| .NET 9 WinForms 深色模式 / WPF Fluent Theme | 對「自家程式」官方路線已存在；我們的價值在**第三方與系統程式** |

### 3.8 總可行性結論

> 「直接注入 XAML Islands 用 WinUI」——**作為口號不行，作為分層架構可行**。
> 把 XAML Island 當成三種接管策略中的高階渲染後端（策略 B/C），以 L0（DWM 屬性）與 L1（攔截重繪）承擔 80% 覆蓋率，L2（WinUI 2 Island）承擔旗艦視覺，L3 以 msstyles + 記憶體修補等效達成系統組件現代化——**整體技術風險可控，全部環節均有可驗證先例**。

---

## 四、總體架構：FluentShell 轉換器套件

### 4.1 分層架構圖

```
┌────────────────────────────────────────────────────────────────┐
│  管理層   FluentShell.Studio（規則編輯/即時預覽/UIA 視覺分析）      │
│           FluentShell.Updater（版本相容/hash 白名單/一鍵回滾）      │
├────────────────────────────────────────────────────────────────┤
│  規則層   FluentShell.Rules（目標描述檔：exe/類別名/控制項對映/      │
│           接管策略/主題選擇——JSON 描述，社群可共享，Windhawk 模式）  │
├────────────────────────────────────────────────────────────────┤
│  注入層   FluentShell.Injector（服務：WinEvent 監聽 + 同位元注入，   │
│           x64 / ARM64 / x86 三組 payload）                        │
├────────────────────────────────────────────────────────────────┤
│  攔截層   FluentShell.Bridge（進程內 DLL：Detours 掛鉤 +             │
│           SetWindowSubclass + NM_CUSTOMDRAW + WM_CTLCOLOR*）       │
├────────────────────────────────────────────────────────────────┤
│  渲染層   ┌─ Renderer.D2D   （策略 A：Fluent 自繪控制項）             │
│           ├─ Renderer.XamlIsland（策略 B/C：WinUI 2 Island 宿主）     │
│           ├─ Renderer.ContentIsland（實驗：WinUI 3 / Content Isl.） │
│           └─ Renderer.Dwm    （L0：DWM 屬性 + 壓克力 + 圓角）        │
├────────────────────────────────────────────────────────────────┤
│  主題層   FluentShell.Theme（Fluent Design Token、深色/淺色、        │
│           Mica/Acrylic 控制器、msstyles 產生器、uxtheme 深色 API）   │
└────────────────────────────────────────────────────────────────┘
```

### 4.2 模組職責與關鍵設計

**FluentShell.Injector（系統服務，常駐）**
- 用 `SetWinEventHook`（`EVENT_OBJECT_CREATE` / `EVENT_SYSTEM_FOREGROUND`）+ 定期 `EnumWindows` 偵測新視窗；以 exe 路徑 + 視窗類別名（`#32770`、`TaskDialog`、`MMCMainFrame`、`ConsoleWindowClass`…）匹配規則描述檔。
- 注入方式依目標選擇：一般程式 → `SetWindowsHookEx`；定點系統組件 → IFEO / Shim；禁止清單（防毒、受保護進程、`csrss`、`winlogon`、`consent` 等）絕不碰。
- 全部 payload 數位簽章；注入前比對目標模組 hash 白名單（未知版本 → 只套用 L0，記錄待分析）。

**FluentShell.Bridge（注入進程內的 native DLL，C++/WinRT + WIL + Detours）**
- 初始化：自建專用 UI 執行緒 → `MddBootstrapInitialize` / `AddPackageDependency` 取得套件身分 → `WindowsXamlManager::InitializeForCurrentThread` → 待命。
- 攔截表（依目標動態啟用）：

| 目標 API / 訊息 | 目的 |
|---|---|
| `TaskDialogIndirect` / `TaskDialog` | 策略 C：解析 `TASKDIALOGCONFIG` → 重建 Fluent 對話框 |
| `MessageBoxW/ExW`（user32） | 策略 C：轉建 Fluent 對話框 |
| `DialogBoxIndirectParam` / `CreateDialogIndirectParam` | 策略 A/C：解析對話框資源模板 → 對映控制項 |
| `CreateWindowExW`（comctl32 類別） | 追蹤控制項建立，掛自繪子類化 |
| `OpenThemeData` / `DrawThemeBackground` / `GetThemeColor` | 導向深色主題句柄（策略 A 的 uxtheme 路線） |
| `GetSysColor` / `GetSysColorBrush` / `SetSysColors` | 回傳 Fluent token 色票 |
| `WM_CTLCOLORBTN/STATIC/EDIT/LISTBOX`、`WM_ERASEBKGND`、`WM_NCPAINT` | 子類化訊息攔截（策略 A） |
| `NM_CUSTOMDRAW`（ListView/TreeView/Toolbar/Tab/Rebar/Header） | comctl32 標準自繪擴充點（策略 A，最穩） |

- 安全機制：所有掛鉤包 SEH 保護；任何例外 → 卸載該 hook 並回退原行為（fail-safe）；遙測（本機記錄，不上傳）。

**Renderer.XamlIsland（L2 旗艦渲染器）**
- 在 Bridge 的專用 UI 執行緒上建立 `DesktopWindowXamlSource`，父 HWND 指向目標視窗（策略 B）或新建的替身視窗（策略 C）。
- 島內跑 WinUI 2.8 控制項；資源字典由 FluentShell.Theme 注入（動態切換深淺色用 `RequestedTheme` + 自訂 `ResourceDictionary` 熱替換）。
- 輸入/焦點銜接：`IDesktopWindowXamlSourceNative`、處理 `TakeFocusRequested`、`NavigateFocus`；島外 Win32 與島內 XAML 的 Tab 循環以 `WS_TABSTOP` 映射表維護。
- 生命週期：跟隨目標 HWND 的 `WM_DESTROY` 依序 `Content=null` → `DesktopWindowXamlSource.Close` → `WindowsXamlManager.Close`（不關會洩漏——XAML Island 已知坑）。

**Renderer.D2D（L1 主力渲染器）**
- Direct2D 1.1 + DirectWrite 自繪 Fluent 控制項外觀（圓角 4px/8px、壓克力底色刷、Segoe Fluent Icons 圖示）；文字一律 DirectWrite（解決 GDI 字型替換的度量漂移）。
- DPI：全部以 `GetDpiForWindow` 逐窗換算，支援 PerMonitorV2；`WM_DPICHANGED` 重算版面。

**Renderer.Dwm（L0 無注入渲染器）**
- 由 Injector 服務**從外部**對他人視窗設定：`DWMWA_USE_IMMERSIVE_DARK_MODE`、`DWMWA_SYSTEMBACKDROP_TYPE`（Mica/Acrylic）[^35^]、`DWMWA_WINDOW_CORNER_PREFERENCE`；需要壓克力時用未公開 `SetWindowCompositionAttribute`。
- 此渲染器**無需注入**，作為所有目標的基礎地板，也是注入失敗/目標在黑名單時的降級方案。

**FluentShell.Theme（主題引擎）**
- Fluent Design Token 單一來源（JSON）：`ControlFillColorDefault`、`AccentFillColorDefault`、`ControlCornerRadius`、`TextFillColorPrimary`…（命名對齊 WinUI 設計 token），所有渲染器共用。
- uxtheme 深色模式：以序數動態解析（`SetPreferredAppMode`、`AllowDarkModeForWindow`、`AllowDarkModeForApp`、`FlushMenuThemes`、`RefreshImmersiveColorPolicyState`），版本間序數漂移以符號/特徵碼雙保險。
- **msstyles 產生器**：把 token 編譯成 `.msstyles`（修改現有 aero 範本），配合 SecureUXTheme 式記憶體繞簽章載入——這是 L3「全域系統組件現代化」的安全等效實現。
- 監聽 `WM_SETTINGCHANGE` / `UISettings.ColorValuesChanged`，跟隨系統深淺色與強調色。

**FluentShell.Rules（轉換描述檔）**
- 每個目標一份 JSON：`{ match: {exe, class, title}, strategy: A|B|C, hooks: [...], theme: "dark", controlMap: {...}, fallback: "L0" }`。
- 內建描述檔：通用 TaskDialog 攔截包、通用 #32770 深色包、MMC 套件（mmc.exe + 各 snap-in）、eventvwr、regedit、控制台 `*.cpl`、Run 對話框、通用 comctl32 NM_CUSTOMDRAW 包、WinForms 通用包、WPF 通用包。
- 這就是「一整套轉換器」的本體：**轉換器 = 規則描述檔 + 對應渲染策略的外掛**，而非每個程式寫死一份程式碼。

**FluentShell.Studio（開發/調試工具，WinUI 3 桌面應用）**
- UIA/MSAA 視覺分析器：框選任意視窗 → 顯示控制項樹、類別名、comctl32 版本、是否 PerMonitorV2、目前 theme handle——半自動產生規則描述檔。
- 即時預覽：對目標視窗套用/撤銷策略並截圖對比（驗收測試也用此機制）。

### 4.3 各程式類型的轉換器對映

| 目標類型 | 主要策略 | 關鍵技術 | 預期覆蓋度 |
|---|---|---|---|
| comctl32 TaskDialog / MessageBox | C + A | Detours 攔 API，XAML Island 重建；失敗時自繪降級 | ~95%（含截圖三例） |
| `#32770` 資源對話框 | A | 子類化 + NM_CUSTOMDRAW + WM_CTLCOLOR* | ~90% |
| 純 Win32 主視窗（選單/工具列/ListView…） | A + L0 | NM_CUSTOMDRAW + uxtheme 深色 + DWM 屬性 | ~85% |
| GDI 全自繪程式 | L0 為主 | 無法攔其像素，給標題列/材質/圓角 | ~40%（邊框級） |
| MFC（標準） | A | 同 Win32（底層就是 comctl32） | ~85% |
| MFC Feature Pack（功能區） | A（部分） | `CMFCVisualManager` 是進程內全域，注入後替換其繪製常數；功能區自繪量大 | ~60% |
| WinForms | A + 反射 | 注入 native bridge 後由 CLR hosting 掛 managed helper：走查 `Control` 樹、`NativeWindow` 子類化、雙緩衝開啟；或借力 .NET 9 `Application.SetColorMode`（若目標是 .NET 9+ 可誘導啟用） | ~75% |
| WPF | B + 官方主題 | WPF 自控渲染管線（milcore），子類化 HWND 無效；改走：注入 managed assembly 替換 Application 級 `ResourceDictionary`（Fluent 樣式包），或誘導 .NET 9 `ThemeMode`；外層 L0 補標題列 | ~70% |
| Console / 終端 | 範圍外 | Windows Terminal 已現代化 | — |

---

## 五、關鍵技術設計細節

### 5.1 XAML Island 在他人進程的初始化序列（L2 核心）

```
Injected Bridge DllMain (LOADER LOCK 內不做任何 XAML 事)
  └─ CreateThread(FluentXamlThread)
       ├─ MddBootstrapInitialize(WinAppSDK) 或 AddPackageDependency(Microsoft.UI.Xaml)  ← 取得套件身分
       ├─ DispatcherQueueController.CreateDispatcherQueueController  ← 專用 DQ
       ├─ WindowsXamlManager::InitializeForCurrentThread()
       ├─ DesktopWindowXamlSource + IDesktopWindowXamlSourceNative::AttachToWindow(targetHwnd | 替身Hwnd)
       ├─ source.Content = BuildFluentDialog(TASKDIALOGCONFIG*, theme)   ← WinUI 2 XAML
       └─ 巢狀訊息迴圈（modal）：GetMessage + 島的 PreTranslateMessage
            └─ 結果寫回 *pnButton / *pnRadioButton → Detour 回傳
```

- **LOADER LOCK 紀律**：所有 COM/WinRT 初始化一律離開 `DllMain`；這是注入 XAML 最常見的死結來源。
- **降級鏈**：任一初始化步驟失敗 → 記錄 → 退回策略 A（D2D 自繪對話框）→ 再失敗 → 放行原 API。
- **同進程已有 XAML 框架**：偵測已載入模組（`Windows.UI.Xaml.dll` / `Microsoft.UI.Xaml.dll`），若目標自己已是 XAML 程式 → 只套 L0/L1，不建島（避免雙框架衝突）。

### 5.2 對話框重建（策略 C）的對映規範

`TASKDIALOGCONFIG` → WinUI 對話框的對映表：

| 原結構 | Fluent 呈現 |
|---|---|
| `pszMainInstruction` | `ContentDialog` 標題樣式（Title，20px semibold） |
| `pszContent` / `pszExpandedInformation` | 內文 + TeachingTip 式展開區 |
| `TD_WARNING_ICON` 等 | Segoe Fluent Icons 向量圖示（⚠ 黃色，如截圖右側） |
| `pButtons` / `TDF_USE_COMMAND_LINKS` | WinUI `Button`（Accent 樣式給預設鈕）/ CommandBar 式命令連結 |
| `pszVerificationText` | `CheckBox` |
| `footerIcon` + `pszFooter` | InfoBar 風格頁尾 |
| `cxWidth`、`TDF_CAN_BE_MINIMIZED`、進度列、`callback` 計時事件 | 對應實作；callback 事件轉發為 XAML 屬性更新 |

截圖中三個案例（MessageBox 風格「Hello World」、MMC 儲存確認、系統保護確認）**全部落在 TaskDialog/簡易對話框範疇**，是本對映表的 MVP 驗收目標。

### 5.3 Win32 串接細節清單

- **DPI**：島 HWND 與目標 HWND 的 DPI 必須一致；PerMonitorV2 目標要轉發 `WM_DPICHANGED` 進島；非 DPI 感知目標（系統縮放）要避免 bitmap 拉伸偽影——以實際像素重建版面。
- **IME**：島內 `TextBox` 需 TSF 正常運作；島外原控制項的 IME 狀態不變。
- **焦點/啟用**：策略 C 的替身對話框要處理 `WM_ACTIVATE`/`SetForegroundWindow` 的擁有者鏈（`GWLP_HWNDPARENT`），避免工作列出現幻影項目（`WS_EX_TOOLWINDOW` + 擁有者設定）。
- **無障礙**：策略 A 保留原生 MSAA/UIA；策略 C 由 WinUI 自動提供 UIA，但需把原對話框的 `accName`/快捷鍵（`&OK` 的 Alt+O）映射過去——快捷鍵表從資源字串解析。
- **多執行緒 UI**：目標若多 UI 執行緒（罕見，如 MMC 某些 snap-in），Bridge 以執行緒 ID 為 key 維護多島表。

### 5.4 系統組件植入（L3 等效實現）三步驟

1. **主題層植入**：Fluent msstyles + SecureUXTheme 式載入 → 一次覆蓋所有走 uxtheme 的系統介面（控制台、MMC 框架、屬性頁）。
2. **規則層植入**：為 `mmc.exe`、`eventvwr.msc` 啟動器、`control.exe`、`rundll32 shell32` 對話框、`Run`（`explorer` 的 `#32770`）各寫一份 Rules 描述檔，走 L1/C 策略。
3. **殼層植入（選配，明確標示高風險）**：ExplorerPatcher 式 `dxgi.dll` 側載進 `explorer.exe` 做記憶體修補；**堅持不落地修改系統檔**，Update 前由 Updater 自動停用（偵測 `pending.xml`/Build 變更 → 卸載 → 等白名單更新）。

---

## 六、開發計畫

### 6.1 里程碑

| 里程碑 | 內容 | 驗收標準 | 預估 |
|---|---|---|---|
| **M0 技術驗證釘子** | 三個最小 PoC：(a) 跨進程 DWM 深色+Mica；(b) 對記事本注入 DLL 並子類化攔 `WM_CTLCOLOR`；(c) 在注入執行緒成功建 XAML Island 並顯示一個 WinUI 2 按鈕於第三方進程視窗上 | 三 PoC 各自穩定執行 1 小時無當機 | 3–4 週 |
| **M1 L0 渲染器** | Renderer.Dwm + Injector 雛形 + 全域規則匹配 | 對任意視窗外部套用深色標題列/材質；注入黑名單生效 | 3 週 |
| **M2 對話框轉換器 MVP** | Detours 攔 `TaskDialogIndirect` + D2D 自繪（策略 A/C 降級鏈先跑通） | **重現附圖三個案例的深色 Fluent 外觀**（MessageBox、MMC 儲存、系統保護） | 6–8 週 |
| **M3 XAML Island 接管** | 策略 C 完整版：XAML Island 重建對話框 + 快捷鍵/UIA/降級鏈 | MMC/eventvwr/系統保護對話框以 WinUI 呈現，行為與原對話框等價（含 hyperlinks、command links、進度列） | 8–10 週 |
| **M4 通用控制項轉換器** | NM_CUSTOMDRAW 家族（ListView/TreeView/Toolbar/Tab/Header）+ uxtheme 深色 API + `#32770` 通用包 | regedit、控制台主要頁面深色 Fluent 化 | 8 週 |
| **M5 WinForms/WPF 轉換器** | CLR hosting + managed helper；WPF 資源字典替換 | 各 2 個代表性第三方 app 達成目標覆蓋度 | 8 週 |
| **M6 系統主題包 + Studio** | msstyles 產生器、SecureUXTheme 整合、Studio 視覺分析器、Updater 回滾 | 全新安裝 Win11 VM 一鍵部署/一鍵還原 | 6 週 |
| **M7 硬化** | 多版本 Windows 測試矩陣、EDR 相容、簽章、Crash 遙測、效能（注入開銷 < 5ms/視窗，記憶體 < 30MB/進程） | 見 6.2 測試矩陣全綠 | 6 週 |

> 總時程粗估：單一資深 Windows 系統工程師 12–15 個月；2–3 人團隊 6–8 個月。M2 完成即有可對外展示的「截圖級」成果。

### 6.2 測試矩陣

- **OS**：Win10 1903 / 21H2 / 22H2；Win11 21H2 / 22H2 / 23H2 / 24H2（+Insider Dev 追蹤）；x64 + ARM64（ARM64 上還要測 x86 模擬進程——位元數三路 payload）。
- **目標程式**：系統內建 20 個（mmc+5 個 snap-in、eventvwr、regedit、taskmgr 舊對話框、control、desk.cpl、sysdm.cpl、notepad 舊版、WordPad、mspaint…）＋ 第三方 15 個（7-Zip、Notepad++（自繪重）、WinForms 範例、WPF 範例、MFC 範例、VB6 執行期…）。
- **情境**：多顯示器異 DPI、遠端桌面、高對比主題、敘述者（Narrator）朗讀、系統深淺色熱切換、Windows 累積更新後自動驗證（VM 快照 + Hyper-V 差異磁碟）。
- **回滾演練**：在 WinRE/安全模式下驗證 Updater 的解除安裝可完整還原（含 msstyles 與 IFEO 項目）。

### 6.3 工具鏈

| 類別 | 項目 |
|---|---|
| 編譯 | Visual Studio 2022（MSVC v143，C++/WinRT、`/std:c++20`）、Windows SDK 22621/26100、WIL、Detours、WinUI 2.8 NuGet、Windows App SDK 1.8[^25^]（實驗分支用 2.0-experimental 追 `XamlIsland`[^34^]） |
| 部署/系統 | Windows ADK（WinPE 測試環境、DISM）、Hyper-V（VM 矩陣）、Application Compatibility Toolkit（自訂 Shim） |
| 逆向/分析 | Spy++、Inspect.exe（UIA）、Accessibility Insights、API Monitor、Process Monitor、WinDbg（含 TTD）、xperf/ETW（DWM/輸入延遲）、IDA/Ghidra（comctl32 TaskDialog 內部結構、uxtheme 序數核對）、微軟公開符號伺服器 |
| 品質 | AppVerifier、ASan（自測模組）、CodeQL、WPP/TraceLogging 遙測、EV 代碼簽章憑證 + SmartScreen 聲譽累積、Defender/主流 EDR 誤報送審流程 |

---

## 七、風險登錄（Top 10）

| # | 風險 | 機率 | 衝擊 | 緩解 |
|---|---|---|---|---|
| 1 | Windows 累積更新改版使記憶體修補/序數失效 | 高 | 中 | 特徵碼+符號雙定位；版本白名單降級 L0；Updater 自動停用機制 |
| 2 | 注入行為被 Defender/EDR 判定為惡意 | 高 | 高 | 全模組 EV 簽章、開源、向廠商送審白名單、行為最小化（不碰網路/檔案） |
| 3 | 目標進程內 XAML 初始化失敗/死結（loader lock、套件身分） | 中 | 中 | M0 釘子 PoC 先行；降級鏈 fail-safe；禁用清單 |
| 4 | 雙 XAML 框架共存衝突（目標自帶 WinUI/系統 XAML） | 中 | 中 | 模組偵測 → 只走 L0/L1 |
| 5 | 策略 C 行為差異（快捷鍵、預設鈕、巢狀 modal、自訂控制項嵌入） | 中 | 中 | 對映規範測試集；逐案例回歸；不支援的組態自動降級策略 A |
| 6 | ARM64/x86 位元數與模擬層注入複雜度 | 中 | 低 | 三路 payload 由架構判斷；CI 覆蓋 |
| 7 | 效能：注入掛鉤拖慢視窗建立 | 低 | 中 | 惰性掛鉤（匹配規則才掛）、ETW 追蹤預算 5ms |
| 8 | 受保護進程（PPL、防毒、consent UI）無法/不得注入 | 確定 | 低 | 明確排除，只做 L0 |
| 9 | dwm 級修改的欲望蔓延（scope creep 到 DWMBlurGlass 路線） | 中 | 高 | 架構紅線：本套件永不注入 dwm.exe |
| 10 | WinUI 3 Islands 政策變動（實驗 API 改動） | 中 | 低 | Content Island 渲染器隔離在實驗分支，主線不依賴 |

---

## 八、附錄：關鍵 API / 常數速查

**DWM（公開）**：`DwmSetWindowAttribute`；`DWMWA_USE_IMMERSIVE_DARK_MODE=20`、`DWMWA_WINDOW_CORNER_PREFERENCE=33`、`DWMWA_SYSTEMBACKDROP_TYPE=38`（`DWMSBT_MAINWINDOW`=Mica、`DWMSBT_TRANSIENTWINDOW`=Backdrop Acrylic、`DWMSBT_TABBEDWINDOW`=Mica Alt）[^35^]、`DWMWA_CAPTION_COLOR=35`、`DWMWA_BORDER_COLOR=34`、`DWMWA_CLOAK=13`；`DwmExtendFrameIntoClientArea`、`DwmRegisterThumbnail`。

**DWM（未公開，隔離使用）**：user32 `SetWindowCompositionAttribute`（`ACCENT_POLICY`，`ACCENT_ENABLE_ACRYLICBLURBEHIND=4`）。

**XAML Islands**：`WindowsXamlManager::InitializeForCurrentThread`、`DesktopWindowXamlSource`、`IDesktopWindowXamlSourceNative::AttachToWindow/GetWindowHandle`、WinUI 2（`Microsoft.UI.Xaml` 2.8）。

**下一代（實驗）**：`Microsoft.UI.Content.ContentIsland`、`DesktopSiteBridge`、`DesktopChildSiteBridge`、`DesktopPopupSiteBridge`、`ContentIsland.CreateForSystemVisual`、`Microsoft.UI.Xaml.XamlIsland`、`SystemBackdropHost`[^34^]。

**套件身分（未封裝進程）**：`MddBootstrapInitialize`（WinAppSDK Bootstrapper）、`AddPackageDependency`（Win11+）。

**uxtheme 深色（未公開序數，1903+，以符號/特徵碼雙保險）**：`SetPreferredAppMode`、`AllowDarkModeForApp`、`AllowDarkModeForWindow`、`FlushMenuThemes`、`RefreshImmersiveColorPolicyState`、`ShouldAppsUseDarkMode`、`IsDarkModeAllowedForWindow`。

**攔截/注入**：Detours；`SetWindowSubclass`；`NM_CUSTOMDRAW`；`SetWinEventHook`；`SetWindowsHookEx`；IFEO。

---

[^25^]: Stable channel release notes for the Windows App SDK（穩定通道 1.8.x 為目前生產版本）— https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/stable-channel
[^34^]: Experimental channel release notes for the Windows App SDK（2.0 實驗通道：`Microsoft.UI.Xaml.XamlIsland`、`Microsoft.UI.Content.ContentIsland`/`DesktopChildSiteBridge`/`DesktopPopupSiteBridge`、`ContentIsland.CreateForSystemVisual`、`SystemBackdropHost`）— https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/experimental-channel
[^35^]: DWM_SYSTEMBACKDROP_TYPE（DWM 系統背板材質列舉）— https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type
