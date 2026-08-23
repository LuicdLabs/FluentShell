using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.Runtime;

namespace FluentShell.Renderer.Windows;

public sealed class WindowRegistry
{
    private readonly Dictionary<Guid, TranslatedWindow> _windows = [];
    private readonly List<TranslatedWindow> _retiredWindows = [];
    private readonly RetiredSurfaceTracker _retiredSurfaces = new(256);
    private readonly Dictionary<Guid, Guid> _modalOwners = [];
    private readonly Dictionary<Guid, int> _modalOwnerBlockCounts = [];
    private readonly Dictionary<Guid, bool> _modalOwnerWasBlocked = [];
    private readonly string _sessionNonce;
    private readonly Func<IProtocolMessage, ulong, Task> _send;
    private long _eventSequence;

    public WindowRegistry(string sessionNonce, Func<IProtocolMessage, ulong, Task> send)
    {
        _sessionNonce = sessionNonce;
        _send = send;
    }

    public async Task HandleAsync(IProtocolMessage message, ulong headerRevision)
    {
        // A violation that names a single surface must not take the session down:
        // every other window in this target process is still projecting correctly.
        // Report it to the Bridge as a non-fatal, surface-scoped error so it rolls
        // that one window back to native, and keep reading frames.
        var scope = SurfaceScopeOf(message);
        if (scope is not { } faultedSurfaceId)
        {
            await DispatchAsync(message, headerRevision);
            return;
        }

        try
        {
            await DispatchAsync(message, headerRevision);
        }
        catch (ProtocolException exception)
        {
            RendererDiagnostics.Log(
                $"surface fault {faultedSurfaceId} on {message.MessageType}: {exception.Message}");
            FaultSurface(faultedSurfaceId);
            await _send(new ErrorMessage
            {
                SessionNonce = _sessionNonce,
                SurfaceId = faultedSurfaceId,
                Code = "surface_protocol_fault",
                Detail = exception.Message,
                Fatal = false,
            }, 0);
        }
    }

    /// <summary>
    /// The surface a fault while handling this message belongs to, or null when the
    /// message is session-scoped and any violation in it is genuinely fatal.
    /// </summary>
    private static Guid? SurfaceScopeOf(IProtocolMessage message) => message switch
    {
        WindowOpenMessage open => open.Window.SurfaceId,
        WindowPatchMessage patch => patch.SurfaceId,
        ActionResultMessage result => result.SurfaceId,
        SurfaceCommitMessage commit => commit.SurfaceId,
        WindowCloseMessage close => close.SurfaceId,
        _ => null,
    };

    private void FaultSurface(Guid surfaceId)
    {
        if (_windows.ContainsKey(surfaceId))
        {
            Close(surfaceId);
            return;
        }
        _retiredSurfaces.Remember(surfaceId);
    }

    private async Task DispatchAsync(IProtocolMessage message, ulong headerRevision)
    {
        switch (message)
        {
            case WindowOpenMessage open:
                await OpenAsync(open, headerRevision);
                break;
            case WindowPatchMessage patch:
                RequireHeaderRevision(headerRevision, patch.Revision, "window.patch");
                if (_retiredSurfaces.Contains(patch.SurfaceId)) break;
                var patched = RequireWindow(patch.SurfaceId);
                patched.ApplyPatch(patch);
                if (!patched.IsCommitted)
                {
                    await SendReadyAsync(patched, await patched.ReadSurfaceReadinessAsync());
                }
                break;
            case ActionResultMessage result:
                RequireHeaderRevision(headerRevision, result.Revision, "action.result");
                if (_retiredSurfaces.Contains(result.SurfaceId)) break;
                RequireWindow(result.SurfaceId).ApplyActionResult(result);
                break;
            case SurfaceCommitMessage commit:
                RequireHeaderRevision(headerRevision, commit.Revision, "surface.commit");
                if (_retiredSurfaces.Contains(commit.SurfaceId)) break;
                RequireWindow(commit.SurfaceId).Commit(
                    commit.Show, ParseRevision(commit.Revision), commit.Interactive);
                break;
            case WindowCloseMessage close:
                if (!_retiredSurfaces.Contains(close.SurfaceId))
                {
                    var closing = RequireWindow(close.SurfaceId);
                    if (headerRevision != closing.ViewModel.Revision)
                        throw new ProtocolException("window.close header revision differs from canonical surface revision.");
                }
                Close(close.SurfaceId);
                break;
            case HeartbeatMessage:
                RequireZeroRevision(headerRevision, "heartbeat");
                break;
            case ErrorMessage error when error.Fatal:
                RequireZeroRevision(headerRevision, "error");
                throw new ProtocolException($"Bridge reported fatal error '{error.Code}': {error.Detail}");
            case ErrorMessage:
                RequireZeroRevision(headerRevision, "error");
                break;
            case ShutdownMessage:
                RequireZeroRevision(headerRevision, "shutdown");
                CloseAll();
                break;
            default:
                throw new ProtocolException($"Renderer cannot consume inbound '{message.MessageType}'.");
        }
    }

