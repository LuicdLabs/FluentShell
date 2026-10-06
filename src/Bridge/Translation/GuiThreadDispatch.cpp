#include "GuiThreadDispatch.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace FluentShell::Bridge::Translation {
namespace {
thread_local std::shared_ptr<std::atomic<bool>> cancellation;
thread_local ULONGLONG deadline = 0;
thread_local bool dispatched = false;
thread_local bool deferredPostedWork = false;

struct Work final {
    HWND window = nullptr;
    HWND sourceRoot = nullptr;
    DWORD thread = 0;
    HHOOK hook = nullptr;
    HHOOK lifetimeHook = nullptr;
    HANDLE ownerThread = nullptr;
    HANDLE completedEvent = nullptr;
    std::shared_ptr<std::atomic<bool>> cancelled;
    ULONGLONG expires = 0;
    std::function<void()> invoke;
    std::atomic<bool> completed{false};
    bool failed = false;
    bool posted = false;
    bool running = false;
    bool deferred = false;
    bool abandoned = false;
    ~Work() {
        if (hook) UnhookWindowsHookEx(hook);
        if (lifetimeHook) UnhookWindowsHookEx(lifetimeHook);
        if (ownerThread) CloseHandle(ownerThread);
        if (completedEvent) CloseHandle(completedEvent);
    }
};

struct Registry final {
    std::mutex mutex;
    std::condition_variable changed;
    std::unordered_map<uint64_t, std::shared_ptr<Work>> pending;
    std::thread cleaner;

    // Called with mutex held. Work can outlive its sender when an application
    // stops pumping, enters a native modal callback, or exits its GUI thread.
    void EnsureCleaner() {
        if (cleaner.joinable()) return;
        cleaner = std::thread([this] {
            std::unique_lock lock(mutex);
            for (;;) {
                std::shared_ptr<Work> retired;
                for (auto iterator = pending.begin(); iterator != pending.end(); ++iterator) {
                    const auto& work = iterator->second;
                    if ((!work->running && (work->abandoned || work->completed.load(std::memory_order_acquire) ||
                            work->cancelled->load(std::memory_order_acquire))) ||
                            WaitForSingleObject(work->ownerThread, 0) != WAIT_TIMEOUT) {
                        retired = std::move(iterator->second);
                        pending.erase(iterator);
                        break;
                    }
                }
                if (retired) {
                    // User-owned closure destructors may themselves retire work.
                    lock.unlock();
                    retired.reset();
                    lock.lock();
                } else if (pending.empty()) {
                    changed.wait(lock);
                } else {
                    changed.wait_for(lock, std::chrono::milliseconds(100));
                }
            }
        });
    }
};

Registry& WorkRegistry() {
    // Bridge is process-pinned. CRT destruction runs under the loader lock,
    // where joining a cleaner thread can deadlock. Only this small registry
    // and its idle worker live until process exit; requests, hooks and owner
    // handles are still reclaimed on completion, cancellation or thread exit.
    static Registry* registry = new Registry();
    return *registry;
}
std::atomic<uint64_t> nextToken{1};

UINT WorkMessage() noexcept {
    static const UINT message = RegisterWindowMessageW(L"FluentShell.GuiThreadDispatch.v1");
    return message;
}
UINT PostedWorkMessage() noexcept {
    static const UINT message = RegisterWindowMessageW(L"FluentShell.GuiThreadPostedDispatch.v1");
    return message;
}

bool BoundedOperationActive() noexcept { return deadline != 0 || dispatched; }

LRESULT CALLBACK TargetLifetimeHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_DESTROYWND) {
        auto& registry = WorkRegistry();
        {
            std::scoped_lock lock(registry.mutex);
            for (auto& [_, work] : registry.pending) {
                if (!work->running && work->thread == GetCurrentThreadId() &&
                        work->window == reinterpret_cast<HWND>(wParam))
                    work->abandoned = true;
            }
        }
        // USER32 discards posts naming a destroyed window. A later HWND reuse
        // must not retain that unreachable callback until the thread itself exits.
        registry.changed.notify_one();
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void ResumeDeferredWork() noexcept {
    if (!deferredPostedWork || BoundedOperationActive()) return;
    deferredPostedWork = false;
    try {
        auto& registry = WorkRegistry();
        std::scoped_lock lock(registry.mutex);
        for (auto& [token, work] : registry.pending) {
            if (!work->posted || !work->deferred || work->thread != GetCurrentThreadId()) continue;
            work->deferred = false;
            if (!PostMessageW(work->window, PostedWorkMessage(), static_cast<WPARAM>(token), 0))
                work->abandoned = true;
        }
        registry.changed.notify_one();
    } catch (...) {}
}

// The registry owns running work until the hook returns or its thread exits.
// A stack shared_ptr would leak that ownership if native code called ExitThread
// without unwinding C++ frames; the thread-handle watcher can reclaim this form.
void RunWork(Work* work) noexcept {
    GuiThreadOperationScope scope(work->cancelled, work->expires, !work->posted);
    const auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        DWORD process = 0;
        const DWORD owner = GetWindowThreadProcessId(work->window, &process);
        if (!previousDpi || owner != GetCurrentThreadId() || process != GetCurrentProcessId() ||
                GuiThreadOperationCancelled()) {
            work->failed = true;
        } else {
            work->invoke();
            if (GuiThreadOperationCancelled()) work->failed = true;
        }
    } catch (...) { work->failed = true; }
    if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
    work->completed.store(true, std::memory_order_release);
    if (work->completedEvent) SetEvent(work->completedEvent);
}

