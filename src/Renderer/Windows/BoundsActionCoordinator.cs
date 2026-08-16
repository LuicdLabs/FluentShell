using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Windows;

internal readonly record struct BoundsActionIntent(
    string Action,
    PixelRect Bounds,
    int RetryCount);

/// <summary>
/// Coalesces outbound window geometry actions so a drag or resize gesture costs one
/// native write instead of one per frame.  Mirrors
/// <see cref="PresenterActionCoordinator"/>: a single action in flight, one queued
/// latest-wins intent behind it, and a bounded replay on staleness.
/// </summary>
internal sealed class BoundsActionCoordinator
{
    private const int MaxStaleRetries = 3;

    public string? InFlightEventId { get; private set; }
    public BoundsActionIntent? InFlightIntent { get; private set; }
    public BoundsActionIntent? QueuedIntent { get; private set; }
    public bool HasInFlight => InFlightEventId is not null;

    public void MarkSent(string eventId, BoundsActionIntent intent)
    {
        ArgumentException.ThrowIfNullOrEmpty(eventId);
        RequireBoundsAction(intent.Action);
        if (intent.RetryCount is < 0 or > MaxStaleRetries)
            throw new ArgumentOutOfRangeException(nameof(intent));
        if (HasInFlight) throw new InvalidOperationException("A bounds action is already in flight.");
        InFlightEventId = eventId;
        InFlightIntent = intent;
    }

    /// <summary>
    /// Replaces any queued intent.  An intermediate pointer position has no value
    /// once a newer one exists, so the newest geometry always wins.
    /// </summary>
    public void QueueLatest(BoundsActionIntent intent)
    {
        RequireBoundsAction(intent.Action);
        if (!HasInFlight) throw new InvalidOperationException("No bounds action is in flight.");
        QueuedIntent = intent with { RetryCount = 0 };
    }

    /// <summary>
    /// The Bridge treats geometry as request semantic, so a stale result is not
    /// expected.  Handle it anyway: dropping the intent would strand the window at
    /// a position the user did not choose.
    /// </summary>
    public void MarkStale(string eventId)
    {
        if (!string.Equals(eventId, InFlightEventId, StringComparison.Ordinal) ||
            InFlightIntent is not { } inFlight || QueuedIntent is not null ||
            inFlight.RetryCount >= MaxStaleRetries)
        {
            return;
        }

        QueuedIntent = inFlight with { RetryCount = inFlight.RetryCount + 1 };
    }

    public BoundsActionIntent? CompletePatch(string? eventId, PixelRect canonicalBounds)
    {
        if (eventId is null || !string.Equals(eventId, InFlightEventId, StringComparison.Ordinal))
            return null;

        InFlightEventId = null;
        InFlightIntent = null;
        var replay = QueuedIntent;
        QueuedIntent = null;
        return replay is { } intent && intent.Bounds != canonicalBounds ? intent : null;
    }

    public void CancelIfMatches(string eventId)
    {
        if (!string.Equals(eventId, InFlightEventId, StringComparison.Ordinal)) return;
        Reset();
    }

    public void Reset()
    {
        InFlightEventId = null;
        InFlightIntent = null;
        QueuedIntent = null;
    }

    private static void RequireBoundsAction(string action)
    {
        if (action is not ("move" or "resize"))
            throw new ArgumentOutOfRangeException(nameof(action));
    }
}
