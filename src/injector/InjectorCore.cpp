#include "InjectorCore.h"

#include "../Common/ProcessPolicy.h"

#include <TlHelp32.h>
#include <wincrypt.h>

#include <array>
#include <cerrno>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;

#ifndef IMAGE_FILE_MACHINE_ARM64EC
#define IMAGE_FILE_MACHINE_ARM64EC 0xA641
#endif

namespace FluentShell::Injector {

namespace {

// Rights the injection needs, requested in one open so the handle can answer every check.
constexpr DWORD kInjectionRights =
    PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION |
    PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;

class Sha256Context final {
public:
    Sha256Context() {
        ok_ = CryptAcquireContextW(&provider_, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) == TRUE &&
              CryptCreateHash(provider_, CALG_SHA_256, 0, 0, &hash_) == TRUE;
    }
    ~Sha256Context() {
        if (hash_) CryptDestroyHash(hash_);
        if (provider_) CryptReleaseContext(provider_, 0);
    }
    Sha256Context(const Sha256Context&) = delete;
    Sha256Context& operator=(const Sha256Context&) = delete;

    bool Update(const void* data, DWORD size) {
        return ok_ && CryptHashData(hash_, static_cast<const BYTE*>(data), size, 0) == TRUE;
    }

    std::optional<std::wstring> Finish() {
        if (!ok_) return std::nullopt;
        std::array<BYTE, 32> digest{};
        DWORD digestSize = static_cast<DWORD>(digest.size());
        if (CryptGetHashParam(hash_, HP_HASHVAL, digest.data(), &digestSize, 0) != TRUE ||
            digestSize != digest.size()) {
            return std::nullopt;
        }
        static constexpr wchar_t hex[] = L"0123456789abcdef";
        std::wstring result;
        result.reserve(digest.size() * 2);
        for (const BYTE byte : digest) {
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0f]);
        }
        return result;
    }

private:
    HCRYPTPROV provider_ = 0;
    HCRYPTHASH hash_ = 0;
    bool ok_ = false;
};

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

bool InjectDll(HANDLE process, const std::wstring& dllPath) {
    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }

    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel, "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        std::wcerr << L"CreateRemoteThread failed: " << GetLastError() << L"\n";
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, 15000);
    if (wait != WAIT_OBJECT_0) {
        std::wcerr << L"Remote LoadLibrary wait failed or timed out: " << wait << L"\n";
        CloseHandle(thread);
        return false;
    }
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    if (!exitCode) {
        std::wcerr << L"LoadLibraryW returned NULL in the target process.\n";
        return false;
    }
    return true;
}

