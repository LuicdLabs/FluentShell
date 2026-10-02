#pragma once

// Included inside main.cpp's test namespace. Real ListView IDs/text/state are
// paired with an application-owned MSAA default action that can enter a modal loop.
constexpr UINT kListActivationProbe = WM_APP + 371;
constexpr UINT kListActivationSentinel = WM_APP + 372;

enum class ListActivationProbe : WPARAM {
    Cancel, Duplicate, Replace, Rename, State, DisabledItem, HiddenList, DisabledParent,
    ChangedAction, ChangedView, ReplaceDuringFinalRead, StateDuringFinalRead,
    ProviderRefused, ProviderUnavailable,
};

// The rest of this test host intentionally exercises legacy common controls.
// Activate v6 only on this fixture's STA, where stable ListView IDs are required.
class ListActivationCommonControls final {
public:
    ListActivationCommonControls() {
        wchar_t directory[MAX_PATH]{};
        const DWORD length = GetTempPathW(MAX_PATH, directory);
        if (length == 0 || length >= MAX_PATH || !GetTempFileNameW(directory, L"fsl", 0, manifest_)) return;
        const HANDLE file = CreateFileW(manifest_, GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        constexpr char manifest[] =
            "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>"
            "<assembly xmlns='urn:schemas-microsoft-com:asm.v1' manifestVersion='1.0'>"
            "<assemblyIdentity type='win32' name='FluentShell.ListActivationFixture' version='1.0.0.0'/>"
            "<dependency><dependentAssembly>"
            "<assemblyIdentity type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "
            "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'/>"
            "</dependentAssembly></dependency></assembly>";
        DWORD written = 0;
        const bool saved = WriteFile(file, manifest, sizeof(manifest) - 1, &written, nullptr) &&
            written == sizeof(manifest) - 1;
        CloseHandle(file);
        if (!saved) return;
        ACTCTXW context{ sizeof(context) };
        context.lpSource = manifest_;
        handle_ = CreateActCtxW(&context);
        if (handle_ == INVALID_HANDLE_VALUE || !ActivateActCtx(handle_, &cookie_)) return;
        active_ = true;
        commonControls_ = LoadLibraryW(L"comctl32.dll");
        const auto initialize = commonControls_ ? reinterpret_cast<BOOL(WINAPI*)(const INITCOMMONCONTROLSEX*)>(
            GetProcAddress(commonControls_, "InitCommonControlsEx")) : nullptr;
        INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_LISTVIEW_CLASSES };
        ready_ = initialize && initialize(&controls) != FALSE;
    }
    ~ListActivationCommonControls() {
        if (commonControls_) FreeLibrary(commonControls_);
        if (active_) DeactivateActCtx(0, cookie_);
        if (handle_ != INVALID_HANDLE_VALUE) ReleaseActCtx(handle_);
        if (manifest_[0]) DeleteFileW(manifest_);
    }
    explicit operator bool() const noexcept { return ready_; }
private:
    wchar_t manifest_[MAX_PATH]{};
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HMODULE commonControls_ = nullptr;
    ULONG_PTR cookie_ = 0;
    bool active_ = false;
    bool ready_ = false;
};

