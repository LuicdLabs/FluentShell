using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

public sealed class MdiNavigationProtocolTests
{
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void MdiCaptionStylesDoNotBecomeDialogTabStops(bool enabled)
    {
        var snapshot = MdiSnapshot(enabled);

        ProtocolValidator.ValidateSnapshot(snapshot);

        Assert.Equal("0x54CF0000", snapshot.Nodes[1].Style);
        Assert.True(snapshot.Nodes[2].TabStop);
        Assert.Equal(enabled ? 0 : -1, snapshot.Nodes[2].TabIndex);
    }

    [Theory]
    [InlineData("mdiClient", true)]
    [InlineData("mdiClient", false)]
    [InlineData("mdiChild", true)]
    [InlineData("mdiChild", false)]
    public void StructuralMdiNodesRejectTabStopsEvenWhenDisabled(string kind, bool enabled)
    {
        var snapshot = MdiSnapshot(enabled);
        var index = kind == "mdiClient" ? 0 : 1;
        var structural = snapshot.Nodes[index];

        snapshot.Nodes[index] = structural with { TabStop = true };
        Assert.Contains("tab stop", Assert.Throws<ProtocolException>(
            () => ProtocolValidator.ValidateSnapshot(snapshot)).Message);

        snapshot.Nodes[index] = structural with { TabIndex = 0 };
        Assert.Contains("tab stop", Assert.Throws<ProtocolException>(
            () => ProtocolValidator.ValidateSnapshot(snapshot)).Message);
    }

    private static WindowSnapshot MdiSnapshot(bool enabled) => TestData.Snapshot() with
    {
        Enabled = enabled,
        Nodes =
        [
            new ControlNode
            {
                NodeId = "20", Generation = "1", NativeHwnd = "0x2000", Kind = "mdiClient",
                ZIndex = 0, TabIndex = -1, TabStop = false, Visible = true, Enabled = enabled,
                Style = "0x50010000", ExStyle = "0x00010000",
                Rect = new PixelRect { Width = 560, Height = 300 },
            },
            new ControlNode
            {
                NodeId = "21", Generation = "1", NativeHwnd = "0x2100", Kind = "mdiChild",
                ParentNodeId = "20", ZIndex = 1, TabIndex = -1, TabStop = false,
                Visible = true, Enabled = enabled, Style = "0x54CF0000", ExStyle = "0x00010040",
                Rect = new PixelRect { X = 10, Y = 10, Width = 340, Height = 200 },
                Text = "Document", AutomationName = "Document", Active = true, WindowState = "normal",
                ClientRect = new PixelRect { X = 4, Y = 24, Width = 332, Height = 172 },
            },
            new ControlNode
            {
                NodeId = "22", Generation = "1", NativeHwnd = "0x2200", Kind = "edit",
                ParentNodeId = "21", ZIndex = 2, TabIndex = enabled ? 0 : -1, TabStop = true,
                Visible = true, Enabled = enabled, Style = "0x50010000", ExStyle = "0x00000000",
                Rect = new PixelRect { X = 24, Y = 44, Width = 180, Height = 24 },
                Text = "document control", AutomationName = "document control",
            },
        ],
    };
}
