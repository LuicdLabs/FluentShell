using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class PresenterActionCoordinatorTests
{
    [Fact]
    public void RapidStateChangesReplayTheLatestIntentAfterCanonicalPatch()
    {
        var coordinator = new PresenterActionCoordinator();
        coordinator.MarkSent("10", new PresenterActionIntent("maximize", 0));
        coordinator.QueueLatest("restore");

        Assert.Equal(new PresenterActionIntent("restore", 0),
            coordinator.CompletePatch("10", "maximized"));

        coordinator.MarkSent("11", new PresenterActionIntent("restore", 0));
        Assert.Null(coordinator.CompletePatch("11", "normal"));
        Assert.False(coordinator.HasInFlight);
    }

    [Fact]
    public void MultipleQueuedChangesCoalesceToLastIntent()
    {
        var coordinator = new PresenterActionCoordinator();
        coordinator.MarkSent("20", new PresenterActionIntent("maximize", 0));
        coordinator.QueueLatest("minimize");
        coordinator.QueueLatest("restore");

        Assert.Equal(new PresenterActionIntent("restore", 0),
            coordinator.CompletePatch("20", "maximized"));
    }

    [Fact]
    public void CanonicalStateMatchingQueuedIntentNeedsNoReplay()
    {
        var coordinator = new PresenterActionCoordinator();
        coordinator.MarkSent("30", new PresenterActionIntent("restore", 0));
        coordinator.QueueLatest("maximize");

        Assert.Null(coordinator.CompletePatch("30", "maximized"));
        Assert.False(coordinator.HasInFlight);
    }

    [Fact]
    public void UnrelatedPatchDoesNotCompleteInFlightAction()
    {
        var coordinator = new PresenterActionCoordinator();
        coordinator.MarkSent("40", new PresenterActionIntent("maximize", 0));
        coordinator.QueueLatest("restore");

        Assert.Null(coordinator.CompletePatch("39", "maximized"));
        Assert.True(coordinator.HasInFlight);
        Assert.Equal("restore", coordinator.QueuedAction);
    }

    [Fact]
    public void DesiredStateTracksInFlightAndQueuedIntentOverCanonicalState()
    {
        var coordinator = new PresenterActionCoordinator();
        Assert.Equal("normal", coordinator.DesiredState("normal"));

        coordinator.MarkSent("45", new PresenterActionIntent("maximize", 0));
        Assert.Equal("maximized", coordinator.DesiredState("normal"));

        coordinator.QueueLatest("restore");
        Assert.Equal("normal", coordinator.DesiredState("normal"));
    }

    [Fact]
    public void StaleActionReplaysOnlyAfterItsResyncPatch()
    {
        var coordinator = new PresenterActionCoordinator();
        coordinator.MarkSent("50", new PresenterActionIntent("restore", 0));

        coordinator.MarkStale("50");

        Assert.True(coordinator.HasInFlight);
        Assert.Equal(new PresenterActionIntent("restore", 1),
            coordinator.CompletePatch("50", "maximized"));
    }

    [Fact]
    public void StaleRetriesAreBounded()
    {
        var coordinator = new PresenterActionCoordinator();

        for (var retry = 0; retry < 3; retry++)
        {
            var eventId = (60 + retry).ToString();
            coordinator.MarkSent(eventId, new PresenterActionIntent("restore", retry));
            coordinator.MarkStale(eventId);
            Assert.Equal(new PresenterActionIntent("restore", retry + 1),
                coordinator.CompletePatch(eventId, "maximized"));
        }

        coordinator.MarkSent("63", new PresenterActionIntent("restore", 3));
        coordinator.MarkStale("63");
        Assert.Null(coordinator.CompletePatch("63", "maximized"));
    }
}
