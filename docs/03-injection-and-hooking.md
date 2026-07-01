# 03 — 注入與 Hook 機制

要把「Fluent 重繪」送進一個沒有原始碼的黑盒程式（甚至系統程序），需要三件事：**偵測對話框出現 → 把程式碼送進目標進程 → 在目標進程裡接管繪製**。本專案實作並實測了三種投遞方式。

## 偵測時機：為何用 `EVENT_SYSTEM_DIALOGSTART`

- `WH_CBT` 的 `HCBT_CREATEWND` 太早——此時子控件尚未建立，`EnumChildWindows` 抓不到按鈕/文字。
- `HCBT_ACTIVATE` 可用，但對「注入時已存在」的視窗才有活化事件。
- **`SetWinEventHook(EVENT_SYSTEM_DIALOGSTART)`** 是最乾淨的觸發點：對話框初始化完成、即將顯示時觸發，子控件都在。本專案 DLL 內即用它。

偵測到後：`GetClassName == "#32770"` 才處理；用 `GetProp/SetProp("AnyFluentDone")` 做**冪等**避免重複；再判斷 classic vs TaskDialog（`FindWindowEx(hDlg, DirectUIHWND)` 存在即 TaskDialog）。

## ⚠️ 踩過的致命坑：Hook 的執行緒生命週期

WinEvent hook（以及 `SetWindowsHookEx`）的生命週期**綁在安裝它的執行緒**上：該執行緒一結束，勾自動被移除。

目標式注入用 `CreateRemoteThread(LoadLibraryW)`，`DllMain` 就跑在這個**暫態遠端執行緒**上；`LoadLibraryW` 一回傳執行緒即結束。若在 `DllMain` 裡 `SetWinEventHook`，勾會**立刻失效**——本專案第一版就這樣，注入成功（`LoadLibraryW` 回傳有效 HMODULE）但對話框完全沒變。

**修正**：`DllMain` 只 `CreateThread` 起一個**常駐工作執行緒**，在該執行緒裡安裝 WinEvent hook 並跑訊息迴圈，讓勾的擁有者持續存活：

```cpp
static DWORD WINAPI HookThread(LPVOID hInst) {
    g_evt = SetWinEventHook(EVENT_SYSTEM_DIALOGSTART, EVENT_SYSTEM_DIALOGSTART,
                            (HINSTANCE)hInst, WinEventProc,
                            GetCurrentProcessId(), 0, WINEVENT_INCONTEXT);
    MSG m; while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (g_evt) UnhookWinEvent(g_evt);
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CloseHandle(CreateThread(0,0,HookThread,h,0,&g_threadId)); }
    else if (reason == DLL_PROCESS_DETACH) { if (g_threadId) PostThreadMessageW(g_threadId, WM_QUIT, 0, 0); }
    return TRUE;
}
```

附帶好處：也避開了「在 `DllMain`（loader lock）裡呼叫 `LoadLibrary`」的死鎖風險——`uxtheme` 的載入延後到 UI 執行緒上的 `ModernizeDialog` 才做。`WINEVENT_INCONTEXT` 讓回呼在**產生事件的 UI 執行緒**上執行，因此重繪/子類化都在正確執行緒。

## 三種投遞方式（`src/injector/main.cpp`）

### A. 目標式注入 `--pid` / `--exe`
經典 `OpenProcess → VirtualAllocEx → WriteProcessMemory(dll 路徑) → CreateRemoteThread(LoadLibraryW)`。安全、可控、可還原，適合已知 PID 的單一目標。`LoadLibraryW` 位址在同 session 各進程一致，可直接當 `CreateRemoteThread` 進入點。

### B. 全域勾 `--global`
`SetWindowsHookEx(WH_CBT, CbtProc, hookDll, 0)`。系統把 DLL 映射進**同桌面每個產生 CBT 事件、載入 user32、有訊息迴圈的 GUI 進程**；DLL 一被載入，其 `DllMain` 就替該進程裝好上述 WinEvent 現代化。`CbtProc`（DLL 匯出）本身只是「載入載體」，並在 `HCBT_ACTIVATE` 做一次補強：

```cpp
extern "C" __declspec(dllexport) LRESULT CALLBACK CbtProc(int code, WPARAM w, LPARAM l) {
    if (code == HCBT_ACTIVATE) TryModernize((HWND)w);
    return CallNextHookEx(nullptr, code, w, l);
}
```

安裝勾的進程需保持存活並泵訊息（本專案 `PumpUntilStop` + `Ctrl+C` 解除；進程結束時系統自動移除勾，故「殺掉 injector = 卸載」）。

### C. 監看 + 外科注入 `--watch`
`SetWinEventHook(EVENT_SYSTEM_DIALOGSTART, ..., WINEVENT_OUTOFCONTEXT)` 在 injector 進程裡監看**全系統**的對話框開始事件；一旦某 PID 冒出 `#32770`，才對「那一個」PID 做目標式注入。附帶影響最小，是比全域勾更外科、更保守的替代。

## 位元數與完整性等級（決定「全域到哪」）

- **32/64 位元必須各一份 DLL**：64 位元 DLL 只能進 64 位元進程，反之亦然。完整全域需 x64 + x86 兩份勾 DLL + 對應載入器。本 PoC 主打 x64（現代系統程序幾乎都是 64 位元）。
- **完整性等級（IL）/ UIPI**：低 IL 進程**不能**注入或勾高 IL 進程（`CreateRemoteThread`/`WriteProcessMemory`/hook 都被擋）。要觸及被提權的系統對話框，injector 必須**以系統管理員（High IL）執行並啟用 `SeDebugPrivilege`**（`SeDebugPrivilege` 只有在 High IL 才能啟用）。本專案啟動即嘗試 `AdjustTokenPrivileges(SE_DEBUG_NAME)`。

## 與 ExplorerPatcher / WindowBlinds 的關係

`WH_CBT` 全域勾是最乾淨、可還原、不動檔案的「全域」機制，本 PoC 採用。若要做成可長期常駐的產品，業界（如 **ExplorerPatcher**）改用**DLL 搜尋順序劫持**：在 `C:\Windows` 放一個 `dxgi.dll`（其實是自己的 DLL），讓 `explorer.exe` 先載入它；再用 **IAT hook** 攔 `CreateWindowEx` 等，並用**符號雜湊快取**適應 Windows 更新。持久化程度更高，但也更具侵入性（見 [06](06-risks-limitations-roadmap.md)）。`AppInit_DLLs` 則因需關閉 Secure Boot、被大量濫用而不建議。

## 出處

- [SetWindowsHookExW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexw)（32/64 位元注入規則）
- [Implementing Global Injection and Hooking in Windows（m417z）](https://m417z.com/Implementing-Global-Injection-and-Hooking-in-Windows/)
- [Integrity Levels and DLL Injection（Didier Stevens）](https://blog.didierstevens.com/2010/09/07/integrity-levels-and-dll-injection/)
- [ExplorerPatcher 架構（DeepWiki）](https://deepwiki.com/valinet/ExplorerPatcher)
- [SetWinEventHook](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook)
