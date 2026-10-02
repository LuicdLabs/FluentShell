using System.Text.Json;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;
using Windows.System;

namespace FluentShell.Renderer.Tests;

public class ListViewModeProtocolTests
{
    [Fact]
    public void ShiftSelectionAfterItemRemovalCannotAddressTheOldAnchor()
    {
        Assert.Equal([0], ListViewPresentation.SelectionRange(2, 9, 0));
        Assert.Equal([1], ListViewPresentation.SelectionRange(2, -1, 1));
        Assert.Equal([1, 2, 3], ListViewPresentation.SelectionRange(4, 3, 1));
        Assert.Empty(ListViewPresentation.SelectionRange(0, 9, 0));
        Assert.Empty(ListViewPresentation.SelectionRange(2, 0, 2));
    }

    [Fact]
    public void IconKeyboardNavigationFollowsCapturedPositionsInsteadOfItemOrder()
    {
        var rects = new List<PixelRect>
        {
            new() { X = 0, Y = 0, Width = 60, Height = 60 },
            new() { X = 0, Y = 80, Width = 60, Height = 60 },
            new() { X = 80, Y = 0, Width = 60, Height = 60 },
            new() { X = 80, Y = 80, Width = 60, Height = 60 },
        };
        Assert.Equal(2, ListViewPresentation.Navigate(rects, 0, VirtualKey.Right));
        Assert.Equal(1, ListViewPresentation.Navigate(rects, 0, VirtualKey.Down));
        Assert.Equal(0, ListViewPresentation.Navigate(rects, 2, VirtualKey.Left));
        Assert.Equal(2, ListViewPresentation.Navigate(rects, 3, VirtualKey.Up));
        Assert.Equal(0, ListViewPresentation.Navigate(rects, 0, VirtualKey.Left));
        Assert.Equal(3, ListViewPresentation.Navigate(rects, 0, VirtualKey.End));
        Assert.Equal(-1, ListViewPresentation.Navigate([], 0, VirtualKey.Right));
    }

    internal static ControlNode IconList(string mode = "largeIcon") => TestData.Snapshot().Nodes[0] with
    {
        Kind = "listView", ListViewMode = mode, ItemActivationSupported = true, ItemNativeIds = ["0", "7"],
        Items = ["Computers", "Event Viewer"], Rows = [], Columns = [], ColumnWidths = [], ColumnOrder = [],
        ColumnHeadersVisible = false, CheckBoxes = false, CheckedIndices = [],
        ItemRects = [new() { X = 0, Y = 0, Width = 80, Height = 70 }, new() { X = 80, Y = 0, Width = 80, Height = 70 }],
        SelectedIndices = [1], FocusedIndex = 1, MultiSelect = true,
        ImageList = [], ItemImages = [-1, -1], EditableLabels = false, EditingIndex = -1,
    };

