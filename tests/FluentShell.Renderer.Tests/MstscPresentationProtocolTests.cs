using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

// Minor 40, from mstsc's dialog: an edit box's grey hint (cueBanner), and a list-style
// toolbar button that draws its own label beside its icon (labelBeside).
public sealed class MstscPresentationProtocolTests
{
    [Fact]
    public void AnEditBoxCarriesItsHint()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with { CueBanner = "Example: computer.fabrikam.com" };
        ProtocolValidator.ValidateSnapshot(snapshot);
    }

    [Fact]
    public void AnEditableComboBoxCarriesItsHint()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with
        {
            Kind = "comboBox", Editable = true, Items = ["alpha"], SelectedIndex = -1, CueBanner = "Computer",
        };
        ProtocolValidator.ValidateSnapshot(snapshot);
    }

    [Theory]
    [InlineData("edit", false, "")]
    [InlineData("button", false, "hint")]
    [InlineData("comboBox", false, "hint")]
    public void AHintIsRefusedWhereNoBoxShowsIt(string kind, bool editable, string cueBanner)
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with
        {
            Kind = kind, Editable = editable, Items = kind == "comboBox" ? ["alpha"] : [],
            SelectedIndex = kind == "comboBox" ? -1 : snapshot.Nodes[0].SelectedIndex, CueBanner = cueBanner,
        };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    private static ImageListEntry Icon() => new()
    {
        ImageWidth = 1, ImageHeight = 1, ImageFormat = "bgra8-premultiplied",
        ImageData = Convert.ToBase64String([1, 30, 20, 255]),
    };

    private static WindowSnapshot Toolbar(ToolbarItemSnapshot item)
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with
        {
            Kind = "toolbar", Style = "0x5603904D", ExStyle = "0x84",
            Rect = new PixelRect { Width = 132, Height = 32 }, ToolbarItems = [item],
        };
        return snapshot;
    }

    private static ToolbarItemSnapshot ShowOptions() => new()
    {
        Kind = "pushButton", CommandId = 5014, Text = "顯示選項(&O)", Enabled = true,
        Rect = new PixelRect { Width = 120, Height = 30 },
        ImageWidth = 1, ImageHeight = 1, ImageFormat = "bgra8-premultiplied", ImageData = Icon().ImageData,
        LabelBeside = true,
    };

    [Fact]
    public void AListToolbarButtonCarriesItsLabelBesideItsIcon()
    {
        ProtocolValidator.ValidateSnapshot(Toolbar(ShowOptions()));
    }

    private static ChromeRegion Band(int x, int y, int width, int height) => new()
    {
        Rect = new PixelRect { X = x, Y = y, Width = width, Height = height },
        ImageWidth = width, ImageHeight = height, ImageFormat = "bgra8-premultiplied",
        ImageData = Convert.ToBase64String(Enumerable.Range(0, width * height)
            .SelectMany(_ => new byte[] { 200, 120, 0, 255 }).ToArray()),
    };

    [Fact]
    public void ADialogCarriesTheBannerItPaintsAboveItsControls()
    {
        var snapshot = TestData.Snapshot();
        var width = snapshot.ClientBounds.Width;
        ProtocolValidator.ValidateSnapshot(snapshot with { PaintedBands = [Band(0, 0, width, 8)] });
        ProtocolValidator.ValidateSnapshot(snapshot with
        {
            PaintedBands = [Band(0, 0, width, 8), Band(0, snapshot.ClientBounds.Height - 8, width, 8)],
        });
    }

    [Fact]
    public void APaintedBandMustBeThePixelsOfARectangleInsideTheClientArea()
    {
        var snapshot = TestData.Snapshot();
        var width = snapshot.ClientBounds.Width;
        foreach (var bands in new List<ChromeRegion>[]
        {
            [],
            [Band(0, 0, width + 1, 8)],
            [Band(0, snapshot.ClientBounds.Height - 4, width, 8)],
            [Band(0, 0, width, 8) with { ImageHeight = 7 }],
            [Band(0, 0, width, 4), Band(0, 4, width, 4), Band(0, 8, width, 4)],
        })
            Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot with { PaintedBands = bands }));
    }

    [Fact]
    public void ALabelBesideNeedsAnIconALabelAndTrue()
    {
        foreach (var broken in new[]
        {
            ShowOptions() with { ImageWidth = null, ImageHeight = null, ImageFormat = null, ImageData = null },
            ShowOptions() with { Text = "" },
            ShowOptions() with { LabelBeside = false },
        })
            Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(Toolbar(broken)));
    }
}
