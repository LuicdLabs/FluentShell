#pragma once

#include "WindowCapture.h"

#include <atomic>
#include <memory>
#include <string>

namespace FluentShell::Bridge::Translation {

struct ActionOutcome final {
    bool accepted = false;
    bool destroyed = false;
    uint64_t closeSequence = 0;
    uint64_t revision = 0;
    WindowSnapshot snapshot;
    std::wstring error;
};

class SourceThreadAgent final : public std::enable_shared_from_this<SourceThreadAgent> {
public:
    static std::shared_ptr<SourceThreadAgent> Attach(HWND root, HMODULE module);
    ~SourceThreadAgent();

    SourceThreadAgent(const SourceThreadAgent&) = delete;
    SourceThreadAgent& operator=(const SourceThreadAgent&) = delete;

    // timedOut distinguishes a source thread that did not acknowledge within the
    // deadline -- a transient condition -- from a capture that ran and rejected the
    // window.  Only the latter means the window is unsupported.
    bool Capture(
        WindowSnapshot& snapshot,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr,
        bool* timedOut = nullptr);
    bool Invoke(
        const ActionRequest& action,
        ActionOutcome& outcome,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool SetCloaked(
        bool cloaked,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool CaptureAndCloak(
        WindowSnapshot& snapshot,
        uint64_t expectedFingerprint,
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool Restore(
        std::wstring& error,
        DWORD timeoutMs = 2000,
        HANDLE cancelEvent = nullptr);
    bool Shutdown() noexcept;

    HWND Root() const noexcept { return root_; }
    DWORD ThreadId() const noexcept { return threadId_; }
    bool IsDirty() const noexcept { return dirty_.load(); }
    void ClearDirty() noexcept { dirty_.store(false); }
    bool IsDestroyed() const noexcept { return destroyed_.load(); }
    uint64_t Generation() const noexcept { return generation_; }
    UINT MessageId() const noexcept { return message_; }
    void MarkDirty() noexcept { dirty_.store(true); }
    void MarkDestroyed() noexcept { destroyed_.store(true); MarkDirty(); }
    uint64_t RegisterCloseRequest() noexcept;
    void MarkCloseRequestCompleted() noexcept;
    uint64_t CompletedCloseSequence() const noexcept {
        return closeCompleted_.load(std::memory_order_acquire);
    }
    bool CaptureOnSourceThread(
        std::wstring_view surfaceId,
        uint64_t revision,
        WindowSnapshot& snapshot,
        std::wstring& error) noexcept;

private:
    SourceThreadAgent(HWND root, HMODULE module, DWORD threadId, UINT message) noexcept;
    bool Post(void* command, DWORD timeoutMs, HANDLE cancelEvent = nullptr) noexcept;
    void UnhookAll() noexcept;

    HWND root_ = nullptr;
    HMODULE module_ = nullptr;
    DWORD threadId_ = 0;
    UINT message_ = 0;
    HHOOK hook_ = nullptr;
    HHOOK cbtHook_ = nullptr;
    HHOOK callWndRetHook_ = nullptr;
    uint64_t generation_ = 0;
    std::atomic<bool> dirty_{ true };
    std::atomic<bool> destroyed_{ false };
    std::atomic<bool> shuttingDown_{ false };
    std::atomic<uint64_t> closeIssued_{ 0 };
    std::atomic<uint64_t> closeCompleted_{ 0 };
    CaptureContext captureContext_;
};

void RetainSourceThreadAgent(std::shared_ptr<SourceThreadAgent> agent) noexcept;

} // namespace FluentShell::Bridge::Translation