    private static ControlNode RoundTrip(ControlNode node)
    {
        var bytes = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce, Window = TestData.Snapshot() with { Nodes = [node] },
        });
        return Assert.Single(Assert.IsType<WindowOpenMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, bytes)).Window.Nodes);
    }

    [Theory]
    [InlineData("largeIcon")]
    [InlineData("smallIcon")]
    [InlineData("list")]
    public void NonReportModesPreserveGeometryAndItemState(string mode)
    {
        var actual = RoundTrip(IconList(mode));
        Assert.Equal(mode, actual.ListViewMode);
        Assert.True(actual.ItemActivationSupported);
        Assert.Equal([1], actual.SelectedIndices);
        Assert.Equal(2, actual.ItemRects!.Count);
        var model = ControlNodeViewModel.FromSnapshot(actual);
        Assert.True(ControlFactory.HasRenderableListViewShape(model));
        Assert.False(ControlFactory.ShouldRenderListViewHeader(model));
    }

    [Fact]
    public void EmptyNonReportListHasNoPhantomHeaderOrRow()
    {
        var actual = RoundTrip(IconList() with
        {
            Items = [], ItemRects = [], ItemImages = [], ItemNativeIds = [], SelectedIndices = [], FocusedIndex = -1,
        });
        Assert.Empty(actual.Items);
        Assert.True(ControlFactory.HasRenderableListViewShape(ControlNodeViewModel.FromSnapshot(actual)));
    }

    [Fact]
    public void ReportSnapshotsRemainCompatibleWhenNewFieldsAreAbsent()
    {
        var report = IconList() with
        {
            ListViewMode = null, ItemActivationSupported = null, ItemNativeIds = null, ItemRects = null,
            Columns = ["Name"], ColumnWidths = [100], ColumnOrder = [0], Rows = [["Computers"], ["Event Viewer"]],
        };
        var model = ControlNodeViewModel.FromSnapshot(RoundTrip(report));
        Assert.Equal("report", model.ListViewMode);
        Assert.False(model.ItemActivationSupported);
    }

    [Fact]
    public void NonListNodesOmitListOnlyMetadataOnTheWire()
    {
        using var json = JsonDocument.Parse(ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce, Window = TestData.Snapshot(),
        }));
        var node = json.RootElement.GetProperty("window").GetProperty("nodes")[0];
        Assert.False(node.TryGetProperty("listViewMode", out _));
        Assert.False(node.TryGetProperty("itemActivationSupported", out _));
        Assert.False(node.TryGetProperty("itemNativeIds", out _));
    }

    [Fact]
    public void ModeChangesRecreateThePresenterAsOneSnapshot()
    {
        var model = WindowViewModel.FromSnapshot(TestData.Snapshot() with { Nodes = [IconList()] });
        Assert.False(model.CanMergeSnapshot(TestData.Snapshot() with { Nodes = [IconList("list")] }));
    }

    [Fact]
    public void MalformedShapesAndItemIndexesAreRejected()
    {
        var valid = IconList();
        foreach (var bad in new[]
        {
            valid with { ListViewMode = "tile" }, valid with { ItemRects = null },
            valid with { ItemRects = [valid.ItemRects![0]] }, valid with { ColumnHeadersVisible = true },
            valid with { Columns = ["Hidden"], ColumnWidths = [20], ColumnOrder = [0] },
            valid with { Rows = [["Hidden"]] }, valid with { SelectedIndices = [2] },
            valid with { FocusedIndex = 2 }, valid with { CheckBoxes = true, CheckedIndices = [2] },
            valid with { ItemImages = [-1] }, valid with { EditingIndex = 2, EditableLabels = true },
            valid with { ItemRects = [new() { Width = 0, Height = 1 }, valid.ItemRects![1]] },
        }) Assert.Throws<ProtocolException>(() => RoundTrip(bad));
    }

    [Fact]
    public void ScrolledAndManuallyPositionedItemsRetainTheirClientPositions()
    {
        var rects = new List<PixelRect>
        {
            new() { X = -60, Y = -10, Width = 80, Height = 70 },
            new() { X = 160, Y = 90, Width = 80, Height = 70 },
        };
        var actual = RoundTrip(IconList() with { ItemRects = rects });
        var bounds = ListViewPresentation.ContentBounds(actual.ItemRects!, new() { Width = 200, Height = 100 });
        Assert.Equal(-60, bounds.X);
        Assert.Equal(-10, bounds.Y);
        Assert.Equal(300, bounds.Width);
        Assert.Equal(170, bounds.Height);
        Assert.Equal(rects[1].X, rects[1].X - bounds.X - -bounds.X);
    }

    [Theory]
    [InlineData(-1, false)]
    [InlineData(0, true)]
    [InlineData(4095, true)]
    [InlineData(4096, false)]
    public void ActivationRequiresNodeAndBoundedIntegerWithoutAutomaticReplay(int index, bool valid)
    {
        var message = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = TestData.Snapshot().SurfaceId,
            EventId = "9", ExpectedRevision = "7", NodeId = "10", Action = "activateItem",
            Value = JsonSerializer.SerializeToElement(index),
        };
        void Validate(ActionInvokeMessage action) => ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke,
            ProtocolSerializer.Serialize(action));
        if (valid) Validate(message);
        else Assert.Throws<ProtocolException>(() => Validate(message));
        Assert.Throws<ProtocolException>(() => Validate(message with { NodeId = null }));
        Assert.False(NodeActionReplayPolicy.IsReplayableAfterStale("activateItem"));
    }
}
