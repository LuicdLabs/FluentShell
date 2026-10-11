#pragma once

// Include after Check() in main.cpp's test namespace, with GuiThreadDispatch.h
// included at file scope. Every dispatched callback owns its fixture state.
constexpr UINT kGuiDispatchBlock = WM_APP + 431;
constexpr UINT kGuiDispatchDrain = WM_APP + 432;
constexpr UINT kGuiDispatchNotify = WM_APP + 433;

struct GuiDispatchRuntime final {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE blocked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE drained = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE actionEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE actionRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE actionDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<HWND> window{nullptr};
    std::atomic<DWORD> thread{0};
    std::atomic<bool> stopping{false};
    std::atomic<bool> expired{false};
    std::atomic<bool> active{false};
    std::atomic<bool> nestedAccepted{false};
    std::atomic<bool> ranInsideBoundedScope{false};
    std::atomic<unsigned> calls{0};
    std::atomic<DWORD> callbackThread{0};
    std::atomic<LRESULT> notificationResult{0};

    bool EventsReady() const {
        return ready && blocked && release && drained && actionEntered && actionRelease && actionDone;
    }
    ~GuiDispatchRuntime() {
        for (const HANDLE event : {ready, blocked, release, drained, actionEntered, actionRelease, actionDone})
            if (event) CloseHandle(event);
    }
};

LRESULT CALLBACK GuiDispatchFixtureSubclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    auto& runtime = *reinterpret_cast<GuiDispatchRuntime*>(data);
    if (message == kGuiDispatchBlock) {
        SetEvent(runtime.blocked);
        if (WaitForSingleObject(runtime.release, 5000) != WAIT_OBJECT_0) runtime.expired.store(true);
        return 0;
    }
    if (message == kGuiDispatchDrain) {
        SetEvent(runtime.drained);
        return 0;
    }
    if (message == kGuiDispatchNotify) {
        runtime.calls.fetch_add(1);
        runtime.callbackThread.store(GetCurrentThreadId());
        return 0x51A7;
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, GuiDispatchFixtureSubclass, subclassId);
    return result;
}

struct GuiDispatchFixture final {
    std::shared_ptr<GuiDispatchRuntime> runtime = std::make_shared<GuiDispatchRuntime>();
    std::thread gui;

