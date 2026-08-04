using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class RetiredSurfaceTrackerTests
{
    [Fact]
    public void DuplicateRememberIsIdempotent()
    {
        var tracker = new RetiredSurfaceTracker(2);
        var surfaceId = Guid.NewGuid();

        tracker.Remember(surfaceId);
        tracker.Remember(surfaceId);

        Assert.True(tracker.Contains(surfaceId));
        Assert.Equal(1, tracker.Count);
    }

    [Fact]
    public void OldestSurfaceExpiresAtCapacity()
    {
        var tracker = new RetiredSurfaceTracker(2);
        var first = Guid.NewGuid();
        var second = Guid.NewGuid();
        var third = Guid.NewGuid();

        tracker.Remember(first);
        tracker.Remember(second);
        tracker.Remember(third);

        Assert.False(tracker.Contains(first));
        Assert.True(tracker.Contains(second));
        Assert.True(tracker.Contains(third));
        Assert.Equal(2, tracker.Count);
    }
}
