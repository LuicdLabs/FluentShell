namespace FluentShell.Renderer.Runtime;

internal sealed class HeartbeatProbeTracker
{
    internal const int MaxConsecutiveMisses = 3;

    internal int ConsecutiveMisses { get; private set; }

    internal void RecordSuccess() => ConsecutiveMisses = 0;

    // Returns true while a miss can be tolerated and false on the terminal miss.
    internal bool RecordMiss()
    {
        ConsecutiveMisses = Math.Min(ConsecutiveMisses + 1, MaxConsecutiveMisses);
        return ConsecutiveMisses < MaxConsecutiveMisses;
    }
}
