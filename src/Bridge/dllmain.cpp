#include "../Common/FluentShell.h"
#include "../Common/ProcessPolicy.h"
#include "Translation/DialogTranslator.h"
#include "Translation/MenuBarCapture.h"
#include "Translation/SourceThreadAgent.h"
#include "Translation/RendererSession.h"

#include "../../third_party/detours/src/detours.h"

#include <commctrl.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace {

using MessageBoxW_t = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
using MessageBoxExW_t = int(WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT, WORD);
using MessageBoxIndirectW_t = int(WINAPI*)(const MSGBOXPARAMSW*);
using TaskDialogIndirect_t = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
using TaskDialog_t = HRESULT(WINAPI*)(HWND, HINSTANCE, PCWSTR, PCWSTR, PCWSTR,
    TASKDIALOG_COMMON_BUTTON_FLAGS, PCWSTR, int*);
using TrackPopupMenu_t = BOOL(WINAPI*)(HMENU, UINT, int, int, int, HWND, const RECT*);
using TrackPopupMenuEx_t = BOOL(WINAPI*)(HMENU, UINT, int, int, HWND, LPTPMPARAMS);

MessageBoxW_t TrueMessageBoxW = nullptr;
MessageBoxExW_t TrueMessageBoxExW = nullptr;
MessageBoxIndirectW_t TrueMessageBoxIndirectW = nullptr;
TaskDialogIndirect_t TrueTaskDialogIndirect = nullptr;
TaskDialog_t TrueTaskDialog = nullptr;
TrackPopupMenu_t TrueTrackPopupMenu = nullptr;
TrackPopupMenuEx_t TrueTrackPopupMenuEx = nullptr;
std::atomic<bool> g_hooksInstalled{ false };
std::atomic<bool> g_workerRunning{ false };
HMODULE g_self = nullptr;
// Depth of native dialog calls on this thread.  A native API entered from a hook
// (MessageBoxW calls MessageBoxExW, a native TaskDialog runs a modal loop) must
// not be translated again from inside itself, but a dialog the application opens
// from a translated dialog's callback -- while the Bridge pumps the caller's
// messages -- is translated like any other, so only the native calls count.
thread_local int g_hookDepth = 0;

struct HookDepthGuard final {
    HookDepthGuard() noexcept { ++g_hookDepth; }
    ~HookDepthGuard() { --g_hookDepth; }
};

template <typename Function, typename... Arguments>
auto CallNative(Function function, Arguments... arguments) {
    HookDepthGuard depth;
    return function(arguments...);
}

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
    if (g_hookDepth == 0) {
        try {
            const auto translated = FluentShell::Bridge::Translation::TranslateMessageBox(
                FluentShell::Bridge::Translation::ActiveRendererSession(),
                owner, text, caption, type);
            if (translated) return *translated;
        } catch (...) {
            FluentShell::Log(L"MessageBox translation threw; using native API");
        }
    }
    return CallNative(TrueMessageBoxW, owner, text, caption, type);
}

int WINAPI HookMessageBoxExW(
    HWND owner,
    LPCWSTR text,
    LPCWSTR caption,
    UINT type,
    WORD languageId) {
    if (!TrueMessageBoxExW) {
        return TrueMessageBoxW ? CallNative(TrueMessageBoxW, owner, text, caption, type) : 0;
    }
    if (g_hookDepth == 0) {
        try {
            const auto translated = FluentShell::Bridge::Translation::TranslateMessageBox(
                FluentShell::Bridge::Translation::ActiveRendererSession(),
                owner, text, caption, type, languageId);
            if (translated) return *translated;
        } catch (...) {
            FluentShell::Log(L"MessageBoxEx translation threw; using native API");
        }
    }
    return CallNative(TrueMessageBoxExW, owner, text, caption, type, languageId);
}

int WINAPI HookMessageBoxIndirectW(const MSGBOXPARAMSW* params) {
    if (!TrueMessageBoxIndirectW) return 0;
    if (g_hookDepth == 0) {
        try {
            const auto translated = FluentShell::Bridge::Translation::TranslateMessageBoxIndirect(
                FluentShell::Bridge::Translation::ActiveRendererSession(), params);
            if (translated) return *translated;
        } catch (...) {
            FluentShell::Log(L"MessageBoxIndirect translation threw; using native API");
        }
    }
    return CallNative(TrueMessageBoxIndirectW, params);
}

// Answers a TaskDialogIndirect call with the projection, or returns false to let
// the native dialog run.
bool TranslateTaskDialogCall(
    const TASKDIALOGCONFIG* config,
    int* button,
    int* radioButton,
    BOOL* verificationChecked) {
    if (g_hookDepth != 0) return false;
    try {
        const auto translated = FluentShell::Bridge::Translation::TranslateTaskDialog(
            FluentShell::Bridge::Translation::ActiveRendererSession(), config);
        if (!translated) return false;
        if (button) *button = translated->button;
        if (radioButton) *radioButton = translated->radioButton;
        if (verificationChecked) {
            *verificationChecked = translated->verificationChecked ? TRUE : FALSE;
        }
        return true;
    } catch (...) {
        FluentShell::Log(L"TaskDialog translation threw; using native API");
        return false;
    }
}

HRESULT WINAPI HookTaskDialogIndirect(
    const TASKDIALOGCONFIG* config,
    int* button,
    int* radioButton,
    BOOL* verificationChecked) {
    if (!TrueTaskDialogIndirect) return E_FAIL;
    if (TranslateTaskDialogCall(config, button, radioButton, verificationChecked)) return S_OK;
    return CallNative(TrueTaskDialogIndirect, config, button, radioButton, verificationChecked);
}

