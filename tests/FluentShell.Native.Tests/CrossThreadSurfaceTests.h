#pragma once

// Include in main.cpp's test namespace after Check(). Both the root and its
// real native child controls have independent GUI threads and message loops.
constexpr wchar_t kCrossThreadEditClass[] = L"WindowsForms10.EDIT.app.0.f17e.0";
constexpr wchar_t kCrossThreadButtonClass[] = L"WindowsForms10.BUTTON.app.0.f17e.0";
constexpr UINT kCrossThreadBoundedPump = WM_APP + 445;

struct CrossThreadSurfaceRuntime final {
    HANDLE rootReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE childReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE clicked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE clickEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE clickRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE boundedEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE boundedRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE boundedDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<HWND> root{nullptr};
    std::atomic<HWND> edit{nullptr};
    std::atomic<HWND> button{nullptr};
    std::atomic<DWORD> rootThread{0};
    std::atomic<DWORD> childThread{0};
    std::atomic<DWORD> textThread{0};
    std::atomic<DWORD> clickThread{0};
    std::atomic<DWORD> notifyThread{0};
    std::atomic<unsigned> notifications{0};
    std::atomic<unsigned> textNotifications{0};
    std::atomic<bool> holdClick{false};
    std::atomic<bool> expired{false};
    bool EventsReady() const {
        return rootReady && childReady && clicked && clickEntered && clickRelease &&
            boundedEntered && boundedRelease && boundedDone;
    }
    ~CrossThreadSurfaceRuntime() {
        for (const HANDLE event : {rootReady, childReady, clicked, clickEntered, clickRelease,
                boundedEntered, boundedRelease, boundedDone})
            if (event) CloseHandle(event);
    }
};

void PumpCrossThreadSurface(CrossThreadSurfaceRuntime& state, HANDLE release) {
    const auto until = GetTickCount64() + 8000;
    while (WaitForSingleObject(release, 0) == WAIT_TIMEOUT) {
        if (GetTickCount64() >= until) { state.expired.store(true); break; }
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        } else {
            MsgWaitForMultipleObjectsEx(1, &release, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
}

LRESULT CALLBACK CrossThreadSurfaceSubclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    auto& state = *reinterpret_cast<CrossThreadSurfaceRuntime*>(data);
    if (message == WM_SETTEXT && window == state.edit.load()) state.textThread.store(GetCurrentThreadId());
    if (message == BM_CLICK && window == state.button.load()) {
        state.clickThread.store(GetCurrentThreadId());
        if (state.holdClick.load()) {
            SetEvent(state.clickEntered);
            PumpCrossThreadSurface(state, state.clickRelease);
        }
    }
    if (message == WM_COMMAND && window == state.root.load()) {
        if (HIWORD(wParam) == BN_CLICKED && reinterpret_cast<HWND>(lParam) == state.button.load()) {
            state.notifyThread.store(GetCurrentThreadId());
            state.notifications.fetch_add(1);
            SetEvent(state.clicked);
        } else if (HIWORD(wParam) == EN_CHANGE && reinterpret_cast<HWND>(lParam) == state.edit.load()) {
            state.textNotifications.fetch_add(1);
        }
    }
    if (message == kCrossThreadBoundedPump) {
        auto cancelled = std::make_shared<std::atomic<bool>>(false);
        {
            FluentShell::Bridge::Translation::GuiThreadOperationScope scope(cancelled, GetTickCount64() + 10000);
            SetEvent(state.boundedEntered);
            PumpCrossThreadSurface(state, state.boundedRelease);
        }
        SetEvent(state.boundedDone);
        return 0;
    }
    const auto result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, CrossThreadSurfaceSubclass, subclassId);
    return result;
}

bool RegisterCrossThreadNativeAlias(const wchar_t* original, const wchar_t* alias) {
    WNDCLASSEXW type{sizeof(type)};
    if (GetClassInfoExW(GetModuleHandleW(nullptr), alias, &type)) return true;
    type = {sizeof(type)};
    if (!GetClassInfoExW(nullptr, original, &type)) return false;
    type.style &= ~CS_GLOBALCLASS;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = alias;
    type.lpszMenuName = nullptr;
    return RegisterClassExW(&type) != 0;
}

