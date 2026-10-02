#pragma once

#include "../../src/Bridge/Translation/ControlAdapters.h"
#include "../../src/Bridge/Translation/WindowCapture.h"

#include <commctrl.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <array>
#include <iostream>
#include <string>
#include <vector>

namespace FluentShell::Tests {
namespace ToolbarRadioGroups {

using namespace Bridge::Translation;

// The parent stays hidden: native toolbar messages provide all fixture state
// and geometry without showing a window or sending desktop input.
struct Fixture final {
    HWND root = nullptr;
    HWND toolbar = nullptr;
    bool ready = false;

    Fixture() {
        root = CreateWindowExW(0, L"Static", L"toolbar-radio-group-regression",
            WS_OVERLAPPEDWINDOW, 0, 0, 1520, 180, nullptr, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (!root) return;
        toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, L"",
            WS_CHILD | WS_VISIBLE | CCS_NOPARENTALIGN | CCS_NORESIZE |
            CCS_NODIVIDER | TBSTYLE_FLAT | TBSTYLE_LIST,
            0, 0, 4000, 160, root, reinterpret_cast<HMENU>(994),
            GetModuleHandleW(nullptr), nullptr);
        if (!toolbar) return;
        SendMessageW(toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        SendMessageW(toolbar, CCM_SETUNICODEFORMAT, TRUE, 0);
        ready = true;
    }

    ~Fixture() {
        if (root) DestroyWindow(root);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    bool Add(int commandId, BYTE style, BYTE state, const wchar_t* label) const {
        TBBUTTON button{};
        button.iBitmap = style == BTNS_SEP ? 8 : I_IMAGENONE;
        button.idCommand = commandId;
        button.fsStyle = style == BTNS_SEP ? style
            : static_cast<BYTE>(style | BTNS_AUTOSIZE | BTNS_SHOWTEXT);
        button.fsState = state;
        button.iString = reinterpret_cast<INT_PTR>(label);
        return SendMessageW(toolbar, TB_ADDBUTTONSW, 1,
            reinterpret_cast<LPARAM>(&button)) != FALSE;
    }

    bool Capture(ControlNode& node, std::wstring& error) const {
        SendMessageW(toolbar, TB_AUTOSIZE, 0, 0);
        node.kind = ControlKind::Toolbar;
        node.nodeId = 74;
        node.generation = 1;
        node.hwnd = toolbar;
        node.visible = true;
        node.enabled = true;
        node.style = static_cast<uint64_t>(GetWindowLongPtrW(toolbar, GWL_STYLE));
        error.clear();
        return CaptureControlDetail(toolbar, node, error);
    }

    bool SetState(int commandId, BYTE state) const {
        return SendMessageW(toolbar, TB_SETSTATE, commandId, MAKELPARAM(state, 0)) != FALSE;
    }

    bool SetStyle(int commandId, BYTE style) const {
        TBBUTTONINFOW info{};
        info.cbSize = sizeof(info);
        info.dwMask = TBIF_STYLE;
        info.fsStyle = static_cast<BYTE>(style | BTNS_AUTOSIZE | BTNS_SHOWTEXT);
        return SendMessageW(toolbar, TB_SETBUTTONINFOW, commandId,
            reinterpret_cast<LPARAM>(&info)) != FALSE;
    }
};

inline void CheckNativeStates(
    const Fixture& fixture, const ControlNode& node, void (*check)(bool, const char*)) {
    for (size_t index = 0; index < node.toolbarItems.size(); ++index) {
        TBBUTTON native{};
        const bool read = SendMessageW(fixture.toolbar, TB_GETBUTTON, index,
            reinterpret_cast<LPARAM>(&native)) != FALSE;
        check(read && node.toolbarItems[index].checked ==
            ((native.fsState & TBSTATE_CHECKED) != 0),
            "toolbar radio projection changed the checked state reported by the native button");
    }
}

inline void CheckWire(const ControlNode& node, void (*check)(bool, const char*)) {
    WindowSnapshot snapshot;
    snapshot.nodes = { node };
    try {
        const auto json = SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
        const auto root = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(json));
        const auto items = root.GetNamedObject(L"window").GetNamedArray(L"nodes")
            .GetObjectAt(0).GetNamedArray(L"toolbarItems");
        check(items.Size() == node.toolbarItems.size(),
            "toolbar radio wire payload changed the native item count");
        if (items.Size() != node.toolbarItems.size()) return;
        for (uint32_t index = 0; index < items.Size(); ++index) {
            const auto wire = items.GetObjectAt(index);
            const auto& native = node.toolbarItems[index];
            const wchar_t* kind = native.kind == ToolbarItemKind::RadioButton ? L"radioButton"
                : native.kind == ToolbarItemKind::ToggleButton ? L"toggleButton"
                : native.kind == ToolbarItemKind::Separator ? L"separator" : L"pushButton";
            check(wire.GetNamedString(L"kind") == kind &&
                wire.HasKey(L"radioGroup") == (native.kind == ToolbarItemKind::RadioButton) &&
                wire.GetNamedNumber(L"radioGroup", 0.0) == native.radioGroup &&
                wire.GetNamedNumber(L"commandId") == native.commandId &&
                wire.GetNamedBoolean(L"checked") == native.checked &&
                wire.GetNamedBoolean(L"hidden") == native.hidden,
                "toolbar radio kind, group, command identity or canonical state was lost on the wire");
        }
    } catch (...) {
        check(false, "toolbar radio wire payload omitted required typed item fields");
    }
}

inline void CheckCommands(const ControlNode& node, void (*check)(bool, const char*)) {
    WindowSnapshot snapshot;
    snapshot.nodes = { node };
    ActionRequest action;
    action.nodeId = node.nodeId;
    action.action = L"toolbarCommand";
    std::wstring error;
    for (const uint32_t command : { 101u, 103u, 105u, 107u, 112u }) {
        action.menuCommandId = command;
        check(ValidateActionForSnapshot(action, snapshot, error),
            "enabled toolbar radio button did not retain the toolbarCommand action");
    }
    for (const uint32_t command : { 102u, 108u, 65535u }) {
        action.menuCommandId = command;
        check(!ValidateActionForSnapshot(action, snapshot, error),
            "toolbarCommand accepted a hidden, disabled or unknown radio button");
    }
    for (const uint32_t command : { 104u, 109u, 110u, 111u }) {
        action.menuCommandId = command;
        check(ValidateActionForSnapshot(action, snapshot, error),
            "adding toolbar radio groups changed ordinary button or toggle commands");
    }
    action.action = L"setCheck";
    action.menuCommandId = 101;
    check(!ValidateActionForSnapshot(action, snapshot, error),
        "toolbar radio button admitted setCheck instead of its native command action");
}

inline void CheckFingerprint(const ControlNode& node, void (*check)(bool, const char*)) {
    WindowSnapshot snapshot;
    snapshot.nodes = { node };
    const auto fingerprint = SnapshotFingerprint(snapshot);
    snapshot.nodes[0].toolbarItems[0].radioGroup = 2;
    check(SnapshotFingerprint(snapshot) != fingerprint,
        "toolbar radio group membership was omitted from the snapshot fingerprint");
    snapshot.nodes[0] = node;
    snapshot.nodes[0].toolbarItems[0].checked = !node.toolbarItems[0].checked;
    check(SnapshotFingerprint(snapshot) != fingerprint,
        "toolbar radio checked state was omitted from the snapshot fingerprint");
}

inline void TestNativeGroups(void (*check)(bool, const char*)) {
    Fixture fixture;
    check(fixture.ready, "toolbar radio group fixture was not created");
    if (!fixture.ready) return;
    const bool inserted =
        fixture.Add(101, BTNS_CHECKGROUP, TBSTATE_ENABLED, L"Large") &&
        fixture.Add(102, BTNS_CHECKGROUP, TBSTATE_ENABLED | TBSTATE_HIDDEN | TBSTATE_CHECKED,
            L"Hidden") &&
        fixture.Add(103, BTNS_CHECKGROUP, TBSTATE_ENABLED, L"Small") &&
        fixture.Add(104, BTNS_BUTTON, TBSTATE_ENABLED, L"Refresh") &&
        fixture.Add(105, BTNS_CHECKGROUP, TBSTATE_ENABLED | TBSTATE_CHECKED, L"Ascending") &&
        fixture.Add(106, BTNS_CHECKGROUP, TBSTATE_ENABLED, L"Descending") &&
        fixture.Add(0, BTNS_SEP, TBSTATE_ENABLED, L"") &&
        fixture.Add(107, BTNS_CHECKGROUP, TBSTATE_ENABLED | TBSTATE_CHECKED, L"Left") &&
        fixture.Add(108, BTNS_CHECKGROUP, 0, L"Right") &&
        fixture.Add(109, BTNS_CHECK, TBSTATE_ENABLED | TBSTATE_CHECKED, L"Toggle one") &&
        fixture.Add(110, BTNS_CHECK, TBSTATE_ENABLED | TBSTATE_CHECKED, L"Toggle two") &&
        fixture.Add(111, BTNS_BUTTON, TBSTATE_ENABLED | TBSTATE_CHECKED, L"Application latch") &&
        fixture.Add(112, BTNS_CHECKGROUP, TBSTATE_ENABLED, L"Final");
    check(inserted, "toolbar radio group fixture buttons were not inserted");
    if (!inserted) return;

    ControlNode node;
    std::wstring error;
    const bool captured = fixture.Capture(node, error);
    if (!captured) std::wcerr << L"Toolbar radio group capture: " << error << L'\n';
    check(captured, "native toolbar CHECKGROUP buttons were rejected");
    constexpr std::array<int, 13> groups{ 1, 1, 1, 0, 2, 2, 0, 3, 3, 0, 0, 0, 4 };
    check(captured && node.toolbarItems.size() == groups.size(),
        "toolbar radio capture omitted a native item or hidden group member");
    if (!captured || node.toolbarItems.size() != groups.size()) return;
    for (size_t index = 0; index < groups.size(); ++index) {
        const auto& item = node.toolbarItems[index];
        check(item.radioGroup == groups[index] &&
            (item.kind == ToolbarItemKind::RadioButton) == (groups[index] != 0),
            "toolbar radio run identities ignored native order or a non-group boundary");
    }
    check(node.toolbarItems[1].hidden && node.toolbarItems[1].checked &&
        !node.toolbarItems[0].checked && !node.toolbarItems[2].checked,
        "a hidden checked radio member was removed, split its group or selected a visible peer");
    check(node.toolbarItems[3].kind == ToolbarItemKind::PushButton &&
        node.toolbarItems[6].kind == ToolbarItemKind::Separator &&
        node.toolbarItems[9].kind == ToolbarItemKind::ToggleButton &&
        node.toolbarItems[10].kind == ToolbarItemKind::ToggleButton &&
        node.toolbarItems[9].checked && node.toolbarItems[10].checked &&
        node.toolbarItems[11].kind == ToolbarItemKind::PushButton && node.toolbarItems[11].checked,
        "toolbar radio support changed separators, independent toggles or application-owned latches");
    CheckNativeStates(fixture, node, check);
    CheckWire(node, check);
    CheckCommands(node, check);
    CheckFingerprint(node, check);

    // An owner may clear a whole group, or set state directly during an update.
    // Capture must preserve the control's current bits without choosing a member.
    check(fixture.SetState(101, TBSTATE_ENABLED) &&
        fixture.SetState(102, TBSTATE_ENABLED | TBSTATE_HIDDEN) &&
        fixture.SetState(103, TBSTATE_ENABLED),
        "native toolbar radio state could not be cleared");
    const bool cleared = fixture.Capture(node, error);
    check(cleared, "toolbar radio recapture failed after native state changed");
    if (!cleared || node.toolbarItems.size() != groups.size()) return;
    check(!node.toolbarItems[0].checked && !node.toolbarItems[1].checked &&
        !node.toolbarItems[2].checked && node.toolbarItems[0].radioGroup == 1 &&
        node.toolbarItems[1].radioGroup == 1 && node.toolbarItems[2].radioGroup == 1,
        "toolbar radio recapture kept stale checks or inferred a selected member");
    CheckNativeStates(fixture, node, check);

    check(fixture.SetState(101, TBSTATE_ENABLED | TBSTATE_CHECKED) &&
        fixture.SetState(103, TBSTATE_ENABLED | TBSTATE_CHECKED),
        "native toolbar radio checked bits could not be updated");
    const bool conflicting = fixture.Capture(node, error);
    check(!conflicting && error.find(L"multiple checked") != std::wstring::npos,
        "toolbar radio capture admitted conflicting checked members in one group");
    check(fixture.SetState(101, TBSTATE_ENABLED),
        "native toolbar conflicting radio state could not be cleared");
    const bool updated = fixture.Capture(node, error);
    check(updated, "toolbar radio recapture rejected application-updated native state");
    if (!updated || node.toolbarItems.size() != groups.size()) return;
    check(!node.toolbarItems[0].checked && !node.toolbarItems[1].checked &&
        node.toolbarItems[2].checked,
        "toolbar radio recapture did not replace the earlier checked member");
    CheckNativeStates(fixture, node, check);
    CheckWire(node, check);
}

inline void TestBareGroupRejection(void (*check)(bool, const char*)) {
    Fixture fixture;
    check(fixture.ready, "bare toolbar GROUP rejection fixture was not created");
    if (!fixture.ready) return;
    check(fixture.Add(121, BTNS_GROUP, TBSTATE_ENABLED, L"Bare group"),
        "bare toolbar GROUP fixture button was not inserted");
    TBBUTTON native{};
    check(SendMessageW(fixture.toolbar, TB_GETBUTTON, 0, reinterpret_cast<LPARAM>(&native)) &&
        (native.fsStyle & (BTNS_CHECK | BTNS_GROUP)) == BTNS_GROUP,
        "bare toolbar GROUP fixture did not retain its native style");
    ControlNode node;
    std::wstring error;
    check(!fixture.Capture(node, error) &&
        (error.find(L"group") != std::wstring::npos || error.find(L"BTNS_GROUP") != std::wstring::npos),
        "a toolbar BTNS_GROUP button without BTNS_CHECK was admitted as a radio button");
}

inline bool ReadNativeStates(HWND toolbar, std::vector<BYTE>& states) {
    states.clear();
    const LRESULT count = SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0);
    if (count <= 0) return false;
    for (LRESULT index = 0; index < count; ++index) {
        TBBUTTON native{};
        if (!SendMessageW(toolbar, TB_GETBUTTON, index, reinterpret_cast<LPARAM>(&native)))
            return false;
        states.push_back(native.fsState);
    }
    return true;
}

struct CommandObserver final {
    HWND root = nullptr;
    HWND toolbar = nullptr;
    bool installed = false;
    bool readAllButtons = false;
    int commandCount = 0;
    uint32_t commandId = 0;
    WORD notification = 0;
    HWND source = nullptr;
    std::vector<uint32_t> checkedCommands;

