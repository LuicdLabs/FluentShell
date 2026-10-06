using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class ToolbarImageVariantTests
{
    private static ImageListEntry Icon(byte blue, int size = 1) => new()
    {
        ImageWidth = size, ImageHeight = size, ImageFormat = "bgra8-premultiplied",
        ImageData = Convert.ToBase64String(Enumerable.Range(0, size * size)
            .SelectMany(_ => new byte[] { blue, 30, 20, 255 }).ToArray()),
    };

    private static ToolbarItemSnapshot Button() => new()
    {
        Kind = "pushButton", CommandId = 1, Text = "Connect", Enabled = true,
        Rect = new PixelRect { Width = 25, Height = 25 },
        ImageWidth = 1, ImageHeight = 1, ImageFormat = "bgra8-premultiplied", ImageData = Icon(1).ImageData,
        HotImage = Icon(2), DisabledImage = Icon(3), PressedImage = Icon(4),
    };

    private static WindowOpenMessage Message(params ToolbarItemSnapshot[] items) => new()
    {
        SessionNonce = TestData.Nonce,
        Window = TestData.Snapshot() with
        {
            Nodes = [TestData.Snapshot().Nodes[0] with
            {
                Kind = "toolbar", Style = "0x50000000", ExStyle = "0x0",
                Rect = new PixelRect { Width = 2000, Height = 25 }, ToolbarItems = items.ToList(),
            }],
        },
    };

    private static WindowOpenMessage Read(WindowOpenMessage message) =>
        Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            ProtocolSerializer.Serialize(message)));

    [Fact]
    public void SharedToolbarFixtureCarriesAllNativeImageStates()
    {
        var payload = File.ReadAllBytes(Path.Combine(
            AppContext.BaseDirectory, "ProtocolFixtures", "window.open.toolbar-images.json"));
        var message = Assert.IsType<WindowOpenMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, payload));
        var node = Assert.Single(message.Window.Nodes);
        Assert.Equal("toolbar", node.Kind);
        var item = Assert.Single(node.ToolbarItems!);
        Assert.Equal(Button() with
        {
            CommandId = 101, Rect = new PixelRect { Width = 36, Height = 36 },
        }, item);
    }

    [Fact]
    public void NativeStateImagesSurviveTheWireAndReconcile()
    {
        var first = Read(Message(Button()));
        var item = first.Window.Nodes[0].ToolbarItems![0];
        Assert.Equal(Button(), item);
        var model = ControlNodeViewModel.FromSnapshot(first.Window.Nodes[0]);
        var changed = Read(Message(item with { Enabled = false, DisabledImage = Icon(12) }));
        model.ApplySnapshot(changed.Window.Nodes[0], preserveTransient: false);
        Assert.False(model.ToolbarItems[0].Enabled);
        Assert.Equal(Icon(12), model.ToolbarItems[0].DisabledImage);
        var cleared = Read(Message(item with { HotImage = null, DisabledImage = null, PressedImage = null }));
        model.ApplySnapshot(cleared.Window.Nodes[0], preserveTransient: false);
        Assert.Null(model.ToolbarItems[0].HotImage);
        Assert.Null(model.ToolbarItems[0].DisabledImage);
        Assert.Null(model.ToolbarItems[0].PressedImage);
    }

    [Theory]
    [InlineData(true, false, false, 0)]
    [InlineData(true, true, false, 1)]
    [InlineData(true, true, true, 3)]
    [InlineData(true, false, true, 3)] // Keyboard press.
    [InlineData(false, true, true, 2)] // Disabled during a click.
    public void EffectiveButtonStateChoosesTheNativeImage(bool enabled, bool over, bool pressed,
        int expected) =>
        Assert.Equal((ToolbarImageState)expected, ToolbarImagePresentation.Select(Button(), enabled, over, pressed,
            isChecked: false, hotTracking: true));

    [Fact]
    public void MissingStateImagesFallBackWithoutUsingAnInteractionImageWhenDisabled()
    {
        var item = Button() with { PressedImage = null, DisabledImage = null };
        Assert.Equal(ToolbarImageState.Normal, ToolbarImagePresentation.Select(item, true, true, true,
            isChecked: false, hotTracking: true));
        Assert.Equal(ToolbarImageState.Normal, ToolbarImagePresentation.Select(item, false, true, true,
            isChecked: false, hotTracking: true));
        item = item with { HotImage = null };
        Assert.Equal(ToolbarImageState.Normal, ToolbarImagePresentation.Select(item, true, true, true,
            isChecked: false, hotTracking: true));
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public void PressWithoutItsOwnImageUsesNormalEvenWhenHotOrChecked(bool over, bool isChecked)
    {
        var item = Button() with { PressedImage = null };
        Assert.Equal(ToolbarImageState.Normal, ToolbarImagePresentation.Select(item,
            enabled: true, pointerOver: over, pressed: true, isChecked, hotTracking: true));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void CheckedButtonsUseHotPixelsWithoutHoverIncludingPlainToolbars(bool hotTracking)
    {
        Assert.Equal(ToolbarImageState.Hot, ToolbarImagePresentation.Select(Button(),
            enabled: true, pointerOver: false, pressed: false, isChecked: true, hotTracking));
        Assert.Equal(ToolbarImageState.Pressed, ToolbarImagePresentation.Select(Button(),
            enabled: true, pointerOver: false, pressed: true, isChecked: true, hotTracking));
        Assert.Equal(ToolbarImageState.Disabled, ToolbarImagePresentation.Select(Button(),
            enabled: false, pointerOver: false, pressed: true, isChecked: true, hotTracking));
        Assert.Equal(ToolbarImageState.Normal, ToolbarImagePresentation.Select(Button() with { HotImage = null },
            enabled: true, pointerOver: false, pressed: false, isChecked: true, hotTracking));
    }

    [Fact]
    public void PlainToolbarHoverKeepsNormalPixels()
    {
        Assert.Equal(ToolbarImageState.Normal, ToolbarImagePresentation.Select(Button(),
            enabled: true, pointerOver: true, pressed: false, isChecked: false, hotTracking: false));
    }

    [Fact]
    public void StateImagesRequireAValidMatchingNormalImageAndButton()
    {
        foreach (var invalid in new[]
        {
            Button() with { ImageWidth = null, ImageHeight = null, ImageFormat = null, ImageData = null },
            Button() with { HotImage = Icon(4, 2) },
            Button() with { DisabledImage = Icon(4) with { ImageData = "AA==" } },
            Button() with { PressedImage = Icon(4) with { ImageData = "////AA==" } }, // Non-premultiplied.
            Button() with { Kind = "separator", CommandId = 0, Text = "" },
        }) Assert.Throws<ProtocolException>(() => Read(Message(invalid)));
    }

    [Theory]
    [InlineData("hotImage")]
    [InlineData("disabledImage")]
    [InlineData("pressedImage")]
    public void RawWireRejectsNullOrIncompleteStateImages(string name)
    {
        var json = JsonNode.Parse(ProtocolSerializer.Serialize(Message(Button())))!;
        var item = json["window"]!["nodes"]![0]!["toolbarItems"]![0]!;
        item[name]!.AsObject().Remove("imageFormat");
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            System.Text.Encoding.UTF8.GetBytes(json.ToJsonString())));
        item[name] = null;
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            System.Text.Encoding.UTF8.GetBytes(json.ToJsonString())));
    }

    [Fact]
    public void AllStatesShareTheExistingToolbarPixelBudget()
    {
        var image = Icon(3, 96);
        var items = Enumerable.Range(0, 17).Select(index => Button() with
        {
            CommandId = index + 1, Rect = new PixelRect { X = index * 25, Width = 25, Height = 25 },
            ImageWidth = 96, ImageHeight = 96, ImageData = image.ImageData,
            HotImage = image, DisabledImage = image, PressedImage = image,
        }).ToArray();
        Assert.Equal(16, Read(Message(items.Take(16).ToArray())).Window.Nodes[0].ToolbarItems!.Count);
        Assert.Throws<ProtocolException>(() => Read(Message(items)));
    }
}