struct CrossThreadSurfaceFixture final {
    std::shared_ptr<CrossThreadSurfaceRuntime> state = std::make_shared<CrossThreadSurfaceRuntime>();
    std::thread rootGui;
    std::thread childGui;
    CrossThreadSurfaceFixture() {
        if (!state->EventsReady() || !RegisterCrossThreadNativeAlias(L"Edit", kCrossThreadEditClass) ||
                !RegisterCrossThreadNativeAlias(L"Button", kCrossThreadButtonClass)) return;
        rootGui = std::thread([runtime = state] {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            runtime->rootThread.store(GetCurrentThreadId());
            const HWND root = CreateWindowExW(0, L"#32770", L"cross-thread native form",
                WS_OVERLAPPEDWINDOW, 40, 40, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            runtime->root.store(root);
            if (root) {
                SetWindowSubclass(root, CrossThreadSurfaceSubclass, 0xAB91, reinterpret_cast<DWORD_PTR>(runtime.get()));
                ShowWindow(root, SW_SHOWNOACTIVATE);
            }
            SetEvent(runtime->rootReady);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (root) DestroyWindow(root);
            runtime->root.store(nullptr);
            winrt::uninit_apartment();
        });
        if (WaitForSingleObject(state->rootReady, 3000) != WAIT_OBJECT_0 || !state->root.load()) return;
        childGui = std::thread([runtime = state] {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            runtime->childThread.store(GetCurrentThreadId());
            const HWND root = runtime->root.load();
            const HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, kCrossThreadEditClass, L"before",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12, 12, 180, 26,
                root, reinterpret_cast<HMENU>(41), GetModuleHandleW(nullptr), nullptr);
            const HWND button = CreateWindowExW(0, kCrossThreadButtonClass, L"Apply",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 12, 50, 100, 30,
                root, reinterpret_cast<HMENU>(42), GetModuleHandleW(nullptr), nullptr);
            runtime->edit.store(edit);
            runtime->button.store(button);
            for (const HWND window : {edit, button}) {
                if (window) SetWindowSubclass(window, CrossThreadSurfaceSubclass, 0xAB92,
                    reinterpret_cast<DWORD_PTR>(runtime.get()));
            }
            SetEvent(runtime->childReady);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (runtime->edit.load()) DestroyWindow(runtime->edit.load());
            if (button) DestroyWindow(button);
            winrt::uninit_apartment();
        });
    }
    bool Ready() const {
        return childGui.joinable() && WaitForSingleObject(state->childReady, 3000) == WAIT_OBJECT_0 &&
            state->edit.load() && state->button.load();
    }
    ~CrossThreadSurfaceFixture() {
        SetEvent(state->clickRelease);
        SetEvent(state->boundedRelease);
        if (childGui.joinable()) {
            PostThreadMessageW(state->childThread.load(), WM_QUIT, 0, 0);
            childGui.join();
        }
        if (rootGui.joinable()) {
            PostThreadMessageW(state->rootThread.load(), WM_QUIT, 0, 0);
            rootGui.join();
        }
    }
};

