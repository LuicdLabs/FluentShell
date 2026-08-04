namespace FluentShell.Renderer.Windows;

internal readonly record struct PresenterActionIntent(string Action, int RetryCount);

internal sealed class PresenterActionCoordinator
{
    private const int MaxStaleRetries = 3;

    public string? InFlightEventId { get; private set; }
    public PresenterActionIntent? InFlightIntent { get; private set; }
    public PresenterActionIntent? QueuedIntent { get; private set; }
    public string? QueuedAction => QueuedIntent?.Action;
    public bool HasInFlight => InFlightEventId is not null;

    public string DesiredState(string canonicalState) =>
        (QueuedIntent ?? InFlightIntent) is { } intent
            ? StateForAction(intent.Action)
            : canonicalState;

    public void MarkSent(string eventId, PresenterActionIntent intent)
    {
        ArgumentException.ThrowIfNullOrEmpty(eventId);
        RequirePresenterAction(intent.Action);
        if (intent.RetryCount is < 0 or > MaxStaleRetries)
            throw new ArgumentOutOfRangeException(nameof(intent));
        if (HasInFlight) throw new InvalidOperationException("A presenter action is already in flight.");
        InFlightEventId = eventId;
        InFlightIntent = intent;
    }

    public void QueueLatest(string action)
    {
        RequirePresenterAction(action);
        if (!HasInFlight) throw new InvalidOperationException("No presenter action is in flight.");
        QueuedIntent = new PresenterActionIntent(action, 0);
    }

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

    public PresenterActionIntent? CompletePatch(string? eventId, string canonicalState)
    {
        if (eventId is null || !string.Equals(eventId, InFlightEventId, StringComparison.Ordinal))
            return null;

        InFlightEventId = null;
        InFlightIntent = null;
        var replay = QueuedIntent;
        QueuedIntent = null;
        return replay is { } intent && !MatchesCanonicalState(intent.Action, canonicalState)
            ? intent
            : null;
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

    private static bool MatchesCanonicalState(string action, string canonicalState) =>
        StateForAction(action) == canonicalState;

    private static string StateForAction(string action) =>
        action switch
        {
            "minimize" => "minimized",
            "maximize" => "maximized",
            "restore" => "normal",
            _ => throw new ArgumentOutOfRangeException(nameof(action)),
        };

    private static void RequirePresenterAction(string action)
    {
        if (action is not ("minimize" or "maximize" or "restore"))
            throw new ArgumentOutOfRangeException(nameof(action));
    }
}
