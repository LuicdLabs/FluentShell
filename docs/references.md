# 參考來源

本專案研究與實作引用的主要出處（皆為深度研究期間查證）。

## DWM 視覺層
- [DWMWINDOWATTRIBUTE (dwmapi.h) — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute) — 屬性列舉與值（dark=20、corner=33、caption=35、border=34、backdrop=38）
- [DWM_SYSTEMBACKDROP_TYPE — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type) — Mica/Acrylic（`DWMSBT_MAINWINDOW=2`），最低 build 22621
- [DwmSetWindowAttribute — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmsetwindowattribute)
- [DwmExtendFrameIntoClientArea — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmextendframeintoclientarea) — sheet of glass / Mica 透出
- [DwmGetColorizationColor — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcolorizationcolor) — 取系統 Accent

## 未公開 uxtheme 深色模式
- [ysc3839/win32-darkmode — DarkMode.h](https://github.com/ysc3839/win32-darkmode/blob/master/win32-darkmode/DarkMode.h) — 序號 104/132/133/135/136 與 `PreferredAppMode` 列舉（公認參考實作）
- [Win32 Dark Mode（gist，rounk-ctrl）](https://gist.github.com/rounk-ctrl/b04e5622e30e0d62956870d5c22b7017)

## 注入與 Hook
- [SetWindowsHookExW — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexw) — 32/64 位元注入規則、全域勾
- [SetWinEventHook — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook) — `EVENT_SYSTEM_DIALOGSTART`、INCONTEXT/OUTOFCONTEXT
- [Implementing Global Injection and Hooking in Windows（m417z）](https://m417z.com/Implementing-Global-Injection-and-Hooking-in-Windows/)
- [Integrity Levels and DLL Injection（Didier Stevens）](https://blog.didierstevens.com/2010/09/07/integrity-levels-and-dll-injection/) — UIPI、低→高 IL 限制
- [Mandatory Integrity Control — Wikipedia](https://en.wikipedia.org/wiki/Mandatory_Integrity_Control)
- [ExplorerPatcher 架構（DeepWiki）](https://deepwiki.com/valinet/ExplorerPatcher) — DLL 搜尋順序劫持、IAT hook、符號雜湊快取

## 控件重繪
- [Create a simple Direct2D application — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-quickstart)
- [ID2D1DCRenderTarget::BindDC — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/d2d1/nf-d2d1-id2d1dcrendertarget-binddc)
- [SetWindowSubclass — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-setwindowsubclass)
- [Segoe Fluent Icons font — Microsoft Learn](https://learn.microsoft.com/en-us/windows/apps/design/iconography/segoe-fluent-icons-font)（警告三角形實測為 `U+E7BA`）

## 目標對話框（#32770 / TaskDialog）
- [TaskDialogIndirect — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-taskdialogindirect)
- [Task Dialogs 概觀 — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/controls/task-dialogs)
- [pywinauto #787 — DirectUIHWND 內容取不到文字](https://github.com/pywinauto/pywinauto/issues/787) — TaskDialog 內文由 DirectUI 繪製的佐證
- [ReactOS comctl32 taskdialog.c](https://doxygen.reactos.org/d9/d92/dll_2win32_2comctl32_2taskdialog_8c.html) — TaskDialog 按鈕以 `WC_BUTTONW` 建立（真實 HWND）

## DPI
- [SetProcessDpiAwarenessContext — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setprocessdpiawarenesscontext)
- [GetDpiForWindow — Microsoft Learn](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdpiforwindow)
