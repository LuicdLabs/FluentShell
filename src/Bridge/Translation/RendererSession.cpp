#include "RendererSession.h"

#include "../../Common/FluentShell.h"
#include "../../Common/ProcessPolicy.h"
#include "../Ipc/FrameCodec.h"
#include "UiAutomationValidator.h"
#include "DirectUiEngine.h"

#include <sddl.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace FluentShell::Bridge::Translation {
namespace {

namespace fs = std::filesystem;

constexpr DWORD kHandshakeTimeoutMs = 5000;
// Renderer heartbeats are gated by a 900 ms dispatcher probe.  Allow the
// third scheduled probe to complete before declaring three consecutive misses.
constexpr uint64_t kRendererHeartbeatTimeoutMs = 4250;
// Reconcile ticks a quiet surface every fourth 250 ms pass.  The dirty whitelist
// cannot prove it observes every way native state can change, so a slow poll stays
// as the backstop instead of trusting the flag alone.
constexpr unsigned kQuietReconcileTicks = 4;
// A source thread inside a modal loop can miss a bounded capture deadline without
// being broken.  Tolerate a few misses before discarding a working projection.
constexpr unsigned kMaxCaptureTimeouts = 3;
// Classic dialogs commonly destroy one owned top-level and create the next a
// moment later. Require a quiet owner graph before reprojecting the root so it
// does not visibly bounce between native and WinUI during that transition.
constexpr uint64_t kOwnerGraphQuietMs = 2000;

class PhysicalCoordinateScope final {
public:
    PhysicalCoordinateScope() noexcept
        : previous_(SetThreadDpiAwarenessContext(
              DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalCoordinateScope() {
        if (previous_) SetThreadDpiAwarenessContext(previous_);
    }
    bool IsValid() const noexcept { return previous_ != nullptr; }

private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

DWORD RemainingTimeout(uint64_t deadline) noexcept {
    const uint64_t now = GetTickCount64();
    if (now >= deadline) return 0;
    return static_cast<DWORD>(std::min<uint64_t>(deadline - now, MAXDWORD));
}

enum class SurfaceState {
    Native,
    Scanning,
    SurfaceReady,
    Projected,
    Restoring,
    Closed,
};

std::mutex g_activeSessionMutex;
std::shared_ptr<RendererSession> g_activeSession;

std::wstring CurrentLogonSidString() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenLogonSid, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    if (!size || !GetTokenInformation(token, TokenLogonSid, buffer.data(), size, &size)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);
    auto* groups = reinterpret_cast<TOKEN_GROUPS*>(buffer.data());
    if (groups->GroupCount != 1 || !IsValidSid(groups->Groups[0].Sid)) return {};
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(groups->Groups[0].Sid, &sidText)) return {};
    std::wstring result(sidText);
    LocalFree(sidText);
    return result;
}

bool CreatePipeSecurity(SECURITY_ATTRIBUTES& attributes, PSECURITY_DESCRIPTOR& descriptor) {
    const auto sid = CurrentLogonSidString();
    if (sid.empty()) return false;
    const std::wstring sddl = L"D:(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
    descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        return false;
    }
    attributes = { sizeof(attributes), descriptor, FALSE };
    return true;
}

RECT CenteredBounds(HWND owner, LONG width, LONG height) {
    RECT reference{};
    if (!owner || !GetWindowRect(owner, &reference)) {
        const HMONITOR monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{ sizeof(info) };
        GetMonitorInfoW(monitor, &info);
        reference = info.rcWork;
    }
    const LONG x = reference.left + std::max(0L, (reference.right - reference.left - width) / 2);
    const LONG y = reference.top + std::max(0L, (reference.bottom - reference.top - height) / 2);
    return { x, y, x + width, y + height };
}

std::wstring MessageBoxIcon(UINT type) {
    switch (type & MB_ICONMASK) {
    case MB_ICONHAND: return L"error";
    case MB_ICONQUESTION: return L"question";
    case MB_ICONEXCLAMATION: return L"warning";
    case MB_ICONASTERISK: return L"info";
    default: return L"none";
    }
}

std::wstring TaskDialogIcon(PCWSTR icon) {
    if (!icon) return L"none";
    if (icon == TD_WARNING_ICON) return L"warning";
    if (icon == TD_ERROR_ICON) return L"error";
    if (icon == TD_INFORMATION_ICON) return L"info";
    if (icon == TD_SHIELD_ICON) return L"shield";
    return L"none";
}

bool RectClose(const RECT& left, const RECT& right, LONG tolerance = 8) noexcept {
    return std::abs(left.left - right.left) <= tolerance &&
           std::abs(left.top - right.top) <= tolerance &&
           std::abs((left.right - left.left) - (right.right - right.left)) <= tolerance &&
           std::abs((left.bottom - left.top) - (right.bottom - right.top)) <= tolerance;
}

bool ClearApplicationCloak(HWND window) noexcept {
    if (!window || !IsWindow(window)) return true;
    BOOL cloak = FALSE;
    if (FAILED(DwmSetWindowAttribute(window, DWMWA_CLOAK, &cloak, sizeof(cloak)))) {
        return false;
    }
    DWORD reasons = DWM_CLOAKED_APP;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &reasons, sizeof(reasons)))) {
        return false;
    }
    return (reasons & DWM_CLOAKED_APP) == 0;
}

bool HideProxyForRestore(HWND window) noexcept {
    if (!window || !IsWindow(window)) return true;
    BOOL cloak = TRUE;
    const HRESULT set = DwmSetWindowAttribute(window, DWMWA_CLOAK, &cloak, sizeof(cloak));
    ShowWindowAsync(window, SW_HIDE);
    const uint64_t deadline = GetTickCount64() + 1000;
    do {
        if (!IsWindow(window) || !IsWindowVisible(window)) return true;
        DWORD reasons = 0;
        const HRESULT get = DwmGetWindowAttribute(
            window, DWMWA_CLOAKED, &reasons, sizeof(reasons));
        if (SUCCEEDED(set) && SUCCEEDED(get) && (reasons & DWM_CLOAKED_APP) != 0)
            return true;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return false;
}

void PumpThreadMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

} // namespace

struct RendererSession::Surface final {
    ~Surface() {
        if (resultEvent) CloseHandle(resultEvent);
    }

    std::mutex mutex;
    std::mutex canonicalMutex;
    std::condition_variable readyCondition;
    SurfaceState state = SurfaceState::Native;
    std::shared_ptr<SourceThreadAgent> agent;
    WindowSnapshot snapshot;
    SurfaceReady ready;
    uint64_t fingerprint = 0;
    bool readyReceived = false;
    bool virtualDialog = false;
    bool verificationChecked = false;
    bool restoreInProgress = false;
    bool restoreRetryScheduled = false;
    bool agentRetained = false;
    unsigned restoreAttempts = 0;
    uint64_t pendingCloseSequence = 0;
    std::optional<ActionRequest> pendingCloseAction;
    // Reconcile bookkeeping.  Written only by the supervisor thread.
    unsigned captureTimeouts = 0;
    unsigned reconcileSkips = 0;
    std::optional<uint64_t> verificationNode;
    std::unordered_map<uint64_t, int> buttonResults;
    std::optional<int> cancelResult;
    std::optional<int> dialogResult;
    bool directUiAdapter = false;
    const DirectUiWindowProfile* directUiProfile = nullptr;
    std::shared_ptr<DirectUiOwnedProfile> directUiOwnedProfile;
    DirectUiNativeEvidence directUiEvidence;
    std::unordered_map<uint64_t, DirectUiActionBinding> directUiBindings;
    HANDLE resultEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
};

RendererSession::RendererSession(HMODULE bridgeModule) noexcept
    : bridgeModule_(bridgeModule),
      shutdownEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

RendererSession::~RendererSession() {
    Stop();
    if (shutdownEvent_) CloseHandle(shutdownEvent_);
}

void SetActiveRendererSession(std::shared_ptr<RendererSession> session) {
    std::scoped_lock lock(g_activeSessionMutex);
    g_activeSession = std::move(session);
}

std::shared_ptr<RendererSession> ActiveRendererSession() {
    std::scoped_lock lock(g_activeSessionMutex);
    return g_activeSession;
}

std::wstring RendererSession::RendererPath() const {
    wchar_t modulePath[MAX_PATH * 4]{};
    const DWORD length = GetModuleFileNameW(
        bridgeModule_, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (!length || length >= std::size(modulePath)) return {};
    return (fs::path(modulePath).parent_path() / L"Renderer" / L"FluentShell.Renderer.exe").wstring();
}

bool RendererSession::CreatePipeAndRenderer(uint64_t handshakeDeadline) {
    nonce_ = Ipc::NewNonceHex();
    if (nonce_.size() != 32) return false;
    std::transform(nonce_.begin(), nonce_.end(), nonce_.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towupper(value));
    });
    pipeName_ = L"\\\\.\\pipe\\FluentShell." +
        std::to_wstring(GetCurrentProcessId()) + L"." + nonce_;

    SECURITY_ATTRIBUTES security{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!CreatePipeSecurity(security, descriptor)) {
        FluentShell::Log(L"Failed to construct renderer pipe ACL");
        return false;
    }
    pipe_ = CreateNamedPipeW(
        pipeName_.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, Ipc::kMaxPayloadBytes, Ipc::kMaxPayloadBytes, 0, &security);
    LocalFree(descriptor);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        FluentShell::Log(L"CreateNamedPipe failed: " + std::to_wstring(GetLastError()));
        return false;
    }

    const auto renderer = RendererPath();
    if (renderer.empty() || !fs::exists(renderer)) {
        FluentShell::Log(L"Renderer executable is missing: " + renderer);
        return false;
    }

    const uint64_t parentCreated = Ipc::ProcessCreationTime(GetCurrentProcess());
    std::wstring commandLine = L"\"" + renderer + L"\" --pipe \"" + pipeName_ +
        L"\" --nonce " + nonce_ +
        L" --parent-pid " + std::to_wstring(GetCurrentProcessId()) +
        L" --parent-created " + std::to_wstring(parentCreated);
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    const auto workingDirectory = fs::path(renderer).parent_path().wstring();
    if (!CreateProcessW(
            renderer.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT, nullptr, workingDirectory.c_str(),
            &startup, &process)) {
        FluentShell::Log(L"CreateProcess renderer failed: " + std::to_wstring(GetLastError()));
        return false;
    }
    rendererProcess_ = process.hProcess;
    rendererThread_ = process.hThread;
    rendererProcessId_ = process.dwProcessId;
    rendererCreated_ = Ipc::ProcessCreationTime(process.hProcess);

    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        FluentShell::Log(L"Failed to create renderer pipe connection event");
        return false;
    }
    BOOL connected = ConnectNamedPipe(pipe_, &overlapped);
    DWORD error = connected ? ERROR_SUCCESS : GetLastError();
    if (!connected && error == ERROR_PIPE_CONNECTED) {
        SetEvent(overlapped.hEvent);
    } else if (!connected && error != ERROR_IO_PENDING) {
        CloseHandle(overlapped.hEvent);
        FluentShell::Log(L"ConnectNamedPipe failed: " + std::to_wstring(error));
        return false;
    }
    HANDLE waits[] = { overlapped.hEvent, rendererProcess_ };
    const DWORD remaining = RemainingTimeout(handshakeDeadline);
    const DWORD wait = remaining == 0
        ? WAIT_TIMEOUT
        : WaitForMultipleObjects(2, waits, FALSE, remaining);
    if (wait != WAIT_OBJECT_0) {
        // OVERLAPPED and its event must remain alive until cancellation reaches
        // terminal completion, even when the renderer exits or times out.
        const BOOL cancelled = CancelIoEx(pipe_, &overlapped);
        const DWORD cancelError = cancelled ? ERROR_SUCCESS : GetLastError();
        DWORD ignored = 0;
        const BOOL drained = GetOverlappedResult(pipe_, &overlapped, &ignored, TRUE);
        const DWORD completionError = drained ? ERROR_SUCCESS : GetLastError();
        CloseHandle(overlapped.hEvent);
        if (!cancelled && cancelError != ERROR_NOT_FOUND) {
            FluentShell::Log(L"ConnectNamedPipe cancellation failed: " +
                std::to_wstring(cancelError));
        } else if (!drained && completionError != ERROR_OPERATION_ABORTED &&
                   completionError != ERROR_PIPE_NOT_CONNECTED) {
            FluentShell::Log(L"ConnectNamedPipe cancellation completed unexpectedly: " +
                std::to_wstring(completionError));
        }
        FluentShell::Log(L"Renderer pipe handshake timed out or renderer exited");
        return false;
    }
    DWORD transferred = 0;
    if (!GetOverlappedResult(pipe_, &overlapped, &transferred, FALSE) &&
        GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(overlapped.hEvent);
        FluentShell::Log(L"Renderer pipe connection did not complete");
        return false;
    }
    CloseHandle(overlapped.hEvent);
    return VerifyRendererClient();
}

bool RendererSession::VerifyRendererClient() const noexcept {
    ULONG clientPid = 0;
    if (!GetNamedPipeClientProcessId(pipe_, &clientPid) || clientPid != rendererProcessId_) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, clientPid);
    if (!process) return false;
    const uint64_t created = Ipc::ProcessCreationTime(process);
    CloseHandle(process);
    return created != 0 && created == rendererCreated_;
}

