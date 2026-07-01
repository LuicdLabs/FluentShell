# 06 — 風險、限制與路線圖

## 已知限制（誠實記錄）

| # | 限制 | 說明 | 現況 |
| :--- | :--- | :--- | :--- |
| 1 | **TaskDialog DirectUI 內文** | 圖示/主指示/內文由 `DirectUIHWND` 內部繪製，非 `Static` 子視窗；已建立的 TaskDialog 無法事後重新著深色 | 邊框 + 真實按鈕可轉；內文為限制（[04](04-control-repaint-direct2d.md)） |
| 2 | **多頁 property sheet** | 分頁、群組框、清單、巢狀頁面對話框 + 自繪背景；且常在「已顯示後」才注入 | 部分轉換（按鈕成功） |
| 3 | **Airspace 殘留** | 若目標內部有 Direct3D/自繪表面，疊繪可能有 Z-order/閃爍 | 需逐案處理 |
| 4 | **Mica 透出 vs 不透明** | 讓 Mica 真正透過 GDI 控件區需把控件改成有 alpha 的合成表面 | 目前採不透明深色（對齊目標圖） |
| 5 | **雙架構** | 完整全域需 x64 + x86 兩份 DLL/載入器 | PoC 主打 x64 |
| 6 | **未公開 API** | uxtheme 序號 104/132/133/135/136 可能隨版本變動 | 需版本防護與退場 |
| 7 | **DPI** | 舊程式多為 System-DPI-aware，強改字級需嚴謹 DPI 數學 | 已按 `GetDpiForWindow` 縮放 |

## 安全與邊界

- **不碰核心**：完全在使用者態；不修改 `win32k.sys` 或任何核心元件，因此**不會觸發 PatchGuard 藍屏**。DWM/Direct2D/子類化全是有支援的使用者態 API。
- **不改系統檔**：不 patch 任何系統 DLL/EXE 二進位，避開 **SFC（系統檔案保護）**。改造只存在於執行期記憶體。
- **可還原、不持久化**：子類化於 `WM_NCDESTROY` 自動移除；全域勾隨 injector 進程結束被系統移除；**不使用 `AppInit_DLLs`**、不寫註冊表持久化。

## 提權與 Windows Defender

- **提權**：要注入被提權（High IL）的系統對話框，injector 必須以**系統管理員**執行並啟用 `SeDebugPrivilege`（UIPI 禁止低 IL → 高 IL）。本 PoC 的真實系統測試對象在使用者 IL 即可達；真正提權的目標需從提權主控台執行 `injector --global`。
- **Windows Defender / SmartScreen**：全域勾 + 遠端執行緒注入是防毒關注的行為模式，未簽章工具可能被攔或要求允許。**本專案不做任何 AV 規避**；正式化的正解是**程式碼簽章 + 與防毒白名單溝通**，而非規避。

## ⚠️ 破壞性操作準則

圖片中確切那個框（刪除所有還原點確認）是**破壞性**的。任何測試：**只看樣式、一律按 Cancel、絕不按 Continue**、不刪任何還原點。優先用 ① Tier-1 複製品 ② 非破壞性 `#32770`（如 System Protection 設定視窗、一般 MessageBox）驗證。

## 路線圖（若要成為可長期使用的「全域 Fluent 主題引擎」）

1. **穩定持久化注入**：改採 ExplorerPatcher 式的 **DLL 搜尋順序劫持 + IAT hook + 符號雜湊快取**，取代每次手動 `--global`；並補齊 **x86** 勾 DLL 與載入器以涵蓋 32 位元進程。
2. **TaskDialog / DirectUI 深度支援**：於進程**早期**（對話框建立前）設定深色模式；或對 `DirectUIHWND` 做 overlay 疊繪重繪圖示與文字，解決限制 #1/#2。
3. **完整控件重繪器**：把 Edit / ComboBox / ListView / TreeView / Header / 捲動軸 / 分頁 / 群組框 各自的 Fluent 重繪補齊，讓 property sheet 這類複雜視窗也能完整轉換。
4. **真 Mica 透出**：把控件改成 DirectComposition/有 alpha 的合成表面，實現全域模糊背景（解決 #4）。
5. **設定 UI 與白名單**：規則式選擇要改造的視窗類別/程式；程式碼簽章；與防毒溝通。
6. **穩定性與相容性**：全域勾注入所有 GUI 進程有相容性風險，預設改用更外科的 `--watch`（WinEvent + 針對性注入），並提供每程式排除清單。

## 一句話總結

`xaml islands.txt` / `dwm method 1.txt` 的判斷正確，而且**在本機用 C++ 實測可行**：一個 `#32770` 舊對話框，可在無原始碼、無 XAML Islands、不碰核心、不改系統檔、可還原的前提下，被全域轉換成深色 Fluent Design——底層靠 **DWM 屬性 + 未公開 uxtheme 深色 + Direct2D/DirectWrite 重繪 + 使用者態 Hook/注入**。
