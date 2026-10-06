#pragma once

#include "../../src/Bridge/Translation/DirectUiEngine.h"

namespace FluentShell::Tests {
namespace DirectUiDetailEvidence {
using namespace Bridge::Translation;

inline ImageListEntry Pixel(uint8_t value) {
    return { 1, 1, L"bgra8-premultiplied", {value, value, value, 255} };
}

inline void CheckRefresh(const ControlNode& detail, void (*check)(bool, const char*)) {
    DirectUiSlot slot;
    slot.semanticKey = L"typed-control";
    slot.kind = detail.kind;
    slot.virtualSource = false;
    slot.captureDetail = true;
    DirectUiWindowProfile profile;
    profile.adapterId = L"microsoft.windows.directui.semantic.v1";
    profile.pageId = L"semantic-v1";
    profile.slots = &slot;
    profile.slotCount = 1;
    DirectUiNativeEvidence native;
    native.slotWindows.resize(1);
    auto& backing = native.slotWindows.front();
    backing.hwnd = reinterpret_cast<HWND>(0x101);
    backing.generation = 1;
    backing.hasDetail = true;
    backing.detail = detail;
    WindowSnapshot snapshot;
    ControlNode node;
    node.nodeId = DirectUiSemanticNodeId(profile.adapterId, slot.semanticKey);
    node.kind = slot.kind;
    node.semanticKey = slot.semanticKey;
    snapshot.nodes.push_back(node);
    std::unordered_map<uint64_t, DirectUiActionBinding> bindings;
    std::wstring error;
    check(RefreshDirectUiSnapshotFromNative(profile, native, snapshot, bindings, error),
        "DirectUI typed refresh rejected unchanged composite evidence");
    DirectUiWindowEvidence projected;
    projected.hasDetail = true;
    projected.detail = snapshot.nodes.front();
    check(SameDirectUiDetailShape(backing, projected) && SameDirectUiDetailContent(backing, projected),
        "DirectUI refresh dropped fields from its backing control's typed contract");
}
} // namespace DirectUiDetailEvidence

inline void TestDirectUiDetailEvidence(void (*check)(bool, const char*)) {
    using namespace DirectUiDetailEvidence;
    DirectUiWindowEvidence before;
    before.hasDetail = true;
    before.detail.kind = ControlKind::Toolbar;
    ToolbarItemSnapshot button;
    button.hotImage = Pixel(1);
    button.disabledImage = Pixel(2);
    button.pressedImage = Pixel(3);
    before.detail.toolbarItems.push_back(button);
    auto after = before;
    check(SameDirectUiDetailContent(before, after), "equal DirectUI toolbar evidence was rejected");
    after.detail.toolbarItems[0].hotImage->imageData[0]++;
    check(!SameDirectUiDetailContent(before, after), "DirectUI bracket ignored hot toolbar pixels");
    after = before;
    after.detail.toolbarItems[0].disabledImage.reset();
    check(!SameDirectUiDetailContent(before, after), "DirectUI bracket ignored disabled image removal");
    after = before;
    after.detail.toolbarItems[0].pressedImage->imageWidth++;
    check(!SameDirectUiDetailContent(before, after), "DirectUI bracket ignored pressed image dimensions");
    after = before;
    after.detail.toolbarItems[0].paintedFace = true;
    check(!SameDirectUiDetailContent(before, after), "DirectUI bracket ignored painted toolbar face changes");
    CheckRefresh(before.detail, check);

    before.detail = {};
    before.detail.kind = ControlKind::ComboBox;
    before.detail.items = {L"choice"};
    before.detail.ownerDrawCombo.emplace();
    before.detail.ownerDrawCombo->mode = L"fixed";
    before.detail.ownerDrawCombo->items.push_back({Pixel(10), Pixel(20)});
    before.detail.ownerDrawCombo->selectionFace = Pixel(30);
    after = before;
    after.detail.ownerDrawCombo->items[0].selected.imageData[0]++;
    check(!SameDirectUiDetailContent(before, after), "DirectUI bracket ignored selected owner-draw pixels");
    after = before;
    after.detail.ownerDrawCombo->selectionFace.reset();
    check(!SameDirectUiDetailContent(before, after), "DirectUI bracket ignored owner-draw selection face removal");
    after = before;
    after.detail.ownerDrawCombo->mode = L"variable";
    check(!SameDirectUiDetailShape(before, after), "DirectUI refresh accepted a changed owner-draw contract");
    CheckRefresh(before.detail, check);

    before.detail = {};
    before.detail.kind = ControlKind::ListView;
    before.detail.groupViewEnabled = true;
    before.detail.itemGroupIds = {7};
    ListViewGroupSnapshot group;
    group.groupId = 7;
    group.header = L"Group";
    group.collapsible = true;
    group.headerRect = {0, 0, 100, 20};
    before.detail.listViewGroups.push_back(group);
    after = before;
    after.detail.listViewGroups[0].collapsed = true;
    check(SameDirectUiDetailShape(before, after) && !SameDirectUiDetailContent(before, after),
        "DirectUI group collapse did not remain mutable canonical state");
    after = before;
    after.detail.itemGroupIds[0] = 8;
    check(!SameDirectUiDetailShape(before, after), "DirectUI group membership changed without admission");
    after = before;
    after.detail.listViewGroups[0].headerAlignment = L"right";
    check(!SameDirectUiDetailContent(before, after), "DirectUI bracket ignored group header alignment");
    after = before;
    after.detail.listViewGroups[0].footerRect.bottom++;
    check(!SameDirectUiDetailContent(before, after), "DirectUI bracket ignored group footer geometry");
    CheckRefresh(before.detail, check);

    before.detail = {};
    before.detail.kind = ControlKind::StaticIcon;
    DirectUiActionBinding groupBinding;
    groupBinding.kind = ControlKind::ListView;
    groupBinding.action = DirectUiAction::SelectListItem;
    groupBinding.secondaryAction = DirectUiAction::SetItemCheck;
    groupBinding.tertiaryAction = DirectUiAction::SetGroupExpanded;
    check(DirectUiActionForRequest(groupBinding, L"setSelection") == DirectUiAction::SelectListItem &&
        DirectUiActionForRequest(groupBinding, L"setItemCheck") == DirectUiAction::SetItemCheck &&
        DirectUiActionForRequest(groupBinding, L"setGroupExpanded") == DirectUiAction::SetGroupExpanded &&
        IsDirectUiInPlaceAction(DirectUiAction::SetGroupExpanded),
        "DirectUI group expansion replaced the selection or checkbox route");
    groupBinding.tertiaryAction = DirectUiAction::None;
    check(DirectUiActionForRequest(groupBinding, L"setGroupExpanded") == DirectUiAction::None,
        "DirectUI binding accepted an undeclared group route");
    before.detail.imageWidth = before.detail.imageHeight = 1;
    before.detail.imageFormat = L"bgra8-premultiplied";
    before.detail.imageData = Pixel(40).imageData;
    after = before;
    after.detail.imageData[0]++;
    check(!SameDirectUiDetailContent(before, after), "DirectUI bracket ignored native static icon pixels");
    CheckRefresh(before.detail, check);
}
} // namespace FluentShell::Tests
