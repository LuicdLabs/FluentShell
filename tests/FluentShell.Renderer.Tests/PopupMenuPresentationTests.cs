using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class PopupMenuPresentationTests
{
    [Fact]
    public void SelectionWinsOverClosedAndRoutinePatchesDoNotReopenTheToken()
    {
        var coordinator = new PopupMenuCoordinator();
        Assert.True(coordinator.Observe("41"));
        Assert.True(coordinator.Respond("41", "0.1"));
        Assert.False(coordinator.Respond("41", null));
        Assert.Equal("0.1", coordinator.PendingResponse!.ItemId);
        Assert.False(coordinator.Observe("41"));
        coordinator.MarkSent("41");
        Assert.Null(coordinator.PendingResponse);
        Assert.True(coordinator.HasResponse);
        Assert.False(coordinator.Respond("41", "0.2"));
    }

    [Fact]
    public void DismissalIsOneTerminalResponseAndRetirementDiscardsIt()
    {
        var coordinator = new PopupMenuCoordinator();
        coordinator.Observe("42");
        Assert.True(coordinator.Respond("42", null));
        Assert.NotNull(coordinator.PendingResponse);
        Assert.Null(coordinator.PendingResponse.ItemId);
        Assert.False(coordinator.Respond("42", null));
        coordinator.Observe(null);
        Assert.Null(coordinator.PendingResponse);
        Assert.False(coordinator.Respond("42", "0"));
    }

    [Fact]
    public void ReplacedPopupRejectsLateEventsFromThePreviousFlyout()
    {
        var coordinator = new PopupMenuCoordinator();
        coordinator.Observe("43");
        coordinator.Respond("43", "1");
        coordinator.Observe("44");
        Assert.Null(coordinator.PendingResponse);
        Assert.False(coordinator.Respond("43", null));
        Assert.True(coordinator.Respond("44", "0"));
        coordinator.MarkSent("43");
        Assert.NotNull(coordinator.PendingResponse);
    }

    [Fact]
    public void DisabledNativeActionRowRemainsAnAnchorWhileHiddenOrRemovedRowsDoNot()
    {
        var snapshot = IslandSnapshot();
        var popup = Popup();
        var node = ControlNodeViewModel.FromSnapshot(snapshot.Nodes[0]);
        Assert.Equal(snapshot.Nodes[0].IslandItems![0].Rect, PopupMenuAnchor.ItemRect(node, popup));
        Assert.Null(PopupMenuAnchor.ItemRect(null, popup));
        Assert.Null(PopupMenuAnchor.ItemRect(node, popup with { ItemIndex = 1 }));
        Assert.Null(PopupMenuAnchor.ItemRect(node, popup with { NodeId = "999" }));
        var hidden = ControlNodeViewModel.FromSnapshot(snapshot.Nodes[0] with { Visible = false });
        Assert.Null(PopupMenuAnchor.ItemRect(hidden, popup));
    }

    [Fact]
    public void PopupOnlySnapshotsPreserveTheProjectedNodeCollection()
    {
        var snapshot = IslandSnapshot();
        var model = WindowViewModel.FromSnapshot(snapshot);
        var node = model.Nodes[0];
        var open = snapshot with { Revision = "8", PopupMenu = Popup() };
        Assert.True(model.CanMergeSnapshot(open));
        model.ApplySnapshot(open, preserveTransient: true);
        Assert.Same(node, model.Nodes[0]);
        Assert.Equal("45", model.PopupMenu!.PopupId);
        var closed = open with { Revision = "9", PopupMenu = null };
        Assert.True(model.CanMergeSnapshot(closed));
        model.ApplySnapshot(closed, preserveTransient: true);
        Assert.Same(node, model.Nodes[0]);
        Assert.Null(model.PopupMenu);
    }

    [Fact]
    public void RootRadioItemsHaveSeparatePopupAndMenuBarGroupsAndAutomationIds()
    {
        Assert.Equal(MenuProjectionFactory.RadioGroupName("0", "45"),
            MenuProjectionFactory.RadioGroupName("1", "45"));
        Assert.NotEqual(MenuProjectionFactory.RadioGroupName("0", "45"),
            MenuProjectionFactory.RadioGroupName("0", "46"));
        Assert.NotEqual(MenuProjectionFactory.RadioGroupName("0", "45"),
            MenuProjectionFactory.RadioGroupName("0"));
        Assert.NotEqual(MenuProjectionFactory.RadioGroupName("0", "45"),
            MenuProjectionFactory.RadioGroupName("0.0", "45"));
        Assert.Equal("FluentShell.Popup.45.Item.0", MenuProjectionFactory.PopupAutomationId("45", "0"));
        Assert.NotEqual(MenuProjectionFactory.AutomationId("0"),
            MenuProjectionFactory.PopupAutomationId("45", "0"));
    }

    private static PopupMenuSnapshot Popup() => new()
    {
        PopupId = "45", NodeId = "10", ItemIndex = 0,
        Items = [new MenuItemSnapshot { ItemId = "0", Kind = "command", Text = "Properties", Enabled = true, CommandId = 77 }],
    };

    private static WindowSnapshot IslandSnapshot() => TestData.Snapshot() with
    {
        Nodes =
        [
            TestData.Snapshot().Nodes[0] with
            {
                Kind = "accessibleIsland", Enabled = false,
                IslandItems =
                [
                    new AccessibleIslandItem
                    {
                        Kind = "button", Name = "More Actions", Enabled = false, DropDown = true,
                        Rect = new PixelRect { X = 8, Y = 24, Width = 120, Height = 24 },
                    },
                ],
            },
        ],
    };
}
