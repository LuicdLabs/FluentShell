#pragma once

#include "../Common/FluentShell.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace FluentShell::Injector {

inline constexpr wchar_t kRendererExeName[] = L"FluentShell.Renderer.exe";
inline constexpr wchar_t kInjectorExeName[] = L"FluentShell.Injector.exe";

struct InjectOptions {
    std::wstring requestedImagePath;
    std::optional<DWORD> pid;
    std::optional<std::wstring> sha256;
};

// Owns a process handle.  While it is open, the process object cannot be freed, so its PID cannot be
// given to another process.  Every check made through the handle therefore describes the process the
// injection reaches, which is what closes the window between checking a PID and opening it.
class ProcessHandle final {
public:
    ProcessHandle() = default;
    explicit ProcessHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ProcessHandle() { Reset(); }
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ProcessHandle(ProcessHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    ProcessHandle& operator=(ProcessHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }
    void Reset() noexcept {
        if (handle_) CloseHandle(handle_);
        handle_ = nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};

// Paths and hashes.
std::wstring ExeDir();
std::wstring BridgePath();
std::wstring RendererPath();
std::optional<std::wstring> CanonicalFilePath(const std::wstring& path);
bool SamePath(std::wstring_view left, std::wstring_view right);
std::wstring ProcessImagePath(HANDLE process);
std::optional<std::wstring> ComputeSha256(const std::wstring& path);
std::optional<std::wstring> Sha256HexOfBytes(std::string_view bytes);

// Process facts, read through a handle that is already open.
std::optional<DWORD> SessionOf(HANDLE process);
std::optional<DWORD> ProcessIntegrityLevel(HANDLE process);
std::optional<ULONGLONG> CreationTimeOf(HANDLE process);
bool IsWow64Process(HANDLE process);
bool IsArchitectureCompatible(HANDLE process);

// Opens a process with the rights injection needs.  Empty when the process cannot be opened, which
// is what an elevated or protected process looks like from here.
ProcessHandle OpenInjectionTarget(DWORD pid);

// Enables SeDebugPrivilege for this process when the account holds it.  Best effort.
bool EnableDebugPrivilege();

enum class InjectStatus { Injected, Refused, Failed };

struct InjectResult {
    InjectStatus status = InjectStatus::Failed;
    std::wstring reason;
};

// Injects the Bridge into one process.  The checks run through a handle held for the whole operation:
// the image, the creation time when expectedCreation is given, the interactive session, the integrity
// level, and the architecture.  A WOW64 target is handed to the Win32 injector while the handle is held.
InjectResult InjectTarget(
    DWORD pid,
    std::optional<ULONGLONG> expectedCreation,
    const std::wstring& canonicalImage,
    const std::optional<std::wstring>& sha256);

// The explicit injector command: validates the request and runs InjectTarget.  Returns the exit code.
int InjectRequestedTarget(const InjectOptions& options);

} // namespace FluentShell::Injector
