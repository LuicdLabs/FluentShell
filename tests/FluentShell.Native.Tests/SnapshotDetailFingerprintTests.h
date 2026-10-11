#pragma once

void TestSnapshotDetailFingerprint() {
    using namespace FluentShell::Bridge::Translation;
    WindowSnapshot original;
    original.nodes.emplace_back();
    auto& node = original.nodes.back();
    ImageListEntry image;
    image.imageWidth = image.imageHeight = 1;
    image.imageFormat = L"bgra8";
    image.imageData = {1, 2, 3, 255};
    node.toolbarItems.emplace_back();
    node.toolbarItems[0].hotImage = image;
    node.toolbarItems[0].disabledImage = image;
    node.toolbarItems[0].pressedImage = image;
    node.ownerDrawCombo.emplace();
    node.ownerDrawCombo->mode = L"fixed";
    node.ownerDrawCombo->items.push_back({image, image});
    node.ownerDrawCombo->selectionFace = image;
    node.groupViewEnabled = true;
    node.listViewGroups.emplace_back();
    node.listViewGroups[0].groupId = 4;
    node.itemGroupIds = {4};
    const auto expected = SnapshotFingerprint(original);
    const auto changed = [&](auto mutation, const char* reason) {
        auto value = original;
        mutation(value.nodes.front());
        Check(SnapshotFingerprint(value) != expected, reason);
    };
    changed([](auto& n) { n.toolbarItems[0].hotImage->imageData[0]++; }, "hot toolbar pixels did not invalidate snapshot");
    changed([](auto& n) { n.toolbarItems[0].disabledImage->imageData[0]++; }, "disabled toolbar pixels did not invalidate snapshot");
    changed([](auto& n) { n.toolbarItems[0].pressedImage->imageData[0]++; }, "pressed toolbar pixels did not invalidate snapshot");
    changed([](auto& n) { n.toolbarItems[0].hotImage.reset(); }, "toolbar image presence did not invalidate snapshot");
    changed([](auto& n) { n.toolbarItems[0].paintedFace = true; }, "toolbar painted-face change did not invalidate snapshot");
    changed([](auto& n) { n.ownerDrawCombo->items[0].normal.imageData[0]++; }, "combo normal pixels did not invalidate snapshot");
    changed([](auto& n) { n.ownerDrawCombo->items[0].selected.imageData[0]++; }, "combo selected pixels did not invalidate snapshot");
    changed([](auto& n) { n.ownerDrawCombo->selectionFace->imageData[0]++; }, "combo closed pixels did not invalidate snapshot");
    changed([](auto& n) { n.ownerDrawCombo->selectionFace.reset(); }, "combo closed face presence did not invalidate snapshot");
    changed([](auto& n) { n.ownerDrawCombo->mode = L"variable"; }, "combo draw mode did not invalidate snapshot");
    changed([](auto& n) { n.listViewGroups[0].header = L"changed"; }, "group title did not invalidate snapshot");
    changed([](auto& n) { n.listViewGroups[0].collapsed = true; }, "group collapse did not invalidate snapshot");
    changed([](auto& n) { n.listViewGroups[0].headerRect.bottom++; }, "group geometry did not invalidate snapshot");
    changed([](auto& n) { n.itemGroupIds[0]++; }, "item group membership did not invalidate snapshot");

    PopupMenuState local;
    PopupMenuState remote;
    MenuItemSnapshot command;
    command.itemId = L"0";
    command.commandId = 10;
    command.text = L"Native command";
    const auto first = local.BeginWithId(1, 0, {command}, 1);
    local.Cancel();
    const auto second = remote.BeginWithId(1, 0, {command}, 2);
    PopupMenuDecision decision;
    Check(first == 1 && second == 2 && !remote.Resolve(first, std::nullopt, decision),
        "a retired local popup token resolved a later remote popup");
    Check(remote.Resolve(second, std::nullopt, decision), "remote popup's current token did not resolve");
    Check(remote.BeginWithId(1, 0, {command}, 1) == 0,
        "popup state accepted a reused externally allocated token");
}
