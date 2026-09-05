#include "../Common/FluentShell.h"
#include "../Common/ProcessPolicy.h"
#include "Translation/DialogTranslator.h"
#include "Translation/MenuBarCapture.h"
#include "Translation/RendererSession.h"

#include "../../third_party/detours/src/detours.h"

#include <commctrl.h>
#include <tlhelp32.h>

#include <atomic>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace {

using MessageBoxW_t = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
using MessageBoxExW_t = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT, WORD);
using TaskDialogIndirect_t = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
using TrackPopupMenu_t = BOOL(WINAPI*)(HMENU, UINT, int, int, int, HWND, const RECT*);
using TrackPopupMenuEx_t = BOOL(WINAPI*)(HMENU, UINT, int, int, HWND, LPTPMPARAMS);

MessageBoxW_t TrueMessageBoxW = nullptr;
MessageBoxExW_t TrueMessageBoxExW = nullptr;
TaskDialogIndirect_t TrueTaskDialogIndirect = nullptr;
TrackPopupMenu_t TrueTrackPopupMenu = nullptr;
TrackPopupMenuEx_t TrueTrackPopupMenuEx = nullptr;
std::atomic<bool> g_hooksInstalled{ false };
std::atomic<bool> g_workerRunning{ false };
HMODULE g_self = nullptr;
thread_local int g_hookDepth = 0;

struct HookDepthGuard final {
    HookDepthGuard() noexcept { ++g_hookDepth; }
    ~HookDepthGuard() { --g_hookDepth; }
};

bool UpdateAllDetourThreads(std::vector<HANDLE>& handles) {
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) return false;
    const DWORD processId = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    bool success = true;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != processId ||
                entry.th32ThreadID == GetCurrentThreadId()) continue;
            HANDLE thread = OpenThread(
                THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME |
                    THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                FALSE, entry.th32ThreadID);
            if (!thread) {
                if (GetLastError() != ERROR_INVALID_PARAMETER) success = false;
                continue;
            }
            if (DetourUpdateThread(thread) != NO_ERROR) success = false;
            handles.push_back(thread);
        } while (success && Thread32Next(snapshot, &entry));
    } else {
        success = false;
    }
    CloseHandle(snapshot);
    return success;
}

void CloseDetourThreadHandles(std::vector<HANDLE>& handles) noexcept {
    for (const HANDLE thread : handles) CloseHandle(thread);
    handles.clear();
}

int WINAPI HookMessageBoxW(
    HWND owner,
    LPCWSTR text,
    LPCWSTR caption,
    UINT type) {
    if (!TrueMessageBoxW) return 0;
    if (g_hookDepth != 0) return TrueMessageBoxW(owner, text, caption, type);
    HookDepthGuard depth;
    try {
        const auto translated = FluentShell::Bridge::Translation::TranslateMessageBox(
            FluentShell::Bridge::Translation::ActiveRendererSession(),
            owner, text, caption, type);
        if (translated) return *translated;
    } catch (...) {
        FluentShell::Log(L"MessageBox translation threw; using native API");
    }
    return TrueMessageBoxW(owner, text, caption, type);
}

int WINAPI HookMessageBoxExW(
    HWND owner,
    LPCWSTR text,
    LPCWSTR caption,
    UINT type,
    WORD languageId) {
    if (!TrueMessageBoxExW) {
        return TrueMessageBoxW ? TrueMessageBoxW(owner, text, caption, type) : 0;
    }
    if (g_hookDepth != 0) {
        return TrueMessageBoxExW(owner, text, caption, type, languageId);
    }
    HookDepthGuard depth;
    try {
        const auto translated = FluentShell::Bridge::Translation::TranslateMessageBox(
            FluentShell::Bridge::Translation::ActiveRendererSession(),
            owner, text, caption, type, languageId);
        if (translated) return *translated;
    } catch (...) {
        FluentShell::Log(L"MessageBoxEx translation threw; using native API");
    }
    return TrueMessageBoxExW(owner, text, caption, type, languageId);
}

HRESULT WINAPI HookTaskDialogIndirect(
    const TASKDIALOGCONFIG* config,
    int* button,
    int* radioButton,
    BOOL* verificationChecked) {
    if (!TrueTaskDialogIndirect) return E_FAIL;
    if (g_hookDepth != 0) {
        return TrueTaskDialogIndirect(config, button, radioButton, verificationChecked);
    }
    HookDepthGuard depth;
    try {
        const auto translated = FluentShell::Bridge::Translation::TranslateTaskDialog(
            FluentShell::Bridge::Translation::ActiveRendererSession(), config);
        if (translated) {
            if (button) *button = translated->button;
            if (radioButton) *radioButton = 0;
            if (verificationChecked) {
                *verificationChecked = translated->verificationChecked ? TRUE : FALSE;
            }
            return S_OK;
        }
    } catch (...) {
        FluentShell::Log(L"TaskDialog translation threw; using native API");
    }
    return TrueTaskDialogIndirect(config, button, radioButton, verificationChecked);
}