    public void CloseAll()
    {
        _modalOwners.Clear();
        _modalOwnerBlockCounts.Clear();
        _modalOwnerWasBlocked.Clear();
        foreach (var (surfaceId, window) in _windows.ToArray())
        {
            _retiredSurfaces.Remember(surfaceId);
            window.CloseFromBridge();
        }
        foreach (var window in _retiredWindows) window.CloseFromBridge();
        _windows.Clear();
        _retiredWindows.Clear();
    }

    private async Task OpenAsync(WindowOpenMessage message, ulong headerRevision)
    {
        RendererDiagnostics.Log($"window.open {message.Window.SurfaceId} revision={message.Window.Revision} nodes={message.Window.Nodes.Count}");
        if (_windows.ContainsKey(message.Window.SurfaceId) ||
            _retiredSurfaces.Contains(message.Window.SurfaceId))
            throw new ProtocolException("Duplicate surfaceId in window.open.");
        var revision = ParseRevision(message.Window.Revision);
        if (headerRevision != revision) throw new ProtocolException("window.open header and payload revisions differ.");
        var translated = new TranslatedWindow(message.Window, SendActionAsync, NextEventId);
        _windows.Add(message.Window.SurfaceId, translated);
        var ownerReady = true;
        if (message.Window.Modal && message.Window.OwnerHwnd is not null)
        {
            var owner = _windows.FirstOrDefault(candidate =>
                string.Equals(candidate.Value.ViewModel.NativeHwnd, message.Window.OwnerHwnd, StringComparison.OrdinalIgnoreCase));
            if (owner.Value is null)
            {
                ownerReady = false;
            }
            else
            {
                translated.SetOwner(owner.Value.Hwnd);
                if (!_modalOwnerBlockCounts.ContainsKey(owner.Key))
                {
                    _modalOwnerBlockCounts[owner.Key] = 0;
                    _modalOwnerWasBlocked[owner.Key] = owner.Value.InteractionBlocked;
                }
                _modalOwnerBlockCounts[owner.Key]++;
                owner.Value.SetInteractionEnabled(false);
                _modalOwners[message.Window.SurfaceId] = owner.Key;
            }
        }
        var prepared = await translated.PrepareSurfaceAsync();
        RendererDiagnostics.Log($"surface prepared hwnd={translated.Hwnd} uia={prepared.UiaReady} bounds={prepared.Bounds.X},{prepared.Bounds.Y},{prepared.Bounds.Width}x{prepared.Bounds.Height}");

        await SendReadyAsync(translated,
            (prepared.Bounds, prepared.NodeCount, ownerReady && prepared.UiaReady));
    }

