using System.Text.Json;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

// Minor 33: a large virtual report ListView (Event Viewer's logs) is carried as a
// window of its rows. items and rows hold the rows from itemWindowStart on, itemCount is
// the list's length, and every index (selection, focus, actions) stays absolute.
public sealed class WindowedListProtocolTests
{
    private const int Total = 10_000;
    private const int Start = 4_900;

    private static ControlNode Windowed(int carried = 192, int start = Start, int total = Total) =>
        TestData.Snapshot().Nodes[0] with
        {
            Kind = "listView", ListViewMode = "report", ItemCount = total, ItemWindowStart = start,
            Columns = ["Level", "Date"], ColumnWidths = [100, 150], ColumnOrder = [0, 1],
            Items = Enumerable.Range(start, carried).Select(index => $"Event {index}").ToList(),
            Rows = Enumerable.Range(start, carried).Select(index => new List<string> { $"Event {index}", "Information" }).ToList(),
            ColumnHeadersVisible = true, CheckBoxes = false, CheckedIndices = [],
            ItemActivationSupported = false, ItemNativeIds = [],
            SelectedIndices = [5_000], FocusedIndex = 5_000, MultiSelect = true,
            ImageList = [], ItemImages = Enumerable.Repeat(-1, carried).ToList(),
            EditableLabels = false, EditingIndex = -1,
        };

