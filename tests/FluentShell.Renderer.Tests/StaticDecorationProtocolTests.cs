using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

public sealed class StaticDecorationProtocolTests
{
    [Theory]
    [InlineData("0x50000004")]
    [InlineData("0x50000005")]
    [InlineData("0x50000006")]
    [InlineData("0x50000007")]
    [InlineData("0x50000008")]
    [InlineData("0x50000009")]
    [InlineData("0x50000012")]
    [InlineData("0x50001007")]
    public void BuiltInStaticFramesAndFillsAreAdmitted(string style)
    {
        ProtocolValidator.ValidateSnapshot(Snapshot(Node(style)));
    }

    [Theory]
    [InlineData("0x50000107", "0x00020004")]
    [InlineData("0x50010007", "0x00020004")]
    [InlineData("0x5000000D", "0x00020004")]
    [InlineData("0x5000000E", "0x00020004")]
    [InlineData("0x50000007", "0x00000020")]
    [InlineData("0x50800007", "0x00020004")]
    public void InteractiveOwnerDrawBitmapAndUnrepresentedChromeAreRejected(string style, string exStyle)
    {
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(
            Snapshot(Node(style) with { ExStyle = exStyle })));
    }

    [Fact]
    public void DecorationsRequireNativeEvidenceAndCarryNoLabelFocusOrActionContract()
    {
        var node = Node();
        ControlNode[] rejected =
        [
            node with { NativeHwnd = null }, node with { NativeHwnd = "0x0" },
            node with { Style = null }, node with { ExStyle = null },
            node with { TabStop = true }, node with { TabIndex = 0 },
            node with { Enabled = false, TabStop = true }, node with { DialogCode = 4 },
            node with { Text = "Label" }, node with { AutomationName = "Label" },
            node with { SupportedActions = ["invoke"] }, node with { Items = ["Item"] },
        ];
        foreach (var invalid in rejected)
            Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(Snapshot(invalid)));
    }

    [Fact]
    public void DecorationCannotOwnAnotherProjectedControl()
    {
        var snapshot = Snapshot(Node());
        snapshot.Nodes.Add(TestData.Snapshot().Nodes[0] with
        {
            NodeId = "11", ZIndex = 1, ParentNodeId = "10",
        });
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    internal static ControlNode Node(string style = "0x50001007") => new()
    {
        NodeId = "10", Generation = "1", NativeHwnd = "0x5678", Kind = "staticDecoration",
        Style = style, ExStyle = "0x00020004", ZIndex = 0, TabIndex = -1,
        Rect = new PixelRect { X = 10, Y = 10, Width = 250, Height = 100 },
        Visible = true, Enabled = true, DialogCode = 0x0100,
    };

    private static WindowSnapshot Snapshot(ControlNode node) => TestData.Snapshot() with { Nodes = [node] };
}
