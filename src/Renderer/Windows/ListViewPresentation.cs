using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using System.ComponentModel;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.UI.Xaml.Controls;
using Windows.System;

namespace FluentShell.Renderer.Windows;

internal static class ListViewPresentation
{
    internal static IEnumerable<int> SelectionRange(int itemCount, int anchor, int destination)
    {
        if (itemCount <= 0 || destination < 0 || destination >= itemCount) return [];
        if (anchor < 0 || anchor >= itemCount) anchor = destination;
        var first = Math.Min(anchor, destination);
        return Enumerable.Range(first, Math.Max(anchor, destination) - first + 1);
    }

    internal static int Navigate(IReadOnlyList<PixelRect> items, int current, VirtualKey key)
    {
        if (items.Count == 0) return -1;
        if (key == VirtualKey.Home) return 0;
        if (key == VirtualKey.End) return items.Count - 1;
        if (current < 0 || current >= items.Count) return 0;
        if (key is not (VirtualKey.Left or VirtualKey.Right or VirtualKey.Up or VirtualKey.Down)) return current;
        var horizontal = key is VirtualKey.Left or VirtualKey.Right;
        var forward = key is VirtualKey.Right or VirtualKey.Down;
        var origin = items[current];
        double Center(PixelRect rect, bool x) => x ? rect.X + rect.Width / 2.0 : rect.Y + rect.Height / 2.0;
        var along = Center(origin, horizontal);
        var across = Center(origin, !horizontal);
        var best = current;
        var bestScore = (Aligned: 2, Across: double.MaxValue, Along: double.MaxValue);
        for (var index = 0; index < items.Count; ++index)
        {
            if (index == current) continue;
            var candidate = items[index];
            var delta = (Center(candidate, horizontal) - along) * (forward ? 1 : -1);
            if (delta <= 0) continue;
            var start = horizontal ? candidate.Y : candidate.X;
            var extent = horizontal ? candidate.Height : candidate.Width;
            var aligned = across >= start && across <= (double)start + extent;
            var score = (Aligned: aligned ? 0 : 1,
                Across: aligned ? 0 : Math.Abs(Center(candidate, !horizontal) - across), Along: delta);
            if (score.CompareTo(bestScore) >= 0) continue;
            bestScore = score;
            best = index;
        }
        return best;
    }

    // Item rectangles are native client coordinates. A scrolled native item can
    // start before zero; retain that origin when forming the scrollable canvas.
    internal static PixelRect ContentBounds(IReadOnlyList<PixelRect> items, PixelRect viewport)
    {
        var left = Math.Min(0, items.Select(rect => rect.X).DefaultIfEmpty(0).Min());
        var top = Math.Min(0, items.Select(rect => rect.Y).DefaultIfEmpty(0).Min());
        var right = Math.Max(viewport.Width, items.Select(rect => (long)rect.X + rect.Width).DefaultIfEmpty(0).Max());
        var bottom = Math.Max(viewport.Height, items.Select(rect => (long)rect.Y + rect.Height).DefaultIfEmpty(0).Max());
        return new PixelRect
        {
            X = left, Y = top,
            Width = checked((int)(right - left)), Height = checked((int)(bottom - top)),
        };
    }

    // The proxy scroll offset is in DIPs and the native list scrolls in pixels.
    // `alreadySent` is the portion of the current gesture already handed to the
    // control, so a stream of ViewChanged events does not replay the same delta.
    internal static bool TryIncrementalScroll(
        double offsetX, double offsetY, double pinnedX, double pinnedY, double scale,
        int alreadySentX, int alreadySentY, out int dx, out int dy)
    {
        dx = dy = 0;
        if (scale <= 0 || !double.IsFinite(offsetX) || !double.IsFinite(offsetY) ||
            !double.IsFinite(pinnedX) || !double.IsFinite(pinnedY)) return false;
        var totalX = (int)Math.Round((offsetX - pinnedX) / scale);
        var totalY = (int)Math.Round((offsetY - pinnedY) / scale);
        dx = Math.Clamp(totalX - alreadySentX, -65535, 65535);
        dy = Math.Clamp(totalY - alreadySentY, -65535, 65535);
        return dx != 0 || dy != 0;
    }
}

internal readonly record struct ListViewActivationIntent(int Index, string NativeId);

internal enum ListViewActivationDecision
{
    Wait,
    Activate,
    Drop,
}

internal static class ListViewActivationIntentPolicy
{
    // Selection and focus acknowledgements can arrive after a row was removed
    // or replaced. Labels and indexes are not identities, even when both match.
    // Keep the native ID captured when the user asked; a move also drops the
    // intention rather than interpreting the old index as a different target.
    public static ListViewActivationDecision Decide(
        ListViewActivationIntent intent,
        IReadOnlyList<string> nativeIds,
        IReadOnlyList<int> selected,
        int focused)
    {
        if (string.IsNullOrEmpty(intent.NativeId) || intent.Index < 0 || intent.Index >= nativeIds.Count ||
            !string.Equals(nativeIds[intent.Index], intent.NativeId, StringComparison.Ordinal))
            return ListViewActivationDecision.Drop;
        return focused == intent.Index && selected.Contains(intent.Index)
            ? ListViewActivationDecision.Activate
            : ListViewActivationDecision.Wait;
    }
}