LRESULT CALLBACK DispatchHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && lParam) {
        const auto& message = *reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (message.message == WorkMessage()) {
            Work* work = nullptr;
            std::shared_ptr<Work> retired;
            const auto token = static_cast<uint64_t>(message.wParam);
            auto& registry = WorkRegistry();
            {
                std::scoped_lock lock(registry.mutex);
                const auto found = registry.pending.find(token);
                if (found != registry.pending.end() && !found->second->posted && !found->second->abandoned &&
                        !found->second->running && found->second->window == message.hwnd &&
                        found->second->thread == GetCurrentThreadId()) {
                    work = found->second.get();
                    work->running = true;
                }
            }
            if (work) {
                RunWork(work);
                {
                    std::scoped_lock lock(registry.mutex);
                    const auto found = registry.pending.find(token);
                    if (found != registry.pending.end()) {
                        retired = std::move(found->second);
                        registry.pending.erase(found);
                    }
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK PostedDispatchHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == PM_REMOVE && lParam) {
        auto& message = *reinterpret_cast<MSG*>(lParam);
        if (message.message == PostedWorkMessage()) {
            Work* work = nullptr;
            std::shared_ptr<Work> retired;
            const auto token = static_cast<uint64_t>(message.wParam);
            auto& registry = WorkRegistry();
            {
                std::scoped_lock lock(registry.mutex);
                const auto found = registry.pending.find(token);
                if (found != registry.pending.end() && found->second->posted && !found->second->abandoned &&
                        !found->second->running && found->second->window == message.hwnd &&
                        found->second->thread == GetCurrentThreadId()) {
                    if (BoundedOperationActive()) {
                        // Reposting immediately would spin inside an application's
                        // nested pump. The outer scope reposts once on its exit.
                        found->second->deferred = true;
                        deferredPostedWork = true;
                    } else {
                        work = found->second.get();
                        work->running = true;
                    }
                    message.hwnd = nullptr;
                    message.message = WM_NULL;
                    message.wParam = 0;
                    message.lParam = 0;
                }
            }
            if (work) {
                RunWork(work);
                {
                    std::scoped_lock lock(registry.mutex);
                    const auto found = registry.pending.find(token);
                    if (found != registry.pending.end()) {
                        retired = std::move(found->second);
                        registry.pending.erase(found);
                    }
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

DWORD ResolveOwnerThread(HWND window, std::wstring& error) {
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (!thread || process != GetCurrentProcessId()) {
        error = L"native operation names a destroyed or foreign-process HWND";
        return 0;
    }
    return thread;
}

bool AttachHook(Work& work, std::wstring& error) {
    work.sourceRoot = GetAncestor(work.window, GA_ROOT);
    work.ownerThread = OpenThread(SYNCHRONIZE, FALSE, work.thread);
    work.completedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!work.sourceRoot || !work.ownerThread || !work.completedEvent) {
        error = L"cannot watch the owning GUI thread or surface";
        return false;
    }
    const HOOKPROC callback = work.posted ? PostedDispatchHook : DispatchHook;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(callback), &module)) {
        error = L"cannot resolve native GUI dispatcher module";
        return false;
    }
    work.hook = SetWindowsHookExW(work.posted ? WH_GETMESSAGE : WH_CALLWNDPROC, callback, module, work.thread);
    work.lifetimeHook = SetWindowsHookExW(WH_CBT, TargetLifetimeHook, module, work.thread);
    if (!work.hook || !work.lifetimeHook || !(work.posted ? PostedWorkMessage() : WorkMessage())) {
        error = L"cannot attach native GUI dispatcher";
        return false;
    }
    return true;
}

// The returned reference keeps any callback destructor outside the registry lock.
std::shared_ptr<Work> RemovePending(uint64_t token) {
    auto& registry = WorkRegistry();
    std::scoped_lock lock(registry.mutex);
    const auto found = registry.pending.find(token);
    if (found == registry.pending.end()) return {};
    if (found->second->running) return {};
    auto work = std::move(found->second);
    registry.pending.erase(found);
    return work;
}

bool DrainRunningWork(const std::vector<std::shared_ptr<Work>>& running, std::wstring& error, DWORD timeoutMs) {
    ULONGLONG expires = GetTickCount64() + timeoutMs;
    if (deadline) expires = (std::min)(expires, deadline);
    for (const auto& work : running) {
        if (work->completed.load(std::memory_order_acquire)) continue;
        if (work->thread == GetCurrentThreadId()) {
            error = L"native GUI rollback cannot drain its own active callback";
            return false;
        }
        const HANDLE waits[] = {work->completedEvent, work->ownerThread};
        while (!work->completed.load(std::memory_order_acquire)) {
            const auto now = GetTickCount64();
            if (now >= expires || GuiThreadOperationCancelled()) {
                error = L"native GUI callback did not finish before the rollback deadline";
                return false;
            }
            const DWORD wait = MsgWaitForMultipleObjectsEx(2, waits, static_cast<DWORD>(expires - now),
                QS_SENDMESSAGE, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0 || wait == WAIT_OBJECT_0 + 1) break;
            if (wait == WAIT_OBJECT_0 + 2) {
                // PeekMessage dispatches sent messages before returning. Do
                // not remove posted input or enter the source command queue.
                MSG message{};
                PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
                continue;
            }
            error = L"native GUI callback did not finish before the rollback deadline";
            return false;
        }
    }
    return true;
}
} // namespace

GuiThreadOperationScope::GuiThreadOperationScope(std::shared_ptr<std::atomic<bool>> cancelled,
    ULONGLONG expires, bool remote) noexcept
    : previousCancellation_(std::move(cancellation)), previousDeadline_(deadline), previousDispatched_(dispatched) {
    cancellation = std::move(cancelled);
    deadline = expires;
    dispatched = remote;
}
GuiThreadOperationScope::~GuiThreadOperationScope() {
    cancellation = std::move(previousCancellation_);
    deadline = previousDeadline_;
    dispatched = previousDispatched_;
    ResumeDeferredWork();
}
bool GuiThreadOperationCancelled() noexcept {
    return (cancellation && cancellation->load(std::memory_order_acquire)) ||
        (deadline && GetTickCount64() >= deadline);
}
bool IsGuiThreadDispatchActive() noexcept { return dispatched; }

bool DispatchToWindowThread(HWND window, std::function<void()> invoke,
    std::wstring& error, DWORD timeoutMs) noexcept {
    uint64_t token = 0;
    try {
        const DWORD thread = ResolveOwnerThread(window, error);
        if (!thread) return false;
        if (!invoke || GuiThreadOperationCancelled() || !timeoutMs) {
            error = L"native GUI operation cancelled or empty";
            return false;
        }
        auto cancelled = cancellation ? cancellation : std::make_shared<std::atomic<bool>>(false);
        ULONGLONG expires = GetTickCount64() + timeoutMs;
        if (deadline) expires = (std::min)(expires, deadline);
        if (thread == GetCurrentThreadId()) {
            GuiThreadOperationScope scope(cancelled, expires, dispatched);
            invoke();
            if (!GuiThreadOperationCancelled()) return true;
            cancelled->store(true, std::memory_order_release);
            error = L"native GUI operation exceeded its deadline or was cancelled";
            return false;
        }
        auto work = std::make_shared<Work>();
        work->window = window;
        work->thread = thread;
        work->cancelled = std::move(cancelled);
        work->expires = expires;
        work->invoke = std::move(invoke);
        if (!AttachHook(*work, error)) return false;
        token = nextToken.fetch_add(1);
        if (!token) { error = L"native GUI dispatcher token space exhausted"; return false; }
        {
            auto& registry = WorkRegistry();
            std::scoped_lock lock(registry.mutex);
            registry.EnsureCleaner();
            registry.pending.emplace(token, work);
            registry.changed.notify_one();
        }
        const auto now = GetTickCount64();
        const DWORD remaining = now >= work->expires ? 0 : static_cast<DWORD>(work->expires - now);
        DWORD_PTR result = 0;
        // Do not set SMTO_BLOCK: a native child can synchronously notify its
        // parent on the calling GUI thread. Only sent messages are pumped by
        // USER32 here, never arbitrary posted keyboard/menu input.
        const bool sent = remaining && SendMessageTimeoutW(window, WorkMessage(),
            static_cast<WPARAM>(token), 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, remaining, &result);
        RemovePending(token); // A late delivery can no longer find the request.
        if (!sent || !work->completed.load(std::memory_order_acquire)) {
            work->cancelled->store(true, std::memory_order_release);
            error = L"owning GUI thread did not acknowledge native operation before its deadline";
            return false;
        }
        if (work->failed || work->cancelled->load(std::memory_order_acquire) || GetTickCount64() >= work->expires) {
            work->cancelled->store(true, std::memory_order_release);
            error = L"owning GUI thread rejected or cancelled native operation";
            return false;
        }
        return true;
    } catch (...) {
        if (token) RemovePending(token);
        error = L"native GUI dispatch failed";
        return false;
    }
}

bool PostToWindowThread(HWND window, std::function<void()> invoke,
    std::shared_ptr<std::atomic<bool>> cancelled, std::wstring& error) noexcept {
    uint64_t token = 0;
    try {
        const DWORD thread = ResolveOwnerThread(window, error);
        if (!thread) return false;
        if (!invoke || !cancelled || cancelled->load(std::memory_order_acquire) || GuiThreadOperationCancelled()) {
            error = L"posted native GUI operation cancelled or empty";
            return false;
        }
        auto work = std::make_shared<Work>();
        work->window = window;
        work->thread = thread;
        work->posted = true;
        work->cancelled = std::move(cancelled);
        work->invoke = std::move(invoke);
        if (!AttachHook(*work, error)) return false;
        token = nextToken.fetch_add(1);
        if (!token) { error = L"native GUI dispatcher token space exhausted"; return false; }
        {
            auto& registry = WorkRegistry();
            std::scoped_lock lock(registry.mutex);
            if (work->cancelled->load(std::memory_order_acquire)) {
                error = L"posted native GUI operation was cancelled before queuing";
                return false;
            }
            registry.EnsureCleaner();
            registry.pending.emplace(token, work);
            registry.changed.notify_one();
        }
        if (!PostMessageW(window, PostedWorkMessage(), static_cast<WPARAM>(token), 0)) {
            RemovePending(token);
            error = L"cannot queue native operation on the owning GUI thread";
            return false;
        }
        return true;
    } catch (...) {
        if (token) RemovePending(token);
        error = L"native GUI posting failed";
        return false;
    }
}

void CancelPostedGuiThreadWork(const std::shared_ptr<std::atomic<bool>>& cancelled) noexcept {
    if (!cancelled) return;
    cancelled->store(true, std::memory_order_release);
    try {
        auto& registry = WorkRegistry();
        for (;;) {
            std::shared_ptr<Work> retired;
            {
                std::scoped_lock lock(registry.mutex);
                for (auto iterator = registry.pending.begin(); iterator != registry.pending.end(); ++iterator) {
                    if (iterator->second->posted && !iterator->second->running &&
                            iterator->second->cancelled == cancelled) {
                        retired = std::move(iterator->second);
                        registry.pending.erase(iterator);
                        break;
                    }
                }
            }
            if (!retired) break;
        }
        registry.changed.notify_one();
    } catch (...) {}
}

bool CancelAndDrainPostedGuiThreadWork(const std::shared_ptr<std::atomic<bool>>& cancelled,
    std::wstring& error, DWORD timeoutMs) noexcept {
    if (!cancelled) return true;
    CancelPostedGuiThreadWork(cancelled);
    try {
        std::vector<std::shared_ptr<Work>> running;
        {
            auto& registry = WorkRegistry();
            std::scoped_lock lock(registry.mutex);
            for (const auto& [_, work] : registry.pending) {
                if (work->posted && work->running && work->cancelled == cancelled)
                    running.push_back(work);
            }
        }
        return DrainRunningWork(running, error, timeoutMs);
    } catch (...) {
        error = L"native GUI rollback drain failed";
        return false;
    }
}

bool CancelAndDrainGuiThreadWork(HWND sourceRoot, std::wstring& error, DWORD timeoutMs) noexcept {
    if (!sourceRoot) {
        error = L"native GUI rollback has no source root";
        return false;
    }
    try {
        std::vector<std::shared_ptr<Work>> running;
        std::vector<std::shared_ptr<Work>> retired;
        {
            auto& registry = WorkRegistry();
            std::scoped_lock lock(registry.mutex);
            for (auto iterator = registry.pending.begin(); iterator != registry.pending.end();) {
                const auto& work = iterator->second;
                if (work->sourceRoot != sourceRoot) { ++iterator; continue; }
                work->cancelled->store(true, std::memory_order_release);
                if (work->running) {
                    running.push_back(work);
                    ++iterator;
                } else {
                    retired.push_back(std::move(iterator->second));
                    iterator = registry.pending.erase(iterator);
                }
            }
        }
        retired.clear();
        return DrainRunningWork(running, error, timeoutMs);
    } catch (...) {
        error = L"native GUI surface rollback drain failed";
        return false;
    }
}
} // namespace FluentShell::Bridge::Translation