bool RendererSession::HasRendererExited() noexcept {
    try {
        std::scoped_lock lock(processMutex_);
        return rendererProcess_ &&
            WaitForSingleObject(rendererProcess_, 0) == WAIT_OBJECT_0;
    } catch (...) {
        return false;
    }
}

void RendererSession::TerminateRenderer(DWORD exitCode) noexcept {
    try {
        std::scoped_lock lock(processMutex_);
        if (rendererProcess_ &&
            WaitForSingleObject(rendererProcess_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(rendererProcess_, exitCode);
        }
    } catch (...) {}
}

bool RendererSession::Handshake(DWORD timeoutMs) {
    const uint64_t deadline = GetTickCount64() + timeoutMs;
    std::wstring error;
    const auto hello = SerializeHello(
        nonce_, L"bridge", GetCurrentProcessId(), Ipc::ProcessCreationTime(GetCurrentProcess()));
    const DWORD writeRemaining = RemainingTimeout(deadline);
    if (writeRemaining == 0 ||
        !Send(Ipc::MessageType::Hello, 0, hello, writeRemaining)) return false;

    Ipc::Frame frame;
    const DWORD readRemaining = RemainingTimeout(deadline);
    if (readRemaining == 0 ||
        !Ipc::ReadFrame(pipe_, 0, frame, error, readRemaining) ||
        frame.header.type != static_cast<uint16_t>(Ipc::MessageType::Hello) ||
        frame.header.revision != 0) {
        FluentShell::Log(L"Renderer hello read failed: " + error);
        return false;
    }
    inboundSequence_ = frame.header.sequence;
    HelloMessage message;
    if (!ParseHello(frame.payload, message, error) || message.role != L"renderer" ||
        message.nonce != nonce_ || message.processId != rendererProcessId_ ||
        message.processCreated != rendererCreated_ ||
        message.protocolMajor != frame.header.major ||
        message.protocolMinor != frame.header.minor ||
        message.protocolMinor < Ipc::kProtocolMinor) {
        FluentShell::Log(L"Renderer hello identity mismatch: " + error);
        return false;
    }
    lastHeartbeatTick_ = GetTickCount64();
    return true;
}

bool RendererSession::Start() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        FluentShell::Log(L"Bridge IPC WinRT initialization failed");
        return false;
    }
    const uint64_t handshakeDeadline = GetTickCount64() + kHandshakeTimeoutMs;
    if (!CreatePipeAndRenderer(handshakeDeadline)) {
        Fail(L"renderer startup failed");
        return false;
    }
    const DWORD remaining = RemainingTimeout(handshakeDeadline);
    if (remaining == 0 || !Handshake(remaining)) {
        Fail(L"renderer handshake failed");
        return false;
    }
    ready_ = true;
    actionWorker_ = std::thread([this] { ActionMain(); });
    reader_ = std::thread([this] { ReaderMain(); });
    heartbeatWorker_ = std::thread([this] { HeartbeatMain(); });
    DiscoverTopLevelWindows();
    FluentShell::Log(L"Out-of-process WinUI renderer connected");
    return true;
}

bool RendererSession::Send(
    Ipc::MessageType type,
    uint64_t revision,
    std::string payload,
    DWORD timeoutMs,
    bool failSessionOnError) noexcept {
    const bool teardown = type == Ipc::MessageType::SurfaceCommit ||
        type == Ipc::MessageType::WindowClose || type == Ipc::MessageType::Shutdown;
    try {
        if (!teardown && (failed_.load() || stopping_.load())) return false;
        std::wstring error;
        {
            std::scoped_lock lock(writeMutex_);
            if (pipeClosing_ || pipe_ == INVALID_HANDLE_VALUE ||
                (!teardown && (failed_.load() || stopping_.load()))) return false;
            const uint64_t sequence = outboundSequence_.fetch_add(1) + 1;
            if (Ipc::WriteFrame(pipe_, type, sequence, revision, payload, error, timeoutMs)) {
                return true;
            }
        }
        if (failSessionOnError) {
            FluentShell::Log(L"Renderer pipe write failed: " + error);
            Fail(L"renderer pipe write failed: " + error);
        }
        return false;
    } catch (...) {
        if (failSessionOnError) Fail(L"renderer pipe write exception");
        return false;
    }
}

bool RendererSession::IsRetiredSurface(std::wstring_view surfaceId) noexcept {
    try {
        std::scoped_lock lock(surfacesMutex_);
        return retiredSurfaceIds_.find(std::wstring(surfaceId)) != retiredSurfaceIds_.end();
    } catch (...) {
        // Treat bookkeeping allocation/locking failure as non-retired so the
        // authenticated peer receives a conservative protocol fault.
        return false;
    }
}

void RendererSession::RememberRetiredSurfaceLocked(std::wstring_view surfaceId) noexcept {
    if (surfaceId.empty()) return;
    try {
        const std::wstring id(surfaceId);
        if (!retiredSurfaceIds_.insert(id).second) return;
        retiredSurfaceOrder_.push_back(id);
        constexpr size_t kMaxRetiredSurfaceIds = 256;
        while (retiredSurfaceOrder_.size() > kMaxRetiredSurfaceIds) {
            retiredSurfaceIds_.erase(retiredSurfaceOrder_.front());
            retiredSurfaceOrder_.pop_front();
        }
    } catch (...) {
        // Retention is a defense-in-depth optimization.  The live Surface is
        // kept in the map when this fails, so late frames still see Closed.
    }
}

void RendererSession::RetireSurfaceIfClosed(
    const std::shared_ptr<Surface>& surface) noexcept {
    if (!surface) return;
    try {
        std::wstring id;
        {
            std::scoped_lock stateLock(surface->mutex);
            if (surface->state != SurfaceState::Closed) return;
            id = surface->snapshot.surfaceId;
        }
        if (id.empty()) return;
        std::scoped_lock surfacesLock(surfacesMutex_);
        const auto found = surfaces_.find(id);
        if (found == surfaces_.end() || found->second != surface) return;
        RememberRetiredSurfaceLocked(id);
        // If insertion above failed, retaining the closed object is safer than
        // erasing it and turning a late authenticated frame into an unknown ID.
        if (retiredSurfaceIds_.contains(id)) surfaces_.erase(found);
    } catch (...) {
        // Keep the closed surface reachable for late-frame handling.
    }
}

void RendererSession::ReaderMain() noexcept {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        while (!stopping_.load() && !failed_.load()) {
            Ipc::Frame frame;
            std::wstring error;
            const uint64_t previous = inboundSequence_.load();
            if (!Ipc::ReadFrame(pipe_, previous, frame, error)) {
                if (!stopping_.load()) Fail(L"renderer pipe read failed: " + error);
                break;
            }
            inboundSequence_ = frame.header.sequence;
            HandleFrame(frame);
        }
    } catch (...) {
        if (!stopping_.load()) Fail(L"renderer reader exception");
    }
}

void RendererSession::HandleFrame(const Ipc::Frame& frame) {
    const auto type = static_cast<Ipc::MessageType>(frame.header.type);
    std::wstring error;
    if (type == Ipc::MessageType::SurfaceReady) {
        SurfaceReady ready;
        if (!ParseSurfaceReady(frame.payload, nonce_, ready, error)) {
            Fail(error);
            return;
        }
        if (frame.header.revision != ready.revision) {
            Fail(L"surface.ready header and payload revisions differ");
            return;
        }
        HandleReady(ready);
    } else if (type == Ipc::MessageType::ActionInvoke) {
        ActionRequest action;
        if (!ParseActionInvoke(frame.payload, nonce_, action, error)) {
            Fail(error);
            return;
        }
        if (frame.header.revision != action.expectedRevision) {
            Fail(L"action.invoke header and payload revisions differ");
            return;
        }
        HandleAction(action);
    } else if (type == Ipc::MessageType::Heartbeat) {
        if (frame.header.revision != 0) {
            Fail(L"heartbeat frame must have revision zero");
            return;
        }
        if (!ParseHeartbeat(frame.payload, nonce_, error)) {
            Fail(error);
            return;
        }
        lastHeartbeatTick_ = GetTickCount64();
    } else if (type == Ipc::MessageType::Error) {
        bool fatal = true;
        std::wstring faultedSurfaceId;
        if (frame.header.revision != 0 ||
            !ParseErrorMessage(frame.payload, nonce_, error, &fatal, &faultedSurfaceId)) {
            Fail(error.empty() ? L"error frame must have revision zero" : error);
            return;
        }
        // A non-fatal error that names one surface is a per-surface fault.  Roll that
        // window back to native and leave the rest of the session projecting; the
        // other windows in this process are still correct.
        if (!fatal && !faultedSurfaceId.empty()) {
            std::shared_ptr<Surface> surface;
            {
                std::scoped_lock lock(surfacesMutex_);
                const auto found = surfaces_.find(faultedSurfaceId);
                if (found != surfaces_.end()) surface = found->second;
            }
            if (surface) {
                try { FluentShell::Log(L"Renderer surface fault: " + error); } catch (...) {}
                RestoreSurface(surface, L"rendererSurfaceFault");
                return;
            }
            if (IsRetiredSurface(faultedSurfaceId)) return;
        }
        Fail(error.empty() ? L"renderer reported a protocol error" : error);
    } else if (type == Ipc::MessageType::Shutdown) {
        if (frame.header.revision != 0 || !ParseShutdownMessage(frame.payload, nonce_, error)) {
            Fail(error.empty() ? L"shutdown frame must have revision zero" : error);
            return;
        }
        Fail(L"renderer requested shutdown");
    } else {
        Fail(L"unexpected renderer message type");
    }
}

void RendererSession::HandleReady(const SurfaceReady& ready) {
    std::shared_ptr<Surface> surface;
    {
        std::scoped_lock lock(surfacesMutex_);
        const auto found = surfaces_.find(ready.surfaceId);
        if (found != surfaces_.end()) surface = found->second;
    }
    if (!surface) {
        if (IsRetiredSurface(ready.surfaceId)) return;
        Fail(L"surface.ready references an unknown surface");
        return;
    }
    bool valid = false;
    {
        std::scoped_lock lock(surface->mutex);
        valid = surface->state == SurfaceState::Scanning && !surface->readyReceived &&
            ready.revision == surface->snapshot.revision;
        if (valid) {
            surface->ready = ready;
            surface->readyReceived = true;
            surface->readyCondition.notify_all();
        }
    }
    if (!valid) {
        std::scoped_lock lock(surface->mutex);
        if (surface->state == SurfaceState::Restoring || surface->state == SurfaceState::Closed)
            return;
        Fail(L"surface.ready revision or state is invalid");
    }
}

// ---------------------------------------------------------------------------
// Projection gate
//
// A window becomes a WinUI surface only if every stage below accepts it.  The
// order matters: the proxy is validated while hidden, the native window is
// cloaked against a verified revision, and renderer input is released last.
// ---------------------------------------------------------------------------

struct RendererSession::ProjectionAttempt final {
    std::shared_ptr<Surface> surface;
    // Null for a virtual dialog, which has no native HWND to cloak.
    std::shared_ptr<SourceThreadAgent> agent;
    HWND nativeRoot = nullptr;
    HWND expectedOwnerProxy = nullptr;
    bool cloakNative = false;
    bool nativeWasForeground = false;
    SurfaceReady ready;
};

namespace {

bool RejectProjection(std::wstring_view reason) {
    try {
        FluentShell::Log(
            std::wstring(L"Projection gate rejected surface: ") + std::wstring(reason));
    } catch (...) {}
    return false;
}

// surface.ready must describe exactly the snapshot the Bridge published.
bool ReadyMatchesSnapshot(const SurfaceReady& ready, const WindowSnapshot& expected) {
    return ready.uiaReady && ready.revision == expected.revision &&
        ready.nodeCount == expected.nodes.size() &&
        RectClose(ready.bounds, expected.bounds);
}

std::wstring DescribeRect(const RECT& rect) {
    return std::to_wstring(rect.left) + L"," + std::to_wstring(rect.top) + L"," +
        std::to_wstring(rect.right - rect.left) + L"x" +
        std::to_wstring(rect.bottom - rect.top);
}

void LogReadyMismatch(const SurfaceReady& ready, const WindowSnapshot& expected) {
    FluentShell::Log(L"surface.ready values: uia=" + std::to_wstring(ready.uiaReady) +
        L" revision=" + std::to_wstring(ready.revision) +
        L" nodes=" + std::to_wstring(ready.nodeCount) +
        L" expectedNodes=" + std::to_wstring(expected.nodes.size()) +
        L" bounds=" + DescribeRect(ready.bounds));
}

} // namespace

// An owned surface may only be projected while its owner is itself a live
// projection, so the proxy can inherit the real modal owner relationship.
bool RendererSession::ResolveOwnerProxy(ProjectionAttempt& attempt) {
    const auto& surface = attempt.surface;
    if (!surface->snapshot.ownerHwnd) return true;
    {
        std::scoped_lock surfacesLock(surfacesMutex_);
        for (const auto& [_, candidate] : surfaces_) {
            std::scoped_lock candidateLock(candidate->mutex);
            if (!candidate->virtualDialog && candidate->agent &&
                candidate->agent->Root() == surface->snapshot.ownerHwnd &&
                candidate->state == SurfaceState::Projected) {
                attempt.expectedOwnerProxy = candidate->ready.proxyHwnd;
                break;
            }
        }
    }
    return attempt.expectedOwnerProxy != nullptr ||
        RejectProjection(L"projected owner proxy is unavailable");
}