// A menu-bar toolbar opens its popup from the click itself, so the projection performs
// that click through the control's own accessible default action and records the HMENU
// here instead of letting the application's popup reach the screen.  Interception is
// armed only around that one call, on that one thread: every other popup goes to the
// real API untouched.
BOOL WINAPI HookTrackPopupMenu(
    HMENU menu,
    UINT flags,
    int x,
    int y,
    int reserved,
    HWND owner,
    const RECT* rect) {
    if (FluentShell::Bridge::Translation::PopupSuppressionActive()) {
        FluentShell::Bridge::Translation::RecordInterceptedPopup(menu);
        // Zero is what the real function answers when the user dismissed the menu
        // without choosing anything, which is exactly what the application should
        // believe happened.  Suppression is process-wide for the length of a menu-bar
        // read, so a popup that arrives late still never reaches the screen.
        return FALSE;
    }
    if (!TrueTrackPopupMenu) return FALSE;
    return TrueTrackPopupMenu(menu, flags, x, y, reserved, owner, rect);
}

BOOL WINAPI HookTrackPopupMenuEx(
    HMENU menu,
    UINT flags,
    int x,
    int y,
    HWND owner,
    LPTPMPARAMS params) {
    if (FluentShell::Bridge::Translation::PopupSuppressionActive()) {
        FluentShell::Bridge::Translation::RecordInterceptedPopup(menu);
        return FALSE;
    }
    if (!TrueTrackPopupMenuEx) return FALSE;
    return TrueTrackPopupMenuEx(menu, flags, x, y, owner, params);
}

bool InstallHooks() {
    if (g_hooksInstalled.exchange(true)) return true;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    HMODULE comctl = LoadLibraryW(L"comctl32.dll");
    if (!user32) {
        g_hooksInstalled = false;
        return false;
    }
    TrueMessageBoxW = reinterpret_cast<MessageBoxW_t>(
        GetProcAddress(user32, "MessageBoxW"));
    TrueMessageBoxExW = reinterpret_cast<MessageBoxExW_t>(
        GetProcAddress(user32, "MessageBoxExW"));
    TrueTrackPopupMenu = reinterpret_cast<TrackPopupMenu_t>(
        GetProcAddress(user32, "TrackPopupMenu"));
    TrueTrackPopupMenuEx = reinterpret_cast<TrackPopupMenuEx_t>(
        GetProcAddress(user32, "TrackPopupMenuEx"));
    if (comctl) {
        TrueTaskDialogIndirect = reinterpret_cast<TaskDialogIndirect_t>(
            GetProcAddress(comctl, "TaskDialogIndirect"));
    }

    std::vector<HANDLE> detourThreads;
    if (DetourTransactionBegin() != NO_ERROR ||
        !UpdateAllDetourThreads(detourThreads)) {
        DetourTransactionAbort();
        CloseDetourThreadHandles(detourThreads);
        g_hooksInstalled = false;
        return false;
    }
    bool attached = true;
    auto attach = [&](PVOID* target, PVOID replacement, const wchar_t* name) {
        if (!target || !*target) return;
        const LONG result = DetourAttach(target, replacement);
        if (result != NO_ERROR) {
            FluentShell::Log(std::wstring(L"DetourAttach failed for ") + name +
                L" (" + std::to_wstring(result) + L")");
            attached = false;
        }
    };
    attach(reinterpret_cast<PVOID*>(&TrueMessageBoxW),
        reinterpret_cast<PVOID>(HookMessageBoxW), L"MessageBoxW");
    attach(reinterpret_cast<PVOID*>(&TrueMessageBoxExW),
        reinterpret_cast<PVOID>(HookMessageBoxExW), L"MessageBoxExW");
    attach(reinterpret_cast<PVOID*>(&TrueTaskDialogIndirect),
        reinterpret_cast<PVOID>(HookTaskDialogIndirect), L"TaskDialogIndirect");
    attach(reinterpret_cast<PVOID*>(&TrueTrackPopupMenu),
        reinterpret_cast<PVOID>(HookTrackPopupMenu), L"TrackPopupMenu");
    attach(reinterpret_cast<PVOID*>(&TrueTrackPopupMenuEx),
        reinterpret_cast<PVOID>(HookTrackPopupMenuEx), L"TrackPopupMenuEx");
    if (!attached || DetourTransactionCommit() != NO_ERROR) {
        DetourTransactionAbort();
        CloseDetourThreadHandles(detourThreads);
        g_hooksInstalled = false;
        return false;
    }
    CloseDetourThreadHandles(detourThreads);
    FluentShell::Log(L"MessageBox/TaskDialog/menu translation hooks installed");
    return true;
}

