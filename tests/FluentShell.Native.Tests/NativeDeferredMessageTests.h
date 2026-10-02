#pragma once

// Included after SourceThreadStabilityRegressionTests.h in main.cpp's namespace.
constexpr UINT kNativeDeferredProbe = WM_APP + 381;
constexpr UINT kNativeDeferredSentinel = WM_APP + 382;
enum class NativeDeferredProbe { Cancel, Replay, Disable, Replace, Uncloak, CancelClose };

struct NativeDeferredRuntime final {
    SourceLifetimeRuntime gate;
    TestIslandPopupRuntime islandProvider;
    SourceLifetimeAccessible* provider = nullptr;
    std::atomic<Translation::SourceThreadAgent*> agent{nullptr};
    HWND button = nullptr;
    HWND toolbar = nullptr;
    HWND link = nullptr;
    std::atomic<bool> armPump{false};
    std::atomic<bool> providerReading{false};
    std::atomic<bool> providerPumped{false};
    std::atomic<bool> providerDrained{false};
    std::atomic<bool> failAfterPump{false};
    std::atomic<bool> waitAfterPump{false};
    std::atomic<bool> modal{false};
    std::atomic<bool> invokedDuringRead{false};
    std::atomic<unsigned> calls{0};
    Translation::ActionRequest action;
    Translation::ControlNode node;
    bool queued = false;
    bool secondRefused = false;
    bool replayPosted = false;
    uint64_t closeSequence = 0;
};

void NativeDeferredProviderRead(void* context) {
    auto& state = *static_cast<NativeDeferredRuntime*>(context);
    auto* agent = state.agent.load();
    if (!agent || !state.armPump.load()) return;
    MSG queued{};
    const bool owned = PeekMessageW(&queued, nullptr, agent->MessageId(), agent->MessageId(), PM_NOREMOVE) && queued.wParam != 0;
    const bool raw = PeekMessageW(&queued, nullptr, BM_CLICK, BM_CLICK, PM_NOREMOVE) ||
        PeekMessageW(&queued, nullptr, WM_CLOSE, WM_CLOSE, PM_NOREMOVE) ||
        PeekMessageW(&queued, nullptr, WM_COMMAND, WM_COMMAND, PM_NOREMOVE) ||
        PeekMessageW(&queued, nullptr, WM_KEYDOWN, WM_KEYDOWN, PM_NOREMOVE);
    if (!owned && !raw) return;
    state.armPump.store(false);
    state.providerReading.store(true);
    state.providerPumped.store(true);
    MSG message{};
    unsigned drained = 0;
    while (drained++ < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    state.providerDrained.store(!PeekMessageW(&queued, nullptr, agent->MessageId(), agent->MessageId(), PM_NOREMOVE));
    if (state.failAfterPump.exchange(false)) state.provider->Metadata().names[0].clear();
    if (state.waitAfterPump.exchange(false)) PumpSourceLifetime(state.gate);
    state.providerReading.store(false);
}

void NativeDeferredInvoked(NativeDeferredRuntime& state) {
    state.calls.fetch_add(1);
    if (state.providerReading.load()) state.invokedDuringRead.store(true);
    SetEvent(state.gate.entered);
    if (state.modal.load()) PumpSourceLifetime(state.gate);
}

LRESULT CALLBACK NativeDeferredRootSubclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    auto& state = *reinterpret_cast<NativeDeferredRuntime*>(data);
    if (message == WM_CLOSE || (message == WM_COMMAND &&
        (LOWORD(wParam) == 2001 || LOWORD(wParam) == 100 || LOWORD(wParam) == 3001))) {
        NativeDeferredInvoked(state);
        return 0; // Veto close so its completed sequence can also be checked.
    }
    if (message == WM_NOTIFY && lParam) {
        const auto* notice = reinterpret_cast<const NMHDR*>(lParam);
        if (notice->hwndFrom == state.link && notice->code == NM_RETURN) {
            NativeDeferredInvoked(state);
            return 0;
        }
    }
    if (message == kNativeDeferredProbe) {
        auto* agent = state.agent.load();
        bool refused = false;
        std::wstring error;
        const bool close = static_cast<NativeDeferredProbe>(wParam) == NativeDeferredProbe::CancelClose;
        auto action = state.action;
        if (close) { action.action = L"close"; action.nodeId.reset(); }
        state.queued = agent && agent->PostNativeAction(action, close ? nullptr : &state.node,
            refused, error, nullptr, &state.closeSequence) && !refused;
        if (state.queued) {
            switch (static_cast<NativeDeferredProbe>(wParam)) {
            case NativeDeferredProbe::Cancel:
            case NativeDeferredProbe::CancelClose: agent->CancelPopupOnSourceThread(); break;
            case NativeDeferredProbe::Replay: {
                bool secondRefused = false;
                state.secondRefused = !agent->PostNativeAction(action, &state.node, secondRefused, error) && secondRefused;
                MSG queued{};
                if (PeekMessageW(&queued, nullptr, agent->MessageId(), agent->MessageId(), PM_NOREMOVE))
                    state.replayPosted = PostThreadMessageW(agent->ThreadId(), queued.message, queued.wParam, queued.lParam) != FALSE;
                break;
            }
            case NativeDeferredProbe::Disable: EnableWindow(state.button, FALSE); break;
            case NativeDeferredProbe::Replace:
                DestroyWindow(state.button);
                state.button = CreateWindowExW(0, L"Button", L"Properties", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    8, 8, 100, 28, window, reinterpret_cast<HMENU>(2001), GetModuleHandleW(nullptr), nullptr);
                break;
            case NativeDeferredProbe::Uncloak: {
                const BOOL cloak = FALSE;
                DwmSetWindowAttribute(window, DWMWA_CLOAK, &cloak, sizeof(cloak));
                break;
            }
            }
        }
        PostMessageW(window, kNativeDeferredSentinel, 0, 0);
        return 0;
    }
    if (message == kNativeDeferredSentinel) {
        EnableWindow(state.button, TRUE);
        state.provider->Metadata().names[0] = L"Actions";
        SetEvent(state.gate.drained);
        return 0;
    }
    const auto result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, NativeDeferredRootSubclass, subclassId);
    return result;
}