    private Task SendReadyAsync(
        TranslatedWindow translated,
        (PixelRect Bounds, int NodeCount, bool UiaReady) prepared) =>
        _send(new SurfaceReadyMessage
        {
            SessionNonce = _sessionNonce,
            SurfaceId = translated.ViewModel.SurfaceId,
            Revision = translated.ViewModel.Revision.ToString(System.Globalization.CultureInfo.InvariantCulture),
            ProxyHwnd = $"0x{(ulong)(nuint)translated.Hwnd:X}",
            Bounds = prepared.Bounds,
            NodeCount = prepared.NodeCount,
            UiaReady = prepared.UiaReady,
        }, translated.ViewModel.Revision);

    private Task SendActionAsync(ActionInvokeMessage message, ulong revision) =>
        _send(message with { SessionNonce = _sessionNonce }, revision);

    private string NextEventId() =>
        checked((ulong)Interlocked.Increment(ref _eventSequence)).ToString(System.Globalization.CultureInfo.InvariantCulture);

    private TranslatedWindow RequireWindow(Guid surfaceId) =>
        _windows.TryGetValue(surfaceId, out var window) ? window : throw new ProtocolException($"Unknown surfaceId '{surfaceId}'.");

    private void Close(Guid surfaceId)
    {
        RendererDiagnostics.Log($"window.close received surface={surfaceId} liveBefore={_windows.Count}");
        if (!_windows.Remove(surfaceId, out var window))
        {
            if (_retiredSurfaces.Contains(surfaceId)) return;
            throw new ProtocolException($"Unknown surfaceId '{surfaceId}'.");
        }
        _retiredSurfaces.Remember(surfaceId);
        if (_windows.Count == 0)
        {
            window.RetireFromBridge();
            foreach (var retired in _retiredWindows) retired.CloseFromBridge();
            _retiredWindows.Clear();
            _retiredWindows.Add(window);
        }
        else
        {
            window.CloseFromBridge();
        }
        RendererDiagnostics.Log($"window.close applied surface={surfaceId} liveAfter={_windows.Count} retired={_retiredWindows.Count}");
        if (_modalOwners.Remove(surfaceId, out var ownerId))
        {
            if (_modalOwnerBlockCounts.TryGetValue(ownerId, out var count))
            {
                if (count > 1)
                {
                    _modalOwnerBlockCounts[ownerId] = count - 1;
                }
                else
                {
                    _modalOwnerBlockCounts.Remove(ownerId);
                    var wasBlocked = _modalOwnerWasBlocked.Remove(ownerId, out var previous) && previous;
                    if (_windows.TryGetValue(ownerId, out var owner))
                    {
                        owner.SetInteractionEnabled(!wasBlocked);
                        if (!wasBlocked) owner.Reactivate();
                    }
                }
            }
        }
    }

    private static ulong ParseRevision(string revision) => ProtocolSerializer.ParseCanonicalUInt64(revision, "revision");

    private static void RequireHeaderRevision(ulong headerRevision, string payloadRevision, string messageType)
    {
        if (headerRevision != ParseRevision(payloadRevision))
            throw new ProtocolException($"{messageType} header and payload revisions differ.");
    }

    private static void RequireZeroRevision(ulong headerRevision, string messageType)
    {
        if (headerRevision != 0)
            throw new ProtocolException($"{messageType} frame must have revision zero.");
    }
}

internal sealed class RetiredSurfaceTracker
{
    private readonly int _capacity;
    private readonly HashSet<Guid> _ids = [];
    private readonly Queue<Guid> _order = [];

    internal RetiredSurfaceTracker(int capacity)
    {
        if (capacity <= 0) throw new ArgumentOutOfRangeException(nameof(capacity));
        _capacity = capacity;
    }

    internal int Count => _ids.Count;

    internal bool Contains(Guid surfaceId) => _ids.Contains(surfaceId);

    internal void Remember(Guid surfaceId)
    {
        if (!_ids.Add(surfaceId)) return;
        _order.Enqueue(surfaceId);
        while (_order.Count > _capacity)
        {
            _ids.Remove(_order.Dequeue());
        }
    }
}
