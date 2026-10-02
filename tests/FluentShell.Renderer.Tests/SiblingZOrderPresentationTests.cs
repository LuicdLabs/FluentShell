using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class SiblingZOrderPresentationTests
{
    [Theory]
    [InlineData("listView", "paneContainer")]
    [InlineData("button", "static")]
    [InlineData("mdiChild", "mdiChild")]
    public void OverlappingNativeSiblingsKeepTheFirstCapturedControlInFront(
        string frontKind, string backKind)
    {
        var front = NativeNode("2", frontKind, 1, "1");
        var back = NativeNode("4", backKind, 3, "1");

        Assert.True(ControlFactory.ProjectionZIndexFor(front) > ControlFactory.ProjectionZIndexFor(back));
        // Paint conversion must not change the canonical parent-before-child
        // ordering or the separate dialog traversal order.
        Assert.Equal(1, front.ZIndex);
        Assert.Equal(3, back.ZIndex);
        Assert.Equal("1", front.ParentNodeId);
        Assert.Equal("1", back.ParentNodeId);
        Assert.Equal(7, front.TabIndex);
        Assert.Equal(7, back.TabIndex);
    }

    [Fact]
    public void EveryNativeLayerStaysAboveChromeAndBelowIconsAndSplitters()
    {
        var layers = Enumerable.Range(0, ProtocolConstants.MaxNodes)
            .Select(index => ControlFactory.ProjectionZIndexFor(
                NativeNode((index + 1).ToString(), "button", index)))
            .ToArray();

        Assert.Equal(ProtocolConstants.MaxNodes, layers.Distinct().Count());
        Assert.Equal(0, layers.Min());
        Assert.Equal(ProtocolConstants.MaxNodes - 1, layers.Max());
        Assert.All(layers, layer =>
        {
            Assert.True(layer > ControlFactory.PaneChromeZIndex);
            Assert.True(layer < ControlFactory.DialogIconZIndex);
            Assert.True(layer < ControlFactory.PaneSplitterZIndex);
        });
        Assert.True(ControlFactory.DialogIconZIndex < ControlFactory.PaneSplitterZIndex);
    }

    [Theory]
    [InlineData("messageBox")]
    [InlineData("taskDialog")]
    public void VirtualDialogNodesRetainTheirPaintOrder(string surfaceKind)
    {
        var snapshot = TestData.DialogSnapshot() with { SurfaceKind = surfaceKind };
        var nodes = snapshot.Nodes.Select(ControlNodeViewModel.FromSnapshot).ToArray();

        Assert.All(nodes, node => Assert.Null(node.NativeHwnd));
        Assert.Equal(nodes.Select(node => node.ZIndex), nodes.Select(ControlFactory.ProjectionZIndexFor));
        Assert.True(ControlFactory.ProjectionZIndexFor(nodes[0]) < ControlFactory.ProjectionZIndexFor(nodes[1]));
    }

    [Theory]
    [InlineData("nativeBacking", "0x1234")]
    [InlineData("uiaVirtual", null)]
    public void DirectUiSlotsRetainTheirProfileOrderEvenWithNativeBacking(
        string sourceKind, string? nativeHwnd)
    {
        ControlNodeViewModel Slot(int index) => ControlNodeViewModel.FromSnapshot(
            TestData.DialogSnapshot().Nodes[1] with
            {
                NodeId = (index + 1).ToString(),
                NativeHwnd = nativeHwnd,
                ZIndex = index,
                AdapterId = ProtocolConstants.GenericDirectUiAdapterId,
                PageId = ProtocolConstants.GenericDirectUiPageId,
                SemanticKey = "slot" + index,
                SourceKind = sourceKind,
            });
        var first = Slot(0);
        var last = Slot(ProtocolConstants.MaxNodes - 1);

        Assert.Equal(first.ZIndex, ControlFactory.ProjectionZIndexFor(first));
        Assert.Equal(last.ZIndex, ControlFactory.ProjectionZIndexFor(last));
        Assert.True(ControlFactory.ProjectionZIndexFor(first) < ControlFactory.ProjectionZIndexFor(last));
    }

    private static ControlNodeViewModel NativeNode(
        string id, string kind, int zIndex, string? parentNodeId = null) =>
        ControlNodeViewModel.FromSnapshot(TestData.Snapshot().Nodes[0] with
        {
            NodeId = id,
            Kind = kind,
            ZIndex = zIndex,
            ParentNodeId = parentNodeId,
            TabIndex = 7,
        });
}
