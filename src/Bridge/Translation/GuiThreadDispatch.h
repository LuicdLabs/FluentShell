#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace FluentShell::Bridge::Translation {

// A capture/action keeps one deadline and cancellation token across all GUI
// threads participating in the same top-level surface. Work passed below must
// own its inputs and outputs: a timed-out receiver can finish after its caller.
class GuiThreadOperationScope final {
public:
    GuiThreadOperationScope(std::shared_ptr<std::atomic<bool>> cancelled, ULONGLONG deadline,
        bool dispatched = false) noexcept;
    ~GuiThreadOperationScope();
    GuiThreadOperationScope(const GuiThreadOperationScope&) = delete;
private:
    std::shared_ptr<std::atomic<bool>> previousCancellation_;
    ULONGLONG previousDeadline_ = 0;
    bool previousDispatched_ = false;
};

bool GuiThreadOperationCancelled() noexcept;
bool IsGuiThreadDispatchActive() noexcept;
bool DispatchToWindowThread(HWND window, std::function<void()> work,
    std::wstring& error, DWORD timeoutMs = 2000) noexcept;

// Queue application work that can enter a native modal loop. It runs only after
// the owner's bounded command scope has exited, with no command deadline. The
// callback owns everything it uses and must check the shared cancellation token
// before native mutations. Success acknowledges queuing, not callback completion.
bool PostToWindowThread(HWND window, std::function<void()> work,
    std::shared_ptr<std::atomic<bool>> cancelled, std::wstring& error) noexcept;

// Marks the token cancelled and promptly releases matching queued callbacks.
// An already-running callback retains its own state and observes cancellation
// through GuiThreadOperationCancelled(); cancellation cannot unwind native code.
void CancelPostedGuiThreadWork(const std::shared_ptr<std::atomic<bool>>& cancelled) noexcept;

// Cancels queued work and waits for running callbacks to finish before rollback.
// Pumps only sent notifications needed by the owning GUI threads. A native modal
// callback that cannot finish within the shared deadline makes the drain fail.
bool CancelAndDrainPostedGuiThreadWork(const std::shared_ptr<std::atomic<bool>>& cancelled,
    std::wstring& error, DWORD timeoutMs = 2000) noexcept;

// Supervisor-side rollback barrier for all remote work belonging to a native
// top-level surface, including a synchronous setter that outlived its caller.
// The source root is recorded when each operation is submitted.
bool CancelAndDrainGuiThreadWork(HWND sourceRoot, std::wstring& error, DWORD timeoutMs = 2000) noexcept;

} // namespace FluentShell::Bridge::Translation