void TestCrossThreadSurface() {
    namespace Dispatch = FluentShell::Bridge::Translation;
    CrossThreadSurfaceFixture fixture;
    Check(fixture.Ready(), "cross-thread form failed to create native WinForms subclass controls");
    if (!fixture.Ready()) return;
    const auto state = fixture.state;
    auto agent = Dispatch::SourceThreadAgent::Attach(state->root.load(), GetModuleHandleW(nullptr));
    Check(agent != nullptr, "cross-thread source agent could not attach");
    if (!agent) return;
    const auto cleanup = [&] {
        SetEvent(state->clickRelease);
        SetEvent(state->boundedRelease);
        std::wstring ignored;
        agent->Restore(ignored);
        if (!agent->Shutdown()) Dispatch::RetainSourceThreadAgent(agent);
    };
    Dispatch::WindowSnapshot snapshot;
    snapshot.surfaceId = L"55555555-3131-4242-5353-646464646464";
    snapshot.revision = 1;
    std::wstring error;
    const bool captured = agent->Capture(snapshot, error);
    if (!captured) std::wcerr << L"cross-thread capture: " << error << L'\n';
    const auto find = [](const Dispatch::WindowSnapshot& value, HWND window) -> const Dispatch::ControlNode* {
        const auto found = std::find_if(value.nodes.begin(), value.nodes.end(),
            [window](const auto& node) { return node.hwnd == window; });
        return found == value.nodes.end() ? nullptr : &*found;
    };
    const auto edit = find(snapshot, state->edit.load());
    const auto button = find(snapshot, state->button.load());
    Check(captured && edit && button && edit->kind == Dispatch::ControlKind::Edit &&
            button->kind == Dispatch::ControlKind::Button && edit->generation != 0 && button->generation != 0,
        "cross-thread capture lost native control kinds or lifecycle generations");
    if (!captured || !edit || !button) { cleanup(); return; }
    const uint64_t editId = edit->nodeId;
    const uint64_t editGeneration = edit->generation;
    const uint64_t buttonId = button->nodeId;
    Check(agent->SetCloaked(true, error), "cross-thread form could not be cloaked");
    Dispatch::ActionRequest action;
    action.surfaceId = snapshot.surfaceId;
    action.expectedRevision = snapshot.revision;
    action.nodeId = editId;
    action.eventId = 1;
    action.action = L"setText";
    action.text = L"owner-thread text";
    Dispatch::ActionOutcome outcome;
    const bool changed = agent->Invoke(action, outcome);
    if (!changed) std::wcerr << L"cross-thread text action: " << outcome.error << L'\n';
    const auto changedEdit = find(outcome.snapshot, state->edit.load());
    Check(changed && outcome.accepted && changedEdit && changedEdit->text == action.text &&
            state->textThread.load() == state->childThread.load() && state->textNotifications.load() != 0,
        "cross-thread property action did not update the owning control and notify its parent");

    state->holdClick.store(true);
    action.nodeId = buttonId;
    action.action = L"invoke";
    action.text.clear();
    ++action.eventId;
    action.expectedRevision = outcome.revision ? outcome.revision : snapshot.revision;
    const auto invokeStart = GetTickCount64();
    Check(agent->Invoke(action, outcome) && outcome.accepted && GetTickCount64() - invokeStart < 1500,
        "cross-thread button invoke waited for the native application's modal callback");
    Check(WaitForSingleObject(state->clickEntered, 3000) == WAIT_OBJECT_0 &&
            state->clickThread.load() == state->childThread.load(),
        "cross-thread button callback did not run on the child's GUI thread");
    SetEvent(state->clickRelease);
    Check(WaitForSingleObject(state->clicked, 2000) == WAIT_OBJECT_0 && state->notifications.load() == 1 &&
            state->notifyThread.load() == state->rootThread.load(),
        "cross-thread button did not synchronously notify its real native parent exactly once");
    state->holdClick.store(false);

    // Hold the child in a bounded native pump so reads still complete but posted
    // application actions stay queued. Restore must retire that surface's token.
    Check(PostMessageW(state->button.load(), kCrossThreadBoundedPump, 0, 0) &&
            WaitForSingleObject(state->boundedEntered, 2000) == WAIT_OBJECT_0,
        "cross-thread cancellation fixture did not enter its bounded native pump");
    ++action.eventId;
    action.expectedRevision = outcome.revision ? outcome.revision : snapshot.revision;
    Check(agent->Invoke(action, outcome) && outcome.accepted,
        "cross-thread native action did not enter its cancellable queue");
    Check(agent->Restore(error), "cross-thread restore did not retire its queued native action");
    SetEvent(state->boundedRelease);
    Check(WaitForSingleObject(state->boundedDone, 2000) == WAIT_OBJECT_0 &&
            Dispatch::DispatchToWindowThread(state->button.load(), [] {}, error) && state->notifications.load() == 1,
        "a queued cross-thread native action ran after surface restore");

    // Recreating a native HWND produces a fresh node identity, even when USER32
    // recycles the same handle value for this Windows Forms subclass.
    Check(Dispatch::DispatchToWindowThread(state->button.load(), [state] {
        DestroyWindow(state->edit.load());
        const HWND replacement = CreateWindowExW(WS_EX_CLIENTEDGE, kCrossThreadEditClass, L"replacement",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12, 12, 180, 26,
            state->root.load(), reinterpret_cast<HMENU>(41), GetModuleHandleW(nullptr), nullptr);
        state->edit.store(replacement);
        if (replacement) SetWindowSubclass(replacement, CrossThreadSurfaceSubclass, 0xAB92,
            reinterpret_cast<DWORD_PTR>(state.get()));
    }, error), "cross-thread native control could not be recreated on its owner thread");
    Dispatch::WindowSnapshot recreated;
    recreated.surfaceId = snapshot.surfaceId;
    recreated.revision = 10;
    const bool recaptured = agent->Capture(recreated, error);
    const auto replacement = find(recreated, state->edit.load());
    Check(recaptured && replacement && replacement->nodeId != editId && replacement->generation != editGeneration,
        "cross-thread recapture reused a retired control's native identity");
    cleanup();
    Check(!state->expired.load(), "cross-thread native fixture exceeded a callback deadline");
}