    private static void Validate(ControlNode node) =>
        ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] });

    [Fact]
    public void WindowedListRoundTripsItsCountAndWindowStart()
    {
        var bytes = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce, Window = TestData.Snapshot() with { Nodes = [Windowed()] },
        });
        var node = Assert.Single(Assert.IsType<WindowOpenMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, bytes)).Window.Nodes);
        Assert.Equal(Total, node.ItemCount);
        Assert.Equal(Start, node.ItemWindowStart);
        Assert.Equal(192, node.Rows.Count);
        Assert.Equal([5_000], node.SelectedIndices);
        Assert.Equal(5_000, node.FocusedIndex);
    }

    [Fact]
    public void AbsoluteSelectionAndFocusMayNameRowsTheWindowDoesNotCarry()
    {
        Validate(Windowed() with { SelectedIndices = [0, 5_000, 9_999], FocusedIndex = 9_999 });
    }

    [Fact]
    public void SelectionAndFocusAreStillBoundedByTheListLength()
    {
        Assert.Throws<ProtocolException>(() => Validate(Windowed() with { SelectedIndices = [Total] }));
        Assert.Throws<ProtocolException>(() => Validate(Windowed() with { FocusedIndex = Total }));
    }

    [Fact]
    public void WindowMustLieInsideTheList()
    {
        Assert.Throws<ProtocolException>(() => Validate(Windowed(carried: 192, start: Total - 100)));
        Assert.Throws<ProtocolException>(() => Validate(Windowed() with { ItemWindowStart = -1 }));
        Assert.Throws<ProtocolException>(() => Validate(Windowed() with { ItemCount = 0 }));
        Validate(Windowed(carried: 192, start: Total - 192));
    }

    [Fact]
    public void CountAndStartAreOneFacet()
    {
        Assert.Throws<ProtocolException>(() => Validate(Windowed() with { ItemWindowStart = null }));
        Assert.Throws<ProtocolException>(() => Validate(Windowed() with { ItemCount = null }));
    }

    [Fact]
    public void WindowedListCannotCarryGroupsCheckboxesOrNativeActivation()
    {
        Assert.Throws<ProtocolException>(() => Validate(Windowed() with { CheckBoxes = true }));
        Assert.Throws<ProtocolException>(() => Validate(Windowed() with { ItemActivationSupported = true }));
        Assert.Throws<ProtocolException>(() => Validate(Windowed() with { ItemNativeIds = ["1"] }));
        Assert.Throws<ProtocolException>(() => Validate(Windowed() with { ListViewMode = "list" }));
    }

    [Fact]
    public void WindowRowsMirrorTheirFirstColumnLabels()
    {
        var node = Windowed();
        node.Items[3] = "different";
        Assert.Throws<ProtocolException>(() => Validate(node));
    }

    [Fact]
    public void OnlyAListViewCarriesAWindow()
    {
        Assert.Throws<ProtocolException>(() => Validate(TestData.Snapshot().Nodes[0] with { ItemCount = 5, ItemWindowStart = 0 }));
    }

    [Fact]
    public void ViewModelPublishesTheWindowWithItsRows()
    {
        var model = ControlNodeViewModel.FromSnapshot(Windowed());
        Assert.True(model.IsWindowedList);
        Assert.Equal(Total, model.ListRowCount);
        Assert.Equal(Start, model.ItemWindowStart);
        Assert.Equal(192, model.Rows.Count);

        var seen = new List<(int Start, int Rows)>();
        model.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(ControlNodeViewModel.Rows)) seen.Add((model.ItemWindowStart, model.Rows.Count));
        };
        // The window moves while the carried rows keep the same labels at other positions.
        model.ApplySnapshot(Windowed(start: 5_000) with { SelectedIndices = [5_000] });
        Assert.Equal(5_000, model.ItemWindowStart);
        Assert.Contains((5_000, 192), seen);

        model.ApplySnapshot(TestData.Snapshot().Nodes[0] with
        {
            Kind = "listView", Columns = ["A"], ColumnWidths = [10], ColumnOrder = [0],
            Items = ["x"], Rows = [["x"]], CheckBoxes = false, CheckedIndices = [], ColumnHeadersVisible = true,
            ItemRects = null, SelectedIndices = [], FocusedIndex = -1, ImageList = [], ItemImages = [-1],
            EditableLabels = false, EditingIndex = -1,
        });
        Assert.False(model.IsWindowedList);
        Assert.Equal(1, model.ListRowCount);
    }

    [Theory]
    [InlineData(0, 0, 22, 100, 0)]
    [InlineData(22 * 40, 0, 22, 100, 40)]
    [InlineData(24 + 22 * 40, 24, 22, 100, 40)]
    [InlineData(10_000_000, 0, 22, 100, 99)]
    [InlineData(5, 40, 22, 100, 0)]
    public void ScrollOffsetMapsToTheTopRow(double offset, double header, double row, int count, int expected)
    {
        Assert.Equal(expected, ListViewPresentation.RowAtOffset(offset, header, row, count));
    }

    [Fact]
    public void ScrollOffsetOfAnEmptyOrDegenerateListIsRowZero()
    {
        Assert.Equal(0, ListViewPresentation.RowAtOffset(500, 0, 22, 0));
        Assert.Equal(0, ListViewPresentation.RowAtOffset(500, 0, 0, 100));
        Assert.Equal(0, ListViewPresentation.RowAtOffset(double.NaN, 0, 22, 100));
    }

    [Theory]
    [InlineData(4_900, 192, 5_000, 20, 10_000, 4, true)]   // visible rows sit well inside the window
    [InlineData(4_900, 192, 4_902, 20, 10_000, 4, false)]  // margin reaches before the window
    [InlineData(4_900, 192, 5_070, 20, 10_000, 4, false)]  // margin reaches past the window
    [InlineData(0, 192, 0, 20, 10_000, 4, true)]           // the top needs no rows above it
    [InlineData(9_808, 192, 9_990, 20, 10_000, 4, true)]   // the bottom needs no rows below it
    public void WindowCoverageIncludesAMarginAroundTheViewport(
        int windowStart, int carried, int firstVisible, int visible, int total, int margin, bool covered)
    {
        Assert.Equal(covered,
            ListViewPresentation.WindowCovers(windowStart, carried, firstVisible, visible, total, margin));
    }

    [Fact]
    public void SpacersStandForTheRowsTheWindowDoesNotCarry()
    {
        var (above, below) = ListViewPresentation.SpacerHeights(4_900, 192, 10_000, 22);
        Assert.Equal(4_900 * 22.0, above);
        Assert.Equal((10_000 - 4_900 - 192) * 22.0, below);
        Assert.Equal((0.0, 0.0), ListViewPresentation.SpacerHeights(0, 40, 40, 22));
        Assert.Equal(10_000 * 22.0, above + 192 * 22.0 + below);
    }

    [Fact]
    public void SetTopIndexActionRoundTripsAndAddressesANode()
    {
        var message = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = TestData.Snapshot().SurfaceId, NodeId = "10",
            EventId = "5", ExpectedRevision = "7", Action = "setTopIndex",
            Value = JsonDocument.Parse("4800").RootElement.Clone(),
        };
        var decoded = Assert.IsType<ActionInvokeMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(message)));
        Assert.Equal(4_800, decoded.Value.GetInt32());
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(message with { NodeId = null })));
    }

    [Theory]
    [InlineData("-1")]
    [InlineData("16777216")]
    [InlineData("\"4800\"")]
    [InlineData("1.5")]
    [InlineData("{}")]
    public void SetTopIndexRejectsAMalformedRow(string valueJson)
    {
        var message = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = TestData.Snapshot().SurfaceId, NodeId = "10",
            EventId = "5", ExpectedRevision = "7", Action = "setTopIndex",
            Value = JsonDocument.Parse(valueJson).RootElement.Clone(),
        };
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(message)));
    }

    [Fact]
    public void SetFocusedIndexNamesAnAbsoluteRow()
    {
        var message = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = TestData.Snapshot().SurfaceId, NodeId = "10",
            EventId = "6", ExpectedRevision = "7", Action = "setFocusedIndex",
            Value = JsonDocument.Parse("9999").RootElement.Clone(),
        };
        Assert.IsType<ActionInvokeMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(message)));
    }
}