std::optional<uintptr_t> RemoteModuleBase(DWORD pid, const std::wstring& modulePath) {
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

std::optional<uintptr_t> LocalExportRva(const std::wstring& modulePath, const char* exportName) {
    HMODULE module = LoadLibraryExW(modulePath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
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
    HANDLE process = OpenProcess(kInjectionRights, FALSE, pid);
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

// The Bridge's own load and readiness protocol.  The process is the one InjectTarget holds open, so
// the PID-addressed module and export calls here reach that same process.
bool InjectBridge(DWORD pid, HANDLE process) {
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
    if (!IsBridgeLoaded(pid, bridge) && !InjectDll(process, bridge)) return false;

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
                    std::wcout << L"Bridge renderer connected in PID " << pid << L"\n"
                               << L"Projection is evaluated per window; see %TEMP%\\FluentShell.log for results.\n";
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

#if defined(_M_X64) || defined(__x86_64__)
// A 32-bit (WOW64) target is injected only by the Win32 injector and Bridge in x86\. The x64
// injector passes the already-validated request on and returns that injector's exit code.  The
// caller holds the target's handle, so the PID the child opens is the same process.
int InjectWithWin32Injector(const std::wstring& canonicalImage, DWORD pid, const std::optional<std::wstring>& sha256) {
    const auto injector = (fs::path(ExeDir()) / L"x86" / kInjectorExeName).wstring();
    if (!fs::is_regular_file(injector)) {
        std::wcerr << L"Missing Win32 injector for a 32-bit target: " << injector << L"\n";
        return 5;
    }
    std::wstring commandLine = L"\"" + injector + L"\" inject \"" + canonicalImage +
                               L"\" --pid " + std::to_wstring(pid);
    if (sha256) commandLine += L" --sha256 " + *sha256;

    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(injector.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
            0, nullptr, nullptr, &startup, &process)) {
        std::wcerr << L"Cannot start the Win32 injector: " << GetLastError() << L"\n";
        return 5;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 5;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}
#endif

void PrintRefusal(const std::wstring& reason) {
    std::wcerr << L"REFUSED: " << reason << L"\n";
}

} // namespace

std::wstring ExeDir() {
    wchar_t buffer[MAX_PATH * 4]{};
    const DWORD size = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return fs::path(std::wstring(buffer, size)).parent_path().wstring();
}

std::wstring BridgePath() {
    return (fs::path(ExeDir()) / FluentShell::kBridgeDllName).wstring();
}

std::wstring RendererPath() {
    // The Win32 injector runs from x86\; the x64 renderer it launches is shared at the production root's Renderer\.
    fs::path directory = ExeDir();
    if (directory.filename() == L"x86") directory = directory.parent_path();
    return (directory / L"Renderer" / kRendererExeName).wstring();
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

std::wstring ProcessImagePath(HANDLE process) {
    wchar_t buffer[MAX_PATH * 4]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!QueryFullProcessImageNameW(process, 0, buffer, &size)) return {};
    return std::wstring(buffer, size);
}

std::optional<std::wstring> ComputeSha256(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;

    Sha256Context context;
    std::array<BYTE, 64 * 1024> buffer{};
    bool ok = true;
    while (ok) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            ok = false;
            break;
        }
        if (read == 0) break;
        ok = context.Update(buffer.data(), read);
    }
    CloseHandle(file);
    if (!ok) return std::nullopt;
    return context.Finish();
}

std::optional<std::wstring> Sha256HexOfBytes(std::string_view bytes) {
    if (bytes.size() > MAXDWORD) return std::nullopt;
    Sha256Context context;
    if (!context.Update(bytes.data(), static_cast<DWORD>(bytes.size()))) return std::nullopt;
    return context.Finish();
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

std::optional<DWORD> SessionOf(HANDLE process) {
    DWORD session = 0;
    if (!process || !ProcessIdToSessionId(GetProcessId(process), &session)) return std::nullopt;
    return session;
}

std::optional<DWORD> ProcessIntegrityLevel(HANDLE process) {
    HANDLE token = nullptr;
    if (!process || !OpenProcessToken(process, TOKEN_QUERY, &token)) return std::nullopt;

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

std::optional<ULONGLONG> CreationTimeOf(HANDLE process) {
    FILETIME creation{}, exited{}, kernel{}, user{};
    if (!process || !GetProcessTimes(process, &creation, &exited, &kernel, &user)) return std::nullopt;
    ULARGE_INTEGER value{};
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    return value.QuadPart;
}

// The target's machines as IsWow64Process2 reports them: processMachine is UNKNOWN for a
// process of the host's native architecture and I386 for a 32-bit (WOW64) process.
bool QueryProcessMachines(HANDLE process, USHORT& processMachine, USHORT& nativeMachine) {
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto kernel = GetModuleHandleW(L"kernel32.dll");
    const auto function = kernel ? reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(kernel, "IsWow64Process2")) : nullptr;
    if (!function || !process) return false;
    processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    return function(process, &processMachine, &nativeMachine) == TRUE;
}

#if defined(_M_X64) || defined(__x86_64__)
bool IsWow64Target(HANDLE process) {
    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    return QueryProcessMachines(process, processMachine, nativeMachine) &&
           processMachine == IMAGE_FILE_MACHINE_I386;
}
#endif

bool IsArchitectureCompatible(HANDLE process) {
    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!QueryProcessMachines(process, processMachine, nativeMachine)) return false;

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

ProcessHandle OpenInjectionTarget(DWORD pid) {
    return ProcessHandle(OpenProcess(kInjectionRights, FALSE, pid));
}

InjectResult InjectTarget(
    DWORD pid,
    std::optional<ULONGLONG> expectedCreation,
    const std::wstring& canonicalImage,
    const std::optional<std::wstring>& sha256) {
    if (pid == GetCurrentProcessId()) {
        return { InjectStatus::Refused, L"the injector cannot inject itself" };
    }
    ProcessHandle process = OpenInjectionTarget(pid);
    if (!process) {
        return { InjectStatus::Refused,
            L"the process cannot be opened for injection; it may be elevated, protected, or gone" };
    }
    if (expectedCreation) {
        const auto created = CreationTimeOf(process.get());
        if (!created || *created != *expectedCreation) {
            return { InjectStatus::Refused, L"the process exited, or its PID was reused" };
        }
    }

    const auto actualImage = CanonicalFilePath(ProcessImagePath(process.get()));
    if (!actualImage || !SamePath(*actualImage, canonicalImage)) {
        return { InjectStatus::Refused, L"the target image changed before injection" };
    }
    if (FluentShell::IsProcessDenied(FluentShell::FileNameOf(*actualImage))) {
        return { InjectStatus::Refused, L"shell, security, XAML, or FluentShell process" };
    }

    const auto session = SessionOf(process.get());
    const auto ownSession = SessionOf(GetCurrentProcess());
    if (!session || !ownSession || *session != *ownSession) {
        return { InjectStatus::Refused, L"target is not in this interactive session" };
    }

    const auto ownIntegrity = ProcessIntegrityLevel(GetCurrentProcess());
    const auto targetIntegrity = ProcessIntegrityLevel(process.get());
    if (!ownIntegrity || !targetIntegrity) {
        return { InjectStatus::Refused, L"the integrity level could not be read" };
    }
    if (*targetIntegrity > *ownIntegrity) {
        return { InjectStatus::Refused, L"target is elevated above this injector" };
    }
    if (*targetIntegrity != *ownIntegrity) {
        return { InjectStatus::Refused, L"injector and target must run at the same integrity level" };
    }

#if defined(_M_X64) || defined(__x86_64__)
    if (IsWow64Target(process.get())) {
        const int exitCode = InjectWithWin32Injector(*actualImage, pid, sha256);
        if (exitCode == 0) return { InjectStatus::Injected, L"injected by the Win32 injector" };
        return { InjectStatus::Failed, L"the Win32 injector exited with " + std::to_wstring(exitCode) };
    }
#endif
    if (!IsArchitectureCompatible(process.get())) {
        return { InjectStatus::Refused, L"target architecture does not match this injector's payload" };
    }

    std::wcout << L"Injecting " << BridgePath() << L"\n"
               << L"  image: " << *actualImage << L"\n"
               << L"  PID:   " << pid << L"\n";
    if (!InjectBridge(pid, process.get())) {
        return { InjectStatus::Failed, L"the Bridge did not become ready" };
    }
    return { InjectStatus::Injected, L"Bridge renderer connected" };
}

int InjectRequestedTarget(const InjectOptions& options) {
    const auto canonical = CanonicalFilePath(options.requestedImagePath);
    if (!canonical) {
        std::wcerr << L"Target must be an existing absolute executable image path.\n";
        return 2;
    }

    const auto name = FluentShell::FileNameOf(*canonical);
    if (FluentShell::IsProcessDenied(name)) {
        PrintRefusal(std::wstring(name) + L" is a shell, security, XAML, or FluentShell process.");
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

    const InjectResult result = InjectTarget(*pid, std::nullopt, *canonical, options.sha256);
    switch (result.status) {
    case InjectStatus::Injected:
        return 0;
    case InjectStatus::Refused:
        PrintRefusal(result.reason);
        return 4;
    case InjectStatus::Failed:
        std::wcerr << L"FAILED: " << result.reason << L"\n";
        return 5;
    }
    return 5;
}

} // namespace FluentShell::Injector