void TestNativeDeferredMessages() {
    NativeDeferredRuntime runtime;
    std::thread gui([&] {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        runtime.gate.thread.store(GetCurrentThreadId());
        WNDCLASSW islandClass{};
        islandClass.lpfnWndProc = DefWindowProcW;
        islandClass.hInstance = GetModuleHandleW(nullptr);
        islandClass.lpszClassName = L"DirectUIHWND";
        const ATOM registered = RegisterClassW(&islandClass);
        const HWND root = CreateWindowExW(0, L"Static", L"deferred-native-message",
            WS_OVERLAPPEDWINDOW, 40, 40, 400, 280, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        const HWND island = root ? CreateWindowExW(0, L"DirectUIHWND", L"Actions",
            WS_CHILD | WS_VISIBLE, 8, 90, 300, 80, root, reinterpret_cast<HMENU>(2003), GetModuleHandleW(nullptr), nullptr) : nullptr;
        runtime.button = root ? CreateWindowExW(0, L"Button", L"Properties", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            8, 8, 100, 28, root, reinterpret_cast<HMENU>(2001), GetModuleHandleW(nullptr), nullptr) : nullptr;
        runtime.link = root ? CreateWindowExW(0, WC_LINK, L"<a>Details</a>", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            130, 8, 130, 28, root, reinterpret_cast<HMENU>(2002), GetModuleHandleW(nullptr), nullptr) : nullptr;
        runtime.toolbar = root ? CreateWindowExW(0, TOOLBARCLASSNAMEW, L"",
            WS_CHILD | WS_VISIBLE | CCS_NOPARENTALIGN | CCS_NORESIZE, 8, 45, 300, 30,
            root, reinterpret_cast<HMENU>(2004), GetModuleHandleW(nullptr), nullptr) : nullptr;
        if (root && island && runtime.button && runtime.link && runtime.toolbar) {
            InitializeSourceTestMenuToolbar(runtime.toolbar);
            runtime.provider = new SourceLifetimeAccessible(runtime.gate, island);
            runtime.provider->Metadata().islandRuntime = &runtime.islandProvider;
            runtime.provider->Metadata().names = {L"Actions"};
            runtime.provider->onRead = NativeDeferredProviderRead;
            runtime.provider->readContext = &runtime;
            SetWindowSubclass(island, SourceLifetimeObjectSubclass, 0xAB81,
                reinterpret_cast<DWORD_PTR>(static_cast<IAccessible*>(runtime.provider)));
            SetWindowSubclass(root, NativeDeferredRootSubclass, 0xAB82, reinterpret_cast<DWORD_PTR>(&runtime));
            const HMENU bar = CreateMenu();
            const HMENU file = CreatePopupMenu();
            AppendMenuW(file, MF_STRING, 3001, L"Properties");
            AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
            SetMenu(root, bar);
            ShowWindow(root, SW_SHOWNOACTIVATE);
            runtime.gate.root.store(root);
        }
        SetEvent(runtime.gate.ready);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (root) DestroyWindow(root);
        if (runtime.provider) runtime.provider->Release();
        if (registered) UnregisterClassW(L"DirectUIHWND", GetModuleHandleW(nullptr));
        winrt::uninit_apartment();
    });
    std::shared_ptr<Translation::SourceThreadAgent> agent;
    const auto cleanup = [&] {
        SetEvent(runtime.gate.release);
        if (agent) {
            std::wstring ignored;
            agent->Restore(ignored);
            if (!agent->Shutdown()) Translation::RetainSourceThreadAgent(agent);
        }
        if (runtime.gate.thread.load()) PostThreadMessageW(runtime.gate.thread.load(), WM_QUIT, 0, 0);
        gui.join();
    };
    const bool ready = WaitForSingleObject(runtime.gate.ready, 5000) == WAIT_OBJECT_0 && runtime.gate.root.load();
    Check(ready, "deferred native message fixture did not start");
    if (!ready) { cleanup(); return; }
    agent = Translation::SourceThreadAgent::Attach(runtime.gate.root.load(), GetModuleHandleW(nullptr));
    std::wstring error;
    const bool attached = agent && agent->SetCloaked(true, error);
    Check(attached, "deferred native message agent could not attach/cloak");
    if (!attached) { cleanup(); return; }
    runtime.agent.store(agent.get());
    Translation::WindowSnapshot snapshot;
    snapshot.surfaceId = L"96969696-3434-5656-7878-909090909090";
    snapshot.revision = 1;
    const auto captureButton = [&] {
        if (!agent->Capture(snapshot, error)) return false;
        const auto button = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(), [&](const auto& node) {
            return node.hwnd == runtime.button;
        });
        if (button == snapshot.nodes.end()) return false;
        runtime.node = *button;
        runtime.action = {};
        runtime.action.surfaceId = snapshot.surfaceId;
        runtime.action.expectedRevision = snapshot.revision;
        runtime.action.eventId = 1;
        runtime.action.action = L"invoke";
        runtime.action.nodeId = button->nodeId;
        return true;
    };
    const auto drain = [&] {
        ResetEvent(runtime.gate.drained);
        return PostMessageW(runtime.gate.root.load(), kNativeDeferredSentinel, 0, 0) &&
            WaitForSingleObject(runtime.gate.drained, 3000) == WAIT_OBJECT_0;
    };
    for (const auto scenario : {NativeDeferredProbe::Cancel, NativeDeferredProbe::Replay,
            NativeDeferredProbe::Disable, NativeDeferredProbe::Replace, NativeDeferredProbe::Uncloak,
            NativeDeferredProbe::CancelClose}) {
        Check(captureButton(), "deferred native message source did not capture before queue probe");
        runtime.calls.store(0);
        runtime.queued = runtime.secondRefused = runtime.replayPosted = false;
        runtime.closeSequence = 0;
        ResetEvent(runtime.gate.drained);
        const bool done = PostMessageW(runtime.gate.root.load(), kNativeDeferredProbe, static_cast<WPARAM>(scenario), 0) &&
            WaitForSingleObject(runtime.gate.drained, 3000) == WAIT_OBJECT_0;
        Check(done && runtime.queued, "deferred native action did not queue/drain on its source thread");
        if (scenario == NativeDeferredProbe::Replay)
            Check(runtime.calls.load() == 1 && runtime.secondRefused && runtime.replayPosted,
                "duplicate or replayed native message did not run exactly once");
        else Check(runtime.calls.load() == 0, "cancelled, stale, disabled or restored native action still ran");
        if (scenario == NativeDeferredProbe::CancelClose)
            Check(runtime.closeSequence != 0 && agent->CompletedCloseSequence() >= runtime.closeSequence,
                "cancelled native close left the renderer close lifetime pending");
        if (scenario == NativeDeferredProbe::Uncloak)
            Check(agent->SetCloaked(true, error), "native message fixture could not recloak after rollback probe");
    }
    for (const bool timeout : {false, true}) {
        Check(captureButton(), "deferred native action did not recapture before failed command");
        runtime.calls.store(0);
        runtime.armPump.store(true);
        runtime.providerPumped.store(false);
        runtime.providerDrained.store(false);
        runtime.failAfterPump.store(!timeout);
        runtime.waitAfterPump.store(timeout);
        ResetEvent(runtime.gate.release);
        Translation::ActionOutcome outcome;
        Check(!agent->Invoke(runtime.action, outcome, timeout ? 150 : 2000),
            "failed or timed-out post-enqueue capture accepted its native action");
        SetEvent(runtime.gate.release);
        Check(drain() && runtime.providerPumped.load() && runtime.providerDrained.load() && runtime.calls.load() == 0,
            "failed or timed-out command left a native message runnable after cancellation");
    }
    for (const std::wstring actionName : {L"button", L"link", L"toolbar", L"menu", L"close"}) {
        Check(captureButton(), "deferred native action did not capture before modal invocation");
        auto action = runtime.action;
        if (actionName == L"close" || actionName == L"menu") {
            action.nodeId.reset();
            action.action = actionName == L"close" ? L"close" : L"menuCommand";
            action.menuCommandId = 3001;
            action.expectedMenuBindingGeneration = snapshot.menuBindingGeneration;
        } else if (actionName != L"button") {
            const HWND target = actionName == L"link" ? runtime.link : runtime.toolbar;
            const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(), [&](const auto& item) { return item.hwnd == target; });
            Check(node != snapshot.nodes.end(), "modal native action target was not captured");
            if (node == snapshot.nodes.end()) continue;
            action.nodeId = node->nodeId;
            if (actionName == L"toolbar") { action.action = L"toolbarCommand"; action.menuCommandId = 100; }
        }
        runtime.calls.store(0);
        runtime.providerPumped.store(false);
        runtime.providerDrained.store(false);
        runtime.invokedDuringRead.store(false);
        runtime.armPump.store(true);
        runtime.modal.store(true);
        ResetEvent(runtime.gate.entered);
        ResetEvent(runtime.gate.release);
        Translation::ActionOutcome outcome;
        const bool accepted = agent->Invoke(action, outcome, 2000) && outcome.accepted;
        const bool entered = WaitForSingleObject(runtime.gate.entered, 2000) == WAIT_OBJECT_0;
        if (!accepted || !entered) std::wcerr << L"Deferred native modal " << actionName << L": " << outcome.error << L'\n';
        Check(accepted && entered && runtime.providerPumped.load() && runtime.providerDrained.load() &&
                !runtime.invokedDuringRead.load(), "native modal action ran inside its bounded post-action capture");
        if (entered) Check(agent->Capture(snapshot, error), "deferred native modal action blocked source capture");
        SetEvent(runtime.gate.release);
        Check(drain() && runtime.calls.load() == 1, "native modal action did not complete exactly once");
        if (actionName == L"close")
            Check(outcome.closeSequence != 0 && agent->CompletedCloseSequence() >= outcome.closeSequence,
                "vetoed native close did not complete its acknowledged sequence");
        runtime.modal.store(false);
    }
    Check(!runtime.invokedDuringRead.load() && !runtime.gate.pumpTimedOut.load(),
        "deferred native message fixture crossed its bounded/provider lifetime");
    cleanup();
}
