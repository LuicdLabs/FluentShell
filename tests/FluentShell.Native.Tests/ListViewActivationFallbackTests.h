#pragma once

// Included after the deferred-provider and stock-control fixtures. Exercise the
// production deferred dispatcher with standard MSAA metadata and explicit method
// results; native notifications still come only from real comctl32 processing.
namespace ListViewActivationFallback {

constexpr UINT kQueue = WM_APP + 381;
constexpr UINT kDrained = WM_APP + 382;

enum class Scenario {
    StandardUnavailable, ProviderRefused, ProviderFailed, CustomDefault,
    ProviderReplace, ProviderRename, ProviderState, ProviderCancel, ProviderDefault,
    ProviderReenter, DoubleClickReplace, DoubleClickCancel, DoubleClickRollback,
    DoubleClickReenter, PreparationCancel, PreparationReenter, FinalIdentityCancel,
    FinalIdentityRollback, DoubleClickCapture, PreparationReleasePump, FinalIdentityReplace,
};

struct Fixture final {
    ListActivationRuntime runtime;
    NativeListViewDoubleClick::Fixture* native = nullptr;
    Scenario scenario = Scenario::StandardUnavailable;
    POINT point{};
    HWND foregroundBefore = nullptr;
    HWND foregroundAfter = nullptr;
    HWND captureBefore = nullptr;
    HWND captureAfter = nullptr;
    bool pointRead = false;
    bool notificationMutated = false;
    bool notificationReentrantRefused = false;
    bool rollbackPumpReleased = false;
    bool drainFencePosted = false;
    bool cloakRead = false;
    DWORD cloakAfter = 0;
    bool queueRefused = false;
    bool finalIdentityReadIntercepted = false;
    UINT finalIdentityReadState = 0;
    size_t itemCountAfter = 0;
    bool captureAssigned = false;
    bool captureReleased = false;
};

void PumpRollback(Fixture& fixture) {
    auto* agent = fixture.runtime.agent.load();
    SetEvent(fixture.runtime.modalStarted);
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (WaitForSingleObject(fixture.runtime.modalRelease, 0) == WAIT_TIMEOUT &&
            GetTickCount64() < deadline) {
        MSG command{};
        // Pump the actual Restore command while the native handler remains on
        // its stack. Keep both drained sentinels queued until this returns.
        if (agent && PeekMessageW(&command, nullptr,
                agent->MessageId(), agent->MessageId(), PM_REMOVE)) {
            TranslateMessage(&command);
            DispatchMessageW(&command);
        } else WaitForSingleObject(fixture.runtime.modalRelease, 1);
    }
    fixture.rollbackPumpReleased =
        WaitForSingleObject(fixture.runtime.modalRelease, 0) == WAIT_OBJECT_0;
}

LRESULT CALLBACK ParentSubclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    auto& fixture = *reinterpret_cast<Fixture*>(data);
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NOTIFY && lParam) {
        const auto& header = *reinterpret_cast<const NMHDR*>(lParam);
        if (header.hwndFrom == fixture.runtime.list.load() &&
            (header.code == NM_DBLCLK || header.code == LVN_ITEMACTIVATE || header.code == NM_CLICK || header.code == NM_RETURN))
            ++fixture.runtime.activationNotifications;
    }
    if (message == WM_NOTIFY && lParam && !fixture.notificationMutated) {
        const auto& header = *reinterpret_cast<const NMHDR*>(lParam);
        if (header.hwndFrom == fixture.runtime.list.load() && header.code == NM_DBLCLK) {
            auto* agent = fixture.runtime.agent.load();
            const HWND list = fixture.runtime.list.load();
            if (fixture.scenario == Scenario::DoubleClickReplace) {
                SendMessageW(list, LVM_DELETEITEM, 0, 0);
                LVITEMW item{};
                item.mask = LVIF_TEXT;
                item.pszText = const_cast<LPWSTR>(L"Component");
                SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
                ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                fixture.notificationMutated = true;
            } else if (fixture.scenario == Scenario::DoubleClickCancel) {
                if (agent) agent->CancelPopupOnSourceThread();
                fixture.notificationMutated = true;
            } else if (fixture.scenario == Scenario::DoubleClickRollback) {
                fixture.notificationMutated = true;
                PumpRollback(fixture);
            } else if (fixture.scenario == Scenario::DoubleClickReenter) {
                fixture.notificationMutated = true;
                std::wstring error;
                bool refused = false;
                fixture.notificationReentrantRefused = agent &&
                    !agent->PostListViewActivation(fixture.runtime.node, 0, refused, error) && refused;
            } else if (fixture.scenario == Scenario::DoubleClickCapture) {
                fixture.notificationMutated = true;
                SetCapture(fixture.runtime.root.load());
                fixture.captureAssigned = GetCapture() == fixture.runtime.root.load();
            }
        }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ParentSubclass, subclassId);
    return result;
}

