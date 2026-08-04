#include "../Common/FluentShell.h"
#include "../Common/ProcessPolicy.h"
#include "../Renderer.Dwm/DwmRenderer.h"

#include <TlHelp32.h>
#include <wincrypt.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;

#ifndef IMAGE_FILE_MACHINE_ARM64EC
#define IMAGE_FILE_MACHINE_ARM64EC 0xA641
#endif

namespace {

constexpr wchar_t kRendererExeName[] = L"FluentShell.Renderer.exe";

struct InjectOptions {
    std::wstring requestedImagePath;
    std::optional<DWORD> pid;
    std::optional<std::wstring> sha256;
};

std::wstring ExeDir() {
    wchar_t buffer[MAX_PATH * 4]{};
    const DWORD size = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return fs::path(std::wstring(buffer, size)).parent_path().wstring();
}

std::wstring BridgePath() {
    return (fs::path(ExeDir()) / FluentShell::kBridgeDllName).wstring();
}

std::wstring RendererPath() {
    return (fs::path(ExeDir()) / L"Renderer" / kRendererExeName).wstring();
}

std::wstring StripExtendedPrefix(std::wstring path) {
    constexpr std::wstring_view uncPrefix = L"\\\\?\\UNC\\";
    constexpr std::wstring_view localPrefix = L"\\\\?\\";
    if (path.starts_with(uncPrefix)) {
        return L"\\\\" + path.substr(uncPrefix.size());
    }
    if (path.starts_with(localPrefix)) {
        return path.substr(localPrefix.size());
    }
    return path;
}

std::optional<std::wstring> CanonicalFilePath(const std::wstring& path) {
    if (path.empty() || !fs::path(path).is_absolute()) {
        return std::nullopt;
    }

    HANDLE file = CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    const DWORD required = GetFinalPathNameByHandleW(
        file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    std::wstring result;
    if (required > 0) {
        result.resize(required);
        const DWORD written = GetFinalPathNameByHandleW(
            file, result.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (written > 0 && written < required) {
            result.resize(written);
        } else {
            result.clear();
        }
    }
    CloseHandle(file);
    if (result.empty()) return std::nullopt;
    return StripExtendedPrefix(std::move(result));
}

bool SamePath(std::wstring_view left, std::wstring_view right) {
    return FluentShell::EqualsIgnoreCase(left, right);
}

bool ParsePid(const wchar_t* text, DWORD& result) {
    if (!text || !*text) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long value = wcstoul(text, &end, 10);
    if (errno != 0 || !end || *end != L'\0' || value == 0 || value > MAXDWORD) {
        return false;
    }
    result = static_cast<DWORD>(value);
    return true;
}

bool IsSha256Text(std::wstring_view value) {
    if (value.size() != 64) return false;
    for (const wchar_t c : value) {
        if (!iswxdigit(c)) return false;
    }
    return true;
}

std::optional<InjectOptions> ParseInjectOptions(int argc, wchar_t** argv) {
    if (argc < 3) return std::nullopt;

    InjectOptions options;
    options.requestedImagePath = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::wstring_view option = argv[i];
        if (option == L"--pid") {
            if (options.pid || i + 1 >= argc) return std::nullopt;
            DWORD pid = 0;
            if (!ParsePid(argv[++i], pid)) return std::nullopt;
            options.pid = pid;
        } else if (option == L"--sha256") {
            if (options.sha256 || i + 1 >= argc) return std::nullopt;
            std::wstring hash = argv[++i];
            if (!IsSha256Text(hash)) return std::nullopt;
            for (auto& c : hash) c = static_cast<wchar_t>(towlower(c));
            options.sha256 = std::move(hash);
        } else if (option == L"--signer") {
            std::wcerr << L"--signer is not implemented; use --sha256 to pin the target binary.\n";
            return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

std::optional<std::wstring> ComputeSha256(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(
                  &provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
              CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    std::array<BYTE, 64 * 1024> buffer{};
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        ok = CryptHashData(hash, buffer.data(), read, 0) == TRUE;
    }

    std::array<BYTE, 32> digest{};
    DWORD digestSize = static_cast<DWORD>(digest.size());
    if (ok) {
        ok = CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &digestSize, 0) == TRUE &&
             digestSize == digest.size();
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    if (!ok) return std::nullopt;

    static constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(digest.size() * 2);
    for (const BYTE byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

bool EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        CloseHandle(token);
        return false;
    }
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    const bool enabled = GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return enabled;
}

bool SameInteractiveSession(DWORD pid) {
    DWORD currentSession = 0;
    DWORD targetSession = 0;
    return ProcessIdToSessionId(GetCurrentProcessId(), &currentSession) &&
           ProcessIdToSessionId(pid, &targetSession) &&
           currentSession == targetSession;
}

std::optional<DWORD> ProcessIntegrityLevel(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return std::nullopt;
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        CloseHandle(process);
        return std::nullopt;
    }
    CloseHandle(process);

    DWORD bytes = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
        CloseHandle(token);
        return std::nullopt;
    }
    std::vector<BYTE> buffer(bytes);
    if (!GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), bytes, &bytes)) {
        CloseHandle(token);
        return std::nullopt;
    }
    CloseHandle(token);

    const auto label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
    const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (count == 0) return std::nullopt;
    return *GetSidSubAuthority(label->Label.Sid, count - 1);
}

bool IsArchitectureCompatible(DWORD pid) {
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto function = kernel ? reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(kernel, "IsWow64Process2")) : nullptr;
    if (!function) return true;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    const BOOL ok = function(process, &processMachine, &nativeMachine);
    CloseHandle(process);
    if (!ok) return false;

#if defined(_M_X64) || defined(__x86_64__)
    return processMachine == IMAGE_FILE_MACHINE_UNKNOWN &&
           nativeMachine == IMAGE_FILE_MACHINE_AMD64;
#elif defined(_M_ARM64)
    return processMachine == IMAGE_FILE_MACHINE_UNKNOWN &&
           (nativeMachine == IMAGE_FILE_MACHINE_ARM64 || nativeMachine == IMAGE_FILE_MACHINE_ARM64EC);
#else
    return processMachine == IMAGE_FILE_MACHINE_I386;
#endif
}

