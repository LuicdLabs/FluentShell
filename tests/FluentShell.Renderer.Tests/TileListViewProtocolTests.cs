using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

// Minor 41: a tile-view ListView (the Sound applet's devices) carries each tile's detail lines.
public sealed class TileListViewProtocolTests
{
    private static ControlNode Tiles() => ListViewModeProtocolTests.IconList("tile") with
    {
        ItemDetails = [["Realtek Audio", "Default Device"], ["USB Audio", "Ready"]],
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

    [Fact]
    public void TilesKeepTheirDetailLinesPositionsAndState()
    {
        var actual = RoundTrip(Tiles());
        Assert.Equal("tile", actual.ListViewMode);
        Assert.Equal(["USB Audio", "Ready"], actual.ItemDetails![1]);
        Assert.Equal(2, actual.ItemRects!.Count);
        var model = ControlNodeViewModel.FromSnapshot(actual);
        Assert.Equal(["Realtek Audio", "Default Device"], model.ItemDetails[0]);
        Assert.True(ControlFactory.HasRenderableListViewShape(model));
        Assert.False(ControlFactory.ShouldRenderListViewHeader(model));
    }

    [Fact]
    public void DetailLinesBelongToTileViewAndToEveryTile()
    {
        foreach (var broken in new[]
        {
            Tiles() with { ItemDetails = null },
            Tiles() with { ItemDetails = [["Realtek Audio"]] },
            Tiles() with { ItemDetails = [Enumerable.Repeat("line", ProtocolConstants.MaxTileDetailLines + 1).ToList(), []] },
            ListViewModeProtocolTests.IconList("largeIcon") with { ItemDetails = [[], []] },
        })
            Assert.Throws<ProtocolException>(() => RoundTrip(broken));
    }
}
