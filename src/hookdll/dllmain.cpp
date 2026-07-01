// anyfluenthook.dll — injected into a target process. A dedicated worker
// thread installs a per-process "dialog start" WinEvent hook and modernizes
// any #32770 (classic dialog or TaskDialog) that appears. The hook must be
// owned by a thread that stays alive: a WinEvent hook is auto-removed when its
// installing thread exits, so we cannot install it on the transient
// CreateRemoteThread that runs DllMain. Also exports CbtProc so the injector
// can use a global WH_CBT hook to load this DLL into every GUI process.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include "../common/FluentCore.h"

static HWINEVENTHOOK g_evt = nullptr;
static DWORD         g_threadId = 0;

static bool IsTaskDialog(HWND h) {
    return FindWindowExW(h, nullptr, L"DirectUIHWND", nullptr) != nullptr;
}

static void TryModernize(HWND hwnd) {
    if (!hwnd) return;
    wchar_t cls[64] = L"";
    if (!GetClassNameW(hwnd, cls, 64)) return;
    if (lstrcmpW(cls, L"#32770") != 0) return;
    if (GetPropW(hwnd, L"AnyFluentDone")) return;         // idempotent
    SetPropW(hwnd, L"AnyFluentDone", (HANDLE)(INT_PTR)1);
    fluent::ModernizeDialog(hwnd, IsTaskDialog(hwnd));    // lazily loads uxtheme (safe: not in loader lock)
}

static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW) return;
    TryModernize(hwnd);
}

// Vehicle for the global WH_CBT hook (loads this DLL into every GUI process).
extern "C" __declspec(dllexport) LRESULT CALLBACK CbtProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_ACTIVATE) TryModernize((HWND)wParam);
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static DWORD WINAPI HookThread(LPVOID param) {
    HINSTANCE hInst = (HINSTANCE)param;
    // In-context so the callback (and the repaint) runs on the UI thread that
    // owns the dialog; idThread 0 = all threads of this process.
    g_evt = SetWinEventHook(EVENT_SYSTEM_DIALOGSTART, EVENT_SYSTEM_DIALOGSTART,
                            hInst, WinEventProc, GetCurrentProcessId(), 0, WINEVENT_INCONTEXT);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_evt) { UnhookWinEvent(g_evt); g_evt = nullptr; }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        HANDLE th = CreateThread(nullptr, 0, HookThread, hInst, 0, &g_threadId);
        if (th) CloseHandle(th);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_threadId) PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    }
    return TRUE;
}