LRESULT CALLBACK ListSubclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    auto& fixture = *reinterpret_cast<Fixture*>(data);
    auto& runtime = fixture.runtime;
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
    if (message == kQueue) {
        fixture.native->mouseMessages.clear();
        fixture.native->notifications.clear();
        runtime.syntheticInputMessages = runtime.activationNotifications = 0;
        fixture.pointRead = Translation::ReadListViewNativeActivationPoint(window, 0, fixture.point);
        fixture.foregroundBefore = GetForegroundWindow();
        fixture.captureBefore = GetCapture();
        // Capture above must remain neutral. Only the requested gesture arms
        // metadata callbacks and the final native identity-read cancellation.
        runtime.cancelDuringPreparationRead = fixture.scenario == Scenario::PreparationCancel;
        runtime.reenterDuringPreparationRead = fixture.scenario == Scenario::PreparationReenter;
        runtime.pumpDuringPreparationRelease = fixture.scenario == Scenario::PreparationReleasePump;
        runtime.completedDefaultActionReads = 0;
        runtime.armFinalIdentityAfterDefaultRead =
            fixture.scenario == Scenario::FinalIdentityCancel || fixture.scenario == Scenario::FinalIdentityRollback ||
            fixture.scenario == Scenario::FinalIdentityReplace
            ? 3u : 0u; // Preparation, initial dispatch preflight, final dispatch preflight.
        std::wstring error;
        bool refused = false;
        auto* agent = runtime.agent.load();
        const bool posted = agent && agent->PostListViewActivation(runtime.node, 0, refused, error);
        runtime.firstQueued = posted && !refused;
        fixture.queueRefused = !posted && refused;
        if (!runtime.firstQueued && fixture.scenario != Scenario::PreparationCancel)
            std::wcerr << L"ListView fallback queue: " << error << L'\n';
        PostMessageW(window, kDrained, 0, 0);
        return 0;
    }
    if (message == kDrained) {
        if (!fixture.drainFencePosted) {
            // A broken busy guard could enqueue one nested action behind this
            // sentinel. Observe that bounded attempt before publishing results.
            fixture.drainFencePosted = true;
            PostMessageW(window, kDrained, 0, 0);
            return 0;
        }
        runtime.stateAfterAction = static_cast<UINT>(SendMessageW(window, LVM_GETITEMSTATE, 0,
            LVIS_SELECTED | LVIS_FOCUSED | LVIS_CUT | LVIS_DROPHILITED | LVIS_STATEIMAGEMASK));
        runtime.idAfterAction = static_cast<uint32_t>(SendMessageW(window, LVM_MAPINDEXTOID, 0, 0));
        fixture.itemCountAfter = static_cast<size_t>(SendMessageW(window, LVM_GETITEMCOUNT, 0, 0));
        wchar_t text[128]{};
        ListView_GetItemText(window, 0, 0, text, 128);
        runtime.textAfterAction = text;
        fixture.foregroundAfter = GetForegroundWindow();
        fixture.captureAfter = GetCapture();
        fixture.cloakRead = SUCCEEDED(DwmGetWindowAttribute(runtime.root.load(), DWMWA_CLOAKED,
            &fixture.cloakAfter, sizeof(fixture.cloakAfter)));
        if (fixture.scenario == Scenario::DoubleClickCapture && fixture.captureAfter == runtime.root.load()) {
            // Preserve the observed application capture as evidence, then
            // release this fixture's capture on its owning GUI thread.
            fixture.captureReleased = ReleaseCapture() != FALSE && GetCapture() == nullptr;
        }
        SetEvent(runtime.drained);
        return 0;
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    constexpr UINT activationState = LVIS_SELECTED | LVIS_FOCUSED |
        LVIS_CUT | LVIS_DROPHILITED | LVIS_STATEIMAGEMASK;
    if (runtime.interceptFinalIdentityRead && message == LVM_GETITEMSTATE &&
        wParam == 0 && lParam == activationState) {
        runtime.interceptFinalIdentityRead = false;
        fixture.finalIdentityReadState = static_cast<UINT>(result);
        fixture.finalIdentityReadIntercepted = true;
        // The control already returned its unchanged state. Invalidation now
        // happens at the final native callback before accDoDefaultAction.
        if (fixture.scenario == Scenario::FinalIdentityReplace) {
            SendMessageW(window, LVM_DELETEITEM, 0, 0);
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.pszText = const_cast<LPWSTR>(L"Component");
            SendMessageW(window, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            ListView_SetItemState(window, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        } else if (fixture.scenario == Scenario::FinalIdentityRollback) PumpRollback(fixture);
        else if (auto* agent = runtime.agent.load()) agent->CancelPopupOnSourceThread();
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ListSubclass, subclassId);
    return result;
}

void Run(Scenario scenario, DWORD mode) {
    Fixture fixture;
    fixture.scenario = scenario;
    const bool preparationCancelled = scenario == Scenario::PreparationCancel;
    const bool finalIdentityInterrupted = scenario == Scenario::FinalIdentityCancel ||
        scenario == Scenario::FinalIdentityRollback || scenario == Scenario::FinalIdentityReplace;
    const bool rollback = scenario == Scenario::DoubleClickRollback ||
        scenario == Scenario::FinalIdentityRollback;
    auto& runtime = fixture.runtime;
    runtime.standardMetadata = true;
    runtime.actionResult = scenario == Scenario::ProviderRefused ? S_FALSE :
        scenario == Scenario::ProviderFailed ? E_FAIL : DISP_E_MEMBERNOTFOUND;
    runtime.changedAction = scenario == Scenario::CustomDefault;
    runtime.replaceDuringAction = scenario == Scenario::ProviderReplace;
    runtime.renameDuringAction = scenario == Scenario::ProviderRename;
    runtime.clearStateDuringAction = scenario == Scenario::ProviderState;
    runtime.cancelDuringAction = scenario == Scenario::ProviderCancel;
    runtime.changeDefaultDuringAction = scenario == Scenario::ProviderDefault;
    runtime.reenterDuringAction = scenario == Scenario::ProviderReenter;
    std::thread gui([&] {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        runtime.thread.store(GetCurrentThreadId());
        {
            const ListActivationCommonControls controls;
            NativeListViewDoubleClick::Fixture native;
            const bool created = controls && NativeListViewDoubleClick::Create(native, mode);
            if (created) {
                fixture.native = &native;
                runtime.root.store(native.root);
                runtime.list.store(native.list);
                runtime.parent = native.root;
                SetWindowSubclass(native.root, ParentSubclass, 0xAD51, reinterpret_cast<DWORD_PTR>(&fixture));
                SetWindowSubclass(native.list, ListSubclass, 0xAD52, reinterpret_cast<DWORD_PTR>(&fixture));
                ListView_SetItemText(native.list, 0, 0, const_cast<LPWSTR>(L"Component"));
                ListView_SetItemState(native.list, 1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_SetItemState(native.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
            SetEvent(runtime.ready);
            if (created) {
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
        }
        winrt::uninit_apartment();
    });
    std::shared_ptr<Translation::SourceThreadAgent> agent;
    const auto cleanup = [&] {
        SetEvent(runtime.modalRelease);
        if (agent) { std::wstring ignored; agent->Restore(ignored); agent->Shutdown(); }
        if (runtime.thread.load()) PostThreadMessageW(runtime.thread.load(), WM_QUIT, 0, 0);
        gui.join();
    };
    const bool started = WaitForSingleObject(runtime.ready, 5000) == WAIT_OBJECT_0 && runtime.list.load();
    Check(started, "ListView native-fallback fixture did not start");
    if (!started) { cleanup(); return; }
    agent = Translation::SourceThreadAgent::Attach(runtime.root.load(), GetModuleHandleW(nullptr));
    std::wstring error;
    const bool cloaked = agent && agent->SetCloaked(true, error);
    Check(cloaked, "ListView native-fallback fixture could not attach/cloak");
    if (!cloaked) { cleanup(); return; }
    runtime.agent.store(agent.get());
    Translation::WindowSnapshot snapshot;
    snapshot.surfaceId = L"75757575-4545-6767-8989-121212121212";
    snapshot.revision = 1;
    const bool captured = agent->Capture(snapshot, error);
    const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(), [](const auto& node) {
        return node.kind == Translation::ControlKind::ListView;
    });
    const bool supported = captured && found != snapshot.nodes.end() && found->itemActivationSupported &&
        found->itemNativeIds.size() == 2 && found->selectedIndices == std::vector<int>{0} && found->focusedIndex == 0;
    if (!supported) std::wcerr << L"ListView fallback capture: " << error << L'\n';
    Check(supported, "ListView native-fallback fixture did not publish selected/focused standard metadata");
    if (!supported) { cleanup(); return; }
    runtime.node = *found;
    const bool queued = PostMessageW(runtime.list.load(), kQueue, 0, 0) != FALSE;
    if (rollback) {
        const bool entered = WaitForSingleObject(runtime.modalStarted, 2000) == WAIT_OBJECT_0;
        Check(entered, "native ListView activation did not enter the rollback probe");
        if (entered) Check(agent->Restore(error), "native ListView activation blocked whole-surface restore");
        SetEvent(runtime.modalRelease);
    }
    const bool drained = queued && WaitForSingleObject(runtime.drained, 5000) == WAIT_OBJECT_0;
    const unsigned expectedProviderCalls = preparationCancelled || finalIdentityInterrupted ? 0u : 1u;
    Check(drained && runtime.firstQueued == !preparationCancelled &&
            (!preparationCancelled || fixture.queueRefused) && runtime.calls.load() == expectedProviderCalls,
        "ListView activation did not preserve queue refusal or exact provider count across cancellation");
    if (drained) {
        const auto& native = *fixture.native;
        const auto count = [&](UINT code) {
            return std::count_if(native.notifications.begin(), native.notifications.end(),
                [&](const auto& item) { return item.code == code; });
        };
        const bool fullPair = scenario == Scenario::StandardUnavailable || scenario == Scenario::ProviderReenter ||
            scenario == Scenario::DoubleClickReenter || scenario == Scenario::PreparationReenter ||
            scenario == Scenario::PreparationReleasePump;
        const bool interruptedPair = scenario == Scenario::DoubleClickReplace ||
            scenario == Scenario::DoubleClickCancel || scenario == Scenario::DoubleClickRollback ||
            scenario == Scenario::DoubleClickCapture;
        const std::vector<UINT> expectedMessages = fullPair ? std::vector<UINT>{WM_LBUTTONDBLCLK, WM_LBUTTONUP} :
            interruptedPair ? std::vector<UINT>{WM_LBUTTONDBLCLK} : std::vector<UINT>{};
        Check(fixture.pointRead && native.mouseMessages == expectedMessages &&
                runtime.syntheticInputMessages == expectedMessages.size(),
            "ListView fallback sent an incorrect native mouse sequence after failure, mutation, or rollback");
        if (fullPair) {
            Check(count(NM_DBLCLK) == 1 && count(LVN_ITEMACTIVATE) == 1 && count(NM_CLICK) == 0 &&
                    runtime.activationNotifications == 2,
                "ListView native fallback did not produce exactly one native double-click activation");
            Check(std::all_of(native.notifications.begin(), native.notifications.end(), [&](const auto& item) {
                    return item.item == 0 && item.subItem == 0 && item.point.x == fixture.point.x &&
                        item.point.y == fixture.point.y && item.sourceThread && item.sourceMessage != 0;
                }), "ListView fallback notification did not originate in its native control at the exact target");
        } else if (interruptedPair) {
            Check(fixture.notificationMutated && count(NM_DBLCLK) == 1 && count(NM_CLICK) == 0,
                "ListView fallback did not exercise native notification cancellation/replacement");
        } else {
            Check(native.notifications.empty() && runtime.activationNotifications == 0,
                "failed ListView provider produced a native activation notification");
        }
        const bool replaced = scenario == Scenario::ProviderReplace || scenario == Scenario::DoubleClickReplace ||
            scenario == Scenario::FinalIdentityReplace;
        Check(fixture.itemCountAfter == runtime.node.itemNativeIds.size() && runtime.idAfterAction != UINT32_MAX &&
                (runtime.idAfterAction != runtime.node.itemNativeIds[0]) == replaced,
            "ListView fallback unexpectedly changed or lost the canonical native item identity");
        Check(runtime.textAfterAction == (scenario == Scenario::ProviderRename ? L"Changed" : L"Component") &&
                runtime.stateAfterAction == (scenario == Scenario::ProviderState ? 0u : LVIS_SELECTED | LVIS_FOCUSED),
            "ListView fallback changed canonical text or state beyond the provider's own operation");
        if (scenario == Scenario::ProviderReplace || scenario == Scenario::ProviderRename ||
            scenario == Scenario::ProviderState || scenario == Scenario::ProviderCancel || scenario == Scenario::ProviderDefault)
            Check(runtime.actionMutated, "ListView fallback did not exercise its provider mutation/cancellation");
        if (scenario == Scenario::ProviderReenter)
            Check(runtime.reentrantRefused, "ListView provider queued another activation while the first was running");
        if (scenario == Scenario::DoubleClickReenter)
            Check(fixture.notificationReentrantRefused, "ListView native notification queued a concurrent activation");
        if (preparationCancelled)
            Check(runtime.preparationCancelled && fixture.cloakRead && (fixture.cloakAfter & DWM_CLOAKED_APP) != 0,
                "ListView preparation cancellation did not reject the gesture while its native window stayed cloaked");
        if (scenario == Scenario::PreparationReenter)
            Check(runtime.preparationReentries == 1 && runtime.preparationReentrantRefused,
                "ListView metadata preparation admitted a nested activation or did not attempt its bounded reentry");
        if (scenario == Scenario::PreparationReleasePump)
            Check(runtime.preparationReleasePumps == 1 && !runtime.pumpDuringPreparationRelease &&
                    !runtime.actionDuringPreparationRelease && !runtime.deferredConsumedDuringPreparationRelease,
                "ListView metadata preparation did not defer activation beyond its provider's final Release message pump");
        if (finalIdentityInterrupted)
            Check(fixture.finalIdentityReadIntercepted && runtime.completedDefaultActionReads == 3 &&
                    !runtime.interceptFinalIdentityRead &&
                    fixture.finalIdentityReadState == (LVIS_SELECTED | LVIS_FOCUSED),
                "ListView final native identity invalidation did not return unchanged state after the final metadata read");
        if (scenario == Scenario::FinalIdentityCancel)
            Check(fixture.cloakRead && (fixture.cloakAfter & DWM_CLOAKED_APP) != 0,
                "ListView final native identity cancellation unexpectedly restored its window");
        if (rollback)
            Check(fixture.rollbackPumpReleased && fixture.cloakRead && (fixture.cloakAfter & DWM_CLOAKED_APP) == 0,
                "ListView rollback probe did not finish its native callback pump and restore the whole surface");
        else if (scenario == Scenario::DoubleClickCapture)
            Check(fixture.captureAssigned && !fixture.captureBefore && fixture.captureAfter == runtime.root.load() &&
                    fixture.captureReleased && fixture.foregroundAfter == fixture.foregroundBefore &&
                    fixture.cloakRead && (fixture.cloakAfter & DWM_CLOAKED_APP) != 0,
                "ListView double-click did not preserve application capture until the fixture released it");
        else
            Check(fixture.foregroundAfter == fixture.foregroundBefore && fixture.captureAfter == fixture.captureBefore,
                "ListView native fallback changed foreground or mouse capture");
        std::wcout << L"ListView fallback scenario=" << static_cast<int>(scenario) << L" mode=" << mode
            << L" providerCalls=" << runtime.calls.load() << L" mouseMessages=" << native.mouseMessages.size()
            << L" NM_DBLCLK=" << count(NM_DBLCLK) << L" LVN_ITEMACTIVATE=" << count(LVN_ITEMACTIVATE) << L'\n';
    }
    Check(!runtime.wrongThread.load(), "ListView standard metadata or provider action ran off the GUI thread");
    cleanup();
}

} // namespace ListViewActivationFallback

void TestListViewNativeActivationFallback() {
    using namespace ListViewActivationFallback;
    for (DWORD mode : {LVS_ICON, LVS_REPORT, LVS_SMALLICON, LVS_LIST}) Run(Scenario::StandardUnavailable, mode);
    for (Scenario scenario : {Scenario::ProviderRefused, Scenario::ProviderFailed, Scenario::CustomDefault,
            Scenario::ProviderReplace, Scenario::ProviderRename, Scenario::ProviderState, Scenario::ProviderCancel,
            Scenario::ProviderDefault, Scenario::ProviderReenter, Scenario::DoubleClickReplace,
            Scenario::DoubleClickCancel, Scenario::DoubleClickRollback, Scenario::DoubleClickReenter,
            Scenario::PreparationCancel, Scenario::PreparationReenter, Scenario::FinalIdentityCancel,
            Scenario::FinalIdentityRollback, Scenario::DoubleClickCapture, Scenario::PreparationReleasePump,
            Scenario::FinalIdentityReplace})
        Run(scenario, LVS_ICON);
}
