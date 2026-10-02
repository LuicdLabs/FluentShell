using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public class ItemImageryProtocolTests
{
    [Theory]
    [InlineData("treeView")]
    [InlineData("listView")]
    public void MoreThanSixtyFourSmallReferencedIconsSurviveTheWire(string kind)
    {
        var indexes = Enumerable.Range(0, 140).ToList();
        var node = ItemNode(kind, Enumerable.Repeat(Icon(16), 140).ToList(), indexes);
        var snapshot = TestData.Snapshot() with { Nodes = [node] };
        var payload = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = snapshot,
        });
        var decoded = Assert.IsType<WindowOpenMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, payload));
        var actual = Assert.Single(decoded.Window.Nodes);
        Assert.Equal(140, actual.ImageList!.Count);
        Assert.Equal(indexes, actual.ItemImages);
        if (kind == "treeView") Assert.Equal(indexes, actual.ItemSelectedImages);
    }

    [Theory]
    [InlineData("treeView")]
    [InlineData("listView")]
    public void DecodedPixelBudgetAcceptsItsBoundaryAndRejectsOneMorePixel(string kind)
    {
        var icons = Enumerable.Repeat(Icon(64), 64).ToList();
        var node = ItemNode(kind, icons, [63]);
        Validate(node);

        icons.Add(Icon(1));
        var exception = Assert.Throws<ProtocolException>(() => Validate(node));
        Assert.Contains("decoded pixel budget", exception.Message);
    }

    [Fact]
    public void EntirePixelBudgetIsCheckedBeforeAnyBase64Decode()
    {
        var icon = Icon(64);
        var invalidPixels = icon with { ImageData = new string('!', icon.ImageData.Length) };
        var icons = Enumerable.Repeat(icon, 65).ToList();
        icons[0] = invalidPixels;

        // An eager decoder would report the first malformed icon. The complete
        // metadata budget must reject this list before decoding even that icon.
        var exception = Assert.Throws<ProtocolException>(() =>
            Validate(ItemNode("treeView", icons, [0])));
        Assert.Contains("decoded pixel budget", exception.Message);
    }

    [Fact]
    public void TwoDistinctReferencesPerMaximumTreeItemFitTheCountCap()
    {
        var icons = Enumerable.Repeat(Icon(1), ProtocolConstants.MaxImageListImages).ToList();
        var node = ItemNode("treeView", icons,
            Enumerable.Range(0, ProtocolConstants.MaxItems).ToList()) with
        {
            ItemSelectedImages = Enumerable.Range(
                ProtocolConstants.MaxItems, ProtocolConstants.MaxItems).ToList(),
        };
        Validate(node);

        icons.Add(Icon(1));
        var exception = Assert.Throws<ProtocolException>(() => Validate(node));
        Assert.Contains("icon cap", exception.Message);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void NativeIndexesCannotEscapeTheCompactSnapshotList(bool selected)
    {
        var node = ItemNode("treeView", [Icon(16), Icon(16)], [0]);
        node = selected
            ? node with { ItemSelectedImages = [139] }
            : node with { ItemImages = [139] };
        var exception = Assert.Throws<ProtocolException>(() => Validate(node));
        Assert.Contains("outside the image list", exception.Message);
    }

    [Fact]
    public void ARecaptureReplacesBothImageMappingsAndSelectedState()
    {
        var first = ItemNode("treeView", [Icon(16), Icon(16)], [0, 0]) with
        {
            SelectedIndex = 1,
            ItemSelectedImages = [1, 1],
        };
        var viewModel = ControlNodeViewModel.FromSnapshot(first);
        Assert.Equal(1, ControlFactory.ImageIndexForItem(viewModel, 1, 1));

        var next = first with
        {
            ImageList = [Icon(16)],
            ItemImages = [-1, 0],
            ItemSelectedImages = [-1, -1],
        };
        Validate(next);
        viewModel.ApplySnapshot(next);
        Assert.Single(viewModel.ImageList);
        Assert.Equal(0, ControlFactory.ImageIndexForItem(viewModel, 1, 0));
        Assert.Equal(-1, ControlFactory.ImageIndexForItem(viewModel, 1, 1));
        Assert.Equal(-1, ControlFactory.ImageIndexForItem(viewModel, 0, 1));
    }

    [Fact]
    public void IconDataLengthIsRejectedBeforeAnOversizedDecode()
    {
        var icon = Icon(1) with { ImageData = new string('A', 100_000) };
        var exception = Assert.Throws<ProtocolException>(() =>
            Validate(ItemNode("treeView", [icon], [0])));
        Assert.Contains("base64 length", exception.Message);
    }

    [Fact]
    public void CompactIconsStillRequireCanonicalPremultipliedPixels()
    {
        var icon = Icon(1) with { ImageData = Convert.ToBase64String([255, 0, 0, 1]) };
        var exception = Assert.Throws<ProtocolException>(() =>
            Validate(ItemNode("treeView", [icon], [0])));
        Assert.Contains("premultiplied", exception.Message);
    }

    private static void Validate(ControlNode node) =>
        ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] });

    private static ImageListEntry Icon(int dimension) => new()
    {
        ImageWidth = dimension,
        ImageHeight = dimension,
        ImageFormat = "bgra8-premultiplied",
        ImageData = Convert.ToBase64String(new byte[dimension * dimension * 4]),
    };

    private static ControlNode ItemNode(string kind, List<ImageListEntry> icons, List<int> indexes)
    {
        var items = indexes.Select((_, index) => $"Item {index}").ToList();
        var node = TestData.Snapshot().Nodes[0] with
        {
            Kind = kind,
            Text = string.Empty,
            AutomationName = string.Empty,
            Items = items,
            SelectedIndex = -1,
            ImageList = icons,
            ItemImages = indexes,
            EditableLabels = false,
            EditingIndex = -1,
        };
        return kind == "treeView" ? node with
        {
            ItemDepths = Enumerable.Repeat(0, items.Count).ToList(),
            ItemExpanded = Enumerable.Repeat(false, items.Count).ToList(),
            ItemHasChildren = Enumerable.Repeat(false, items.Count).ToList(),
            ItemSelectedImages = indexes.ToList(),
        } : node with
        {
            Columns = ["Name"],
            ColumnWidths = [160],
            ColumnOrder = [0],
            Rows = items.Select(item => new List<string> { item }).ToList(),
            SelectedIndices = [],
            FocusedIndex = -1,
            MultiSelect = false,
            ColumnHeadersVisible = true,
            CheckBoxes = false,
            CheckedIndices = [],
        };
    }
}
