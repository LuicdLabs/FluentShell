using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class BoundsActionCoordinatorTests
{
    private static PixelRect Rect(int x, int y, int width = 400, int height = 300) =>
        new() { X = x, Y = y, Width = width, Height = height };

    [Fact]
    public void DragFramesCoalesceToTheFinalPosition()
    {
        var coordinator = new BoundsActionCoordinator();
        coordinator.MarkSent("10", new BoundsActionIntent("move", Rect(10, 10), 0));
        coordinator.QueueLatest(new BoundsActionIntent("move", Rect(20, 20), 0));
        coordinator.QueueLatest(new BoundsActionIntent("move", Rect(30, 30), 0));

        Assert.Equal(new BoundsActionIntent("move", Rect(30, 30), 0),
            coordinator.CompletePatch("10", Rect(10, 10)));
        Assert.False(coordinator.HasInFlight);
    }

    [Fact]
    public void CanonicalBoundsMatchingQueuedIntentNeedsNoReplay()
    {
        var coordinator = new BoundsActionCoordinator();
        coordinator.MarkSent("20", new BoundsActionIntent("move", Rect(10, 10), 0));
        coordinator.QueueLatest(new BoundsActionIntent("move", Rect(40, 40), 0));

        Assert.Null(coordinator.CompletePatch("20", Rect(40, 40)));
        Assert.False(coordinator.HasInFlight);
    }

    [Fact]
    public void UnrelatedPatchDoesNotCompleteInFlightAction()
    {
        var coordinator = new BoundsActionCoordinator();
        coordinator.MarkSent("30", new BoundsActionIntent("move", Rect(10, 10), 0));
        coordinator.QueueLatest(new BoundsActionIntent("resize", Rect(10, 10, 800, 600), 0));

        Assert.Null(coordinator.CompletePatch("29", Rect(10, 10)));
        Assert.True(coordinator.HasInFlight);
        Assert.Equal(new BoundsActionIntent("resize", Rect(10, 10, 800, 600), 0),
            coordinator.QueuedIntent);
    }

    [Fact]
    public void StaleActionReplaysOnlyAfterItsResyncPatch()
    {
        var coordinator = new BoundsActionCoordinator();
        coordinator.MarkSent("40", new BoundsActionIntent("move", Rect(50, 50), 0));

        coordinator.MarkStale("40");

        Assert.True(coordinator.HasInFlight);
        Assert.Equal(new BoundsActionIntent("move", Rect(50, 50), 1),
            coordinator.CompletePatch("40", Rect(10, 10)));
    }

    [Fact]
    public void StaleRetriesAreBounded()
    {
        var coordinator = new BoundsActionCoordinator();

        for (var retry = 0; retry < 3; retry++)
        {
            var eventId = (50 + retry).ToString();
            coordinator.MarkSent(eventId, new BoundsActionIntent("move", Rect(50, 50), retry));
            coordinator.MarkStale(eventId);
            Assert.Equal(new BoundsActionIntent("move", Rect(50, 50), retry + 1),
                coordinator.CompletePatch(eventId, Rect(10, 10)));
        }

        coordinator.MarkSent("53", new BoundsActionIntent("move", Rect(50, 50), 3));
        coordinator.MarkStale("53");
        Assert.Null(coordinator.CompletePatch("53", Rect(10, 10)));
    }

    [Fact]
    public void RejectedActionClearsTheGestureSoPlacementCanResume()
    {
        var coordinator = new BoundsActionCoordinator();
        coordinator.MarkSent("60", new BoundsActionIntent("move", Rect(50, 50), 0));
        coordinator.QueueLatest(new BoundsActionIntent("move", Rect(60, 60), 0));

        coordinator.CancelIfMatches("60");

        Assert.False(coordinator.HasInFlight);
        Assert.Null(coordinator.QueuedIntent);
    }

    [Fact]
    public void OnlyGeometryActionsAreAccepted()
    {
        var coordinator = new BoundsActionCoordinator();

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            coordinator.MarkSent("70", new BoundsActionIntent("maximize", Rect(10, 10), 0)));
        Assert.Throws<InvalidOperationException>(() =>
            coordinator.QueueLatest(new BoundsActionIntent("move", Rect(10, 10), 0)));

        coordinator.MarkSent("71", new BoundsActionIntent("resize", Rect(10, 10), 0));
        Assert.Throws<InvalidOperationException>(() =>
            coordinator.MarkSent("72", new BoundsActionIntent("move", Rect(10, 10), 0)));
    }
}