    GuiDispatchFixture() {
        if (!runtime->EventsReady()) return;
        gui = std::thread([state = runtime] {
            state->thread.store(GetCurrentThreadId());
            const HWND window = CreateWindowExW(0, L"Static", L"owner-thread-dispatch",
                WS_OVERLAPPEDWINDOW, 30, 30, 200, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (window) SetWindowSubclass(window, GuiDispatchFixtureSubclass, 0xAB81,
                reinterpret_cast<DWORD_PTR>(state.get()));
            state->window.store(window);
            SetEvent(state->ready);
            MSG message{};
            while (!state->stopping.load() && GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (window) DestroyWindow(window);
            state->window.store(nullptr);
        });
    }
    bool Ready() const {
        return gui.joinable() && WaitForSingleObject(runtime->ready, 3000) == WAIT_OBJECT_0 && runtime->window.load();
    }
    bool Block() const {
        ResetEvent(runtime->blocked);
        ResetEvent(runtime->release);
        return PostMessageW(runtime->window.load(), kGuiDispatchBlock, 0, 0) &&
            WaitForSingleObject(runtime->blocked, 2000) == WAIT_OBJECT_0;
    }
    bool Drain() const {
        ResetEvent(runtime->drained);
        SetEvent(runtime->release);
        return PostMessageW(runtime->window.load(), kGuiDispatchDrain, 0, 0) &&
            WaitForSingleObject(runtime->drained, 2000) == WAIT_OBJECT_0;
    }
    void Stop() {
        if (!gui.joinable()) return;
        runtime->stopping.store(true);
        SetEvent(runtime->release);
        SetEvent(runtime->actionRelease);
        PostThreadMessageW(runtime->thread.load(), WM_QUIT, 0, 0);
        gui.join();
    }
    ~GuiDispatchFixture() { Stop(); }
};

void PumpGuiDispatchAction(const std::shared_ptr<GuiDispatchRuntime>& runtime) {
    runtime->callbackThread.store(GetCurrentThreadId());
    runtime->active.store(FluentShell::Bridge::Translation::IsGuiThreadDispatchActive());
    SetEvent(runtime->actionEntered);
    const auto until = GetTickCount64() + 5000;
    while (WaitForSingleObject(runtime->actionRelease, 0) == WAIT_TIMEOUT) {
        if (GetTickCount64() >= until) {
            runtime->expired.store(true);
            break;
        }
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        } else {
            MsgWaitForMultipleObjectsEx(1, &runtime->actionRelease, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
    SetEvent(runtime->actionDone);
}

bool WaitForGuiDispatchRelease(const std::weak_ptr<int>& lifetime) {
    const auto until = GetTickCount64() + 2000;
    while (!lifetime.expired() && GetTickCount64() < until) Sleep(10);
    return lifetime.expired();
}

DWORD WINAPI GuiDispatchExitThreadFixture(void* context) {
    auto& runtime = *static_cast<GuiDispatchRuntime*>(context);
    runtime.thread.store(GetCurrentThreadId());
    const HWND window = CreateWindowExW(0, L"Static", L"native-exit-thread-dispatch",
        WS_OVERLAPPEDWINDOW, 30, 30, 200, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    runtime.window.store(window);
    SetEvent(runtime.ready);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (window) DestroyWindow(window);
    return 0;
}

void TestGuiDispatchNativeThreadExit() {
    namespace Dispatch = FluentShell::Bridge::Translation;
    // Use a Win32 thread entry without a captured C++ owner: this fixture
    // intentionally exits inside application code without unwinding its stack.
    GuiDispatchRuntime runtime;
    const HANDLE thread = runtime.EventsReady()
        ? CreateThread(nullptr, 0, GuiDispatchExitThreadFixture, &runtime, 0, nullptr) : nullptr;
    Check(thread != nullptr, "native-exit dispatcher fixture could not create its GUI thread");
    if (!thread) return;
    const bool ready = WaitForSingleObject(runtime.ready, 3000) == WAIT_OBJECT_0 && runtime.window.load();
    Check(ready, "native-exit dispatcher fixture did not create its target window");
    if (ready) {
        auto lifetime = std::make_shared<int>(5);
        std::weak_ptr<int> weak = lifetime;
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        std::wstring error;
        const bool posted = Dispatch::PostToWindowThread(runtime.window.load(),
            [held = lifetime, entered = runtime.actionEntered] {
                SetEvent(entered);
                ExitThread(0);
            }, cancelled, error);
        lifetime.reset();
        Check(posted && WaitForSingleObject(runtime.actionEntered, 2000) == WAIT_OBJECT_0 &&
                WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0 && WaitForGuiDispatchRelease(weak),
            "application ExitThread retained the running dispatcher callback's closure state");
    }
    PostThreadMessageW(runtime.thread.load(), WM_QUIT, 0, 0);
    WaitForSingleObject(thread, 3000);
    CloseHandle(thread);
}

void TestGuiThreadDispatch() {
    namespace Dispatch = FluentShell::Bridge::Translation;
    GuiDispatchFixture fixture;
    Check(fixture.Ready(), "GUI dispatcher owner thread did not start");
    if (!fixture.Ready()) return;
    const auto runtime = fixture.runtime;
    const HWND window = runtime->window.load();
    std::wstring error;

    // A control on the receiver synchronously notifies the originating GUI
    // thread. SMTO_BLOCK would prevent that GUI thread from answering it.
    auto parentState = std::make_shared<GuiDispatchRuntime>();
    const HWND parent = CreateWindowExW(0, L"Static", L"dispatch-notification-parent",
        WS_OVERLAPPEDWINDOW, 40, 40, 200, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(parent != nullptr, "GUI dispatcher notification parent was not created");
    if (parent) {
        SetWindowSubclass(parent, GuiDispatchFixtureSubclass, 0xAB82,
            reinterpret_cast<DWORD_PTR>(parentState.get()));
        const DWORD caller = GetCurrentThreadId();
        Check(Dispatch::DispatchToWindowThread(window, [runtime, parent] {
            runtime->callbackThread.store(GetCurrentThreadId());
            runtime->active.store(Dispatch::IsGuiThreadDispatchActive());
            runtime->notificationResult.store(SendMessageW(parent, kGuiDispatchNotify, 0, 0));
        }, error, 1000), "GUI dispatcher deadlocked on a synchronous parent notification");
        Check(runtime->callbackThread.load() == runtime->thread.load() && runtime->active.load() &&
                runtime->notificationResult.load() == 0x51A7 && parentState->calls.load() == 1 &&
                parentState->callbackThread.load() == caller,
            "GUI dispatcher did not execute and notify on each window's owning thread");
        DestroyWindow(parent);
    }

    // The target cannot receive this token until after the caller's shared
    // deadline. Removing its token must also release all queued closure state.
    Check(fixture.Block(), "GUI dispatcher timeout fixture did not block");
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    auto lifetime = std::make_shared<int>(1);
    std::weak_ptr<int> weak = lifetime;
    runtime->calls.store(0);
    const auto started = GetTickCount64();
    bool accepted = false;
    {
        Dispatch::GuiThreadOperationScope scope(cancelled, started + 80);
        accepted = Dispatch::DispatchToWindowThread(window, [runtime, held = lifetime] {
            runtime->calls.fetch_add(1);
        }, error, 2000);
    }
    lifetime.reset();
    Check(!accepted && cancelled->load() && GetTickCount64() - started < 1500,
        "GUI dispatcher did not share the caller's shorter cancellation deadline");
    Check(weak.expired(), "timed-out queued GUI callback retained its closure state");
    Check(fixture.Drain() && runtime->calls.load() == 0,
        "timed-out queued GUI callback executed after its caller returned");

    // A callback that has already started owns its state after timeout and sees
    // cancellation before any subsequent native mutation.
    ResetEvent(runtime->actionEntered);
    ResetEvent(runtime->actionRelease);
    ResetEvent(runtime->actionDone);
    lifetime = std::make_shared<int>(2);
    weak = lifetime;
    const bool runningAccepted = Dispatch::DispatchToWindowThread(window, [runtime, held = lifetime] {
        SetEvent(runtime->actionEntered);
        if (WaitForSingleObject(runtime->actionRelease, 5000) != WAIT_OBJECT_0) runtime->expired.store(true);
        if (!Dispatch::GuiThreadOperationCancelled()) runtime->calls.fetch_add(1);
        SetEvent(runtime->actionDone);
    }, error, 80);
    lifetime.reset();
    Check(!runningAccepted && WaitForSingleObject(runtime->actionEntered, 0) == WAIT_OBJECT_0 && !weak.expired(),
        "GUI dispatcher lost running callback state when its sender timed out");
    Check(!Dispatch::CancelAndDrainGuiThreadWork(window, error, 30),
        "surface rollback lost track of a synchronous callback after its sender timed out");
    SetEvent(runtime->actionRelease);
    Check(WaitForSingleObject(runtime->actionDone, 2000) == WAIT_OBJECT_0 &&
            Dispatch::CancelAndDrainGuiThreadWork(window, error) &&
            WaitForGuiDispatchRelease(weak) && runtime->calls.load() == 0,
        "running GUI callback did not observe cancellation or release its state");

    // Posting acknowledges only the queue operation. The receiver may remain in
    // an application modal pump while the caller continues issuing bounded work.
    ResetEvent(runtime->actionEntered);
    ResetEvent(runtime->actionRelease);
    ResetEvent(runtime->actionDone);
    cancelled = std::make_shared<std::atomic<bool>>(false);
    const auto postStarted = GetTickCount64();
    const bool posted = Dispatch::PostToWindowThread(window, [runtime] { PumpGuiDispatchAction(runtime); }, cancelled, error);
    Check(posted && GetTickCount64() - postStarted < 1000 &&
            WaitForSingleObject(runtime->actionEntered, 2000) == WAIT_OBJECT_0,
        "posted GUI action blocked its sender or did not enter its native modal pump");
    Check(!runtime->active.load() && runtime->callbackThread.load() == runtime->thread.load(),
        "posted GUI action ran as a bounded command or on the wrong GUI thread");
    Check(Dispatch::DispatchToWindowThread(window, [runtime] { runtime->calls.fetch_add(1); }, error, 1000),
        "posted GUI action's modal pump did not admit synchronous owner-thread reads");
    Check(!Dispatch::CancelAndDrainPostedGuiThreadWork(cancelled, error, 50),
        "GUI rollback drain passed while a native modal callback was still running");
    SetEvent(runtime->actionRelease);
    Check(WaitForSingleObject(runtime->actionDone, 2000) == WAIT_OBJECT_0,
        "posted GUI action did not leave its native modal pump");
    Check(Dispatch::CancelAndDrainPostedGuiThreadWork(cancelled, error),
        "GUI rollback drain rejected a completed native callback");

    // Pause after dispatch starts but before the native mutation. Cancellation
    // must be observed when it resumes, and rollback waits for that return.
    ResetEvent(runtime->actionEntered);
    ResetEvent(runtime->actionRelease);
    runtime->calls.store(0);
    cancelled = std::make_shared<std::atomic<bool>>(false);
    Check(Dispatch::PostToWindowThread(window, [runtime] {
        SetEvent(runtime->actionEntered);
        if (WaitForSingleObject(runtime->actionRelease, 5000) != WAIT_OBJECT_0) runtime->expired.store(true);
        if (!Dispatch::GuiThreadOperationCancelled()) runtime->calls.fetch_add(1);
    }, cancelled, error) && WaitForSingleObject(runtime->actionEntered, 2000) == WAIT_OBJECT_0,
        "GUI rollback race fixture did not pause before its native mutation");
    std::thread releaseAfterCancel([runtime, cancelled] {
        const auto until = GetTickCount64() + 2000;
        while (!cancelled->load() && GetTickCount64() < until) Sleep(1);
        SetEvent(runtime->actionRelease);
    });
    Check(Dispatch::CancelAndDrainPostedGuiThreadWork(cancelled, error) && runtime->calls.load() == 0,
        "GUI rollback completed before its cancelled native callback returned");
    releaseAfterCancel.join();

    // A nested pump inside a bounded callback may remove the queued message,
    // but it must not execute the native action until the command scope exits.
    ResetEvent(runtime->actionDone);
    runtime->calls.store(0);
    cancelled = std::make_shared<std::atomic<bool>>(false);
    Check(Dispatch::DispatchToWindowThread(window, [runtime, cancelled, window] {
        std::wstring nestedError;
        runtime->nestedAccepted.store(Dispatch::PostToWindowThread(window, [runtime] {
            runtime->ranInsideBoundedScope.store(Dispatch::IsGuiThreadDispatchActive());
            runtime->calls.fetch_add(1);
            SetEvent(runtime->actionDone);
        }, cancelled, nestedError));
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (runtime->calls.load()) runtime->ranInsideBoundedScope.store(true);
    }, error, 1000) && runtime->nestedAccepted.load(), "GUI action could not be queued from a bounded command");
    Check(WaitForSingleObject(runtime->actionDone, 2000) == WAIT_OBJECT_0 && runtime->calls.load() == 1 &&
            !runtime->ranInsideBoundedScope.load(),
        "nested GUI pump executed a posted native action before its bounded scope exited");
    Dispatch::CancelPostedGuiThreadWork(cancelled);

    // Explicit cancellation releases immediately; a directly set token is also
    // reclaimed without needing the target's message loop to make progress.
    for (const bool explicitCancel : {true, false}) {
        Check(fixture.Block(), "GUI dispatcher cancellation fixture did not block");
        runtime->calls.store(0);
        cancelled = std::make_shared<std::atomic<bool>>(false);
        lifetime = std::make_shared<int>(3);
        weak = lifetime;
        Check(Dispatch::PostToWindowThread(window, [runtime, held = lifetime] {
            runtime->calls.fetch_add(1);
        }, cancelled, error), "GUI dispatcher cancellation callback did not queue");
        lifetime.reset();
        if (explicitCancel) Dispatch::CancelPostedGuiThreadWork(cancelled);
        else cancelled->store(true);
        Check(WaitForGuiDispatchRelease(weak),
            "cancelled posted GUI callback retained its closure while its thread was blocked");
        Check(fixture.Drain() && runtime->calls.load() == 0,
            "cancelled posted GUI callback executed when its GUI thread resumed");
        Check(!Dispatch::PostToWindowThread(window, [] {}, cancelled, error),
            "GUI dispatcher accepted new work for a cancelled token");
    }

    // Destroying a target discards its posted messages even while the owner
    // thread stays alive. Immediate handle reuse must not retain that closure.
    runtime->calls.store(0);
    cancelled = std::make_shared<std::atomic<bool>>(false);
    lifetime = std::make_shared<int>(4);
    weak = lifetime;
    auto targetRecreated = std::make_shared<HWND>(nullptr);
    auto targetQueued = std::make_shared<bool>(false);
    Check(Dispatch::DispatchToWindowThread(window, [runtime, window, cancelled, targetRecreated,
            targetQueued, held = lifetime] {
        const HWND target = CreateWindowExW(0, L"Static", L"retired dispatch target", WS_CHILD,
            0, 0, 10, 10, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        std::wstring queueError;
        if (target) *targetQueued = Dispatch::PostToWindowThread(target, [runtime, held] {
            runtime->calls.fetch_add(1);
        }, cancelled, queueError);
        if (target) DestroyWindow(target);
        *targetRecreated = CreateWindowExW(0, L"Static", L"replacement dispatch target", WS_CHILD,
            0, 0, 10, 10, window, nullptr, GetModuleHandleW(nullptr), nullptr);
    }, error) && *targetQueued, "GUI dispatcher target-retirement callback did not queue");
    lifetime.reset();
    Check(WaitForGuiDispatchRelease(weak) && runtime->calls.load() == 0,
        "destroying a queued target retained its callback or invoked its replacement HWND");
    Dispatch::DispatchToWindowThread(window, [targetRecreated] {
        if (*targetRecreated) DestroyWindow(*targetRecreated);
    }, error);

    // A thread can exit without dequeuing a pending action. The thread handle
    // watch must reclaim both its callback state and hook without a later post.
    Check(fixture.Block(), "GUI dispatcher thread-exit fixture did not block");
    cancelled = std::make_shared<std::atomic<bool>>(false);
    lifetime = std::make_shared<int>(4);
    weak = lifetime;
    Check(Dispatch::PostToWindowThread(window, [runtime, held = lifetime] {
        runtime->calls.fetch_add(1);
    }, cancelled, error), "GUI dispatcher thread-exit callback did not queue");
    lifetime.reset();
    fixture.Stop();
    Check(WaitForGuiDispatchRelease(weak) && runtime->calls.load() == 0,
        "GUI dispatcher retained or invoked queued work after its owner thread exited");
    Check(!runtime->expired.load(), "GUI dispatcher fixture exceeded a native callback deadline");
    TestGuiDispatchNativeThreadExit();
}
