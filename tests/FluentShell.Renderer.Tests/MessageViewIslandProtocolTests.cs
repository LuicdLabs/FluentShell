using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;

namespace FluentShell.Renderer.Tests;

// MMC's message view travels as an accessible island of inert parts: a heading, body
// text, and the graphic the view painted for its icon.
public sealed class MessageViewIslandProtocolTests
{
    private static readonly string OnePixel = Convert.ToBase64String([0x10, 0x20, 0x30, 0xff]);

    private static AccessibleIslandItem Heading() => new()
    {
        Kind = "heading", Name = "No authorization store selected", Description = "Title",
        Enabled = true, Rect = new PixelRect { X = 56, Y = 12, Width = 400, Height = 20 },
    };

    private static AccessibleIslandItem Body() => new()
    {
        Kind = "text", Name = "Open an existing store from the Action menu.", Description = "Body",
        Enabled = true, Rect = new PixelRect { X = 56, Y = 40, Width = 400, Height = 80 },
    };

    private static AccessibleIslandItem Icon() => new()
    {
        Kind = "image", Name = "Icon", Enabled = true,
        Rect = new PixelRect { X = 12, Y = 12, Width = 1, Height = 1 },
        ImageWidth = 1, ImageHeight = 1, ImageFormat = "bgra8-premultiplied", ImageData = OnePixel,
    };

    private static ControlNode Node(params AccessibleIslandItem[] items) => new()
    {
        NodeId = "11", Generation = "1", NativeHwnd = "0x6789", Kind = "accessibleIsland",
        Style = "0x56000000", ExStyle = "0x0", Rect = new PixelRect { Width = 480, Height = 200 },
        Visible = true, Enabled = true, TabIndex = -1, IslandItems = [.. items],
    };

    private static void Validate(ControlNode node) =>
        ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] });

    [Fact]
    public void HeadingBodyAndPaintedIconAreAdmittedAndReachTheViewModel()
    {
        var node = Node(Heading(), Body(), Icon());
        Validate(node);
        var model = ControlNodeViewModel.FromSnapshot(node);
        Assert.Equal(["heading", "text", "image"], model.IslandItems.Select(item => item.Kind));
        Assert.Equal(OnePixel, model.IslandItems[2].ImageData);
    }

    [Fact]
    public void InertPartsCannotCarryActionsOrMenus()
    {
        foreach (var item in new[]
        {
            Heading() with { ActionName = "Press" },
            Icon() with { ActionName = "Open" },
            Heading() with { DropDown = true },
            Icon() with { Selected = true },
        })
            Assert.Throws<ProtocolException>(() => Validate(Node(item, Body())));
    }

    [Fact]
    public void OnlyAnImageCarriesPixelsAndThosePixelsMustBeCanonical()
    {
        var notPremultiplied = Convert.ToBase64String([0xff, 0x20, 0x30, 0x10]);
        foreach (var item in new[]
        {
            Heading() with { ImageWidth = 1, ImageHeight = 1, ImageFormat = "bgra8-premultiplied", ImageData = OnePixel },
            Body() with { ImageData = OnePixel },
            Icon() with { ImageData = null },
            Icon() with { ImageFormat = "rgba8" },
            Icon() with { ImageWidth = 2 },
            Icon() with { ImageWidth = 97, ImageHeight = 1 },
            Icon() with { ImageData = notPremultiplied },
            Icon() with { ImageData = "not base64!" },
        })
            Assert.Throws<ProtocolException>(() => Validate(Node(Heading(), item)));
    }
}
