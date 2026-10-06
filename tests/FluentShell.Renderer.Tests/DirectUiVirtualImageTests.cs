using System.Text;
using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

public sealed class DirectUiVirtualImageTests
{
    private static string Fixture() => File.ReadAllText(Path.Combine(
        AppContext.BaseDirectory, "ProtocolFixtures", "window.open.directui-image.json"));

    private static WindowSnapshot Read(string payload) => Assert.IsType<WindowOpenMessage>(
        ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, Encoding.UTF8.GetBytes(payload))).Window;

    [Fact]
    public void VirtualImagePreservesOwnedPixelsAndItsProviderIdentity()
    {
        var node = Read(Fixture()).Nodes[0];
        Assert.Equal("staticIcon", node.Kind);
        Assert.Equal("uiaVirtual", node.SourceKind);
        Assert.Equal("standard", node.PresentationVariant);
        Assert.Null(node.NativeHwnd);
        Assert.Equal("Provider image", node.AutomationName);
        Assert.Equal(2, node.ImageWidth);
        Assert.Equal(16, Convert.FromBase64String(node.ImageData!).Length);
        Assert.Empty(node.SupportedActions!);
    }

    [Theory]
    [InlineData("bitmapDisplay")]
    [InlineData("monitorPalette")]
    public void VirtualImageCannotClaimAnHwndBackedBitmapContract(string variant)
    {
        var snapshot = Read(Fixture());
        snapshot.Nodes[0] = snapshot.Nodes[0] with { PresentationVariant = variant };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Fact]
    public void VirtualImageCannotGainActionsOrKeyboardFocus()
    {
        var snapshot = Read(Fixture());
        var image = snapshot.Nodes[0];
        snapshot.Nodes[0] = image with { SupportedActions = ["invoke"] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = image with { TabStop = true, TabIndex = 5 };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Theory]
    [InlineData("imageWidth")]
    [InlineData("imageHeight")]
    [InlineData("imageFormat")]
    [InlineData("imageData")]
    public void VirtualImageRequiresTheCompleteOwnedPixelPayload(string missing)
    {
        var payload = JsonNode.Parse(Fixture())!;
        payload["window"]!["nodes"]![0]!.AsObject().Remove(missing);
        Assert.Throws<ProtocolException>(() => Read(payload.ToJsonString()));
    }

    [Fact]
    public void VirtualImageRetainsTheIconAllocationCap()
    {
        var snapshot = Read(Fixture());
        snapshot.Nodes[0] = snapshot.Nodes[0] with
        {
            ImageWidth = 97,
            ImageHeight = 1,
            ImageData = Convert.ToBase64String(new byte[97 * 4]),
        };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }
}