// Publishes window.open and waits for the renderer to report a hidden, fully
// realized proxy that matches the snapshot exactly.
bool RendererSession::PublishAndAwaitReady(ProjectionAttempt& attempt) {
    const auto& surface = attempt.surface;
    {
        std::scoped_lock lock(surface->mutex);
        surface->state = SurfaceState::Scanning;
    }
    if (!Send(Ipc::MessageType::WindowOpen, surface->snapshot.revision,
            SerializeWindowOpen(nonce_, surface->snapshot))) {
        return RejectProjection(L"window.open send failed");
    }

    std::unique_lock lock(surface->mutex);
    if (!surface->readyCondition.wait_for(lock, std::chrono::seconds(5), [&] {
            return surface->readyReceived || failed_.load();
        }) || failed_.load()) {
        return RejectProjection(L"surface.ready timeout or renderer fault");
    }
    attempt.ready = surface->ready;
    if (!ReadyMatchesSnapshot(attempt.ready, surface->snapshot)) {
        LogReadyMismatch(attempt.ready, surface->snapshot);
        return RejectProjection(L"surface.ready validation failed");
    }
    DWORD proxyPid = 0;
    GetWindowThreadProcessId(attempt.ready.proxyHwnd, &proxyPid);
    if (!IsWindow(attempt.ready.proxyHwnd) || proxyPid != rendererProcessId_) {
        return RejectProjection(L"proxy HWND identity failed");
    }

    // The proxy must already be at its final geometry while still cloaked, so
    // the commit below can only change visibility and never move the window.
    RECT actualBounds{};
    DWORD cloakReasons = 0;
    const BOOL boundsRead = GetWindowRect(attempt.ready.proxyHwnd, &actualBounds);
    const BOOL proxyVisible = IsWindowVisible(attempt.ready.proxyHwnd);
    const HRESULT cloakRead = DwmGetWindowAttribute(
        attempt.ready.proxyHwnd, DWMWA_CLOAKED, &cloakReasons, sizeof(cloakReasons));
    if (!boundsRead || !RectClose(actualBounds, surface->snapshot.bounds) ||
        !proxyVisible || FAILED(cloakRead) || (cloakReasons & DWM_CLOAKED_APP) == 0) {
        FluentShell::Log(L"Hidden proxy gate values: rectRead=" +
            std::to_wstring(boundsRead != FALSE) +
            L" visible=" + std::to_wstring(proxyVisible != FALSE) +
            L" cloakHr=" + std::to_wstring(static_cast<long>(cloakRead)) +
            L" cloakBits=" + std::to_wstring(cloakReasons) +
            L" actual=" + DescribeRect(actualBounds) +
            L" expected=" + DescribeRect(surface->snapshot.bounds));
        return RejectProjection(L"hidden proxy geometry or visibility failed");
    }
    surface->state = SurfaceState::SurfaceReady;
    return true;
}

bool RendererSession::RunUiaGate(
    const ProjectionAttempt& attempt,
    const WindowSnapshot& snapshot,
    const wchar_t* stage) {
    UiAutomationValidationOptions options;
    options.proxy = attempt.ready.proxyHwnd;
    options.expectedOwner = attempt.expectedOwnerProxy;
    options.rendererProcessId = rendererProcessId_;
    options.rendererCreated = rendererCreated_;
    std::wstring error;
    if (ValidateProjectedSurface(options, snapshot, error)) return true;
    FluentShell::Log(std::wstring(stage) + L" UIA gate failed: " + error);
    return RejectProjection(std::wstring(stage) + L" UIA validation failed");
}

bool RendererSession::RunCommittedUiaGate(const ProjectionAttempt& attempt) {
    const auto& snapshot = attempt.surface->snapshot;
    UiAutomationValidationOptions options;
    options.proxy = attempt.ready.proxyHwnd;
    options.nativeRoot = attempt.nativeRoot;
    options.expectedOwner = attempt.expectedOwnerProxy;
    options.rendererProcessId = rendererProcessId_;
    options.rendererCreated = rendererCreated_;
    options.committed = true;
    options.requireVisible = snapshot.visible && snapshot.state != L"minimized";
    options.requireFocus = options.requireVisible &&
        (attempt.nativeWasForeground || snapshot.modal);

    // DWM visibility and the desktop UIA hit-test are published on different
    // asynchronous paths.  A proxy can be visible and uncloaked while
    // ElementFromPoint still returns the old provider for one or two compositor
    // turns.  Do not restore the native window for that transient state; keep
    // the gate strict and retry it briefly.
    constexpr unsigned kAttempts = 8;
    std::wstring error;
    for (unsigned attemptIndex = 1; attemptIndex <= kAttempts; ++attemptIndex) {
        if (ValidateProjectedSurface(options, snapshot, error)) return true;
        FluentShell::Log(L"Committed UIA gate attempt " + std::to_wstring(attemptIndex) +
            L" failed: " + error);
        if (attemptIndex == kAttempts) {
            return RejectProjection(L"committed UIA validation failed");
        }
        Sleep(50 * attemptIndex);
    }
    return RejectProjection(L"committed UIA validation failed");
}

bool RendererSession::RejectAfterCommit(
    const ProjectionAttempt& attempt, std::wstring_view reason) {
    if (attempt.cloakNative && attempt.surface->agent) {
        std::wstring ignored;
        attempt.surface->agent->Restore(ignored, 2000, shutdownEvent_);
    }
    return RejectProjection(reason);
}

// The native window is cloaked only against the exact revision the renderer has
// already validated.  If native state moved between the two, the surface is
// republished and revalidated -- a bounded number of times -- before giving up.
bool RendererSession::SynchronizeNativeRevision(ProjectionAttempt& attempt) {
    if (!attempt.cloakNative || !attempt.agent) return true;
    const auto& surface = attempt.surface;
    const auto& agent = attempt.agent;
    if (surface->directUiAdapter) {
        std::wstring error;
        if (agent->VerifyDirectUiAndCloak(
                *surface->directUiProfile, surface->directUiEvidence, error,
                2000, shutdownEvent_)) return true;
        FluentShell::Log(L"DirectUI cloak barrier rejected: " + error);
        return RejectProjection(L"DirectUI native revision barrier or cloak failed");
    }

    constexpr unsigned kMaxResyncs = 4;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    unsigned resyncs = 0;
    for (;;) {
        WindowSnapshot barrier;
        uint64_t baseRevision = 0;
        uint64_t expectedGeneration = 0;
        uint64_t expectedFingerprint = 0;
        {
            std::scoped_lock lock(surface->mutex);
            if (surface->state != SurfaceState::SurfaceReady || surface->agent != agent) {
                return RejectProjection(L"surface changed during initial revision resync");
            }
            barrier = surface->snapshot;
            baseRevision = barrier.revision;
            expectedGeneration = barrier.generation;
            expectedFingerprint = surface->fingerprint;
        }

        std::wstring error;
        if (agent->CaptureAndCloak(
                barrier, expectedFingerprint, error, 2000, shutdownEvent_)) {
            std::scoped_lock lock(surface->mutex);
            const bool barrierValid = surface->state == SurfaceState::SurfaceReady &&
                surface->agent == agent &&
                agent->Generation() == expectedGeneration &&
                barrier.revision == surface->snapshot.revision;
            return barrierValid ||
                RejectProjection(L"native revision barrier returned an invalid revision");
        }
        // Only a losing race against a native change is retryable.  Anything
        // else means the cloak itself, or the surface identity, is broken.
        if (error != L"native revision changed before cloak" ||
            barrier.surfaceId != surface->snapshot.surfaceId) {
            return RejectProjection(L"native revision barrier or cloak failed");
        }
        if (++resyncs > kMaxResyncs || std::chrono::steady_clock::now() >= deadline) {
            return RejectProjection(
                L"native did not stabilize during initial revision resync");
        }
        if (!RepublishResyncRevision(
                attempt, barrier, baseRevision, expectedGeneration, deadline)) {
            return false;
        }
    }
}

bool RendererSession::RepublishResyncRevision(
    ProjectionAttempt& attempt,
    WindowSnapshot& barrier,
    uint64_t baseRevision,
    uint64_t expectedGeneration,
    std::chrono::steady_clock::time_point deadline) {
    const auto& surface = attempt.surface;
    const auto& agent = attempt.agent;
    barrier.revision = baseRevision + 1;
    {
        std::scoped_lock lock(surface->mutex);
        if (surface->state != SurfaceState::SurfaceReady || surface->agent != agent ||
            agent->Generation() != expectedGeneration ||
            surface->snapshot.revision != baseRevision) {
            return RejectProjection(L"surface changed during initial revision resync");
        }
        surface->snapshot = barrier;
        surface->fingerprint = SnapshotFingerprint(surface->snapshot);
        surface->readyReceived = false;
        surface->state = SurfaceState::Scanning;
    }
    if (!Send(Ipc::MessageType::WindowPatch, surface->snapshot.revision,
            SerializeWindowPatch(nonce_, baseRevision, surface->snapshot))) {
        return RejectProjection(L"initial revision resync send failed");
    }

    WindowSnapshot refreshedSnapshot;
    {
        std::unique_lock lock(surface->mutex);
        if (!surface->readyCondition.wait_until(lock, deadline, [&] {
                return surface->readyReceived || failed_.load();
            }) || failed_.load()) {
            return RejectProjection(L"initial revision resync surface.ready timeout");
        }
        const auto refreshedReady = surface->ready;
        refreshedSnapshot = surface->snapshot;
        if (!ReadyMatchesSnapshot(refreshedReady, refreshedSnapshot) ||
            refreshedReady.proxyHwnd != attempt.ready.proxyHwnd) {
            return RejectProjection(
                L"initial revision resync surface.ready validation failed");
        }
        attempt.ready = refreshedReady;
    }
    if (!RunUiaGate(attempt, refreshedSnapshot, L"Resync")) return false;

    std::scoped_lock lock(surface->mutex);
    if (surface->state != SurfaceState::Scanning || surface->agent != agent ||
        agent->Generation() != expectedGeneration) {
        return RejectProjection(L"surface changed before resync commit");
    }
    surface->state = SurfaceState::SurfaceReady;
    return true;
}

// Provisional commit: show the proxy so the committed UIA gate can run, but keep
// renderer input gated until the whole contract has passed.
bool RendererSession::CommitProvisional(ProjectionAttempt& attempt) {
    const auto& snapshot = attempt.surface->snapshot;
    if (Send(Ipc::MessageType::SurfaceCommit, snapshot.revision,
            SerializeSurfaceCommit(
                nonce_, snapshot.surfaceId, snapshot.revision, true, false))) {
        return true;
    }
    return RejectAfterCommit(attempt, L"surface.commit send failed");
}

bool RendererSession::AwaitProxyVisible(ProjectionAttempt& attempt) {
    const HWND proxy = attempt.ready.proxyHwnd;
    const uint64_t deadline = GetTickCount64() + 1000;
    DWORD cloakReasons = DWM_CLOAKED_APP;
    while (GetTickCount64() < deadline) {
        cloakReasons = DWM_CLOAKED_APP;
        DwmGetWindowAttribute(proxy, DWMWA_CLOAKED, &cloakReasons, sizeof(cloakReasons));
        if (IsWindowVisible(proxy) && (cloakReasons & DWM_CLOAKED_APP) == 0) break;
        Sleep(25);
    }
    if (!IsWindowVisible(proxy) || (cloakReasons & DWM_CLOAKED_APP) != 0) {
        return RejectAfterCommit(attempt, L"proxy did not become visible and uncloaked");
    }
    return true;
}