struct ListActivationRuntime final {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE drained = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE modalStarted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE modalRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<HWND> root{nullptr};
    std::atomic<HWND> list{nullptr};
    HWND parent = nullptr;
    std::atomic<DWORD> thread{0};
    std::atomic<Translation::SourceThreadAgent*> agent{nullptr};
    Translation::ControlNode node;
    std::atomic<unsigned> calls{0};
    std::atomic<bool> wrongThread{false};
    std::atomic<bool> modal{false};
    std::atomic<bool> pumpAfterQueue{false};
    std::atomic<bool> pumpedQueuedActivation{false};
    std::atomic<bool> providerPumpDrained{false};
    std::atomic<bool> actionInsideProviderRead{false};
    std::atomic<bool> invalidateCaptureAfterQueue{false};
    unsigned providerReadDepth = 0;
    unsigned mutateOnDefaultActionRead = 0;
    bool replaceOnRead = false;
    bool mutatedDuringRead = false;
    bool firstQueued = false;
    bool secondRefused = false;
    bool replayPosted = false;
    long itemState = 0;
    bool changedAction = false;
    HRESULT actionResult = S_OK;
    bool standardMetadata = false;
    bool replaceDuringAction = false;
    bool renameDuringAction = false;
    bool clearStateDuringAction = false;
    bool cancelDuringAction = false;
    bool changeDefaultDuringAction = false;
    bool reenterDuringAction = false;
    bool reentrantRefused = false;
    bool actionMutated = false;
    bool cancelDuringPreparationRead = false;
    bool reenterDuringPreparationRead = false;
    bool preparationCancelled = false;
    unsigned preparationReentries = 0;
    bool preparationReentrantRefused = false;
    bool pumpDuringPreparationRelease = false;
    unsigned preparationReleasePumps = 0;
    bool actionDuringPreparationRelease = false;
    bool deferredConsumedDuringPreparationRelease = false;
    unsigned completedDefaultActionReads = 0;
    unsigned armFinalIdentityAfterDefaultRead = 0;
    bool interceptFinalIdentityRead = false;
    unsigned syntheticInputMessages = 0;
    unsigned activationNotifications = 0;
    UINT stateAfterAction = 0;
    uint32_t idAfterAction = UINT32_MAX;
    std::wstring textAfterAction;
    ~ListActivationRuntime() {
        for (HANDLE event : {ready, drained, modalStarted, modalRelease, returned})
            if (event) CloseHandle(event);
    }
};

