using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

// A splitter drag tracks locally and makes one native request, on release.
public sealed class PaneSplitDragTests
{
    [Fact]
    public void ManyPointerMovesProduceOneRequestForTheFinalPosition()
    {
        var drag = new PaneSplitDrag();
        drag.Begin(200);
        var previews = 0;
        for (var position = 201; position <= 320; ++position)
            if (drag.Move(position)) ++previews;
        Assert.Equal(120, previews);
        Assert.False(drag.Move(320));
        Assert.Equal(320, drag.Complete());
        Assert.False(drag.Active);
        Assert.Null(drag.Complete());
    }

    [Fact]
    public void ADragThatReturnsToItsOriginRequestsNothing()
    {
        var drag = new PaneSplitDrag();
        drag.Begin(200);
        drag.Move(260);
        drag.Move(200);
        Assert.Null(drag.Complete());
    }

    [Fact]
    public void CancelledDragRequestsNothingAndIgnoresLaterMoves()
    {
        var drag = new PaneSplitDrag();
        drag.Begin(200);
        drag.Move(240);
        drag.Cancel();
        Assert.False(drag.Move(250));
        Assert.Null(drag.Complete());
        Assert.Equal(200, drag.Origin);
    }
}
