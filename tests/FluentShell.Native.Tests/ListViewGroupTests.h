#pragma once
#include "ListViewModeRegressionTests.h"
#include <fstream>
#include <iterator>

namespace FluentShell::Tests {
inline void TestListViewGroups(void (*check)(bool, const char*)) {
    using namespace Bridge::Translation;
    ListViewModes::Fixture fixture(LVS_REPORT);
    check(fixture.ready, "grouped ListView fixture failed");
    if (!fixture.ready) return;
    auto addGroup = [&](int id, const wchar_t* label, bool collapsed) {
        LVGROUP group{ sizeof(group) };
        group.mask = LVGF_GROUPID | LVGF_HEADER | LVGF_FOOTER | LVGF_STATE;
        group.iGroupId = id;
        group.pszHeader = const_cast<LPWSTR>(label);
        group.pszFooter = const_cast<LPWSTR>(L"Native footer");
        group.stateMask = LVGS_COLLAPSIBLE | LVGS_COLLAPSED;
        group.state = LVGS_COLLAPSIBLE | (collapsed ? LVGS_COLLAPSED : 0);
        return SendMessageW(fixture.list, LVM_INSERTGROUP, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&group)) != -1;
    };
    const bool inserted = addGroup(17, L"First group", false) && addGroup(7, L"Second group", true);
    check(inserted, "native groups could not be inserted");
    if (!inserted) return;
    for (int row = 0; row < 2; ++row) {
        // Assign by the native label: group assignment may reorder the v6
        // control's storage, so a previous numeric row is not an identity.
        LVFINDINFOW find{}; find.flags = LVFI_STRING; find.psz = row == 0 ? L"First" : L"Second";
        const int at = static_cast<int>(SendMessageW(fixture.list, LVM_FINDITEMW,
            static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&find)));
        LVITEMW item{}; item.mask = LVIF_GROUPID; item.iItem = at; item.iGroupId = row == 0 ? 7 : 17;
        check(at >= 0, "native group row identity disappeared");
        if (at < 0) return;
        check(SendMessageW(fixture.list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&item)) != FALSE, "native row group assignment failed");
    }
    SendMessageW(fixture.list, LVM_ENABLEGROUPVIEW, TRUE, 0);
    ControlNode node;
    std::wstring error;
    const bool captured = fixture.Capture(node, error);
    if (!captured) std::wcerr << L"group capture: " << error << L'\n';
    check(captured, "native grouped ListView was refused");
    if (!captured) return;
    const bool groupsMatch = node.groupViewEnabled && node.listViewGroups.size() == 2 &&
        node.listViewGroups[0].groupId == 17 && node.listViewGroups[1].groupId == 7 &&
        node.items.size() == 2 && node.itemGroupIds.size() == node.items.size() &&
        std::equal(node.itemGroupIds.begin(), node.itemGroupIds.end(), node.items.begin(),
            [](int id, const std::wstring& label) { return id == (label == L"First" ? 7 : 17); });
    if (!groupsMatch) {
        std::wcerr << L"group fixture display order:";
        for (const auto& group : node.listViewGroups) std::wcerr << L' ' << group.groupId;
        std::wcerr << L"; rows:";
        for (size_t index = 0; index < (std::min)(node.items.size(), node.itemGroupIds.size()); ++index)
            std::wcerr << L" [" << node.items[index] << L"=" << node.itemGroupIds[index] << L"]";
        std::wcerr << L'\n';
    }
    check(groupsMatch, "group order or native membership was lost");
    if (!groupsMatch) return;
    const auto groupRowVisible = [&](int groupId, bool visible) {
        if (node.itemRects.size() != node.items.size() ||
            node.itemGroupIds.size() != node.items.size()) return false;
        for (size_t index = 0; index < node.items.size(); ++index)
            if (node.itemGroupIds[index] == groupId)
                return (IsRectEmpty(&node.itemRects[index]) == FALSE) == visible;
        return false;
    };
    check(node.listViewGroups[1].collapsed && node.listViewGroups[0].footer == L"Native footer" &&
        groupRowVisible(7, false) && groupRowVisible(17, true),
        "collapsed group rows or group labels lost native state");
    check(SetListViewGroupExpanded(fixture.list, 7, true), "native group expand action failed");
    check(fixture.Capture(node, error) && node.listViewGroups.size() == 2 &&
        !node.listViewGroups[1].collapsed && groupRowVisible(7, true),
        "expanded native group was not recaptured");
    check(SetListViewGroupExpanded(fixture.list, 7, false), "native group collapse action failed");
    check(!SetListViewGroupExpanded(fixture.list, 999, true), "unknown native group accepted an action");
    WindowSnapshot snapshot; snapshot.nodes.push_back(node);
    ActionRequest action; action.nodeId = node.nodeId; action.action = L"setGroupExpanded"; action.groupId = 7;
    check(ValidateActionForSnapshot(action, snapshot, error), "captured native group action validation failed");
    action.groupId = 999;
    check(!ValidateActionForSnapshot(action, snapshot, error), "snapshot accepted an unknown native group ID");
    const auto json = SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    std::string membership = "\"itemGroupIds\":[";
    for (size_t index = 0; index < node.itemGroupIds.size(); ++index) {
        if (index) membership += ',';
        membership += '"' + std::to_string(node.itemGroupIds[index]) + '"';
    }
    membership += ']';
    check(json.find("\"groupId\":\"17\"") != std::string::npos &&
        json.find(membership) != std::string::npos,
        "native groups or membership IDs were not serialized canonically");
    std::ifstream shared("tests/ProtocolFixtures/window.open.list-view-groups.json", std::ios::binary);
    const std::string sharedText((std::istreambuf_iterator<char>(shared)), std::istreambuf_iterator<char>());
    check(sharedText.find("\"groupId\": \"17\"") != std::string::npos &&
        sharedText.find("\"itemGroupIds\": [\"7\", \"17\"]") != std::string::npos,
        "shared grouped ListView fixture is missing native group identities");
    const std::string actionPrefix = "{\"messageType\":\"action.invoke\",\"sessionNonce\":\"00112233445566778899aabbccddeeff\","
        "\"surfaceId\":\"4f17d4bb-b2bf-42b8-a334-2f9ad8d54d42\",\"nodeId\":\"71\",\"eventId\":\"1\",\"expectedRevision\":\"7\","
        "\"action\":\"setGroupExpanded\",\"value\":{\"expanded\":true,\"groupId\":";
    check(ParseActionInvoke(actionPrefix + "\"-7\"}}", L"00112233445566778899aabbccddeeff", action, error) && action.groupId == -7 && action.booleanValue,
        "native codec rejected a signed group ID");
    for (const char* id : {"+7", "07", "-0", "-1", "-2", "2147483648"})
        check(!ParseActionInvoke(actionPrefix + "\"" + id + "\"}}", L"00112233445566778899aabbccddeeff", action, error),
            "native group action accepted a noncanonical or reserved identity");
    SendMessageW(fixture.list, LVM_ENABLEGROUPVIEW, FALSE, 0);
    check(fixture.Capture(node, error) && !node.groupViewEnabled && node.listViewGroups.empty() && node.itemGroupIds.empty() && node.itemRects.empty(),
        "disabling native groups retained grouped projection state");
}
} // namespace FluentShell::Tests