class ListActivationAccessible final : public IAccessible {
public:
    explicit ListActivationAccessible(ListActivationRuntime& runtime) : runtime_(runtime) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDispatch && iid != IID_IAccessible) return E_NOINTERFACE;
        *object = static_cast<IAccessible*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --references_;
        if (!n) {
            if (pumpOnFinalRelease_) {
                pumpOnFinalRelease_ = false;
                ++runtime_.preparationReleasePumps;
                runtime_.wrongThread.store(runtime_.wrongThread.load() ||
                    GetCurrentThreadId() != runtime_.thread.load());
                const unsigned callsBefore = runtime_.calls.load();
                if (auto* agent = runtime_.agent.load()) {
                    // Release can enter application code too. The outer Post
                    // must not have published its deferred action at this point.
                    for (unsigned removed = 0; removed < 32; ++removed) {
                        MSG queued{};
                        if (!PeekMessageW(&queued, nullptr, agent->MessageId(), agent->MessageId(), PM_NOREMOVE)) break;
                        const bool listActivation = queued.wParam == 3;
                        if (!PeekMessageW(&queued, nullptr, agent->MessageId(), agent->MessageId(), PM_REMOVE)) break;
                        runtime_.deferredConsumedDuringPreparationRelease =
                            runtime_.deferredConsumedDuringPreparationRelease || listActivation;
                        TranslateMessage(&queued);
                        DispatchMessageW(&queued);
                    }
                }
                runtime_.actionDuringPreparationRelease = runtime_.calls.load() != callsBefore;
            }
            delete this;
        }
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override { if (count) *count = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** value) override { if (value) *value = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override {
        if (!count) return E_POINTER;
        *count = static_cast<long>(SendMessageW(runtime_.list.load(), LVM_GETITEMCOUNT, 0, 0));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** value) override { if (value) *value = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override {
        if (!name || child.vt != VT_I4 || child.lVal <= 0) return E_INVALIDARG;
        if (runtime_.standardMetadata)
            return ReadStandard([&](IAccessible* object) { return object->get_accName(child, name); });
        wchar_t text[128]{};
        LVITEMW item{};
        item.pszText = text;
        item.cchTextMax = 128;
        SendMessageW(runtime_.list.load(), LVM_GETITEMTEXTW, child.lVal - 1, reinterpret_cast<LPARAM>(&item));
        *name = SysAllocString(text);
        return *name ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override {
        if (!role) return E_POINTER;
        if (runtime_.standardMetadata)
            return ReadStandard([&](IAccessible* object) { return object->get_accRole(child, role); });
        role->vt = VT_I4;
        role->lVal = child.lVal == CHILDID_SELF ? ROLE_SYSTEM_LIST : ROLE_SYSTEM_LISTITEM;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* state) override {
        if (!state) return E_POINTER;
        if (runtime_.standardMetadata)
            return ReadStandard([&](IAccessible* object) { return object->get_accState(child, state); });
        runtime_.wrongThread.store(runtime_.wrongThread.load() || GetCurrentThreadId() != runtime_.thread.load());
        state->vt = VT_I4;
        state->lVal = STATE_SYSTEM_SELECTABLE | runtime_.itemState;
        if (SendMessageW(runtime_.list.load(), LVM_GETITEMSTATE, child.lVal - 1, LVIS_SELECTED))
            state->lVal |= STATE_SYSTEM_SELECTED;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR*, VARIANT, long*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child, BSTR* action) override {
        if (!action) return E_POINTER;
        ++runtime_.providerReadDepth;
        if (runtime_.pumpAfterQueue.load()) {
            if (auto* agent = runtime_.agent.load()) {
                MSG queued{};
                if (PeekMessageW(&queued, nullptr, agent->MessageId(), agent->MessageId(), PM_NOREMOVE) &&
                    queued.wParam == 3) {
                    runtime_.pumpAfterQueue.store(false);
                    runtime_.pumpedQueuedActivation.store(true);
                    runtime_.providerPumpDrained.store(false);
                    // The hook must consume this message without starting the
                    // action or endlessly reposting into this provider's loop.
                    for (unsigned removed = 0; removed < 32; ++removed) {
                        if (!PeekMessageW(&queued, nullptr, agent->MessageId(), agent->MessageId(), PM_REMOVE)) {
                            runtime_.providerPumpDrained.store(true);
                            break;
                        }
                        TranslateMessage(&queued);
                        DispatchMessageW(&queued);
                    }
                    if (runtime_.invalidateCaptureAfterQueue.exchange(false))
                        SendMessageW(runtime_.list.load(), LVM_SETVIEW, LV_VIEW_TILE, 0);
                }
            }
        }
        if (runtime_.mutateOnDefaultActionRead != 0 && --runtime_.mutateOnDefaultActionRead == 0) {
            runtime_.mutatedDuringRead = true;
            if (runtime_.replaceOnRead) {
                SendMessageW(runtime_.list.load(), LVM_DELETEITEM, 0, 0);
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.pszText = const_cast<LPWSTR>(L"Component");
                SendMessageW(runtime_.list.load(), LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            } else {
                ListView_SetItemState(runtime_.list.load(), 0, LVIS_SELECTED, LVIS_SELECTED);
            }
        }
        --runtime_.providerReadDepth;
        HRESULT result = S_OK;
        if (runtime_.standardMetadata && !runtime_.changedAction)
            result = ReadStandard([&](IAccessible* object) { return object->get_accDefaultAction(child, action); });
        else {
            *action = SysAllocString(runtime_.changedAction ? L"Different action" : L"Open");
            result = *action ? S_OK : E_OUTOFMEMORY;
        }
        if (runtime_.cancelDuringPreparationRead) {
            runtime_.cancelDuringPreparationRead = false;
            if (auto* agent = runtime_.agent.load()) {
                agent->CancelPopupOnSourceThread();
                runtime_.preparationCancelled = true;
            }
        }
        if (runtime_.reenterDuringPreparationRead) {
            // A faulty preparing guard must still produce a bounded test.
            runtime_.reenterDuringPreparationRead = false;
            ++runtime_.preparationReentries;
            std::wstring error;
            bool refused = false;
            auto* agent = runtime_.agent.load();
            runtime_.preparationReentrantRefused = agent &&
                !agent->PostListViewActivation(runtime_.node, 0, refused, error) && refused;
        }
        if (runtime_.pumpDuringPreparationRelease) {
            runtime_.pumpDuringPreparationRelease = false;
            pumpOnFinalRelease_ = true;
        }
        ++runtime_.completedDefaultActionReads;
        // Arm only after the standard provider has returned, so its own state
        // queries cannot consume the final native identity-read probe early.
        if (runtime_.armFinalIdentityAfterDefaultRead != 0 &&
            runtime_.completedDefaultActionReads == runtime_.armFinalIdentityAfterDefaultRead)
            runtime_.interceptFinalIdentityRead = true;
        return result;
    }
    HRESULT STDMETHODCALLTYPE accSelect(long, VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accLocation(long*, long*, long*, long*, VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accNavigate(long, VARIANT, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accHitTest(long, long, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
        runtime_.wrongThread.store(runtime_.wrongThread.load() || GetCurrentThreadId() != runtime_.thread.load());
        runtime_.actionInsideProviderRead.store(runtime_.actionInsideProviderRead.load() || runtime_.providerReadDepth != 0);
        if (child.vt != VT_I4 || child.lVal != 1) return E_INVALIDARG;
        runtime_.calls.fetch_add(1);
        if (runtime_.reenterDuringAction) {
            runtime_.reenterDuringAction = false;
            std::wstring error;
            bool refused = false;
            auto* agent = runtime_.agent.load();
            runtime_.reentrantRefused = agent &&
                !agent->PostListViewActivation(runtime_.node, 0, refused, error) && refused;
        }
        if (runtime_.replaceDuringAction) {
            SendMessageW(runtime_.list.load(), LVM_DELETEITEM, 0, 0);
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.pszText = const_cast<LPWSTR>(L"Component");
            SendMessageW(runtime_.list.load(), LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            ListView_SetItemState(runtime_.list.load(), 0, LVIS_SELECTED | LVIS_FOCUSED,
                LVIS_SELECTED | LVIS_FOCUSED);
            runtime_.actionMutated = true;
        }
        if (runtime_.renameDuringAction) {
            ListView_SetItemText(runtime_.list.load(), 0, 0, const_cast<LPWSTR>(L"Changed"));
            runtime_.actionMutated = true;
        }
        if (runtime_.clearStateDuringAction) {
            ListView_SetItemState(runtime_.list.load(), 0, 0, LVIS_SELECTED | LVIS_FOCUSED);
            runtime_.actionMutated = true;
        }
        if (runtime_.changeDefaultDuringAction) {
            runtime_.changedAction = true;
            runtime_.actionMutated = true;
        }
        if (runtime_.cancelDuringAction) {
            if (auto* agent = runtime_.agent.load()) agent->CancelPopupOnSourceThread();
            runtime_.actionMutated = true;
        }
        if (runtime_.modal.load()) {
            SetEvent(runtime_.modalStarted);
            while (WaitForSingleObject(runtime_.modalRelease, 0) == WAIT_TIMEOUT) {
                MSG message{};
                if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                } else MsgWaitForMultipleObjectsEx(1, &runtime_.modalRelease, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
        }
        SetEvent(runtime_.returned);
        return runtime_.actionResult;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }
private:
    template<typename Read>
    HRESULT ReadStandard(Read&& read) {
        runtime_.wrongThread.store(runtime_.wrongThread.load() || GetCurrentThreadId() != runtime_.thread.load());
        winrt::com_ptr<IAccessible> standard;
        const HRESULT result = CreateStdAccessibleObject(runtime_.list.load(), OBJID_CLIENT,
            IID_IAccessible, standard.put_void());
        return FAILED(result) || !standard ? result : read(standard.get());
    }
    std::atomic<ULONG> references_{1};
    bool pumpOnFinalRelease_ = false;
    ListActivationRuntime& runtime_;
};

LRESULT CALLBACK ListActivationParentSubclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    auto& runtime = *reinterpret_cast<ListActivationRuntime*>(data);
    if (message == WM_NOTIFY && lParam) {
        const auto& notification = *reinterpret_cast<const NMHDR*>(lParam);
        if (notification.hwndFrom == runtime.list.load() &&
            (notification.code == NM_DBLCLK || notification.code == NM_RETURN ||
                notification.code == LVN_ITEMACTIVATE)) ++runtime.activationNotifications;
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ListActivationParentSubclass, subclassId);
    return result;
}

LRESULT CALLBACK ListActivationSubclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    auto& runtime = *reinterpret_cast<ListActivationRuntime*>(data);
    if (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR ||
        message == WM_SYSKEYDOWN || message == WM_SYSKEYUP || message == WM_SYSCHAR ||
        message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK)
        ++runtime.syntheticInputMessages;
    if (message == WM_GETOBJECT && static_cast<LONG>(lParam) == OBJID_CLIENT) {
        auto* accessible = new ListActivationAccessible(runtime);
        const LRESULT result = LresultFromObject(IID_IAccessible, wParam, accessible);
        accessible->Release();
        return result;
    }
    if (message == kListActivationProbe) {
        auto* agent = runtime.agent.load();
        std::wstring error;
        bool refused = false;
        runtime.firstQueued = agent && agent->PostListViewActivation(runtime.node, 0, refused, error) && !refused;
        if (!runtime.firstQueued)
            std::wcerr << L"ListView activation queue probe " << wParam << L": " << error << L'\n';
        switch (static_cast<ListActivationProbe>(wParam)) {
        case ListActivationProbe::Cancel: agent->CancelPopupOnSourceThread(); break;
        case ListActivationProbe::Duplicate: {
            bool secondRefused = false;
            runtime.secondRefused = !agent->PostListViewActivation(runtime.node, 0, secondRefused, error) && secondRefused;
            MSG queued{};
            if (PeekMessageW(&queued, nullptr, agent->MessageId(), agent->MessageId(), PM_NOREMOVE))
                runtime.replayPosted = PostThreadMessageW(agent->ThreadId(), queued.message, queued.wParam, queued.lParam) != FALSE;
            break;
        }
        case ListActivationProbe::Replace: {
            SendMessageW(window, LVM_DELETEITEM, 0, 0);
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.pszText = const_cast<LPWSTR>(L"Component");
            SendMessageW(window, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            break;
        }
        case ListActivationProbe::Rename: ListView_SetItemText(window, 0, 0, const_cast<LPWSTR>(L"Changed")); break;
        case ListActivationProbe::State: ListView_SetItemState(window, 0, LVIS_SELECTED, LVIS_SELECTED); break;
        case ListActivationProbe::DisabledItem: runtime.itemState = STATE_SYSTEM_UNAVAILABLE; break;
        case ListActivationProbe::HiddenList: ShowWindow(window, SW_HIDE); break;
        case ListActivationProbe::DisabledParent: EnableWindow(runtime.parent, FALSE); break;
        case ListActivationProbe::ChangedAction: runtime.changedAction = true; break;
        case ListActivationProbe::ChangedView: SendMessageW(window, LVM_SETVIEW, LV_VIEW_SMALLICON, 0); break;
        case ListActivationProbe::ProviderRefused: runtime.actionResult = S_FALSE; break;
        case ListActivationProbe::ProviderUnavailable: runtime.actionResult = DISP_E_MEMBERNOTFOUND; break;
        case ListActivationProbe::ReplaceDuringFinalRead:
        case ListActivationProbe::StateDuringFinalRead:
            runtime.mutateOnDefaultActionRead = 2;
            runtime.replaceOnRead = static_cast<ListActivationProbe>(wParam) == ListActivationProbe::ReplaceDuringFinalRead;
            runtime.mutatedDuringRead = false;
            break;
        }
        PostMessageW(window, kListActivationSentinel, wParam, 0);
        return 0;
    }
    if (message == kListActivationSentinel) {
        runtime.stateAfterAction = static_cast<UINT>(SendMessageW(window, LVM_GETITEMSTATE, 0,
            LVIS_SELECTED | LVIS_FOCUSED | LVIS_CUT | LVIS_DROPHILITED | LVIS_STATEIMAGEMASK));
        runtime.idAfterAction = static_cast<uint32_t>(SendMessageW(window, LVM_MAPINDEXTOID, 0, 0));
        wchar_t text[128]{};
        ListView_GetItemText(window, 0, 0, text, 128);
        runtime.textAfterAction = text;
        runtime.itemState = 0;
        runtime.changedAction = false;
        runtime.actionResult = S_OK;
        runtime.mutateOnDefaultActionRead = 0;
        ShowWindow(window, SW_SHOWNOACTIVATE);
        EnableWindow(runtime.parent, TRUE);
        ListView_SetItemText(window, 0, 0, const_cast<LPWSTR>(L"Component"));
        ListView_SetItemState(window, 0, 0, LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(window, LVM_SETVIEW, LV_VIEW_ICON, 0);
        SetEvent(runtime.drained);
        return 0;
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ListActivationSubclass, subclassId);
    return result;
}

void TestDeferredListViewActivation() {
    ListActivationRuntime runtime;
    std::thread gui([&] {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        runtime.thread.store(GetCurrentThreadId());
        const ListActivationCommonControls controls;
        if (!controls) {
            std::wcerr << L"ListView activation could not activate common-controls v6\n";
            SetEvent(runtime.ready);
            winrt::uninit_apartment();
            return;
        }
        const HWND root = CreateWindowExW(0, L"Static", L"list-activation",
            WS_OVERLAPPEDWINDOW, 40, 40, 360, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        runtime.parent = root ? CreateWindowExW(0, L"Static", L"", WS_CHILD | WS_VISIBLE,
            0, 0, 320, 180, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
        const HWND list = runtime.parent ? CreateWindowExW(0, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_ICON, 0, 0, 320, 180, runtime.parent,
            reinterpret_cast<HMENU>(1201), GetModuleHandleW(nullptr), nullptr) : nullptr;
        runtime.root.store(root);
        runtime.list.store(list);
        if (list) {
            SetWindowSubclass(runtime.parent, ListActivationParentSubclass, 0xAB32, reinterpret_cast<DWORD_PTR>(&runtime));
            SetWindowSubclass(list, ListActivationSubclass, 0xAB31, reinterpret_cast<DWORD_PTR>(&runtime));
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.pszText = const_cast<LPWSTR>(L"Component");
            SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            ShowWindow(root, SW_SHOWNOACTIVATE);
        }
        SetEvent(runtime.ready);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (root) DestroyWindow(root);
        winrt::uninit_apartment();
    });
    std::shared_ptr<Translation::SourceThreadAgent> agent;
    const auto cleanup = [&] {
        SetEvent(runtime.modalRelease);
        if (agent) { std::wstring ignored; agent->Restore(ignored); agent->Shutdown(); }
        if (runtime.thread.load()) PostThreadMessageW(runtime.thread.load(), WM_QUIT, 0, 0);
        gui.join();
    };
    Check(WaitForSingleObject(runtime.ready, 5000) == WAIT_OBJECT_0 && runtime.list.load(),
        "ListView activation fixture did not start");
    if (!runtime.list.load()) { cleanup(); return; }
    agent = Translation::SourceThreadAgent::Attach(runtime.root.load(), GetModuleHandleW(nullptr));
    std::wstring error;
    Check(agent && agent->SetCloaked(true, error), "ListView activation agent could not attach/cloak");
    if (!agent) { cleanup(); return; }
    runtime.agent.store(agent.get());
    Translation::WindowSnapshot snapshot;
    snapshot.surfaceId = L"74747474-4545-6767-8989-121212121212";
    snapshot.revision = 1;
    const auto capture = [&] {
        error.clear();
        if (!agent->Capture(snapshot, error)) return false;
        const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(), [](const auto& node) {
            return node.kind == Translation::ControlKind::ListView;
        });
        if (found == snapshot.nodes.end()) {
            error = L"ListView node was absent";
            return false;
        }
        if (!found->itemActivationSupported || found->itemNativeIds.size() != 1) {
            error = L"mode=" + found->listViewMode + L" items=" + std::to_wstring(found->items.size()) +
                L" nativeIds=" + std::to_wstring(found->itemNativeIds.size()) +
                L" activation=" + std::to_wstring(found->itemActivationSupported);
            return false;
        }
        runtime.node = *found;
        return true;
    };
    for (const auto scenario : {ListActivationProbe::Cancel, ListActivationProbe::Duplicate,
            ListActivationProbe::Replace, ListActivationProbe::Rename, ListActivationProbe::State,
            ListActivationProbe::DisabledItem, ListActivationProbe::HiddenList,
            ListActivationProbe::DisabledParent, ListActivationProbe::ChangedAction,
            ListActivationProbe::ChangedView, ListActivationProbe::ReplaceDuringFinalRead,
            ListActivationProbe::StateDuringFinalRead, ListActivationProbe::ProviderRefused,
            ListActivationProbe::ProviderUnavailable}) {
        const bool ready = capture();
        if (!ready) std::wcerr << L"ListView activation capture: " << error << L'\n';
        Check(ready, "ListView activation capability or stable native ID was not captured");
        if (!ready) { cleanup(); return; }
        ResetEvent(runtime.drained);
        runtime.calls.store(0);
        runtime.syntheticInputMessages = runtime.activationNotifications = 0;
        runtime.firstQueued = runtime.secondRefused = runtime.replayPosted = false;
        const bool done = PostMessageW(runtime.list.load(), kListActivationProbe, static_cast<WPARAM>(scenario), 0) &&
            WaitForSingleObject(runtime.drained, 2000) == WAIT_OBJECT_0;
        Check(done && runtime.firstQueued, "ListView action did not queue/drain on its source thread");
        if (scenario == ListActivationProbe::Duplicate)
            Check(runtime.calls.load() == 1 && runtime.secondRefused && runtime.replayPosted,
                "duplicate/replayed ListView activation did not execute exactly once");
        else if (scenario == ListActivationProbe::ProviderRefused || scenario == ListActivationProbe::ProviderUnavailable) {
            Check(runtime.calls.load() == 1 && runtime.syntheticInputMessages == 0 && runtime.activationNotifications == 0,
                "failed ListView default action was retried or replaced with synthetic activation");
            Check(runtime.stateAfterAction == 0 && runtime.idAfterAction == runtime.node.itemNativeIds[0] &&
                    runtime.textAfterAction == runtime.node.items[0],
                "failed ListView default action changed canonical item identity, text or state");
        }
        else Check(runtime.calls.load() == 0, "cancelled or stale ListView activation invoked its provider");
        if (scenario == ListActivationProbe::ReplaceDuringFinalRead || scenario == ListActivationProbe::StateDuringFinalRead)
            Check(runtime.mutatedDuringRead, "ListView final provider-read mutation was not exercised");
    }
    if (capture()) {
        Translation::ActionRequest action;
        action.surfaceId = snapshot.surfaceId;
        action.expectedRevision = snapshot.revision;
        action.eventId = 1;
        action.nodeId = runtime.node.nodeId;
        action.action = L"activateItem";
        action.itemIndex = 0;
        action.expectedNativeFingerprint = Translation::SnapshotFingerprint(snapshot) ^ 1;
        Translation::ActionOutcome outcome;
        Check(!agent->Invoke(action, outcome) && outcome.refused,
            "ListView activation retargeted an unreconciled native revision");
        action.expectedNativeFingerprint = Translation::SnapshotFingerprint(snapshot);
        runtime.calls.store(0);
        runtime.pumpAfterQueue.store(true);
        runtime.invalidateCaptureAfterQueue.store(true);
        Check(!agent->Invoke(action, outcome),
            "ListView activation accepted a failed post-enqueue capture");
        ResetEvent(runtime.drained);
        Check(PostMessageW(runtime.list.load(), kListActivationSentinel, 0, 0) &&
                WaitForSingleObject(runtime.drained, 2000) == WAIT_OBJECT_0 && runtime.calls.load() == 0,
            "failed ListView activation command left a runnable deferred action");
        Check(capture(), "ListView activation did not recapture before modal action");
        action.expectedNativeFingerprint = Translation::SnapshotFingerprint(snapshot);
        action.expectedRevision = snapshot.revision;
        runtime.modal.store(true);
        runtime.pumpAfterQueue.store(true);
        runtime.pumpedQueuedActivation.store(false);
        runtime.actionInsideProviderRead.store(false);
        runtime.calls.store(0);
        ResetEvent(runtime.returned);
        Check(agent->Invoke(action, outcome) && outcome.accepted &&
                WaitForSingleObject(runtime.modalStarted, 2000) == WAIT_OBJECT_0 &&
                WaitForSingleObject(runtime.returned, 0) == WAIT_TIMEOUT,
            "native modal default action blocked the bounded activation command");
        Check(runtime.pumpedQueuedActivation.load() && runtime.providerPumpDrained.load() &&
                !runtime.actionInsideProviderRead.load(),
            "ListView activation ran inside the post-enqueue capture provider read");
        Check(agent->Capture(snapshot, error), "modal ListView default action blocked source-thread capture");
        SetEvent(runtime.modalRelease);
        Check(WaitForSingleObject(runtime.returned, 2000) == WAIT_OBJECT_0 && runtime.calls.load() == 1,
            "modal ListView action did not return exactly once");
    }
    Check(!runtime.wrongThread.load(), "ListView provider was read/invoked off its GUI thread");
    Check(!Translation::IsRequestSemanticAction(L"activateItem"), "ListView activation was made replayable/rebasable");
    cleanup();
}