std::vector<DWORD> FindProcessesByCanonicalPath(const std::wstring& expectedPath) {
    std::vector<DWORD> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{sizeof(entry)};
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == 0) continue;
            const auto processPath = FluentShell::GetProcessImagePath(entry.th32ProcessID);
            const auto canonical = CanonicalFilePath(processPath);
            if (canonical && SamePath(*canonical, expectedPath)) {
                result.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::optional<DWORD> ResolveTargetPid(
    const std::wstring& expectedPath,
    const std::optional<DWORD>& requestedPid) {
    if (requestedPid) {
        const auto actual = CanonicalFilePath(FluentShell::GetProcessImagePath(*requestedPid));
        if (!actual) {
            std::wcerr << L"Cannot resolve the image path for PID " << *requestedPid << L".\n";
            return std::nullopt;
        }
        if (!SamePath(*actual, expectedPath)) {
            std::wcerr << L"REFUSED: PID " << *requestedPid << L" runs a different image.\n"
                       << L"  expected: " << expectedPath << L"\n"
                       << L"  actual:   " << *actual << L"\n";
            return std::nullopt;
        }
        return requestedPid;
    }

    const auto matches = FindProcessesByCanonicalPath(expectedPath);
    if (matches.empty()) {
        std::wcerr << L"No running process uses the requested image path.\n";
        return std::nullopt;
    }
    if (matches.size() > 1) {
        std::wcerr << L"REFUSED: " << matches.size()
                   << L" processes use this image path; select one with --pid.\n";
        for (const DWORD pid : matches) std::wcerr << L"  PID " << pid << L"\n";
        return std::nullopt;
    }
    return matches.front();
}

std::wstring ProcessImagePath(HANDLE process) {
    wchar_t buffer[MAX_PATH * 4]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!QueryFullProcessImageNameW(process, 0, buffer, &size)) return {};
    return std::wstring(buffer, size);
}

bool InjectDll(
    DWORD pid,
    const std::wstring& expectedTargetPath,
    const std::wstring& dllPath) {
    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!process) {
        std::wcerr << L"OpenProcess failed: " << GetLastError() << L"\n";
        return false;
    }

    const auto actualTargetPath = CanonicalFilePath(ProcessImagePath(process));
    if (!actualTargetPath || !SamePath(*actualTargetPath, expectedTargetPath)) {
        std::wcerr << L"REFUSED: target image changed before injection.\n";
        CloseHandle(process);
        return false;
    }

    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        CloseHandle(process);
        return false;
    }
    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel, "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        std::wcerr << L"CreateRemoteThread failed: " << GetLastError() << L"\n";
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, 15000);
    if (wait != WAIT_OBJECT_0) {
        std::wcerr << L"Remote LoadLibrary wait failed or timed out: " << wait << L"\n";
        CloseHandle(thread);
        CloseHandle(process);
        return false;
    }
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    if (!exitCode) {
        std::wcerr << L"LoadLibraryW returned NULL in the target process.\n";
        return false;
    }
    return true;
}

