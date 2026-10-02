#pragma once

// Include after MenuBarIdentityRegressionTests.h inside main.cpp's test namespace.
// These fixtures enter real native modal pumps, then retire the source agent from
// the other thread before allowing its older callback to return.
constexpr UINT kSourceLifetimeSentinel = WM_APP + 371;
constexpr UINT kSourceLifetimeCommand = 0x3A51;

bool InitializeSourceTestMenuToolbar(HWND toolbar) {
    SendMessageW(toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    TBBUTTON buttons[2]{};
    for (int index = 0; index < 2; ++index) {
        buttons[index].iBitmap = I_IMAGENONE;
        buttons[index].idCommand = 100 + index;
        buttons[index].fsState = TBSTATE_ENABLED;
        buttons[index].fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
        buttons[index].iString = reinterpret_cast<INT_PTR>(index == 0 ? L"&File" : L"&View");
    }
    const bool added = SendMessageW(toolbar, TB_ADDBUTTONSW, 2,
        reinterpret_cast<LPARAM>(buttons)) != FALSE;
    SendMessageW(toolbar, TB_AUTOSIZE, 0, 0);
    return added;
}

struct SourceLifetimeRuntime final {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE drained = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<DWORD> thread{0};
    std::atomic<HWND> root{nullptr};
    std::atomic<HWND> child{nullptr};
    std::atomic<bool> blockProviderRead{false};
    std::atomic<bool> pumpTimedOut{false};

    ~SourceLifetimeRuntime() {
        for (const HANDLE event : {ready, entered, release, drained})
            if (event) CloseHandle(event);
    }
};

void PumpSourceLifetime(SourceLifetimeRuntime& state) {
    SetEvent(state.entered);
    const auto deadline = GetTickCount64() + 10000;
    while (WaitForSingleObject(state.release, 0) == WAIT_TIMEOUT) {
        if (GetTickCount64() >= deadline) {
            state.pumpTimedOut.store(true);
            return;
        }
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                return;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        } else {
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
}

// Delegate the ordinary accessible menu metadata to the existing fixture. Only
// this provider read blocks, after WM_GETOBJECT has returned, so no HWND subclass
// is on the stack to accidentally supply the command dispatch's lifetime guard.
class SourceLifetimeAccessible final : public IAccessible, public IEnumVARIANT {
public:
    void (*onRead)(void*) = nullptr;
    void* readContext = nullptr;
    TestMenuAccessible& Metadata() noexcept { return *delegate_; }
    explicit SourceLifetimeAccessible(SourceLifetimeRuntime& state, HWND toolbar)
        : state_(state), delegate_(new TestMenuAccessible()) {
        delegate_->toolbar = toolbar;
        delegate_->names = {L"&File", L"&View"};
    }
    ~SourceLifetimeAccessible() { delegate_->Release(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid == IID_IEnumVARIANT) {
            *value = static_cast<IEnumVARIANT*>(this);
            AddRef();
            return S_OK;
        }
        if (iid != IID_IUnknown && iid != IID_IDispatch && iid != IID_IAccessible) return E_NOINTERFACE;
        *value = static_cast<IAccessible*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto refs = --references_;
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override { return delegate_->GetTypeInfoCount(count); }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT index, LCID locale, ITypeInfo** info) override { return delegate_->GetTypeInfo(index, locale, info); }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID iid, LPOLESTR* names, UINT count, LCID locale, DISPID* ids) override { return delegate_->GetIDsOfNames(iid, names, count, locale, ids); }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID iid, LCID locale, WORD flags, DISPPARAMS* args, VARIANT* result, EXCEPINFO* exception, UINT* error) override { return delegate_->Invoke(id, iid, locale, flags, args, result, exception, error); }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** value) override { return delegate_->get_accParent(value); }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override {
        if (onRead) onRead(readContext);
        if (state_.blockProviderRead.exchange(false)) PumpSourceLifetime(state_);
        return delegate_->get_accChildCount(count);
    }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, VARIANT* values, ULONG* fetched) override { return delegate_->Next(count, values, fetched); }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override { return delegate_->Skip(count); }
    HRESULT STDMETHODCALLTYPE Reset() override { return delegate_->Reset(); }
    HRESULT STDMETHODCALLTYPE Clone(IEnumVARIANT** value) override { return delegate_->Clone(value); }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT child, IDispatch** value) override { return delegate_->get_accChild(child, value); }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* value) override { return delegate_->get_accName(child, value); }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR* value) override { return delegate_->get_accValue(child, value); }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child, BSTR* value) override { return delegate_->get_accDescription(child, value); }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* value) override { return delegate_->get_accRole(child, value); }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* value) override { return delegate_->get_accState(child, value); }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT child, BSTR* value) override { return delegate_->get_accHelp(child, value); }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* file, VARIANT child, long* topic) override { return delegate_->get_accHelpTopic(file, child, topic); }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT child, BSTR* value) override { return delegate_->get_accKeyboardShortcut(child, value); }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* value) override { return delegate_->get_accFocus(value); }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* value) override { return delegate_->get_accSelection(value); }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* value) override { return delegate_->get_accDefaultAction(child, value); }
    HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT child) override { return delegate_->accSelect(flags, child); }
    HRESULT STDMETHODCALLTYPE accLocation(long* x, long* y, long* width, long* height, VARIANT child) override { return delegate_->accLocation(x, y, width, height, child); }
    HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT child, VARIANT* value) override { return delegate_->accNavigate(direction, child, value); }
    HRESULT STDMETHODCALLTYPE accHitTest(long x, long y, VARIANT* value) override { return delegate_->accHitTest(x, y, value); }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override { return delegate_->accDoDefaultAction(child); }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT child, BSTR value) override { return delegate_->put_accName(child, value); }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT child, BSTR value) override { return delegate_->put_accValue(child, value); }