// Owns one gesture through its real selection/focus acknowledgements. A request
// that finishes without those states, or is definitively rejected, consumes the
// gesture; selecting the same item later must not revive an old double-click.
internal sealed class ListViewActivationCoordinator : IDisposable
{
    private readonly ControlNodeViewModel _node;
    private readonly Action<string, object?> _send;
    private readonly Func<string, bool> _allows;
    private readonly Func<bool> _applyingCanonical;
    private readonly Func<Action, bool> _enqueue;
    private ListViewActivationIntent? _intent;
    private bool _selectionRequested;
    private bool _focusRequested;
    private bool _flushQueued;
    private bool _disposed;

    internal ListViewActivationCoordinator(ControlNodeViewModel node,
        Action<string, object?> send, Func<string, bool> allows,
        Func<bool> applyingCanonical, Func<Action, bool> enqueue)
    {
        _node = node;
        _send = send;
        _allows = allows;
        _applyingCanonical = applyingCanonical;
        _enqueue = enqueue;
        node.PropertyChanged += OnCanonicalChanged;
        node.PendingActionsChanged += QueueFlush;
        node.PendingActionRejected += OnRejected;
    }

    internal bool HasPending => _intent is not null;

    internal void Request(int index)
    {
        Cancel();
        if (_disposed || _applyingCanonical() || !_node.ItemActivationSupported ||
            !_allows("activateItem") || index < 0 || index >= _node.ItemNativeIds.Count) return;
        _intent = new(index, _node.ItemNativeIds[index]);
        Flush();
    }

    internal void Cancel()
    {
        _intent = null;
        _selectionRequested = _focusRequested = false;
    }

    private void OnRejected(string property)
    {
        if (property is "selectedIndices" or "focusedIndex") Cancel();
    }

    private void OnCanonicalChanged(object? sender, PropertyChangedEventArgs args)
    {
        if (args.PropertyName is nameof(_node.ItemNativeIds) or nameof(_node.SelectedIndices) or
            nameof(_node.FocusedIndex) or nameof(_node.ItemActivationSupported) or nameof(_node.Enabled) or
            nameof(_node.Visible)) QueueFlush();
    }

    private void QueueFlush()
    {
        if (_disposed || _intent is null || _flushQueued) return;
        _flushQueued = _enqueue(() =>
        {
            _flushQueued = false;
            Flush();
        });
        if (!_flushQueued) Cancel();
    }

    private void Flush()
    {
        if (_disposed || _intent is not { } intent) return;
        if (_applyingCanonical()) { QueueFlush(); return; }
        if (!_node.ItemActivationSupported || !_node.Enabled || !_node.Visible || !_allows("activateItem") ||
            ListViewActivationIntentPolicy.Decide(intent, _node.ItemNativeIds,
                _node.SelectedIndices, _node.FocusedIndex) == ListViewActivationDecision.Drop)
        {
            Cancel();
            return;
        }
        var selected = _node.SelectedIndices.Contains(intent.Index);
        var focused = _node.FocusedIndex == intent.Index;
        if ((_selectionRequested && !_node.HasPending("selectedIndices") && !selected) ||
            (_focusRequested && !_node.HasPending("focusedIndex") && !focused))
        {
            Cancel();
            return;
        }
        if (!_selectionRequested)
        {
            _selectionRequested = true;
            if (!selected && !_node.HasPending("selectedIndices") && _allows("setSelection"))
            {
                var selection = _node.MultiSelect
                    ? _node.SelectedIndices.Append(intent.Index).Distinct().Order().ToArray()
                    : new[] { intent.Index };
                _send("setSelection", selection);
            }
        }
        if (_intent is null) return;
        if (!_focusRequested)
        {
            _focusRequested = true;
            if (!focused && !_node.HasPending("focusedIndex") && _allows("setFocusedIndex"))
                _send("setFocusedIndex", intent.Index);
        }
        if (_intent is null) return;
        // Emission can be gated, and an existing request may have been joined.
        // Each prerequisite gets one attempt; only an actual pending request
        // can justify waiting for a state that has not arrived.
        if ((!selected && !_node.HasPending("selectedIndices")) ||
            (!focused && !_node.HasPending("focusedIndex")))
        {
            Cancel();
            return;
        }
        if (_node.HasPending("selectedIndices") || _node.HasPending("focusedIndex")) return;
        Cancel();
        _send("activateItem", intent.Index);
    }

    public void Dispose()
    {
        _disposed = true;
        Cancel();
        _node.PropertyChanged -= OnCanonicalChanged;
        _node.PendingActionsChanged -= QueueFlush;
        _node.PendingActionRejected -= OnRejected;
    }
}

internal sealed class ActivatableListViewItem : ListViewItem
{
    public ActivatableListViewItem() => DefaultStyleKey = typeof(ListViewItem);
    internal Action? Activate { get; init; }
    protected override AutomationPeer OnCreateAutomationPeer() => new ActivatableListViewItemPeer(this);
}

internal sealed class ActivatableListViewItemPeer(ActivatableListViewItem owner) :
    ListViewItemAutomationPeer(owner), IInvokeProvider
{
    protected override object GetPatternCore(PatternInterface patternInterface) =>
        patternInterface == PatternInterface.Invoke && owner.Activate is not null
            ? this : base.GetPatternCore(patternInterface);

    public void Invoke()
    {
        if (!owner.IsEnabled) throw new ElementNotEnabledException();
        owner.Activate?.Invoke();
    }
}
