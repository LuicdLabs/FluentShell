using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class AccessibleTabProtocolTests
{
    private static ControlNode Node() => new()
    {
        NodeId = "10", Generation = "1", NativeHwnd = "0x5678", Kind = "accessibleIsland",
        Style = "0x50000000", ExStyle = "0x0", Rect = new PixelRect { Width = 240, Height = 28 },
        Visible = true, Enabled = true, TabIndex = -1,
        IslandItems = [Tab("Extended", 0, true), Tab("Standard", 120, false)],
    };

    private static AccessibleIslandItem Tab(string name, int x, bool selected) => new()
    {
        Kind = "pageTab", Name = name, ActionName = "Switch", Enabled = true, Selected = selected,
        Rect = new PixelRect { X = x, Width = 120, Height = 28 },
    };

    private static void Validate(ControlNode node) =>
        ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] });

    [Fact]
    public void CanonicalSelectedTabAndEnabledStateReachExistingViewModel()
    {
        var node = Node();
        Validate(node);
        var model = ControlNodeViewModel.FromSnapshot(node);
        var changed = new List<string?>();
        model.PropertyChanged += (_, args) => changed.Add(args.PropertyName);
        var updated = node with { IslandItems =
            [node.IslandItems![0] with { Selected = false }, node.IslandItems[1] with { Selected = true, Enabled = false }] };
        Validate(updated);
        model.ApplySnapshot(updated, preserveTransient: true);
        Assert.False(model.IslandItems[0].Selected);
        Assert.True(model.IslandItems[1].Selected);
        Assert.False(model.IslandItems[1].Enabled);
        Assert.Contains(nameof(model.IslandItems), changed);
    }

    [Fact]
    public void MixedRolesMissingOrMultipleSelectionAndTabDropdownAreRejected()
    {
        var node = Node();
        var first = node.IslandItems![0];
        var second = node.IslandItems[1];
        foreach (var items in new List<AccessibleIslandItem>[]
        {
            [first with { Selected = false }, second],
            [first, second with { Selected = true }],
            [first, second with { Kind = "button" }],
            [first with { DropDown = true }, second],
            [first with { ActionName = "" }, second],
            [first with { Kind = "button" }, second with { Kind = "button" }],
        })
            Assert.Throws<ProtocolException>(() => Validate(node with { IslandItems = items }));
    }

    [Theory]
    [InlineData(-1, 120)]
    [InlineData(0, 120)]
    [InlineData(121, 120)]
    public void TabBoundsCannotBeDuplicateContainedOrOutsideHost(int x, int width)
    {
        var node = Node();
        var second = node.IslandItems![1];
        Assert.Throws<ProtocolException>(() => Validate(node with { IslandItems =
            [node.IslandItems[0], second with { Rect = second.Rect with { X = x, Width = width } }] }));
    }

    [Fact]
    public void DisabledCommandsCanOmitActionUntilTheirProviderEnablesThem()
    {
        var node = Node();
        var disabled = node.IslandItems![1] with { Enabled = false, ActionName = "" };
        Validate(node with { IslandItems = [node.IslandItems[0], disabled] });
        Assert.Throws<ProtocolException>(() => Validate(node with { IslandItems =
            [node.IslandItems[0], disabled with { Enabled = true }] }));
    }

    [Fact]
    public void MmcSlantedTabEdgeOverlapPreservesBothNativeRectangles()
    {
        var node = Node();
        var snapshot = node with { IslandItems =
            [node.IslandItems![0] with { Rect = new PixelRect { Width = 73, Height = 19 } },
             node.IslandItems[1] with { Rect = new PixelRect { X = 65, Width = 70, Height = 19 } }] };
        Validate(snapshot);
        var rects = snapshot.IslandItems!.Select(item => item.Rect).ToArray();
        var row = Assert.Single(ControlFactory.GroupTabHeaderRows(rects, allowEdgeOverlap: true));
        Assert.Equal(135, row.Bounds.Width);
        Assert.Equal(rects, row.Items.Select(item => item.Rect));
        Assert.Throws<ArgumentException>(() => ControlFactory.GroupTabHeaderRows(rects));
    }
}