bool RendererSession::EstablishProxyZOrder(const ProjectionAttempt& attempt) {
    // WinUI activation does not guarantee sibling z-order against a cloaked
    // native HWND.  Establish the proxy position explicitly before the isolation
    // gate; NOACTIVATE retains the current activation state.
    if (!SetWindowPos(attempt.ready.proxyHwnd, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        return RejectProjection(L"proxy z-order placement failed");
    }
    if (attempt.nativeRoot &&
        !SetWindowPos(attempt.nativeRoot, attempt.ready.proxyHwnd, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        return RejectProjection(L"native/proxy relative z-order placement failed");
    }
    return true;
}

// Isolation: exactly one of the two windows may be composited.
bool RendererSession::VerifyNativeCloaked(const ProjectionAttempt& attempt) {
    if (!attempt.cloakNative || !attempt.surface->agent) return true;
    DWORD reasons = 0;
    if (FAILED(DwmGetWindowAttribute(attempt.surface->agent->Root(), DWMWA_CLOAKED,
            &reasons, sizeof(reasons))) ||
        (reasons & DWM_CLOAKED_APP) == 0) {
        return RejectProjection(L"native HWND was not application-cloaked after commit");
    }
    return true;
}

// Releases the renderer input gate.  The Bridge state moves to Projected first,
// so an action emitted the moment the renderer consumes this frame cannot race
// the local transition.
bool RendererSession::CommitInteractive(ProjectionAttempt& attempt) {
    const auto& surface = attempt.surface;
    {
        std::scoped_lock lock(surface->mutex);
        surface->state = SurfaceState::Projected;
    }
    const auto& snapshot = surface->snapshot;
    if (Send(Ipc::MessageType::SurfaceCommit, snapshot.revision,
            SerializeSurfaceCommit(
                nonce_, snapshot.surfaceId, snapshot.revision, true, true))) {
        return true;
    }
    return RejectAfterCommit(attempt, L"surface.interactive commit send failed");
}

bool RendererSession::OpenSurface(
    const std::shared_ptr<Surface>& surface,
    bool cloakNative) {
    if (!surface) return RejectProjection(L"null surface");
    // WindowCapture and the renderer exchange physical pixel bounds.  The
    // injected process can be system-DPI-aware or DPI-unaware, so querying the
    // proxy from its inherited thread context would virtualize GetWindowRect
    // (for example 900x625 becomes 720x500 at 125% DPI) and falsely fail the
    // strict pre-commit geometry gate.
    PhysicalCoordinateScope dpiScope;
    if (!dpiScope.IsValid()) {
        return RejectProjection(L"cannot establish physical-coordinate DPI context");
    }
    // Projection, initial revision resync, and native rollback all share this
    // barrier.  It prevents a source-thread capture from being published after a
    // concurrent restore has started.
    std::unique_lock canonicalLock(surface->canonicalMutex);

    ProjectionAttempt attempt;
    attempt.surface = surface;
    attempt.cloakNative = cloakNative;
    attempt.agent = surface->agent;
    attempt.nativeRoot = cloakNative && attempt.agent ? attempt.agent->Root() : nullptr;
    attempt.nativeWasForeground = attempt.nativeRoot &&
        GetAncestor(GetForegroundWindow(), GA_ROOT) == attempt.nativeRoot;

    return ResolveOwnerProxy(attempt) &&
        PublishAndAwaitReady(attempt) &&
        RunUiaGate(attempt, surface->snapshot, L"Prepared") &&
        SynchronizeNativeRevision(attempt) &&
        CommitProvisional(attempt) &&
        AwaitProxyVisible(attempt) &&
        EstablishProxyZOrder(attempt) &&
        VerifyNativeCloaked(attempt) &&
        RunCommittedUiaGate(attempt) &&
        CommitInteractive(attempt);
}

bool IsOwnedBy(HWND window, HWND possibleOwner) noexcept {
    if (!window || !possibleOwner || window == possibleOwner) return false;
    HWND owner = GetWindow(window, GW_OWNER);
    for (size_t depth = 0; owner && depth < 256; ++depth) {
        if (owner == possibleOwner) return true;
        const HWND next = GetWindow(owner, GW_OWNER);
        if (next == owner) break;
        owner = next;
    }
    return false;
}

namespace {

// Visible, unowned-or-owned application top-levels in this process.  Shell,
// XAML host, and the transient HMENU popup surface are never candidates: the
// projected MenuBar owns that command surface, and attaching a source agent to
// the popup only produces a false owner-graph rejection.
std::vector<HWND> EnumerateCandidateTopLevels() {
    struct Context final {
        DWORD processId;
        std::vector<HWND> windows;
    } context{ GetCurrentProcessId(), {} };
    EnumWindows([](HWND hwnd, LPARAM raw) -> BOOL {
        auto& context = *reinterpret_cast<Context*>(raw);
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (processId != context.processId || !IsWindowVisible(hwnd) ||
            GetAncestor(hwnd, GA_ROOT) != hwnd) {
            return TRUE;
        }
        wchar_t className[256]{};
        GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
        if (FluentShell::EqualsIgnoreCase(className, L"#32768")) return TRUE;
        if (!FluentShell::IsShellOrXamlWindowClass(className)) {
            context.windows.push_back(hwnd);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.windows;
}

} // namespace

// Discovery bookkeeping is keyed by HWND, so entries for destroyed windows must
// go before an address is reused by a new window.
void RendererSession::PruneDiscoveryState() {
    std::scoped_lock lock(surfacesMutex_);
    for (auto iterator = discoveryAttempts_.begin(); iterator != discoveryAttempts_.end();) {
        if (!IsWindow(*iterator)) iterator = discoveryAttempts_.erase(iterator);
        else ++iterator;
    }
    for (auto iterator = ownerGraphDeferrals_.begin();
         iterator != ownerGraphDeferrals_.end();) {
        if (!IsWindow(iterator->first)) iterator = ownerGraphDeferrals_.erase(iterator);
        else ++iterator;
    }
}

RendererSession::DiscoveryDecision RendererSession::ClassifyTopLevel(
    HWND window,
    bool ownsVisibleTopLevel) {
    const HWND directOwner = GetWindow(window, GW_OWNER);
    DiscoveryDecision decision;
    std::scoped_lock lock(surfacesMutex_);

    // A root that recently owned a visible top-level stays native until the owner
    // graph has been quiet for kOwnerGraphQuietMs.  Classic dialog sequences
    // destroy one owned window and create the next a moment later, and without
    // this the root would visibly bounce between native and WinUI.
    bool retryDeferredOwner = false;
    if (!directOwner) {
        const auto deferral = ownerGraphDeferrals_.find(window);
        if (deferral != ownerGraphDeferrals_.end()) {
            if (ownsVisibleTopLevel) {
                deferral->second = 0;
                return decision;
            }
            const uint64_t now = GetTickCount64();
            if (deferral->second == 0) {
                deferral->second = now;
                return decision;
            }
            if (now - deferral->second < kOwnerGraphQuietMs) return decision;
            ownerGraphDeferrals_.erase(deferral);
            discoveryAttempts_.erase(window);
            retryDeferredOwner = true;
        }
    }
    // One attempt per window until it is destroyed or explicitly retried, so a
    // rejected window does not reappear in the log every second.
    if (!discoveryAttempts_.insert(window).second) return decision;

    std::shared_ptr<Surface> projectedOwner;
    for (const auto& [_, surface] : surfaces_) {
        if (surface->agent && surface->agent->Root() == window) return decision;
        if (!directOwner || surface->virtualDialog || !surface->agent) continue;
        // Owner graphs are not projected by the bounded v1 adapter.  If a visible
        // owned top-level appears above a projected native ancestor, keeping that
        // ancestor cloaked can strand the child behind an unrelated renderer
        // HWND, so the ancestor is resolved back to native instead.
        if (IsOwnedBy(window, surface->agent->Root())) {
            std::scoped_lock surfaceLock(surface->mutex);
            if (surface->state == SurfaceState::Projected) projectedOwner = surface;
        }
    }

    if (directOwner) {
        // An owned surface is never projected independently from its owner: it is
        // either covered by the fallback below or already fully native.
        if (!projectedOwner) return decision;
        decision.action = DiscoveryAction::RestoreOwnerGraph;
        decision.projectedOwner = std::move(projectedOwner);
        const HWND ownerRoot = decision.projectedOwner->agent
            ? decision.projectedOwner->agent->Root()
            : nullptr;
        if (ownerRoot) ownerGraphDeferrals_.insert_or_assign(ownerRoot, 0);
        return decision;
    }
    if (ownsVisibleTopLevel) {
        // Discovery order follows desktop z-order, so an owned dialog can be
        // visited before its root during startup.  Keep the root native until
        // every owned top-level closes; otherwise the next pass could cloak it
        // underneath the dialog that was already skipped.
        decision.action = DiscoveryAction::DeferOwnerGraph;
        decision.firstDeferral = ownerGraphDeferrals_.try_emplace(window, 0).second;
        return decision;
    }
    decision.action = retryDeferredOwner
        ? DiscoveryAction::ProjectAfterDeferral
        : DiscoveryAction::Project;
    return decision;
}

void RendererSession::DiscoverTopLevelWindows() {
    const auto candidates = EnumerateCandidateTopLevels();
    PruneDiscoveryState();
    for (const HWND window : candidates) {
        const bool ownsVisibleTopLevel = !GetWindow(window, GW_OWNER) &&
            std::any_of(candidates.begin(), candidates.end(),
                [window](HWND candidate) { return IsOwnedBy(candidate, window); });
        auto decision = ClassifyTopLevel(window, ownsVisibleTopLevel);
        switch (decision.action) {
        case DiscoveryAction::Skip:
            break;
        case DiscoveryAction::RestoreOwnerGraph:
            FluentShell::Log(
                L"Visible owned top-level requires native owner-graph fallback");
            RestoreSurface(decision.projectedOwner, L"ownedTopLevel");
            break;
        case DiscoveryAction::DeferOwnerGraph:
            if (decision.firstDeferral) {
                FluentShell::Log(
                    L"Native owner graph remains untranslated while an owned top-level is visible");
            }
            break;
        case DiscoveryAction::ProjectAfterDeferral:
            FluentShell::Log(L"Owned top-level closed; retrying native owner projection");
            OpenNativeWindow(window);
            break;
        case DiscoveryAction::Project:
            OpenNativeWindow(window);
            break;
        }
    }
}

bool RendererSession::OpenNativeWindow(HWND root) {
    auto agent = SourceThreadAgent::Attach(root, bridgeModule_);
    if (!agent) {
        FluentShell::Log(L"Native window source-thread attach failed");
        return false;
    }
    auto surface = std::make_shared<Surface>();
    surface->agent = agent;
    surface->snapshot.surfaceId = Ipc::NewGuidString();
    surface->snapshot.revision = 1;
    std::wstring error;
    const DirectUiWindowProfile* profile = agent->DirectUiProfile();
    DirectUiAdmissionResult admission = DirectUiAdmissionResult::NotApplicable;
    if (profile) {
        admission = InspectDirectUiSurface(*agent, *profile, 2000, shutdownEvent_, error,
            &surface->snapshot, &surface->directUiBindings, &surface->directUiEvidence);
    } else if (agent->GenericDirectUiCandidate()) {
        admission = InspectGenericDirectUiSurface(*agent, 2000, shutdownEvent_, error,
            &surface->directUiOwnedProfile, &surface->snapshot,
            &surface->directUiBindings, &surface->directUiEvidence);
        profile = agent->DirectUiProfile();
    }
    if (admission == DirectUiAdmissionResult::Rejected) {
        if (!agent->Shutdown()) RetainSourceThreadAgent(agent);
        FluentShell::Log(error);
        return false;
    }
    surface->directUiAdapter = admission == DirectUiAdmissionResult::Admitted;
    surface->directUiProfile = profile;
    if (surface->snapshot.surfaceId.empty() ||
        (!surface->directUiAdapter &&
         !agent->Capture(surface->snapshot, error, 2000, shutdownEvent_))) {
        if (!agent->Shutdown()) RetainSourceThreadAgent(agent);
        FluentShell::Log(L"Native window remains untranslated: " + error);
        return false;
    }
    surface->fingerprint = SnapshotFingerprint(surface->snapshot);
    {
        std::scoped_lock lock(surfacesMutex_);
        surfaces_.emplace(surface->snapshot.surfaceId, surface);
    }
    if (!OpenSurface(surface, true)) {
        RestoreSurface(surface, L"unsupported");
        return false;
    }
    FluentShell::Log(L"Projected native window: " + surface->snapshot.title);
    return true;
}

bool RendererSession::IsProjectedOwner(HWND owner) {
    if (!owner) return true;
    owner = GetAncestor(owner, GA_ROOT);
    if (!owner) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(owner, &processId);
    if (processId != GetCurrentProcessId()) return false;
    std::vector<std::shared_ptr<Surface>> surfaces;
    {
        std::scoped_lock lock(surfacesMutex_);
        for (const auto& [_, surface] : surfaces_) surfaces.push_back(surface);
    }
    for (const auto& surface : surfaces) {
        std::scoped_lock lock(surface->mutex);
        if (!surface->virtualDialog && surface->agent &&
            surface->agent->Root() == owner && surface->state == SurfaceState::Projected) {
            return true;
        }
    }
    return false;
}

void RendererSession::RejectAction(
    const std::shared_ptr<Surface>& surface,
    std::unique_lock<std::mutex>& lock,
    const ActionRequest& action,
    std::wstring_view status,
    std::wstring_view reason) {
    const uint64_t revision = surface->snapshot.revision;
    auto patch = SerializeWindowPatch(nonce_, revision, surface->snapshot, action.eventId);
    lock.unlock();
    Send(Ipc::MessageType::ActionResult, revision,
        SerializeActionResult(nonce_, action, status, revision, reason));
    Send(Ipc::MessageType::WindowPatch, revision, std::move(patch));
}

void RendererSession::ApplyVirtualDialogAction(
    const std::shared_ptr<Surface>& surface,
    std::unique_lock<std::mutex>& lock,
    const ActionRequest& action) {
    std::wstring_view status = L"rejected";
    bool completesDialog = false;
    bool patchRenderer = false;
    if (action.action == L"invoke" && action.nodeId) {
        // Only a button the synthesized dialog actually published can complete it.
        const auto button = surface->buttonResults.find(*action.nodeId);
        if (button != surface->buttonResults.end()) {
            surface->dialogResult = button->second;
            status = L"accepted";
            completesDialog = true;
        }
    } else if (action.action == L"setCheck" && action.nodeId &&
               surface->verificationNode == action.nodeId) {
        surface->verificationChecked = action.integerValue != 0;
        ++surface->snapshot.revision;
        for (auto& node : surface->snapshot.nodes) {
            if (node.nodeId == *action.nodeId) node.checked = action.integerValue;
        }
        status = L"accepted";
        patchRenderer = true;
    } else if (action.action == L"close") {
        // A dialog without a cancel result has no close semantics to honor.
        if (surface->cancelResult) {
            surface->dialogResult = *surface->cancelResult;
            status = L"accepted";
            completesDialog = true;
        } else {
            status = L"closeRejected";
        }
    }
    const uint64_t revision = surface->snapshot.revision;
    auto patch = SerializeWindowPatch(
        nonce_, action.expectedRevision, surface->snapshot, action.eventId);
    lock.unlock();
    Send(Ipc::MessageType::ActionResult, revision,
        SerializeActionResult(nonce_, action, status, revision));
    if (patchRenderer) {
        Send(Ipc::MessageType::WindowPatch, revision, std::move(patch));
    }
    if (completesDialog) SetEvent(surface->resultEvent);
}

void RendererSession::HandleAction(const ActionRequest& action) {
    std::shared_ptr<Surface> surface;
    {
        std::scoped_lock lock(surfacesMutex_);
        const auto found = surfaces_.find(action.surfaceId);
        if (found != surfaces_.end()) surface = found->second;
    }
    if (!surface) {
        // A retired surface is a benign late frame; anything else is a protocol
        // violation, because the renderer named a surface it was never given.
        if (IsRetiredSurface(action.surfaceId)) return;
        Fail(L"action references an unknown surface");
        return;
    }

    std::unique_lock canonicalLock(surface->canonicalMutex);
    std::unique_lock lock(surface->mutex);
    if (surface->state != SurfaceState::Projected) {
        const auto state = surface->state;
        lock.unlock();
        // A renderer action can race the bridge restore/close commit.  The native
        // window is already authoritative in Restoring and Closed, so dropping
        // the authenticated late action is safer than faulting the session.
        if (state == SurfaceState::Native || state == SurfaceState::Scanning ||
            state == SurfaceState::SurfaceReady) {
            canonicalLock.unlock();
            Fail(L"action references a surface that is not projected");
        }
        return;
    }
    // Geometry actions are latest-wins and carry request semantics, so they are
    // rebased onto the current revision instead of being revision-gated.
    if (!IsRequestSemanticAction(action.action) &&
        action.expectedRevision != surface->snapshot.revision) {
        RejectAction(surface, lock, action, L"stale", L"revision mismatch");
        return;
    }
    std::wstring semanticError;
    if (!ValidateActionForSnapshot(action, surface->snapshot, semanticError)) {
        RejectAction(surface, lock, action, L"rejected", semanticError);
        return;
    }
    if (surface->virtualDialog) {
        ApplyVirtualDialogAction(surface, lock, action);
        return;
    }

    // Native work needs the source UI thread, so it is queued for the action
    // worker rather than run on the pipe reader.
    lock.unlock();
    canonicalLock.unlock();
    {
        std::scoped_lock actionLock(actionMutex_);
        if (stopping_.load() || failed_.load()) return;
        actionQueue_.emplace_back(action, surface);
    }
    actionCondition_.notify_one();
}

void RendererSession::HandleNativeAction(
    const ActionRequest& action,
    const std::shared_ptr<Surface>& surface) {
    std::unique_lock canonicalLock(surface->canonicalMutex);
    std::unique_lock lock(surface->mutex);
    if (surface->state != SurfaceState::Projected || surface->restoreInProgress) {
        return;
    }
    if (action.action == L"close" && surface->pendingCloseAction) {
        const uint64_t revision = surface->snapshot.revision;
        lock.unlock();
        canonicalLock.unlock();
        Send(Ipc::MessageType::ActionResult, revision,
            SerializeActionResult(nonce_, action, L"rejected", revision,
                L"a native close request is already pending"));
        return;
    }
    auto agent = surface->agent;
    const uint64_t sourceGeneration = surface->snapshot.generation;
    if (!agent || agent->Generation() != sourceGeneration) {
        lock.unlock();
        canonicalLock.unlock();
        RestoreSurface(surface, L"sourceIdentityChanged");
        return;
    }
    ActionRequest effectiveAction = action;
    const bool requestSemantic = IsRequestSemanticAction(action.action);
    if (requestSemantic) effectiveAction.expectedRevision = surface->snapshot.revision;
    if (!requestSemantic && action.expectedRevision != surface->snapshot.revision) {
        RejectAction(surface, lock, action, L"stale", L"revision mismatch");
        return;
    }

    if (surface->directUiAdapter &&
        (action.action == L"move" || action.action == L"resize")) {
        const auto width = [](const RECT& bounds) {
            return static_cast<int64_t>(bounds.right) - bounds.left;
        };
        const auto height = [](const RECT& bounds) {
            return static_cast<int64_t>(bounds.bottom) - bounds.top;
        };
        if (action.action == L"resize" ||
            width(action.rect) != width(surface->snapshot.bounds) ||
            height(action.rect) != height(surface->snapshot.bounds)) {
            FluentShell::Log(
                L"DirectUI geometry changed size; restoring the non-resizable native window");
            lock.unlock();
            canonicalLock.unlock();
            RestoreSurface(surface, L"unsupported");
            return;
        }
        const auto* directUiProfile = surface->directUiProfile;
        const DirectUiNativeEvidence expectedEvidence = surface->directUiEvidence;
        if (!directUiProfile) {
            lock.unlock();
            canonicalLock.unlock();
            RestoreSurface(surface, L"sourceIdentityChanged");
            return;
        }
        lock.unlock();
        DirectUiNativeEvidence movedEvidence;
        std::wstring error;
        if (!agent->MoveDirectUiWindow(*directUiProfile, expectedEvidence,
                action.rect, movedEvidence, error, 2000, shutdownEvent_)) {
            FluentShell::Log(L"DirectUI native move rejected: " + error);
            canonicalLock.unlock();
            RestoreSurface(surface, L"restore");
            return;
        }

        WindowSnapshot published;
        uint64_t baseRevision = 0;
        uint64_t revision = 0;
        bool updated = false;
        {
            std::scoped_lock updateLock(surface->mutex);
            if (surface->state == SurfaceState::Projected &&
                !surface->restoreInProgress && surface->agent == agent &&
                agent->Generation() == sourceGeneration &&
                surface->directUiProfile == directUiProfile) {
                baseRevision = surface->snapshot.revision;
                surface->snapshot.bounds = movedEvidence.root.bounds;
                surface->snapshot.clientBounds = movedEvidence.clientBounds;
                surface->snapshot.dpi = movedEvidence.dpi;
                ++surface->snapshot.revision;
                surface->directUiEvidence = std::move(movedEvidence);
                surface->fingerprint = SnapshotFingerprint(surface->snapshot);
                revision = surface->snapshot.revision;
                published = surface->snapshot;
                updated = true;
            }
        }
        if (!updated) {
            canonicalLock.unlock();
            RestoreSurface(surface, L"sourceIdentityChanged");
            return;
        }
        canonicalLock.unlock();
        FluentShell::Log(L"DirectUI native move accepted: " +
            DescribeRect(published.bounds));
        Send(Ipc::MessageType::ActionResult, revision,
            SerializeActionResult(nonce_, action, L"accepted", revision));
        Send(Ipc::MessageType::WindowPatch, revision,
            SerializeWindowPatch(nonce_, baseRevision, published, action.eventId));
        return;
    }

    if (surface->directUiAdapter &&
        (action.action == L"invoke" || action.action == L"close" ||
         action.action == L"setCheck")) {
        const auto fallbackWithoutClick = [&](std::wstring_view reason) {
            lock.unlock();
            canonicalLock.unlock();
            RestoreSurface(surface, reason);
        };
        DirectUiActionBinding requested;
        bool foundBinding = false;
        if ((action.action == L"invoke" || action.action == L"setCheck") && action.nodeId) {
            const auto found = surface->directUiBindings.find(*action.nodeId);
            if (found != surface->directUiBindings.end()) {
                requested = found->second;
                foundBinding = true;
            }
        } else if (action.action == L"close") {
            const auto found = std::find_if(surface->directUiBindings.begin(),
                surface->directUiBindings.end(), [](const auto& entry) {
                    return entry.second.cancel;
                });
            if (found != surface->directUiBindings.end()) {
                requested = found->second;
                foundBinding = true;
            }
        }
        if (!foundBinding) {
            fallbackWithoutClick(L"sourceIdentityChanged");
            return;
        }
        if ((action.action == L"invoke" &&
                requested.action != DirectUiAction::HandoffClick) ||
            (action.action == L"setCheck" &&
                requested.action != DirectUiAction::ToggleCheck) ||
            (action.action == L"close" && !requested.cancel)) {
            fallbackWithoutClick(L"sourceIdentityChanged");
            return;
        }
        const auto* directUiProfile = surface->directUiProfile;
        const DirectUiNativeEvidence expectedEvidence = surface->directUiEvidence;
        if (!directUiProfile) {
            fallbackWithoutClick(L"sourceIdentityChanged");
            return;
        }
        // A projected toggle stays inside the projection. The initial UIA
        // contract has already been admitted; while cloaked, actions use fresh
        // native evidence and permit only the requested checkbox delta.
        if (action.action == L"setCheck") {
            std::wstring error;
            lock.unlock();
            DirectUiNativeEvidence beforeToggle;
            if (!agent->CaptureDirectUiNativeEvidence(*directUiProfile, beforeToggle,
                    error, 2000, shutdownEvent_) ||
                !MatchDirectUiMutationBracket(*directUiProfile,
                    expectedEvidence, beforeToggle, error, false) ||
                requested.slotIndex >= beforeToggle.slotWindows.size() ||
                beforeToggle.slotWindows[requested.slotIndex].hwnd != requested.hwnd ||
                beforeToggle.slotWindows[requested.slotIndex].generation != requested.generation) {
                canonicalLock.unlock();
                RestoreSurface(surface, L"sourceIdentityChanged");
                return;
            }
            if (!agent->InvokeDirectUiToggle(
                    requested, action.integerValue, error, 2000, shutdownEvent_)) {
                FluentShell::Log(L"DirectUI native toggle rejected: " + error);
                canonicalLock.unlock();
                RestoreSurface(surface, L"restore");
                return;
            }
            FluentShell::Log(L"DirectUI native toggle accepted: requested=" +
                std::to_wstring(action.integerValue));
            DirectUiNativeEvidence expectedAfter = beforeToggle;
            expectedAfter.slotWindows[requested.slotIndex].checked =
                action.integerValue != 0;
            DirectUiNativeEvidence afterToggle;
            if (!agent->CaptureDirectUiNativeEvidence(*directUiProfile, afterToggle,
                    error, 2000, shutdownEvent_) ||
                !MatchDirectUiMutationBracket(*directUiProfile,
                    expectedAfter, afterToggle, error, false)) {
                canonicalLock.unlock();
                RestoreSurface(surface, L"sourceIdentityChanged");
                return;
            }
            WindowSnapshot published;
            uint64_t revision = 0;
            bool updated = false;
            {
                std::scoped_lock updateLock(surface->mutex);
                if (surface->state != SurfaceState::Projected ||
                    surface->restoreInProgress || surface->agent != agent ||
                    agent->Generation() != sourceGeneration || !action.nodeId) {
                    return;
                }
                const auto node = std::find_if(surface->snapshot.nodes.begin(),
                    surface->snapshot.nodes.end(), [&](const auto& candidate) {
                        return candidate.nodeId == *action.nodeId;
                    });
                if (node != surface->snapshot.nodes.end() &&
                    node->generation == requested.generation) {
                    node->checked = action.integerValue;
                    ++surface->snapshot.revision;
                    surface->directUiEvidence = std::move(afterToggle);
                    surface->fingerprint = SnapshotFingerprint(surface->snapshot);
                    revision = surface->snapshot.revision;
                    published = surface->snapshot;
                    updated = true;
                }
            }
            if (!updated) {
                canonicalLock.unlock();
                RestoreSurface(surface, L"sourceIdentityChanged");
                return;
            }
            canonicalLock.unlock();
            Send(Ipc::MessageType::WindowPatch, revision,
                SerializeWindowPatch(nonce_, revision - 1, published, action.eventId),
                250, false);
            Send(Ipc::MessageType::ActionResult, revision,
                SerializeActionResult(nonce_, action, L"accepted", revision), 250, false);
            return;
        }
        DirectUiNativeEvidence verifiedEvidence;
        std::wstring error;
        lock.unlock();
        if (!agent->CaptureDirectUiNativeEvidence(*directUiProfile, verifiedEvidence,
                error, 2000, shutdownEvent_) ||
            !MatchDirectUiMutationBracket(*directUiProfile,
                expectedEvidence, verifiedEvidence, error, false) ||
            requested.slotIndex >= verifiedEvidence.slotWindows.size() ||
            verifiedEvidence.slotWindows[requested.slotIndex].hwnd != requested.hwnd ||
            verifiedEvidence.slotWindows[requested.slotIndex].generation != requested.generation) {
            canonicalLock.unlock();
            RestoreSurface(surface, L"sourceIdentityChanged");
            return;
        }
        const uint64_t revision = surface->snapshot.revision;
        const HWND proxy = surface->ready.proxyHwnd;
        FluentShell::Log(L"DirectUI projected-page handoff to native: adapter=" +
            std::wstring(directUiProfile->adapterId) + L" page=" +
            std::wstring(directUiProfile->pageId) + L" action=" + action.action);
        Send(Ipc::MessageType::SurfaceCommit, revision,
            SerializeSurfaceCommit(nonce_, surface->snapshot.surfaceId, revision, false, false),
            250, false);
        if (!HideProxyForRestore(proxy)) {
            FluentShell::Log(L"DirectUI handoff could not isolate the renderer proxy");
            TerminateRenderer(ERROR_CANCELLED);
            canonicalLock.unlock();
            RestoreSurface(surface, L"restore");
            return;
        }
        if (!agent->RestoreThenDirectUiButtonClick(
                *directUiProfile, verifiedEvidence, requested,
                error, 2000, shutdownEvent_)) {
            FluentShell::Log(L"DirectUI restore-before-click rejected without posting: " + error);
            canonicalLock.unlock();
            RestoreSurface(surface, L"restore");
            return;
        }
        {
            std::scoped_lock terminalLock(surface->mutex);
            surface->state = SurfaceState::Closed;
        }
        if (!agent->Shutdown()) {
            surface->agentRetained = true;
            RetainSourceThreadAgent(agent);
        }
        const auto surfaceId = surface->snapshot.surfaceId;
        canonicalLock.unlock();
        Send(Ipc::MessageType::ActionResult, revision,
            SerializeActionResult(nonce_, action, L"accepted", revision), 250, false);
        Send(Ipc::MessageType::WindowClose, revision,
            SerializeWindowClose(nonce_, surfaceId, L"restore"), 250, false);
        RetireSurfaceIfClosed(surface);
        {
            std::scoped_lock discoveryLock(surfacesMutex_);
            discoveryAttempts_.erase(agent->Root());
        }
        return;
    }

    lock.unlock();
    ActionOutcome outcome;
    if (!agent || !agent->Invoke(effectiveAction, outcome, 2000, shutdownEvent_)) {
        canonicalLock.unlock();
        RestoreSurface(surface, L"restore");
        return;
    }
    if (outcome.destroyed) {
        Send(Ipc::MessageType::ActionResult, effectiveAction.expectedRevision,
            SerializeActionResult(nonce_, action, L"accepted", effectiveAction.expectedRevision));
        canonicalLock.unlock();
        RestoreSurface(surface, L"nativeDestroyed");
        return;
    }
    {
        std::unique_lock updateLock(surface->mutex);
        if (surface->state != SurfaceState::Projected ||
            surface->restoreInProgress || surface->agent != agent ||
            agent->Generation() != sourceGeneration) {
            return;
        }
        const uint64_t base = surface->snapshot.revision;
        if (outcome.snapshot.surfaceId != surface->snapshot.surfaceId ||
            outcome.snapshot.generation != sourceGeneration ||
            outcome.snapshot.revision != base + 1) {
            updateLock.unlock();
            canonicalLock.unlock();
            RestoreSurface(surface, L"sourceIdentityChanged");
            return;
        }
        surface->snapshot = std::move(outcome.snapshot);
        surface->fingerprint = SnapshotFingerprint(surface->snapshot);
        if (action.action == L"close" && outcome.closeSequence != 0) {
            surface->pendingCloseSequence = outcome.closeSequence;
            surface->pendingCloseAction = action;
        }
        const uint64_t revision = surface->snapshot.revision;
        const auto resultPayload = SerializeActionResult(
            nonce_, action, L"accepted", revision);
        // WM_CLOSE is merely queued at this point. Keep its renderer pending
        // entry alive until the root is destroyed or reconcile observes the
        // completed handler and returns closeRejected.
        const auto patchPayload = action.action == L"close"
            ? SerializeWindowPatch(nonce_, base, surface->snapshot)
            : SerializeWindowPatch(nonce_, base, surface->snapshot, action.eventId);
        updateLock.unlock();
        Send(Ipc::MessageType::ActionResult, revision, resultPayload);
        Send(Ipc::MessageType::WindowPatch, revision, patchPayload);
    }
}

void RendererSession::ActionMain() noexcept {
    try {
        while (true) {
            std::pair<ActionRequest, std::shared_ptr<Surface>> work;
            {
                std::unique_lock lock(actionMutex_);
                actionCondition_.wait(lock, [&] {
                    return stopping_.load() || failed_.load() || !actionQueue_.empty();
                });
                if (stopping_.load() || failed_.load()) break;
                work = std::move(actionQueue_.front());
                actionQueue_.pop_front();
            }
            HandleNativeAction(work.first, work.second);
        }
    } catch (...) {
        Fail(L"native action worker exception");
    }
}

struct RendererSession::ReconcilePass final {
    std::shared_ptr<Surface> surface;
    std::shared_ptr<SourceThreadAgent> agent;
    uint64_t sourceGeneration = 0;
    uint64_t baseRevision = 0;
    WindowSnapshot next;
    bool directUiAdapter = false;
    const DirectUiWindowProfile* directUiProfile = nullptr;
    DirectUiNativeEvidence directUiEvidence;
    std::unordered_map<uint64_t, DirectUiActionBinding> directUiBindings;
};

void RendererSession::ReconcileNativeSurfaces() {
    std::vector<std::shared_ptr<Surface>> surfaces;
    {
        std::scoped_lock lock(surfacesMutex_);
        for (const auto& [_, surface] : surfaces_) surfaces.push_back(surface);
    }
    for (const auto& surface : surfaces) {
        if (const wchar_t* reason = ReconcileSurface(surface)) {
            RestoreSurface(surface, reason);
        }
    }
}

const wchar_t* RendererSession::ReconcileSurface(const std::shared_ptr<Surface>& surface) {
    // A surface another thread is already operating on is skipped, never waited
    // on: reconcile must not hold up an action or a rollback.
    std::unique_lock canonicalLock(surface->canonicalMutex, std::try_to_lock);
    if (!canonicalLock.owns_lock()) return nullptr;

    ReconcilePass pass;
    pass.surface = surface;
    {
        std::scoped_lock lock(surface->mutex);
        if (surface->virtualDialog || surface->state != SurfaceState::Projected) return nullptr;
        pass.agent = surface->agent;
        pass.sourceGeneration = surface->snapshot.generation;
        if (!pass.agent || pass.agent->Generation() != pass.sourceGeneration ||
            pass.agent->IsDestroyed()) {
            return L"nativeDestroyed";
        }
        pass.next = surface->snapshot;
        pass.baseRevision = pass.next.revision;
        pass.next.revision = pass.baseRevision + 1;
        pass.directUiAdapter = surface->directUiAdapter;
        pass.directUiProfile = surface->directUiProfile;
        if (pass.directUiAdapter) {
            pass.directUiEvidence = surface->directUiEvidence;
            pass.directUiBindings = surface->directUiBindings;
        }
    }

    // A quiet source thread does not need a full capture on every tick.  This is
    // what keeps a drag or an open menu from being hammered with 2 s-deadline
    // source-thread work it cannot service in time.  The slow poll stays as the
    // backstop because the dirty whitelist cannot prove it observes every way
    // native state can change.
    if (!pass.agent->IsDirty() && ++surface->reconcileSkips < kQuietReconcileTicks) {
        return nullptr;
    }
    surface->reconcileSkips = 0;

    std::wstring error;
    bool timedOut = false;
    if (pass.directUiAdapter) {
        DirectUiNativeEvidence current;
        if (!pass.agent->CaptureDirectUiNativeEvidence(
                *pass.directUiProfile, current, error, 2000, shutdownEvent_) ||
            !MatchDirectUiMutationBracket(*pass.directUiProfile,
                pass.directUiEvidence, current, error, false)) {
            FluentShell::Log(L"DirectUI reconcile native evidence rejected: " + error);
            return L"unsupported";
        }
        std::scoped_lock lock(surface->mutex);
        if (surface->state != SurfaceState::Projected || surface->restoreInProgress ||
            surface->agent != pass.agent ||
            pass.agent->Generation() != pass.sourceGeneration ||
            surface->snapshot.revision != pass.baseRevision ||
            surface->directUiProfile != pass.directUiProfile)
            return L"nativeStateChanged";
        surface->directUiEvidence = std::move(current);
        surface->captureTimeouts = 0;
        return nullptr;
    }
    if (!pass.agent->Capture(pass.next, error, 2000, shutdownEvent_, &timedOut)) {
        // A source thread inside a modal loop can miss a bounded deadline without
        // being broken, so a few misses keep the projection.
        if (timedOut && !stopping_.load() && !failed_.load() &&
            ++surface->captureTimeouts < kMaxCaptureTimeouts) {
            try {
                FluentShell::Log(L"Reconcile capture missed its deadline (" +
                    std::to_wstring(surface->captureTimeouts) + L" of " +
                    std::to_wstring(kMaxCaptureTimeouts) + L"); keeping the projection");
            } catch (...) {}
            return nullptr;
        }
        return timedOut ? L"timeout" : L"unsupported";
    }
    surface->captureTimeouts = 0;
    return PublishReconciledSnapshot(pass);
}

// Runs with the surface canonical barrier held by ReconcileSurface.  The capture
// above ran without surface->mutex, so every identity fact is rechecked before
// the snapshot is allowed to become canonical.
const wchar_t* RendererSession::PublishReconciledSnapshot(ReconcilePass& pass) {
    const auto& surface = pass.surface;
    const auto& agent = pass.agent;
    const uint64_t fingerprint = SnapshotFingerprint(pass.next);

    bool changed = false;
    uint64_t revision = 0;
    std::string payload;
    std::optional<ActionRequest> rejectedClose;
    {
        std::scoped_lock lock(surface->mutex);
        if (surface->state != SurfaceState::Projected || surface->restoreInProgress ||
            surface->agent != agent ||
            agent->Generation() != pass.sourceGeneration ||
            surface->snapshot.revision != pass.baseRevision ||
            pass.next.surfaceId != surface->snapshot.surfaceId ||
            pass.next.generation != pass.sourceGeneration) {
            return L"nativeStateChanged";
        }
        if (pass.directUiAdapter) {
            if (!surface->directUiAdapter ||
                pass.directUiBindings.size() != surface->directUiBindings.size())
                return L"nativeStateChanged";
            for (const auto& [nodeId, binding] : pass.directUiBindings) {
                const auto existing = surface->directUiBindings.find(nodeId);
                if (existing == surface->directUiBindings.end() ||
                    existing->second.hwnd != binding.hwnd ||
                    existing->second.generation != binding.generation ||
                    existing->second.cancel != binding.cancel)
                    return L"nativeStateChanged";
            }
            surface->directUiEvidence = pass.directUiEvidence;
        }
        changed = fingerprint != surface->fingerprint;
        if (pass.directUiAdapter && changed) return L"nativeStateChanged";
        if (changed) {
            surface->snapshot = std::move(pass.next);
            surface->fingerprint = fingerprint;
            payload = SerializeWindowPatch(nonce_, pass.baseRevision, surface->snapshot);
        }
        revision = surface->snapshot.revision;
        if (surface->pendingCloseAction && surface->pendingCloseSequence != 0 &&
            agent->CompletedCloseSequence() >= surface->pendingCloseSequence) {
            // The queued WM_CLOSE handler has returned and the root survived, so
            // native code vetoed or cancelled the request.  Only now may the
            // renderer accept another close attempt; this avoids both a stuck
            // close gate and duplicate prompts during a modal lifetime.
            rejectedClose = std::move(surface->pendingCloseAction);
            surface->pendingCloseAction.reset();
            surface->pendingCloseSequence = 0;
        }
    }

    bool sent = true;
    if (changed) {
        sent = Send(Ipc::MessageType::WindowPatch, revision, std::move(payload));
    }
    if (sent && rejectedClose) {
        sent = Send(Ipc::MessageType::ActionResult, revision,
            SerializeActionResult(nonce_, *rejectedClose, L"closeRejected", revision));
    }
    return sent ? nullptr : L"restore";
}

struct RendererSession::RestoreAttempt final {
    std::shared_ptr<Surface> surface;
    // Null for a virtual dialog, or once the native window is already gone.
    std::shared_ptr<SourceThreadAgent> agent;
    std::wstring surfaceId;
    uint64_t revision = 0;
    HWND proxy = nullptr;
    HWND nativeRoot = nullptr;
    DWORD sourceThread = 0;
    uint64_t sourceGeneration = 0;
    bool virtualDialog = false;
};

namespace {

// window.close carries a closed enum, so a specific cause has to be logged
// separately or a fallback leaves no trace of why it happened.
std::wstring_view WireRestoreReason(std::wstring_view reason) noexcept {
    return reason == L"nativeDestroyed" || reason == L"unsupported" ||
        reason == L"restore" || reason == L"shutdown"
        ? reason
        : std::wstring_view(L"restore");
}

} // namespace

// Claims the rollback.  Returns false when the surface is already closed or
// another thread owns its rollback, which makes RestoreSurface idempotent.
bool RendererSession::BeginRestore(RestoreAttempt& attempt) {
    const auto& surface = attempt.surface;
    std::scoped_lock lock(surface->mutex);
    if (surface->state == SurfaceState::Closed || surface->restoreInProgress) return false;
    surface->restoreInProgress = true;
    ++surface->restoreAttempts;
    surface->state = SurfaceState::Restoring;
    attempt.agent = surface->agent;
    attempt.proxy = surface->ready.proxyHwnd;
    attempt.virtualDialog = surface->virtualDialog;
    attempt.revision = surface->snapshot.revision;
    attempt.surfaceId = surface->snapshot.surfaceId;
    if (attempt.agent) {
        attempt.nativeRoot = attempt.agent->Root();
        attempt.sourceThread = attempt.agent->ThreadId();
        attempt.sourceGeneration = attempt.agent->Generation();
    }
    // A caller blocked in ShowMessageBox/ShowTaskDialog is waiting on this event
    // and would otherwise hang for the whole dialog deadline.
    if (attempt.virtualDialog && surface->resultEvent) SetEvent(surface->resultEvent);
    return true;
}

// The proxy goes away before native visibility changes.  If DWM cannot establish
// that isolation, kill the renderer rather than show the user two of the window.
void RendererSession::IsolateProxyForRestore(const RestoreAttempt& attempt) noexcept {
    if (HideProxyForRestore(attempt.proxy)) return;
    try {
        FluentShell::Log(L"Proxy isolation failed during restore; terminating renderer");
    } catch (...) {}
    TerminateRenderer(ERROR_CANCELLED);
}

// Hands the native window back to the user.  A virtual dialog and an already
// destroyed window need nothing, so both count as restored.
bool RendererSession::RestoreNativeWindow(const RestoreAttempt& attempt) {
    const auto& agent = attempt.agent;
    if (attempt.virtualDialog) return true;
    if (!agent || agent->IsDestroyed() || !IsWindow(attempt.nativeRoot)) return true;

    std::wstring error;
    bool restored = agent->Restore(error, 2000, shutdownEvent_);
    if (!restored) {
        // The source thread may be stalled.  DWM uncloak is the bounded emergency
        // path that keeps the target usable without it.
        restored = ClearApplicationCloak(attempt.nativeRoot);
        if (!restored) {
            try { FluentShell::Log(L"Emergency native uncloak failed: " + error); }
            catch (...) {}
        }
    }
    if (!agent->Shutdown()) {
        // The hooks could not be removed, so the agent must outlive this surface:
        // a later callback would otherwise dereference freed memory.  Retain it
        // exactly once, however many rollback attempts this surface sees.
        bool retain = false;
        {
            std::scoped_lock lock(attempt.surface->mutex);
            if (!attempt.surface->agentRetained) {
                attempt.surface->agentRetained = true;
                retain = true;
            }
        }
        if (retain) RetainSourceThreadAgent(agent);
    }
    return restored;
}

// Publishes the terminal state.  Returns true when a still-cloaked native window
// needs the detached uncloak retry.
bool RendererSession::PublishRestoredState(
    const RestoreAttempt& attempt, bool nativeRestored) {
    const auto& surface = attempt.surface;
    std::scoped_lock lock(surface->mutex);
    surface->restoreInProgress = false;
    if (nativeRestored) {
        surface->state = SurfaceState::Closed;
        return false;
    }
    surface->state = SurfaceState::Restoring;
    if (!attempt.nativeRoot || surface->restoreRetryScheduled) return false;
    surface->restoreRetryScheduled = true;
    return true;
}

// Advisory only: native visibility is already restored, so these frames are sent
// with a short deadline and are not allowed to fault the session.
void RendererSession::NotifyRendererOfRollback(
    const RestoreAttempt& attempt, std::wstring_view protocolReason) noexcept {
    if (attempt.surfaceId.empty()) return;
    try {
        Send(Ipc::MessageType::SurfaceCommit, attempt.revision,
            SerializeSurfaceCommit(nonce_, attempt.surfaceId, attempt.revision, false),
            250, false);
        Send(Ipc::MessageType::WindowClose, attempt.revision,
            SerializeWindowClose(nonce_, attempt.surfaceId, protocolReason), 250, false);
    } catch (...) {
        // Keep the native fallback even when serialization or allocation fails
        // while the pipe is already faulted.
    }
}

// Last resort: the source thread never acknowledged the restore, so a detached
// thread keeps trying the bounded DWM uncloak until the window is visible again
// or its identity changes.  It must never terminate the injected process, so
// every path ends with the surface marked closed.
void RendererSession::ScheduleEmergencyUncloakRetry(const RestoreAttempt& attempt) noexcept {
    const auto surface = attempt.surface;
    const auto agent = attempt.agent;
    if (!agent) return;
    const HWND nativeRoot = attempt.nativeRoot;
    const DWORD sourceThread = attempt.sourceThread;
    const uint64_t sourceGeneration = attempt.sourceGeneration;
    try {
        std::thread([weakSession = weak_from_this(), surface, agent,
                     nativeRoot, sourceThread, sourceGeneration] {
            const auto markClosed = [&surface] {
                try {
                    std::scoped_lock lock(surface->mutex);
                    surface->state = SurfaceState::Closed;
                    surface->restoreRetryScheduled = false;
                } catch (...) {}
            };
            try {
                while (!agent->IsDestroyed() &&
                       agent->Generation() == sourceGeneration &&
                       agent->ThreadId() == sourceThread &&
                       agent->Root() == nativeRoot && IsWindow(nativeRoot)) {
                    if (ClearApplicationCloak(nativeRoot)) {
                        SetWindowPos(nativeRoot, nullptr, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                            SWP_NOACTIVATE | SWP_FRAMECHANGED);
                        markClosed();
                        try { FluentShell::Log(L"Emergency native uncloak retry succeeded"); }
                        catch (...) {}
                        if (const auto session = weakSession.lock())
                            session->RetireSurfaceIfClosed(surface);
                        return;
                    }
                    Sleep(250);
                }
            } catch (...) {
                // Never let a detached recovery thread take down the target.
            }
            markClosed();
            if (const auto session = weakSession.lock())
                session->RetireSurfaceIfClosed(surface);
        }).detach();
    } catch (...) {
        std::scoped_lock lock(surface->mutex);
        surface->restoreRetryScheduled = false;
    }
}

void RendererSession::RestoreSurface(
    const std::shared_ptr<Surface>& surface,
    std::wstring_view reason) noexcept {
    if (!surface) return;
    RestoreAttempt attempt;
    attempt.surface = surface;
    bool nativeRestored = false;
    try {
        const std::wstring_view protocolReason = WireRestoreReason(reason);
        try {
            FluentShell::Log(L"Restoring native window: reason=" + std::wstring(reason) +
                L" wire=" + std::wstring(protocolReason));
        } catch (...) {}

        // The single native-operation barrier.  Action and reconcile workers take
        // the same lock, so neither can issue a source-thread call once rollback
        // has begun.
        std::unique_lock canonicalLock(surface->canonicalMutex);
        if (!BeginRestore(attempt)) return;
        IsolateProxyForRestore(attempt);
        nativeRestored = RestoreNativeWindow(attempt);
        const bool scheduleRetry = PublishRestoredState(attempt, nativeRestored);
        canonicalLock.unlock();

        // Native visibility is restored before any potentially backpressured pipe
        // operation, so a stalled renderer cannot delay the fallback.
        NotifyRendererOfRollback(attempt, protocolReason);
        if (nativeRestored) RetireSurfaceIfClosed(surface);
        if (scheduleRetry) ScheduleEmergencyUncloakRetry(attempt);
    } catch (...) {
        // Allocation and IPC failures must not escape a noexcept rollback
        // boundary.  Only bounded, non-allocating emergency operations here.
        if (!nativeRestored && attempt.nativeRoot && IsWindow(attempt.nativeRoot))
            nativeRestored = ClearApplicationCloak(attempt.nativeRoot);
        try {
            std::scoped_lock lock(surface->mutex);
            surface->restoreInProgress = false;
            surface->state = nativeRestored || !attempt.nativeRoot
                ? SurfaceState::Closed
                : SurfaceState::Restoring;
        } catch (...) {}
        if (nativeRestored) RetireSurfaceIfClosed(surface);
    }
}

void RendererSession::RestoreAll(std::wstring_view reason) noexcept {
    // The protocol caps a window tree, but not the number of top-levels.  Keep
    // this emergency path allocation-free so an OOM cannot skip native restore.
    std::array<std::shared_ptr<Surface>, 512> surfaces{};
    size_t count = 0;
    try {
        std::scoped_lock lock(surfacesMutex_);
        for (const auto& [_, surface] : surfaces_) {
            if (count == surfaces.size()) break;
            surfaces[count++] = surface;
        }
    } catch (...) {
        return;
    }
    for (size_t index = 0; index < count; ++index) {
        try { RestoreSurface(surfaces[index], reason); }
        catch (...) { /* RestoreSurface has its own firewall. */ }
    }
}

void RendererSession::HeartbeatMain() noexcept {
    uint64_t lastOutboundHeartbeat = 0;
    try {
        while (!stopping_.load() && !failed_.load()) {
            if (HasRendererExited()) {
                Fail(L"renderer process exited");
                break;
            }
            const uint64_t now = GetTickCount64();
            if (now - lastHeartbeatTick_.load() > kRendererHeartbeatTimeoutMs) {
                Fail(L"renderer heartbeat expired");
                break;
            }
            if (now - lastOutboundHeartbeat >= 1000) {
                if (!Send(Ipc::MessageType::Heartbeat, 0, SerializeHeartbeat(nonce_, now)))
                    break;
                lastOutboundHeartbeat = now;
            }
            if (shutdownEvent_ && WaitForSingleObject(shutdownEvent_, 250) == WAIT_OBJECT_0)
                break;
        }
    } catch (...) {
        Fail(L"heartbeat worker exception");
    }
}

void RendererSession::Fail(std::wstring_view reason) noexcept {
    if (failed_.exchange(true)) return;
    ready_ = false;
    if (shutdownEvent_) SetEvent(shutdownEvent_);
    try {
        FluentShell::Log(std::wstring(L"Renderer session failed: ") + std::wstring(reason));
    } catch (...) {
        OutputDebugStringW(L"FluentShell renderer session failed\n");
    }
    try {
        std::scoped_lock lock(surfacesMutex_);
        for (const auto& [_, surface] : surfaces_) {
            if (surface->resultEvent) SetEvent(surface->resultEvent);
            surface->readyCondition.notify_all();
        }
    } catch (...) {}
    try { actionCondition_.notify_all(); } catch (...) {}
}

void RendererSession::RunSupervisor() {
    uint64_t lastDiscovery = 0;
    while (!stopping_.load() && !failed_.load()) {
        const uint64_t now = GetTickCount64();
        if (now - lastDiscovery >= 1000) {
            DiscoverTopLevelWindows();
            lastDiscovery = now;
        }
        ReconcileNativeSurfaces();
        Sleep(250);
    }
    RestoreAll(L"restore");
    Stop();
}

void RendererSession::Stop() noexcept {
    std::scoped_lock stopLock(stopMutex_);
    if (stopping_.exchange(true)) return;
    ready_ = false;
    if (shutdownEvent_) SetEvent(shutdownEvent_);
    try { actionCondition_.notify_all(); } catch (...) {}
    try {
        std::scoped_lock lock(surfacesMutex_);
        for (const auto& [_, surface] : surfaces_) {
            if (surface->resultEvent) SetEvent(surface->resultEvent);
        }
    } catch (...) {}
    if (actionWorker_.joinable() && actionWorker_.get_id() != std::this_thread::get_id()) {
        try { actionWorker_.join(); } catch (...) {}
    }
    if (heartbeatWorker_.joinable() && heartbeatWorker_.get_id() != std::this_thread::get_id()) {
        try { heartbeatWorker_.join(); } catch (...) {}
    }
    RestoreAll(L"shutdown");
    // Announce the close even on the fault path.  Tearing the pipe down without a
    // shutdown frame makes an ordinary fault indistinguishable from a crash on the
    // renderer side, where it surfaces as a mid-FLSH-frame end of stream.
    try {
        Send(Ipc::MessageType::Shutdown, 0,
            SerializeShutdown(nonce_,
                failed_.load() ? L"bridge session failed" : L"bridge shutdown"),
            250, false);
    } catch (...) {}
    HANDLE pipeToCancel = INVALID_HANDLE_VALUE;
    {
        std::scoped_lock lock(writeMutex_);
        pipeClosing_ = true;
        pipeToCancel = pipe_;
    }
    if (pipeToCancel != INVALID_HANDLE_VALUE) {
        CancelIoEx(pipeToCancel, nullptr);
    }
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) {
        try { reader_.join(); } catch (...) {}
    }
    {
        std::scoped_lock lock(writeMutex_);
        if (pipe_ != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(pipe_);
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
    }
    {
        std::scoped_lock lock(processMutex_);
        if (rendererProcess_) {
            if (WaitForSingleObject(rendererProcess_, 2000) == WAIT_TIMEOUT) {
                TerminateProcess(rendererProcess_, 0);
                WaitForSingleObject(rendererProcess_, 1000);
            }
            CloseHandle(rendererProcess_);
            rendererProcess_ = nullptr;
        }
    }
    if (rendererThread_) {
        CloseHandle(rendererThread_);
        rendererThread_ = nullptr;
    }
}

WindowSnapshot RendererSession::BuildMessageBoxSnapshot(
    HWND owner,
    std::wstring_view text,
    std::wstring_view caption,
    UINT type,
    std::unordered_map<uint64_t, int>& results) {
    WindowSnapshot snapshot;
    snapshot.surfaceId = Ipc::NewGuidString();
    snapshot.surfaceKind = SurfaceKind::MessageBox;
    snapshot.modal = true;
    snapshot.generation = GetTickCount64();
    snapshot.revision = 1;
    snapshot.ownerHwnd = owner;
    snapshot.title = caption.empty() ? L"Message" : std::wstring(caption);
    snapshot.dpi = owner ? GetDpiForWindow(owner) : 96;
    if (!snapshot.dpi) snapshot.dpi = 96;
    snapshot.bounds = CenteredBounds(owner, 540, 300);
    snapshot.clientBounds = { 0, 0, 540, 300 };
    snapshot.windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    snapshot.windowExStyle = WS_EX_DLGMODALFRAME;
    snapshot.showInTaskbar = owner == nullptr;
    snapshot.icon = MessageBoxIcon(type);
    snapshot.rtl = (type & (MB_RTLREADING | MB_RIGHT)) != 0;

    std::vector<std::pair<int, std::wstring>> buttons;
    switch (type & MB_TYPEMASK) {
    case MB_OKCANCEL: buttons = {{ IDOK, L"OK" }, { IDCANCEL, L"Cancel" }}; break;
    case MB_ABORTRETRYIGNORE: buttons = {{ IDABORT, L"Abort" }, { IDRETRY, L"Retry" }, { IDIGNORE, L"Ignore" }}; break;
    case MB_YESNOCANCEL: buttons = {{ IDYES, L"Yes" }, { IDNO, L"No" }, { IDCANCEL, L"Cancel" }}; break;
    case MB_YESNO: buttons = {{ IDYES, L"Yes" }, { IDNO, L"No" }}; break;
    case MB_RETRYCANCEL: buttons = {{ IDRETRY, L"Retry" }, { IDCANCEL, L"Cancel" }}; break;
    case MB_CANCELTRYCONTINUE: buttons = {{ IDCANCEL, L"Cancel" }, { IDTRYAGAIN, L"Try Again" }, { IDCONTINUE, L"Continue" }}; break;
    default: buttons = {{ IDOK, L"OK" }}; break;
    }
    snapshot.canCancel = std::any_of(buttons.begin(), buttons.end(), [](const auto& button) {
        return button.first == IDCANCEL;
    });

    uint64_t nodeId = 1;
    ControlNode content;
    content.nodeId = nodeId++;
    content.generation = snapshot.generation;
    content.kind = ControlKind::StaticText;
    content.rect = { 32, 40, 508, 190 };
    content.text = std::wstring(text);
    content.automationName = content.text;
    snapshot.nodes.push_back(std::move(content));

    int defaultIndex = static_cast<int>((type & MB_DEFMASK) >> 8);
    defaultIndex = std::clamp(defaultIndex, 0, static_cast<int>(buttons.size()) - 1);
    const LONG buttonWidth = 112;
    const LONG gap = 12;
    LONG left = 508 - static_cast<LONG>(buttons.size()) * buttonWidth -
        static_cast<LONG>(buttons.size() - 1) * gap;
    for (size_t index = 0; index < buttons.size(); ++index) {
        ControlNode button;
        button.nodeId = nodeId++;
        button.generation = snapshot.generation;
        button.kind = ControlKind::Button;
        button.controlId = buttons[index].first;
        button.zIndex = static_cast<int>(snapshot.nodes.size());
        button.rect = { left, 224, left + buttonWidth, 264 };
        button.visible = true;
        button.enabled = true;
        button.tabStop = true;
        button.tabIndex = static_cast<int>(index);
        button.isDefault = static_cast<int>(index) == defaultIndex;
        button.text = buttons[index].second;
        button.automationName = button.text;
        results.emplace(button.nodeId, buttons[index].first);
        snapshot.nodes.push_back(std::move(button));
        left += buttonWidth + gap;
    }
    return snapshot;
}

WindowSnapshot RendererSession::BuildTaskDialogSnapshot(
    const TASKDIALOGCONFIG& config,
    const std::vector<std::pair<int, std::wstring>>& buttons,
    std::wstring_view title,
    std::wstring_view instruction,
    std::wstring_view content,
    std::wstring_view footer,
    std::wstring_view verification,
    std::unordered_map<uint64_t, int>& results,
    std::optional<uint64_t>& verificationNode) {
    WindowSnapshot snapshot;
    snapshot.surfaceId = Ipc::NewGuidString();
    snapshot.surfaceKind = SurfaceKind::TaskDialog;
    snapshot.modal = true;
    snapshot.canCancel = (config.dwFlags & TDF_ALLOW_DIALOG_CANCELLATION) != 0 ||
        std::any_of(buttons.begin(), buttons.end(), [](const auto& value) { return value.first == IDCANCEL; });
    snapshot.icon = TaskDialogIcon(config.pszMainIcon);
    snapshot.generation = GetTickCount64();
    snapshot.revision = 1;
    snapshot.ownerHwnd = config.hwndParent;
    snapshot.title = title.empty() ? L"Dialog" : std::wstring(title);
    snapshot.dpi = config.hwndParent ? GetDpiForWindow(config.hwndParent) : 96;
    if (!snapshot.dpi) snapshot.dpi = 96;
    snapshot.bounds = CenteredBounds(config.hwndParent, 620, 410);
    snapshot.clientBounds = { 0, 0, 620, 410 };
    snapshot.windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    snapshot.windowExStyle = WS_EX_DLGMODALFRAME;
    snapshot.showInTaskbar = config.hwndParent == nullptr;
    snapshot.rtl = (config.dwFlags & TDF_RTL_LAYOUT) != 0;

    uint64_t nodeId = 1;
    auto addText = [&](std::wstring_view value, const RECT& rect) {
        if (value.empty()) return;
        ControlNode node;
        node.nodeId = nodeId++;
        node.generation = snapshot.generation;
        node.kind = ControlKind::StaticText;
        node.rect = rect;
        node.text = std::wstring(value);
        node.automationName = node.text;
        snapshot.nodes.push_back(std::move(node));
    };
    addText(instruction, { 32, 32, 588, 82 });
    addText(content, { 32, 88, 588, 205 });
    addText(footer, { 32, 252, 588, 292 });
    if (!verification.empty()) {
        ControlNode check;
        check.nodeId = nodeId++;
        check.generation = snapshot.generation;
        check.kind = ControlKind::CheckBox;
        check.controlId = -1;
        check.rect = { 32, 212, 588, 244 };
        check.tabStop = true;
        check.tabIndex = 0;
        check.text = std::wstring(verification);
        check.automationName = check.text;
        check.checked = (config.dwFlags & TDF_VERIFICATION_FLAG_CHECKED) != 0 ? 1 : 0;
        verificationNode = check.nodeId;
        snapshot.nodes.push_back(std::move(check));
    }
    const LONG buttonWidth = 120;
    const LONG gap = 12;
    LONG left = 588 - static_cast<LONG>(buttons.size()) * buttonWidth -
        static_cast<LONG>(buttons.empty() ? 0 : buttons.size() - 1) * gap;
    int buttonTabIndex = verification.empty() ? 0 : 1;
    for (const auto& [id, label] : buttons) {
        ControlNode button;
        button.nodeId = nodeId++;
        button.generation = snapshot.generation;
        button.kind = ControlKind::Button;
        button.controlId = id;
        button.rect = { left, 324, left + buttonWidth, 366 };
        button.tabStop = true;
        button.tabIndex = buttonTabIndex++;
        button.isDefault = id == config.nDefaultButton ||
            (config.nDefaultButton == 0 && results.empty());
        button.text = label;
        button.automationName = label;
        results.emplace(button.nodeId, id);
        snapshot.nodes.push_back(std::move(button));
        left += buttonWidth + gap;
    }
    return snapshot;
}

std::optional<int> RendererSession::WaitForDialogResult(
    const std::shared_ptr<Surface>& surface,
    HWND owner,
    bool ownerWasEnabled) {
    HANDLE event = surface->resultEvent;
    while (!failed_.load() && !stopping_.load()) {
        const DWORD wait = MsgWaitForMultipleObjectsEx(
            1, &event, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE | MWMO_ALERTABLE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_OBJECT_0 + 1) PumpThreadMessages();
        else break;
    }
    std::optional<int> result;
    {
        std::scoped_lock lock(surface->mutex);
        result = surface->dialogResult;
    }
    // RestoreSurface owns the close transaction.  Keeping the operation
    // idempotent avoids a duplicate window.close when renderer failure or a
    // native veto wakes this waiter concurrently.
    RestoreSurface(surface, L"nativeDestroyed");
    if (owner && IsWindow(owner)) {
        EnableWindow(owner, ownerWasEnabled ? TRUE : FALSE);
        // A projected owner remains DWM-cloaked while its renderer modal
        // closes.  Let WindowRegistry reactivate the owner proxy in that case;
        // foregrounding the hidden native HWND can steal activation from it.
        if (ownerWasEnabled && !IsProjectedOwner(owner)) SetForegroundWindow(owner);
    }
    {
        std::scoped_lock lock(surfacesMutex_);
        const auto id = surface->snapshot.surfaceId;
        RememberRetiredSurfaceLocked(id);
        const auto found = surfaces_.find(id);
        if (found != surfaces_.end() && retiredSurfaceIds_.contains(id)) surfaces_.erase(found);
    }
    return result;
}

std::optional<int> RendererSession::ShowMessageBox(
    HWND owner,
    std::wstring_view text,
    std::wstring_view caption,
    UINT type,
    WORD) {
    owner = owner ? GetAncestor(owner, GA_ROOT) : nullptr;
    if (!IsReady() || !IsProjectedOwner(owner) ||
        (type & (MB_SERVICE_NOTIFICATION | MB_DEFAULT_DESKTOP_ONLY | MB_HELP)) != 0) {
        return std::nullopt;
    }
    auto surface = std::make_shared<Surface>();
    surface->virtualDialog = true;
    surface->snapshot = BuildMessageBoxSnapshot(owner, text, caption, type, surface->buttonResults);
    surface->fingerprint = SnapshotFingerprint(surface->snapshot);
    if (surface->snapshot.canCancel) surface->cancelResult = IDCANCEL;
    {
        std::scoped_lock lock(surfacesMutex_);
        surfaces_.emplace(surface->snapshot.surfaceId, surface);
    }
    const bool ownerWasEnabled = owner && IsWindowEnabled(owner);
    if (ownerWasEnabled) EnableWindow(owner, FALSE);
    if (!OpenSurface(surface, false)) {
        RestoreSurface(surface, L"restore");
        if (owner && IsWindow(owner)) EnableWindow(owner, ownerWasEnabled ? TRUE : FALSE);
        return std::nullopt;
    }
    return WaitForDialogResult(surface, owner, ownerWasEnabled);
}

std::optional<TaskDialogResult> RendererSession::ShowTaskDialog(
    const TASKDIALOGCONFIG& config,
    const std::vector<std::pair<int, std::wstring>>& buttons,
    std::wstring_view title,
    std::wstring_view instruction,
    std::wstring_view content,
    std::wstring_view footer,
    std::wstring_view verification) {
    const HWND owner = config.hwndParent ? GetAncestor(config.hwndParent, GA_ROOT) : nullptr;
    if (!IsReady() || !IsProjectedOwner(owner) || buttons.empty()) return std::nullopt;
    auto surface = std::make_shared<Surface>();
    surface->virtualDialog = true;
    surface->snapshot = BuildTaskDialogSnapshot(config, buttons, title, instruction,
        content, footer, verification, surface->buttonResults, surface->verificationNode);
    surface->snapshot.ownerHwnd = owner;
    surface->fingerprint = SnapshotFingerprint(surface->snapshot);
    surface->verificationChecked = (config.dwFlags & TDF_VERIFICATION_FLAG_CHECKED) != 0;
    if (surface->snapshot.canCancel) surface->cancelResult = IDCANCEL;
    {
        std::scoped_lock lock(surfacesMutex_);
        surfaces_.emplace(surface->snapshot.surfaceId, surface);
    }
    const bool ownerWasEnabled = owner && IsWindowEnabled(owner);
    if (ownerWasEnabled) EnableWindow(owner, FALSE);
    if (!OpenSurface(surface, false)) {
        RestoreSurface(surface, L"restore");
        if (owner && IsWindow(owner)) EnableWindow(owner, ownerWasEnabled ? TRUE : FALSE);
        return std::nullopt;
    }
    const auto button = WaitForDialogResult(surface, owner, ownerWasEnabled);
    if (!button) return std::nullopt;
    return TaskDialogResult{ *button, surface->verificationChecked };
}

} // namespace FluentShell::Bridge::Translation