void UninstallHooks() noexcept {
    if (!g_hooksInstalled.load()) return;
    std::vector<HANDLE> detourThreads;
    try {
        if (DetourTransactionBegin() != NO_ERROR ||
            !UpdateAllDetourThreads(detourThreads)) {
            DetourTransactionAbort();
            CloseDetourThreadHandles(detourThreads);
            return;
        }
        if (TrueMessageBoxW) {
            DetourDetach(reinterpret_cast<PVOID*>(&TrueMessageBoxW), HookMessageBoxW);
        }
        if (TrueMessageBoxExW) {
            DetourDetach(reinterpret_cast<PVOID*>(&TrueMessageBoxExW), HookMessageBoxExW);
        }
        if (TrueTaskDialogIndirect) {
            DetourDetach(reinterpret_cast<PVOID*>(&TrueTaskDialogIndirect), HookTaskDialogIndirect);
        }
        if (TrueTrackPopupMenu) {
            DetourDetach(reinterpret_cast<PVOID*>(&TrueTrackPopupMenu), HookTrackPopupMenu);
        }
        if (TrueTrackPopupMenuEx) {
            DetourDetach(reinterpret_cast<PVOID*>(&TrueTrackPopupMenuEx), HookTrackPopupMenuEx);
        }
        if (DetourTransactionCommit() == NO_ERROR) g_hooksInstalled = false;
        CloseDetourThreadHandles(detourThreads);
    } catch (...) {
        DetourTransactionAbort();
        CloseDetourThreadHandles(detourThreads);
        OutputDebugStringW(L"FluentShell hook cleanup failed\n");
    }
}

bool HostAlreadyHasXaml() noexcept {
    return GetModuleHandleW(L"Microsoft.UI.Xaml.dll") != nullptr ||
           GetModuleHandleW(L"Windows.UI.Xaml.dll") != nullptr;
}

DWORD WINAPI BridgeWorker(LPVOID) noexcept {
    struct WorkerReset final {
        std::shared_ptr<FluentShell::Bridge::Translation::RendererSession> session;
        bool published = false;

        ~WorkerReset() noexcept {
            if (session) session->Stop();
            UninstallHooks();
            if (published) {
                try {
                    FluentShell::Bridge::Translation::SetActiveRendererSession(nullptr);
                } catch (...) {
                    OutputDebugStringW(L"FluentShell active-session cleanup failed\n");
                }
            }
            g_workerRunning.store(false);
        }
    } reset;
    try {
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&g_self), &pinned) || pinned != g_self) {
            FluentShell::Log(L"Bridge module pin failed; refusing to install hooks");
            return 0;
        }

        wchar_t executablePath[MAX_PATH * 4]{};
        GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
        const auto executableName = FluentShell::FileNameOf(executablePath);
        if (FluentShell::IsProcessDenied(executableName) || HostAlreadyHasXaml()) {
            FluentShell::Log(L"Bridge refused denied or existing-XAML target");
            return 0;
        }

        // The hooks go in before the session starts: the very first capture reads a
        // menu-bar toolbar by opening the application's own menus with the popup
        // interception armed, and that interception *is* one of these hooks.
        if (!InstallHooks()) {
            FluentShell::Log(L"Translation hook installation failed; whole-window projection remains active");
        }
        reset.session = std::make_shared<
            FluentShell::Bridge::Translation::RendererSession>(g_self);
        FluentShell::Bridge::Translation::SetActiveRendererSession(reset.session);
        reset.published = true;
        if (!reset.session->Start()) {
            FluentShell::Log(L"Renderer unavailable; native UI remains authoritative");
            return 0;
        }
        reset.session->RunSupervisor();
    } catch (const std::exception& exception) {
        OutputDebugStringA(exception.what());
        OutputDebugStringA("\n");
        try {
            std::wstring detail;
            for (const unsigned char character : std::string_view(exception.what()))
                detail.push_back(static_cast<wchar_t>(character));
            FluentShell::Log(L"Bridge worker std::exception: " + detail);
        }
        catch (...) {}
    } catch (...) {
        OutputDebugStringW(L"FluentShell bridge worker exception; restoring native UI\n");
        try { FluentShell::Log(L"Bridge worker exception; restoring native UI"); }
        catch (...) {}
    }
    return 0;
}

DWORD StartBridgeWorker() noexcept {
    bool expected = false;
    if (!g_workerRunning.compare_exchange_strong(expected, true))
        return ERROR_ALREADY_EXISTS;
    HANDLE worker = CreateThread(nullptr, 0, BridgeWorker, nullptr, 0, nullptr);
    if (!worker) {
        g_workerRunning.store(false);
        return ERROR_FUNCTION_FAILED;
    }
    CloseHandle(worker);
    return ERROR_SUCCESS;
}

} // namespace

extern "C" __declspec(dllexport) void FluentShell_Ping() noexcept {
    try { FluentShell::Log(L"Bridge ping"); } catch (...) {}
}

extern "C" __declspec(dllexport) DWORD WINAPI FluentShell_IsRendererReady(LPVOID) noexcept {
    try {
        const auto session = FluentShell::Bridge::Translation::ActiveRendererSession();
        return session && session->IsReady() ? TRUE : FALSE;
    } catch (...) {
        return FALSE;
    }
}

extern "C" __declspec(dllexport) DWORD WINAPI FluentShell_Start(LPVOID) {
    return StartBridgeWorker();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        StartBridgeWorker();
    }
    return TRUE;
}