std::optional<uintptr_t> RemoteModuleBase(
    DWORD pid, const std::wstring& modulePath) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
    const auto expected = CanonicalFilePath(modulePath);
    MODULEENTRY32W entry{sizeof(entry)};
    std::optional<uintptr_t> found;
    if (expected && Module32FirstW(snapshot, &entry)) {
        do {
            const auto actual = CanonicalFilePath(entry.szExePath);
            if (actual && SamePath(*actual, *expected)) {
                found = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

std::optional<uintptr_t> LocalExportRva(
    const std::wstring& modulePath, const char* exportName) {
    HMODULE module = LoadLibraryExW(modulePath.c_str(), nullptr,
        DONT_RESOLVE_DLL_REFERENCES);
    if (!module) return std::nullopt;
    const auto address = reinterpret_cast<uintptr_t>(GetProcAddress(module, exportName));
    const auto base = reinterpret_cast<uintptr_t>(module);
    FreeLibrary(module);
    if (!address || address < base) return std::nullopt;
    return address - base;
}

bool InvokeRemoteExport(
    DWORD pid,
    const std::wstring& modulePath,
    const char* exportName,
    DWORD timeoutMs,
    DWORD* exitCode = nullptr) {
    const auto base = RemoteModuleBase(pid, modulePath);
    const auto rva = LocalExportRva(modulePath, exportName);
    if (!base || !rva) return false;
    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!process) return false;
    auto entry = reinterpret_cast<LPTHREAD_START_ROUTINE>(*base + *rva);
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, entry, nullptr, 0, nullptr);
    if (!thread) {
        CloseHandle(process);
        return false;
    }
    const DWORD wait = WaitForSingleObject(thread, timeoutMs);
    DWORD result = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeThread(thread, &result);
    if (exitCode) *exitCode = result;
    CloseHandle(thread);
    CloseHandle(process);
    return wait == WAIT_OBJECT_0;
}

bool IsBridgeLoaded(DWORD pid, const std::wstring& bridgePath) {
    return RemoteModuleBase(pid, bridgePath).has_value();
}

bool InjectBridge(DWORD pid, const std::wstring& expectedTargetPath) {
    const auto bridge = BridgePath();
    const auto renderer = RendererPath();
    if (!fs::is_regular_file(bridge)) {
        std::wcerr << L"Missing production payload: " << bridge << L"\n";
        return false;
    }
    if (!fs::is_regular_file(renderer)) {
        std::wcerr << L"Missing renderer payload: " << renderer << L"\n";
        return false;
    }
    // Explicit reinjection may target a process that still has the pinned
    // Bridge loaded after a renderer fault.  Do not call LoadLibraryW again:
    // each call increments the target's module reference count.
    if (!IsBridgeLoaded(pid, bridge) && !InjectDll(pid, expectedTargetPath, bridge))
        return false;

    const ULONGLONG deadline = GetTickCount64() + 5000;
    do {
        if (IsBridgeLoaded(pid, bridge)) {
            // A previous worker can still be unwinding after a renderer/pipe
            // fault.  Poll readiness while periodically asking the exported
            // start entry to launch a new generation once that worker clears.
            const ULONGLONG readyDeadline = GetTickCount64() + 20000;
            ULONGLONG nextStart = 0;
            while (GetTickCount64() < readyDeadline) {
                DWORD readyResult = 0;
                if (InvokeRemoteExport(pid, bridge, "FluentShell_IsRendererReady", 2000,
                        &readyResult) && readyResult == TRUE) {
                    std::wcout << L"Bridge renderer ready in PID " << pid << L"\n";
                    return true;
                }
                const ULONGLONG now = GetTickCount64();
                if (now >= nextStart) {
                    DWORD startResult = ERROR_FUNCTION_FAILED;
                    if (!InvokeRemoteExport(pid, bridge, "FluentShell_Start", 5000,
                            &startResult) ||
                        (startResult != ERROR_SUCCESS &&
                         startResult != ERROR_ALREADY_EXISTS)) {
                        std::wcerr << L"Bridge restart entry failed in PID " << pid << L".\n";
                        return false;
                    }
                    nextStart = now + 500;
                }
                Sleep(100);
            }
            std::wcerr << L"Bridge loaded but renderer did not become ready in PID " << pid << L".\n";
            return false;
        }
        Sleep(50);
    } while (GetTickCount64() < deadline);
    std::wcerr << L"Bridge was loaded but did not become observable in PID " << pid << L".\n";
    return false;
}

void RunL0Diagnostic() {
    auto rules = FluentShell::Dwm::DefaultRules();
    auto apply = [](HWND hwnd, const FluentShell::MatchRule& rule, void*) -> bool {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (FluentShell::IsProcessDeniedByPid(pid)) return true;
        wchar_t className[128]{};
        GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
        if (FluentShell::IsShellOrXamlWindowClass(className)) return true;
        FluentShell::Dwm::ApplyToWindow(hwnd, rule.dwm);
        wchar_t title[256]{};
        GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        std::wcout << L"[L0 diagnostic] " << className << L" | " << title << L"\n";
        return true;
    };
    FluentShell::Dwm::EnumTopLevelWindows(rules, apply, nullptr);
}

int InjectRequestedTarget(const InjectOptions& options) {
    const auto canonical = CanonicalFilePath(options.requestedImagePath);
    if (!canonical) {
        std::wcerr << L"Target must be an existing absolute executable image path.\n";
        return 2;
    }

    const auto name = FluentShell::FileNameOf(*canonical);
    if (FluentShell::IsProcessDenied(name)) {
        std::wcerr << L"REFUSED: " << name << L" is a shell, security, XAML, or FluentShell process.\n";
        return 4;
    }
    if (options.sha256) {
        const auto actualHash = ComputeSha256(*canonical);
        if (!actualHash) {
            std::wcerr << L"Unable to hash target image: " << *canonical << L"\n";
            return 2;
        }
        if (*actualHash != *options.sha256) {
            std::wcerr << L"REFUSED: SHA-256 mismatch.\n"
                       << L"  expected: " << *options.sha256 << L"\n"
                       << L"  actual:   " << *actualHash << L"\n";
            return 4;
        }
    }

    const auto pid = ResolveTargetPid(*canonical, options.pid);
    if (!pid) return 3;
    if (*pid == GetCurrentProcessId() || !SameInteractiveSession(*pid)) {
        std::wcerr << L"REFUSED: target must be another process in this interactive session.\n";
        return 4;
    }
    const auto injectorIntegrity = ProcessIntegrityLevel(GetCurrentProcessId());
    const auto targetIntegrity = ProcessIntegrityLevel(*pid);
    if (!injectorIntegrity || !targetIntegrity || *injectorIntegrity != *targetIntegrity) {
        std::wcerr << L"REFUSED: injector and target must run at the same integrity level.\n";
        return 4;
    }
    if (!IsArchitectureCompatible(*pid)) {
        std::wcerr << L"REFUSED: target architecture does not match the x64 payload.\n";
        return 4;
    }

    std::wcout << L"Injecting " << BridgePath() << L"\n"
               << L"  image: " << *canonical << L"\n"
               << L"  PID:   " << *pid << L"\n";
    return InjectBridge(*pid, *canonical) ? 0 : 5;
}

void PrintUsage() {
    std::wcout <<
        L"FluentShell.Injector - explicit Win32 to WinUI translation\n\n"
        L"Usage:\n"
        L"  FluentShell.Injector inject <absolute-image-path> [--pid <pid>] [--sha256 <hex>]\n"
        L"  FluentShell.Injector l0                 One-pass DWM diagnostic\n\n"
        L"Injection requires one canonical image path. If more than one matching process is\n"
        L"running, --pid is mandatory. Bridge is loaded directly; Renderer\\FluentShell.Renderer.exe\n"
        L"must be present beside the injector directory. System-wide and name-only injection\n"
        L"are intentionally unsupported.\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::wstring_view command = argv[1];
    if (command == L"inject") {
        const auto options = ParseInjectOptions(argc, argv);
        if (!options) {
            PrintUsage();
            return 1;
        }
        EnableDebugPrivilege();
        return InjectRequestedTarget(*options);
    }
    if (command == L"l0" && argc == 2) {
        RunL0Diagnostic();
        return 0;
    }
    PrintUsage();
    return 1;
}