// TaskDialog is TaskDialogIndirect with a fixed configuration; comctl32 calls its
// own implementation directly, so it is hooked on its own.
HRESULT WINAPI HookTaskDialog(
    HWND owner,
    HINSTANCE instance,
    PCWSTR title,
    PCWSTR instruction,
    PCWSTR content,
    TASKDIALOG_COMMON_BUTTON_FLAGS buttons,
    PCWSTR icon,
    int* button) {
    if (!TrueTaskDialog) return E_FAIL;
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.hInstance = instance;
    config.dwCommonButtons = buttons;
    config.pszWindowTitle = title;
    config.pszMainInstruction = instruction;
    config.pszContent = content;
    config.pszMainIcon = icon;
    // TaskDialog lets Escape close a dialog that has no Cancel button.
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    if (TranslateTaskDialogCall(&config, button, nullptr, nullptr)) return S_OK;
    return CallNative(TrueTaskDialog, owner, instance, title, instruction, content, buttons, icon, button);
}

// A menu-bar toolbar opens its popup from the click itself, so the projection performs
// that click through the control's own accessible default action and records the HMENU
// here instead of letting the application's popup reach the screen.  Interception is
// armed around the read. Late callbacks from a read owner remain suppressed while
// that owner's native window is cloaked; restored/unrelated owners use the real API.
BOOL WINAPI HookTrackPopupMenu(
    HMENU menu,
    UINT flags,
    int x,
    int y,
    int reserved,
    HWND owner,
    const RECT* rect) {
    BOOL projectedResult = FALSE;
    if (!FluentShell::Bridge::Translation::PopupSuppressionActive() &&
        FluentShell::Bridge::Translation::TryTrackIslandPopup(menu, flags, owner, projectedResult))
        return projectedResult;
    if (FluentShell::Bridge::Translation::ShouldSuppressPopup(owner)) {
        // Reads/late callbacks dismiss; an explicit projected selection is resolved
        // against this live HMENU and preserves its owner and TPM_RETURNCMD contract.
        return FluentShell::Bridge::Translation::RecordInterceptedPopup(menu, flags, owner);
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
    BOOL projectedResult = FALSE;
    if (!FluentShell::Bridge::Translation::PopupSuppressionActive() &&
        FluentShell::Bridge::Translation::TryTrackIslandPopup(menu, flags, owner, projectedResult))
        return projectedResult;
    if (FluentShell::Bridge::Translation::ShouldSuppressPopup(owner)) {
        return FluentShell::Bridge::Translation::RecordInterceptedPopup(menu, flags, owner);
    }
    if (!TrueTrackPopupMenuEx) return FALSE;
    return TrueTrackPopupMenuEx(menu, flags, x, y, owner, params);
}

// The comctl32 that exports TaskDialogIndirect: version 6, which a process loads
// side by side with version 5 when its manifest asks for it.  LoadLibrary from the
// Bridge thread resolves through the process default activation context.
HMODULE TaskDialogModule() noexcept {
    HMODULE modules[1024]{};
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        const DWORD count = (std::min)(needed / static_cast<DWORD>(sizeof(HMODULE)),
            static_cast<DWORD>(std::size(modules)));
        for (DWORD index = 0; index < count; ++index) {
            wchar_t name[MAX_PATH]{};
            if (!GetModuleBaseNameW(GetCurrentProcess(), modules[index], name, MAX_PATH) ||
                _wcsicmp(name, L"comctl32.dll") != 0) continue;
            if (GetProcAddress(modules[index], "TaskDialogIndirect")) return modules[index];
        }
    }
    return LoadLibraryW(L"comctl32.dll");
}

bool InstallHooks() {
    if (g_hooksInstalled.exchange(true)) return true;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    HMODULE comctl = TaskDialogModule();
    if (!user32) {
        g_hooksInstalled = false;
        return false;
    }
    TrueMessageBoxW = reinterpret_cast<MessageBoxW_t>(
        GetProcAddress(user32, "MessageBoxW"));
    TrueMessageBoxExW = reinterpret_cast<MessageBoxExW_t>(
        GetProcAddress(user32, "MessageBoxExW"));
    TrueMessageBoxIndirectW = reinterpret_cast<MessageBoxIndirectW_t>(
        GetProcAddress(user32, "MessageBoxIndirectW"));
    TrueTrackPopupMenu = reinterpret_cast<TrackPopupMenu_t>(
        GetProcAddress(user32, "TrackPopupMenu"));
    TrueTrackPopupMenuEx = reinterpret_cast<TrackPopupMenuEx_t>(
        GetProcAddress(user32, "TrackPopupMenuEx"));
    if (comctl) {
        TrueTaskDialogIndirect = reinterpret_cast<TaskDialogIndirect_t>(
            GetProcAddress(comctl, "TaskDialogIndirect"));
        TrueTaskDialog = reinterpret_cast<TaskDialog_t>(
            GetProcAddress(comctl, "TaskDialog"));
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
    attach(reinterpret_cast<PVOID*>(&TrueMessageBoxIndirectW),
        reinterpret_cast<PVOID>(HookMessageBoxIndirectW), L"MessageBoxIndirectW");
    attach(reinterpret_cast<PVOID*>(&TrueTaskDialogIndirect),
        reinterpret_cast<PVOID>(HookTaskDialogIndirect), L"TaskDialogIndirect");
    attach(reinterpret_cast<PVOID*>(&TrueTaskDialog),
        reinterpret_cast<PVOID>(HookTaskDialog), L"TaskDialog");
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
        if (TrueTaskDialog) {
            DetourDetach(reinterpret_cast<PVOID*>(&TrueTaskDialog), HookTaskDialog);
        }
        if (TrueMessageBoxIndirectW) {
            DetourDetach(reinterpret_cast<PVOID*>(&TrueMessageBoxIndirectW), HookMessageBoxIndirectW);
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
