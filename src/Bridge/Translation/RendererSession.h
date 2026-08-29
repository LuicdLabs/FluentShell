#pragma once

#include "SourceThreadAgent.h"

#include <commctrl.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <string>
#include <thread>
#include <unordered_map>

namespace FluentShell::Bridge::Translation {

struct TaskDialogResult final {
    int button = 0;
    bool verificationChecked = false;
};

class RendererSession final : public std::enable_shared_from_this<RendererSession> {
public:
    explicit RendererSession(HMODULE bridgeModule) noexcept;
    ~RendererSession();

    RendererSession(const RendererSession&) = delete;
    RendererSession& operator=(const RendererSession&) = delete;

    bool Start();
    void RunSupervisor();
    void Stop() noexcept;
    bool IsReady() const noexcept { return ready_.load() && !failed_.load(); }

    std::optional<int> ShowMessageBox(
        HWND owner,
        std::wstring_view text,
        std::wstring_view caption,
        UINT type,
        WORD languageId = 0);

    std::optional<TaskDialogResult> ShowTaskDialog(
        const TASKDIALOGCONFIG& config,
        const std::vector<std::pair<int, std::wstring>>& buttons,
        std::wstring_view title,
        std::wstring_view instruction,
        std::wstring_view content,
        std::wstring_view footer,
        std::wstring_view verification);

private:
    struct Surface;

    bool CreatePipeAndRenderer(uint64_t handshakeDeadline);
    bool Handshake(DWORD timeoutMs);
    void HeartbeatMain() noexcept;
    void ReaderMain() noexcept;
    bool Send(
        Ipc::MessageType type,
        uint64_t revision,
        std::string payload,
        DWORD timeoutMs = 2000,
        bool failSessionOnError = true) noexcept;
    void HandleFrame(const Ipc::Frame& frame);
    void HandleReady(const SurfaceReady& ready);
    void HandleAction(const ActionRequest& action);
    void HandleNativeAction(const ActionRequest& action, const std::shared_ptr<Surface>& surface);
    void ActionMain() noexcept;
    void DiscoverTopLevelWindows();
    bool OpenNativeWindow(HWND root);
    bool IsProjectedOwner(HWND owner);
    bool OpenSurface(const std::shared_ptr<Surface>& surface, bool cloakNative);
    void ReconcileNativeSurfaces();
    void RestoreSurface(const std::shared_ptr<Surface>& surface, std::wstring_view reason) noexcept;
    void RestoreAll(std::wstring_view reason) noexcept;
    bool IsRetiredSurface(std::wstring_view surfaceId) noexcept;
    void RememberRetiredSurfaceLocked(std::wstring_view surfaceId) noexcept;
    void RetireSurfaceIfClosed(const std::shared_ptr<Surface>& surface) noexcept;
    std::optional<int> WaitForDialogResult(
        const std::shared_ptr<Surface>& surface,
        HWND owner,
        bool ownerWasEnabled);
    WindowSnapshot BuildMessageBoxSnapshot(
        HWND owner,
        std::wstring_view text,
        std::wstring_view caption,
        UINT type,
        std::unordered_map<uint64_t, int>& results);
    WindowSnapshot BuildTaskDialogSnapshot(
        const TASKDIALOGCONFIG& config,
        const std::vector<std::pair<int, std::wstring>>& buttons,
        std::wstring_view title,
        std::wstring_view instruction,
        std::wstring_view content,
        std::wstring_view footer,
        std::wstring_view verification,
        std::unordered_map<uint64_t, int>& results,
        std::optional<uint64_t>& verificationNode);
    std::wstring RendererPath() const;
    bool VerifyRendererClient() const noexcept;
    bool HasRendererExited() noexcept;
    void TerminateRenderer(DWORD exitCode) noexcept;
    void Fail(std::wstring_view reason) noexcept;

    HMODULE bridgeModule_ = nullptr;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE rendererProcess_ = nullptr;
    HANDLE rendererThread_ = nullptr;
    HANDLE shutdownEvent_ = nullptr;
    DWORD rendererProcessId_ = 0;
    uint64_t rendererCreated_ = 0;
    std::wstring pipeName_;
    std::wstring nonce_;
    std::atomic<bool> ready_{ false };
    std::atomic<bool> failed_{ false };
    std::atomic<bool> stopping_{ false };
    std::atomic<uint64_t> outboundSequence_{ 0 };
    std::atomic<uint64_t> inboundSequence_{ 0 };
    std::atomic<uint64_t> lastHeartbeatTick_{ 0 };
    std::mutex stopMutex_;
    std::mutex writeMutex_;
    bool pipeClosing_ = false;
    std::mutex processMutex_;
    std::mutex surfacesMutex_;
    std::unordered_map<std::wstring, std::shared_ptr<Surface>> surfaces_;
    std::unordered_set<std::wstring> retiredSurfaceIds_;
    std::deque<std::wstring> retiredSurfaceOrder_;
    std::unordered_set<HWND> discoveryAttempts_;
    // Zero means an owned top-level is still visible. A nonzero tick starts the
    // quiet period that must elapse before the native owner is projected again.
    std::unordered_map<HWND, uint64_t> ownerGraphDeferrals_;
    std::mutex actionMutex_;
    std::condition_variable actionCondition_;
    std::deque<std::pair<ActionRequest, std::shared_ptr<Surface>>> actionQueue_;
    std::thread reader_;
    std::thread actionWorker_;
    std::thread heartbeatWorker_;
};

void SetActiveRendererSession(std::shared_ptr<RendererSession> session);
std::shared_ptr<RendererSession> ActiveRendererSession();

} // namespace FluentShell::Bridge::Translation
