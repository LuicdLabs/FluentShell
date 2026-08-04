using FluentShell.Renderer.Runtime;

namespace FluentShell.Renderer.Tests;

public sealed class HeartbeatProbeTrackerTests
{
    [Fact]
    public void FirstTwoConsecutiveMissesAreTolerated()
    {
        var tracker = new HeartbeatProbeTracker();

        Assert.True(tracker.RecordMiss());
        Assert.Equal(1, tracker.ConsecutiveMisses);
        Assert.True(tracker.RecordMiss());
        Assert.Equal(2, tracker.ConsecutiveMisses);
    }

    [Fact]
    public void ThirdMissIsTerminalAndSuccessResetsTheSequence()
    {
        var tracker = new HeartbeatProbeTracker();

        Assert.True(tracker.RecordMiss());
        Assert.True(tracker.RecordMiss());
        Assert.False(tracker.RecordMiss());
        Assert.Equal(3, tracker.ConsecutiveMisses);

        tracker.RecordSuccess();

        Assert.Equal(0, tracker.ConsecutiveMisses);
        Assert.True(tracker.RecordMiss());
    }
}