private:
    ULONG references_ = 1;
    SourceLifetimeRuntime& state_;
    TestMenuAccessible* delegate_;
};

LRESULT CALLBACK SourceLifetimeSubclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    auto& state = *reinterpret_cast<SourceLifetimeRuntime*>(data);
    if (message == WM_COMMAND && LOWORD(wParam) == kSourceLifetimeCommand) {
        PumpSourceLifetime(state);
        return 0;
    }
    if (message == kSourceLifetimeSentinel) {
        SetEvent(state.drained);
        return 0;
    }
    const auto result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, SourceLifetimeSubclass, subclassId);
    return result;
}

LRESULT CALLBACK SourceLifetimeObjectSubclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    if (message == WM_GETOBJECT && static_cast<LONG>(lParam) == OBJID_CLIENT)
        return LresultFromObject(IID_IAccessible, wParam, reinterpret_cast<IAccessible*>(data));
    const auto result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, SourceLifetimeObjectSubclass, subclassId);
    return result;
}

void TestSourceThreadCallbackLifetime() {
    // Root and child callbacks cover both native subclass entry points; the
    // provider case covers a bounded command after its caller has timed out.
    for (const int scenario : {0, 1, 2}) {
        SourceLifetimeRuntime runtime;
        Check(runtime.ready && runtime.entered && runtime.release && runtime.drained,
            "source callback lifetime events were not created");
        if (!runtime.ready || !runtime.entered || !runtime.release || !runtime.drained) return;
        std::thread gui([&] {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            runtime.thread.store(GetCurrentThreadId());
            const HWND root = CreateWindowExW(0, L"Static", L"source-lifetime",
                WS_OVERLAPPEDWINDOW, 40, 40, 360, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            const HWND child = root ? CreateWindowExW(0, scenario == 2 ? TOOLBARCLASSNAMEW : L"Static", L"",
                WS_CHILD | WS_VISIBLE, 0, 0, 320, 30, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
            SourceLifetimeAccessible* accessible = nullptr;
            if (root && child) {
                for (const HWND window : {root, child})
                    SetWindowSubclass(window, SourceLifetimeSubclass, 0xAB71, reinterpret_cast<DWORD_PTR>(&runtime));
                if (scenario == 2) {
                    Check(InitializeSourceTestMenuToolbar(child), "source lifetime toolbar buttons were not created");
                    accessible = new SourceLifetimeAccessible(runtime, child);
                    SetWindowSubclass(child, SourceLifetimeObjectSubclass, 0xAB72,
                        reinterpret_cast<DWORD_PTR>(static_cast<IAccessible*>(accessible)));
                }
                ShowWindow(root, SW_SHOWNOACTIVATE);
            }
            runtime.root.store(root);
            runtime.child.store(child);
            SetEvent(runtime.ready);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (root) DestroyWindow(root);
            if (accessible) accessible->Release();
            winrt::uninit_apartment();
        });
        std::shared_ptr<Translation::SourceThreadAgent> agent;
        std::thread captureWorker;
        const auto cleanup = [&] {
            SetEvent(runtime.release);
            if (captureWorker.joinable()) captureWorker.join();
            if (agent && !agent->Shutdown()) Translation::RetainSourceThreadAgent(agent);
            agent.reset();
            if (runtime.thread.load()) PostThreadMessageW(runtime.thread.load(), WM_QUIT, 0, 0);
            gui.join();
        };
        const bool ready = WaitForSingleObject(runtime.ready, 5000) == WAIT_OBJECT_0 && runtime.child.load();
        Check(ready, "source callback lifetime windows did not start");
        if (!ready) { cleanup(); continue; }
        agent = Translation::SourceThreadAgent::Attach(runtime.root.load(), GetModuleHandleW(nullptr));
        Check(agent != nullptr, "source callback lifetime agent did not attach");
        if (!agent) { cleanup(); continue; }
        std::weak_ptr<Translation::SourceThreadAgent> weak = agent;
        std::atomic<bool> captureAccepted{true};
        if (scenario == 2) {
            runtime.blockProviderRead.store(true);
            captureWorker = std::thread([&, heldAgent = agent] {
                Translation::WindowSnapshot snapshot;
                snapshot.surfaceId = L"98989898-3434-5656-7878-909090909090";
                std::wstring error;
                captureAccepted.store(heldAgent->Capture(snapshot, error, 100));
            });
        } else {
            Check(PostMessageW(scenario == 0 ? runtime.root.load() : runtime.child.load(),
                WM_COMMAND, kSourceLifetimeCommand, 0) != FALSE, "native lifetime command did not post");
        }
        const bool entered = WaitForSingleObject(runtime.entered, 3000) == WAIT_OBJECT_0;
        Check(entered, "native callback did not enter its modal lifetime pump");
        if (!entered) { cleanup(); continue; }
        if (captureWorker.joinable()) captureWorker.join();
        if (scenario == 2) Check(!captureAccepted.load(), "blocked source capture did not honor its deadline");
        const bool shutDown = agent->Shutdown();
        Check(shutDown, "nested shutdown did not remove source callbacks");
        if (!shutDown) { cleanup(); continue; }
        agent.reset();
        Check(!weak.expired(), "agent died while a native subclass or bounded callback still used it");
        SetEvent(runtime.release);
        const bool drained = PostMessageW(runtime.root.load(), kSourceLifetimeSentinel, 0, 0) &&
            WaitForSingleObject(runtime.drained, 3000) == WAIT_OBJECT_0;
        Check(drained && weak.expired(), "completed native callback retained its retired source agent");
        Check(!runtime.pumpTimedOut.load(), "native callback lifetime fixture exceeded its modal deadline");
        cleanup();
    }
}

struct SourceMenuBindingRuntime final {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE selected = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<DWORD> thread{0};
    std::atomic<HWND> root{nullptr};
    std::atomic<unsigned> selections{0};
    MenuIdentityRuntime identity;
    ~SourceMenuBindingRuntime() {
        if (ready) CloseHandle(ready);
        if (selected) CloseHandle(selected);
    }
};

LRESULT CALLBACK SourceMenuBindingSubclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    auto& state = *reinterpret_cast<SourceMenuBindingRuntime*>(data);
    const auto result = DefSubclassProc(window, message, wParam, lParam);
    if (message == kTestMenuPopup && state.identity.returnValue != FALSE) {
        state.selections.fetch_add(1);
        SetEvent(state.selected);
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, SourceMenuBindingSubclass, subclassId);
    return result;
}

void TestMenuActionBindingGeneration() {
    SourceMenuBindingRuntime runtime;
    Check(runtime.ready && runtime.selected, "menu binding source events were not created");
    if (!runtime.ready || !runtime.selected) return;
    std::thread gui([&] {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        runtime.thread.store(GetCurrentThreadId());
        const HWND root = CreateWindowExW(0, L"Static", L"source-menu-binding",
            WS_OVERLAPPEDWINDOW, 40, 40, 360, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        const HWND toolbar = root ? CreateWindowExW(0, TOOLBARCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE,
            0, 0, 320, 30, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
        TestMenuAccessible* accessible = nullptr;
        if (toolbar) {
            Check(InitializeSourceTestMenuToolbar(toolbar), "source menu binding toolbar buttons were not created");
            accessible = new TestMenuAccessible();
            accessible->toolbar = toolbar;
            accessible->names = {L"&File", L"&View"};
            runtime.identity.root = root;
            runtime.identity.otherOwner = toolbar;
            SetWindowSubclass(toolbar, TestMenuToolbarSubclass, 0xAB73, reinterpret_cast<DWORD_PTR>(accessible));
            SetWindowSubclass(toolbar, MenuIdentitySubclass, 0xAB74, reinterpret_cast<DWORD_PTR>(&runtime.identity));
            SetWindowSubclass(toolbar, SourceMenuBindingSubclass, 0xAB75, reinterpret_cast<DWORD_PTR>(&runtime));
            ShowWindow(root, SW_SHOWNOACTIVATE);
        }
        runtime.root.store(toolbar ? root : nullptr);
        SetEvent(runtime.ready);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (root) DestroyWindow(root);
        if (accessible) accessible->Release();
        winrt::uninit_apartment();
    });
    std::shared_ptr<Translation::SourceThreadAgent> agent;
    const auto cleanup = [&] {
        if (agent) {
            std::wstring ignored;
            agent->Restore(ignored);
            if (!agent->Shutdown()) Translation::RetainSourceThreadAgent(agent);
        }
        if (runtime.thread.load()) PostThreadMessageW(runtime.thread.load(), WM_QUIT, 0, 0);
        gui.join();
    };
    const bool ready = WaitForSingleObject(runtime.ready, 5000) == WAIT_OBJECT_0 && runtime.root.load();
    Check(ready, "menu binding source window did not start");
    if (!ready) { cleanup(); return; }
    agent = Translation::SourceThreadAgent::Attach(runtime.root.load(), GetModuleHandleW(nullptr));
    std::wstring error;
    Check(agent && agent->SetCloaked(true, error), "menu binding source agent could not attach/cloak");
    if (!agent) { cleanup(); return; }
    const auto refresh = [&] {
        const auto deadline = GetTickCount64() + 5000;
        agent->RequestMenuBarRefresh();
        do {
            bool changed = false;
            if (!agent->RefreshMenuBarToolbar(30, changed, error, 3000)) return false;
            if (changed) return true;
            Sleep(30);
        } while (GetTickCount64() < deadline);
        return false;
    };
    Translation::WindowSnapshot published;
    published.surfaceId = L"97979797-3434-5656-7878-909090909090";
    published.revision = 1;
    const bool captured = refresh() && agent->Capture(published, error) &&
        published.menuBindingGeneration != 0 && !published.menu.empty() && !published.menu[0].items.empty();
    Check(captured, "menu binding source did not publish its first toolbar generation");
    if (!captured) { cleanup(); return; }
    Translation::ActionRequest action;
    action.surfaceId = published.surfaceId;
    action.expectedRevision = published.revision;
    action.expectedMenuBindingGeneration = published.menuBindingGeneration;
    action.eventId = 1;
    action.action = L"menuCommand";
    action.menuCommandId = published.menu[0].items[0].commandId;

    // Keep the old published snapshot after the source cache has refreshed. This
    // is the window left by a timeout/failure of the post-refresh recapture.
    Check(refresh(), "menu binding source did not replace its cached generation");
    Translation::ActionOutcome outcome;
    Check(!agent->Invoke(action, outcome) && outcome.refused &&
            outcome.error == L"native menu binding changed before selection" && runtime.selections.load() == 0,
        "old published menu action selected a refreshed native binding");
    Translation::WindowSnapshot current = published;
    const bool recaptured = agent->Capture(current, error) &&
        current.menuBindingGeneration > published.menuBindingGeneration;
    Check(recaptured, "menu binding generation did not advance after refresh");
    if (recaptured) {
        action.expectedMenuBindingGeneration = current.menuBindingGeneration;
        ++action.eventId;
        Check(agent->Invoke(action, outcome) && outcome.accepted &&
                WaitForSingleObject(runtime.selected, 2000) == WAIT_OBJECT_0 && runtime.selections.load() == 1,
            "current published toolbar binding did not invoke exactly once");
    }
    cleanup();
}