    CommandObserver(HWND parent, HWND control) : root(parent), toolbar(control) {
        installed = SetWindowSubclass(root, Observe, 995,
            reinterpret_cast<DWORD_PTR>(this)) != FALSE;
    }

    ~CommandObserver() {
        if (installed) RemoveWindowSubclass(root, Observe, 995);
    }

    CommandObserver(const CommandObserver&) = delete;
    CommandObserver& operator=(const CommandObserver&) = delete;

    static LRESULT CALLBACK Observe(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR reference) {
        auto* observation = reinterpret_cast<CommandObserver*>(reference);
        if (message == WM_COMMAND) {
            ++observation->commandCount;
            observation->commandId = LOWORD(wParam);
            observation->notification = HIWORD(wParam);
            observation->source = reinterpret_cast<HWND>(lParam);
            observation->checkedCommands.clear();
            const LRESULT count = SendMessageW(observation->toolbar, TB_BUTTONCOUNT, 0, 0);
            observation->readAllButtons = count > 0;
            for (LRESULT index = 0; index < count; ++index) {
                TBBUTTON native{};
                if (!SendMessageW(observation->toolbar, TB_GETBUTTON, index,
                        reinterpret_cast<LPARAM>(&native))) {
                    observation->readAllButtons = false;
                    break;
                }
                if ((native.fsState & TBSTATE_CHECKED) != 0)
                    observation->checkedCommands.push_back(static_cast<uint32_t>(native.idCommand));
            }
        }
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (message == WM_NCDESTROY)
            RemoveWindowSubclass(window, Observe, subclassId);
        return result;
    }
};

inline void CheckRejectedWithoutMutation(
    const Fixture& fixture, const ControlNode& snapshot, uint32_t commandId,
    void (*check)(bool, const char*), const char* message) {
    std::vector<BYTE> before;
    std::vector<BYTE> after;
    std::wstring error;
    const bool readBefore = ReadNativeStates(fixture.toolbar, before);
    const bool applied = ApplyToolbarCheckState(fixture.toolbar, snapshot, commandId, error);
    const bool readAfter = ReadNativeStates(fixture.toolbar, after);
    check(readBefore && !applied && !error.empty() && readAfter && before == after, message);
}

inline void TestNativeCommandState(void (*check)(bool, const char*)) {
    Fixture fixture;
    check(fixture.ready, "toolbar radio command fixture was not created");
    if (!fixture.ready) return;
    const bool inserted =
        fixture.Add(201, BTNS_CHECKGROUP, TBSTATE_ENABLED, L"First") &&
        fixture.Add(202, BTNS_CHECKGROUP, TBSTATE_ENABLED | TBSTATE_HIDDEN | TBSTATE_CHECKED,
            L"Hidden selected") &&
        fixture.Add(203, BTNS_CHECKGROUP, TBSTATE_ENABLED, L"Target") &&
        fixture.Add(204, BTNS_BUTTON, TBSTATE_ENABLED, L"Boundary") &&
        fixture.Add(205, BTNS_CHECKGROUP, TBSTATE_ENABLED | TBSTATE_CHECKED, L"Other group") &&
        fixture.Add(206, BTNS_CHECKGROUP, TBSTATE_ENABLED, L"Other peer") &&
        fixture.Add(0, BTNS_SEP, TBSTATE_ENABLED, L"") &&
        fixture.Add(207, BTNS_CHECK, TBSTATE_ENABLED | TBSTATE_CHECKED, L"Independent toggle") &&
        fixture.Add(208, BTNS_BUTTON, TBSTATE_ENABLED | TBSTATE_CHECKED, L"Application latch");
    check(inserted, "toolbar radio command buttons were not inserted");
    if (!inserted) return;
    ControlNode node;
    std::wstring error;
    const bool captured = fixture.Capture(node, error);
    check(captured && node.toolbarItems.size() == 9,
        "toolbar radio command fixture capture failed");
    if (!captured || node.toolbarItems.size() != 9) return;
    CommandObserver observation(fixture.root, fixture.toolbar);
    check(observation.installed, "toolbar command observer was not installed");
    if (!observation.installed) return;

    const bool applied = ApplyToolbarCheckState(fixture.toolbar, node, 203, error);
    check(applied, "toolbar radio command did not update native group state");
    check(observation.commandCount == 0,
        "applying toolbar radio check state independently dispatched a command");
    if (!applied) return;
    SendMessageW(fixture.root, WM_COMMAND, MAKEWPARAM(203, 0),
        reinterpret_cast<LPARAM>(fixture.toolbar));
    check(observation.commandCount == 1 && observation.commandId == 203 &&
        observation.notification == 0 && observation.source == fixture.toolbar &&
        observation.readAllButtons &&
        observation.checkedCommands == std::vector<uint32_t>{203, 205, 207, 208},
        "native WM_COMMAND observed stale radio checks or changes outside the selected group");
    const bool selected = fixture.Capture(node, error);
    check(selected && node.toolbarItems.size() == 9 &&
        !node.toolbarItems[0].checked && !node.toolbarItems[1].checked && node.toolbarItems[2].checked,
        "native toolbar radio command state was not authoritative on recapture");
    if (!selected || node.toolbarItems.size() != 9) return;

    const bool reapplied = ApplyToolbarCheckState(fixture.toolbar, node, 203, error);
    check(reapplied, "clicking the selected toolbar radio button was rejected");
    if (reapplied) {
        SendMessageW(fixture.root, WM_COMMAND, MAKEWPARAM(203, 0),
            reinterpret_cast<LPARAM>(fixture.toolbar));
        check(observation.commandCount == 2 && observation.readAllButtons &&
            observation.checkedCommands == std::vector<uint32_t>{203, 205, 207, 208},
            "clicking an already-selected toolbar radio button toggled it off");
    }

    auto malformed = node;
    malformed.toolbarItems[2].radioGroup = 0;
    CheckRejectedWithoutMutation(fixture, malformed, 203, check,
        "malformed snapshot radio group was accepted or changed native state");
    malformed = node;
    malformed.toolbarItems[1].radioGroup = 2;
    CheckRejectedWithoutMutation(fixture, malformed, 203, check,
        "inconsistent snapshot radio peer membership was accepted or changed native state");

    check(fixture.SetStyle(202, BTNS_CHECK), "hidden radio peer style could not be changed");
    CheckRejectedWithoutMutation(fixture, node, 203, check,
        "a live radio run split at a hidden peer was accepted or changed native state");
    check(fixture.SetStyle(202, BTNS_CHECKGROUP), "hidden radio peer style could not be restored");
    check(fixture.SetStyle(204, BTNS_CHECKGROUP), "radio group boundary style could not be changed");
    CheckRejectedWithoutMutation(fixture, node, 203, check,
        "live radio groups merged across a button boundary were accepted or changed native state");
    check(fixture.SetStyle(204, BTNS_BUTTON), "radio group boundary style could not be restored");
    check(fixture.SetStyle(203, BTNS_GROUP), "radio command target style could not be changed");
    CheckRejectedWithoutMutation(fixture, node, 203, check,
        "a live radio target changed to bare GROUP was accepted or changed native state");
}

} // namespace ToolbarRadioGroups

inline void TestToolbarRadioGroups(void (*check)(bool, const char*)) {
    ToolbarRadioGroups::TestNativeGroups(check);
    ToolbarRadioGroups::TestBareGroupRejection(check);
    ToolbarRadioGroups::TestNativeCommandState(check);
}

} // namespace FluentShell::Tests
