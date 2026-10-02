#pragma once

#include "OwnerDataListViewTests.h"

#include <winrt/base.h>
#include <atomic>

namespace FluentShell::Tests {
namespace OwnerDataListViewActions {

using namespace Bridge::Translation;

constexpr UINT kReset = WM_APP + 271;
constexpr UINT kReorder = WM_APP + 272;
constexpr UINT kEditCount = WM_APP + 273;
constexpr UINT kScrollReset = WM_APP + 274;

inline LRESULT CALLBACK Probe(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
    auto& fixture = *reinterpret_cast<OwnerDataListView::Fixture*>(data);
    if (message == kReset) {
        fixture.rows = {{L"First virtual row", L"Running"}, {L"Second virtual row", L"Stopped"}};
        ListView_SetItemState(fixture.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        fixture.editNotifications = 0;
        return 0;
    }
    if (message == kReorder) {
        // Application-owned data can change without touching the native count
        // or sending a message which would already have triggered reconcile.
        std::swap(fixture.rows[0], fixture.rows[1]);
        return 0;
    }
    if (message == kEditCount) return fixture.editNotifications;
    if (message == kScrollReset) {
        fixture.rows.assign(80, {L"Scrollable virtual item", L"State"});
        fixture.SetCount();
        ListView_SetItemState(fixture.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(fixture.list, 1, LVIS_SELECTED, LVIS_SELECTED);
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, Probe, id);
    return DefSubclassProc(window, message, wParam, lParam);
}

inline void TestIndexedActions(DWORD mode, void (*check)(bool, const char*)) {
    const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(ready != nullptr, "virtual action fixture could not allocate its ready event");
    if (!ready) return;
    std::atomic<DWORD> threadId{0};
    std::atomic<HWND> root{nullptr};
    std::thread gui([&] {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        threadId.store(GetCurrentThreadId());
        {
            OwnerDataListView::Fixture fixture(mode);
            if (fixture.ready) {
                SetWindowSubclass(fixture.root, Probe, 993, reinterpret_cast<DWORD_PTR>(&fixture));
                ShowWindow(fixture.root, SW_SHOWNOACTIVATE);
                // Stock legacy ListView's first paint changes the image list's
                // first-pixel alpha from 255 to 0. Complete that real native
                // initialization before publishing any action witness.
                RedrawWindow(fixture.root, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
                root.store(fixture.root);
            }
            SetEvent(ready);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        winrt::uninit_apartment();
    });
    std::shared_ptr<SourceThreadAgent> agent;
    const auto cleanup = [&] {
        if (agent) {
            std::wstring ignored;
            agent->Restore(ignored);
            agent->Shutdown();
        }
        if (threadId.load()) PostThreadMessageW(threadId.load(), WM_QUIT, 0, 0);
        gui.join();
        CloseHandle(ready);
    };
    const bool started = WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0 && root.load();
    check(started, "virtual action fixture did not start");
    if (!started) { cleanup(); return; }
    agent = SourceThreadAgent::Attach(root.load(), GetModuleHandleW(nullptr));
    std::wstring error;
    const bool attached = agent && agent->SetCloaked(true, error);
    check(attached, "virtual action agent could not attach and cloak");
    if (!attached) { cleanup(); return; }

    const auto listIn = [](const WindowSnapshot& snapshot) -> const ControlNode* {
        const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(), [](const ControlNode& node) {
            return node.kind == ControlKind::ListView;
        });
        return found == snapshot.nodes.end() ? nullptr : &*found;
    };
    for (const wchar_t* verb : {L"setSelection", L"setFocusedIndex", L"setItemText"}) {
        SendMessageW(root.load(), kReset, 0, 0);
        WindowSnapshot baseline;
        baseline.surfaceId = L"68686868-3434-5656-7878-909090909090";
        baseline.revision = 1;
        const bool captured = agent->Capture(baseline, error);
        const ControlNode* node = captured ? listIn(baseline) : nullptr;
        if (!node) std::wcerr << L"virtual action capture: " << error << L'\n';
        check(node != nullptr, "virtual action fixture did not capture its native ListView");
        if (!node) { cleanup(); return; }
        ActionRequest action;
        action.surfaceId = baseline.surfaceId;
        action.expectedRevision = baseline.revision;
        action.eventId = 1;
        action.nodeId = node->nodeId;
        action.action = verb;
        action.itemIndex = 0;
        action.integerValues = {0};
        action.text = L"Renamed requested row";
        action.expectedNativeFingerprint = SnapshotFingerprint(baseline);
        SendMessageW(root.load(), kReorder, 0, 0);
        ActionOutcome outcome;
        check(!agent->Invoke(action, outcome) && outcome.refused && !outcome.accepted,
            "an indexed virtual action retargeted a same-count native reorder");
        const ControlNode* current = listIn(outcome.snapshot);
        check(current && current->items.size() == 2 && current->items[0] == node->items[1] &&
            current->items[1] == node->items[0] && current->selectedIndices.empty() &&
            current->focusedIndex == -1 && outcome.snapshot.revision == baseline.revision + 1 &&
            outcome.snapshot.generation == baseline.generation,
            "refused virtual action did not return the untouched new canonical ordering");
        check(SendMessageW(root.load(), kEditCount, 0, 0) == 0,
            "a stale virtual rename entered the application's label-edit handler");
        if (!current) { cleanup(); return; }

        // The latest published ordering admits a new gesture. Missing witnesses
        // remain a refusal, even when the index itself is still within bounds.
        baseline = outcome.snapshot;
        action.expectedRevision = baseline.revision;
        action.expectedNativeFingerprint = 0;
        check(!agent->Invoke(action, outcome) && outcome.refused,
            "virtual indexed action ran without a published native witness");
        // Even a refusal publishes a fresh canonical snapshot. A new gesture
        // must use that snapshot's revision and witness, not the previous read.
        baseline = outcome.snapshot;
        action.expectedRevision = baseline.revision;
        action.expectedNativeFingerprint = SnapshotFingerprint(baseline);
        const bool applied = agent->Invoke(action, outcome);
        if (!applied || !outcome.accepted)
            std::wcerr << L"virtual current-order action " << action.action << L": " << outcome.error << L'\n';
        check(applied && outcome.accepted,
            "a virtual indexed action against the current ordering did not run");
        current = listIn(outcome.snapshot);
        const bool correct = current && (action.action == L"setSelection"
            ? current->selectedIndices == std::vector<int>{0}
            : action.action == L"setFocusedIndex" ? current->focusedIndex == 0
            : current->items[0] == action.text && current->items[1] == L"First virtual row");
        check(correct, "accepted virtual action did not report the application's canonical state");
    }
    if (mode == LVS_LIST) {
        SendMessageW(root.load(), kScrollReset, 0, 0);
        WindowSnapshot before;
        before.surfaceId = L"68686868-3434-5656-7878-909090909090";
        before.revision = 1;
        const bool captured = agent->Capture(before, error);
        const ControlNode* node = captured ? listIn(before) : nullptr;
        check(node && node->itemRects.size() == 80, "native scroll action fixture did not capture its items");
        if (node && node->itemRects.size() == 80) {
            ActionRequest action;
            action.surfaceId = before.surfaceId;
            action.expectedRevision = before.revision;
            action.eventId = 1;
            action.nodeId = node->nodeId;
            action.action = L"scrollBy";
            action.integerValue = 180;
            action.itemIndex = 0;
            ActionOutcome outcome;
            const bool scrolled = agent->Invoke(action, outcome);
            if (!scrolled) std::wcerr << L"virtual scroll action: " << outcome.error << L'\n';
            const ControlNode* after = scrolled ? listIn(outcome.snapshot) : nullptr;
            check(after && outcome.accepted && after->itemRects.size() == 80 &&
                after->itemRects[0].left < node->itemRects[0].left &&
                after->selectedIndices == std::vector<int>{1},
                "native scroll action failed to move the canonical viewport while preserving selection");
        }
    }
    cleanup();
}

inline void TestActionAdmission(void (*check)(bool, const char*)) {
    WindowSnapshot snapshot;
    ControlNode node;
    node.kind = ControlKind::ListView;
    node.nodeId = 71;
    node.visible = node.enabled = true;
    node.listViewMode = L"list";
    node.items = {L"First", L"Second"};
    snapshot.nodes = {node};
    ActionRequest action;
    action.nodeId = node.nodeId;
    action.action = L"setFocusedIndex";
    std::wstring error;
    for (const int index : {-2, -1, 0, 1, 2}) {
        action.itemIndex = index;
        check(ValidateActionForSnapshot(action, snapshot, error) == (index >= -1 && index < 2),
            "ListView focus action did not validate its item bounds");
    }
    snapshot.nodes[0].items.clear();
    action.itemIndex = -1;
    check(ValidateActionForSnapshot(action, snapshot, error), "empty ListView could not clear native focus");
    action.itemIndex = 0;
    check(!ValidateActionForSnapshot(action, snapshot, error), "empty ListView admitted a focused item");
    snapshot.nodes[0] = node;
    action.action = L"scrollBy";
    action.itemIndex = 0;
    for (const wchar_t* mode : {L"report", L"largeIcon", L"smallIcon", L"list"}) {
        snapshot.nodes[0].listViewMode = mode;
        check(ValidateActionForSnapshot(action, snapshot, error) == (std::wstring_view(mode) != L"report"),
            "native scroll action did not respect the ListView presentation mode");
    }
    snapshot.nodes[0].listViewMode = L"tile";
    check(!ValidateActionForSnapshot(action, snapshot, error), "native scroll action admitted an unsupported view mode");
    snapshot.nodes[0].listViewMode = L"list";
    for (const int delta : {-Bridge::Ipc::kMaxCoordinate, Bridge::Ipc::kMaxCoordinate}) {
        action.integerValue = action.itemIndex = delta;
        check(ValidateActionForSnapshot(action, snapshot, error), "native scroll action rejected a bounded delta");
    }
    action.integerValue = Bridge::Ipc::kMaxCoordinate + 1;
    action.itemIndex = 0;
    check(!ValidateActionForSnapshot(action, snapshot, error), "native scroll action admitted an excessive horizontal delta");
    action.integerValue = 0;
    action.itemIndex = -Bridge::Ipc::kMaxCoordinate - 1;
    check(!ValidateActionForSnapshot(action, snapshot, error), "native scroll action admitted an excessive vertical delta");
    snapshot.nodes[0].kind = ControlKind::Button;
    action.itemIndex = 0;
    check(!ValidateActionForSnapshot(action, snapshot, error), "native scroll action targeted a non-ListView control");
    action.action = L"setFocusedIndex";
    check(!ValidateActionForSnapshot(action, snapshot, error), "item focus action targeted a non-ListView control");
    snapshot.nodes[0] = node;
    snapshot.nodes[0].itemNativeIds = {0, 7};
    snapshot.nodes[0].itemActivationSupported = true;
    action.action = L"activateItem";
    for (const int index : {-2, -1, 0, 1, 2}) {
        action.itemIndex = index;
        check(ValidateActionForSnapshot(action, snapshot, error) == (index >= 0 && index < 2),
            "native activation admitted an invalid item index");
    }
}

} // namespace OwnerDataListViewActions

inline void TestOwnerDataListViewActions(void (*check)(bool, const char*)) {
    OwnerDataListViewActions::TestActionAdmission(check);
    OwnerDataListViewActions::TestIndexedActions(LVS_REPORT, check);
    OwnerDataListViewActions::TestIndexedActions(LVS_LIST, check);
}

} // namespace FluentShell::Tests
