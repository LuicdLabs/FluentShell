// injector.exe — delivers anyfluenthook.dll into target processes.
//   --pid <N>          targeted injection (CreateRemoteThread + LoadLibraryW)
//   --exe <path>       launch a process, then inject it
//   --global           system-wide WH_CBT hook (any #32770 on the desktop)
//   --watch            WinEvent watcher: inject each process that shows a #32770
//   --dll <path>       override DLL path (default: anyfluenthook.dll beside injector)
// Run elevated to reach elevated/system dialogs (UIPI + SeDebugPrivilege).
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <stdio.h>

static void EnableDebugPrivilege() {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
    CloseHandle(tok);
}

static bool InjectDll(DWORD pid, const wchar_t* dll) {
    HANDLE hp = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                            PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hp) { wprintf(L"  OpenProcess(%lu) failed: %lu\n", pid, GetLastError()); return false; }
    SIZE_T sz = (lstrlenW(dll) + 1) * sizeof(wchar_t);
    void* rem = VirtualAllocEx(hp, nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rem || !WriteProcessMemory(hp, rem, dll, sz, nullptr)) {
        wprintf(L"  alloc/write failed: %lu\n", GetLastError());
        if (rem) VirtualFreeEx(hp, rem, 0, MEM_RELEASE);
        CloseHandle(hp); return false;
    }
    auto load = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE th = CreateRemoteThread(hp, nullptr, 0, load, rem, 0, nullptr);
    if (!th) { wprintf(L"  CreateRemoteThread failed: %lu\n", GetLastError());
               VirtualFreeEx(hp, rem, 0, MEM_RELEASE); CloseHandle(hp); return false; }
    WaitForSingleObject(th, 7000);
    DWORD code = 0; GetExitCodeThread(th, &code);
    VirtualFreeEx(hp, rem, 0, MEM_RELEASE);
    CloseHandle(th); CloseHandle(hp);
    wprintf(L"  injected PID %lu; remote LoadLibraryW -> 0x%08lX %s\n",
            pid, code, code ? L"(loaded)" : L"(FAILED)");
    return code != 0;
}

static const wchar_t* g_dll = nullptr;
static volatile bool  g_stop = false;
static BOOL WINAPI CtrlHandler(DWORD) { g_stop = true; return TRUE; }

static void PumpUntilStop() {
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    MSG msg;
    while (!g_stop) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(15);
    }
}

static void CALLBACK WatchProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD pid, DWORD) {
    if (idObject != OBJID_WINDOW || !hwnd) return;
    wchar_t cls[64] = L"";
    GetClassNameW(hwnd, cls, 64);
    if (lstrcmpW(cls, L"#32770") != 0 || pid == GetCurrentProcessId()) return;
    wprintf(L"[watch] #32770 in PID %lu -> injecting\n", pid);
    InjectDll(pid, g_dll);
}

int wmain(int argc, wchar_t** argv) {
    EnableDebugPrivilege();

    wchar_t defdll[MAX_PATH];
    GetModuleFileNameW(nullptr, defdll, MAX_PATH);
    if (wchar_t* slash = wcsrchr(defdll, L'\\')) { slash[1] = 0; lstrcatW(defdll, L"anyfluenthook.dll"); }
    const wchar_t* dll = defdll;
    const wchar_t* exe = nullptr;
    DWORD pid = 0;
    int mode = 0;  // 0 targeted, 1 global, 2 watch

    for (int i = 1; i < argc; i++) {
        if (!lstrcmpW(argv[i], L"--pid") && i + 1 < argc)      pid = (DWORD)_wtoi(argv[++i]);
        else if (!lstrcmpW(argv[i], L"--dll") && i + 1 < argc) dll = argv[++i];
        else if (!lstrcmpW(argv[i], L"--exe") && i + 1 < argc) exe = argv[++i];
        else if (!lstrcmpW(argv[i], L"--global")) mode = 1;
        else if (!lstrcmpW(argv[i], L"--watch"))  mode = 2;
    }

    wprintf(L"anyfluent injector | dll=%s\n", dll);
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { wprintf(L"DLL not found: %s\n", dll); return 2; }
    g_dll = dll;

    if (exe) {
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            wprintf(L"CreateProcess failed: %lu\n", GetLastError()); return 2;
        }
        pid = pi.dwProcessId;
        wprintf(L"launched %s (PID %lu)\n", exe, pid);
        Sleep(900);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }

    if (mode == 1) {  // global WH_CBT
        HMODULE m = LoadLibraryW(dll);
        HOOKPROC proc = m ? (HOOKPROC)GetProcAddress(m, "CbtProc") : nullptr;
        HHOOK hook = proc ? SetWindowsHookExW(WH_CBT, proc, m, 0) : nullptr;
        if (!hook) { wprintf(L"global hook failed: %lu\n", GetLastError()); return 1; }
        wprintf(L"GLOBAL WH_CBT hook installed. Any #32770 on this desktop is modernized.\n"
                L"Press Ctrl+C to uninstall...\n");
        PumpUntilStop();
        UnhookWindowsHookEx(hook);
        wprintf(L"global hook removed.\n");
        return 0;
    }

    if (mode == 2) {  // watch + targeted inject
        HWINEVENTHOOK w = SetWinEventHook(EVENT_SYSTEM_DIALOGSTART, EVENT_SYSTEM_DIALOGSTART,
                                          nullptr, WatchProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (!w) { wprintf(L"SetWinEventHook failed: %lu\n", GetLastError()); return 1; }
        wprintf(L"WATCH mode: injecting any process that shows a #32770. Press Ctrl+C to stop...\n");
        PumpUntilStop();
        UnhookWinEvent(w);
        return 0;
    }

    if (pid) return InjectDll(pid, dll) ? 0 : 1;

    wprintf(L"usage: injector [--exe path | --pid N] [--dll path] [--global] [--watch]\n");
    return 0;
}
