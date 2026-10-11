using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.Runtime;
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
    // Minor 33: a windowed virtual list shows its carried rows between two spacers that
    // stand for the rows it does not carry, so the scroll extent is the whole list's.

    // The row at the top of the viewport, from the list's scroll offset.
    internal static int RowAtOffset(double verticalOffset, double headerHeight, double rowHeight, int rowCount)
    {
        if (rowCount <= 0 || rowHeight <= 0 || double.IsNaN(verticalOffset)) return 0;
        var row = Math.Floor(Math.Max(0, verticalOffset - headerHeight) / rowHeight);
        return (int)Math.Clamp(row, 0, rowCount - 1);
    }

    // Whether the rows on screen, with a margin either side, lie inside the carried
    // window.  The margin asks for the next window before any spacer comes into view.
    internal static bool WindowCovers(int windowStart, int carried, int firstVisible, int visibleRows,
        int rowCount, int margin)
    {
        var first = Math.Max(0, firstVisible - margin);
        var last = Math.Min(rowCount, (long)firstVisible + visibleRows + margin);
        return first >= windowStart && last <= (long)windowStart + carried;
    }

    // The spacer heights above and below the carried rows.
    internal static (double Above, double Below) SpacerHeights(int windowStart, int carried, int rowCount,
        double rowHeight) =>
        (Math.Max(0, windowStart) * rowHeight, Math.Max(0, rowCount - windowStart - carried) * rowHeight);

    // A positioned list is a Canvas holding every item of the native list, wherever its scroll position
    // puts it, and the accessibility provider has to describe each one that exists: a list of a couple of
    // hundred counter sets (Performance Monitor's Add Counters) kept it busy for seconds. Only an item that
    // is shown, or lies within two viewports of the native viewport, is built; native scrolling recaptures
    // the list and builds the rest as the viewport reaches it. The rectangle is in the list's client
    // coordinates, so the native viewport is (0, 0, viewportWidth, viewportHeight).
    internal static bool InBuildWindow(PixelRect rect, int viewportWidth, int viewportHeight)
    {
        if (rect.Width == 0 || rect.Height == 0) return false;
        if (viewportWidth <= 0 || viewportHeight <= 0) return true;
        return rect.Y + rect.Height >= -2 * viewportHeight && rect.Y <= 3 * viewportHeight &&
            rect.X + rect.Width >= -2 * viewportWidth && rect.X <= 3 * viewportWidth;
    }

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
        var visible = Enumerable.Range(0, items.Count).Where(index => items[index].Width > 0 && items[index].Height > 0).ToArray();
        if (visible.Length == 0) return -1;
        if (key == VirtualKey.Home) return visible[0];
        if (key == VirtualKey.End) return visible[^1];
        if (current < 0 || current >= items.Count || items[current].Width <= 0 || items[current].Height <= 0) return visible[0];
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
            if (candidate.Width <= 0 || candidate.Height <= 0) continue;
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
        Cancel("a new request");
        if (_disposed || _applyingCanonical() || !_node.ItemActivationSupported ||
            !_allows("activateItem") || index < 0 || index >= _node.ItemNativeIds.Count)
        {
            RendererDiagnostics.Log($"list activation node={_node.NodeId} index={index} not requested " +
                $"(disposed={_disposed} applying={_applyingCanonical()} supported={_node.ItemActivationSupported} " +
                $"allowed={_allows("activateItem")} ids={_node.ItemNativeIds.Count})");
            return;
        }
        _intent = new(index, _node.ItemNativeIds[index]);
        Flush();
    }

    // A gesture that is dropped says why: a double-click that does nothing has to be explainable from the log.
    internal void Cancel(string reason = "")
    {
        if (_intent is { } dropped && reason.Length != 0)
            RendererDiagnostics.Log($"list activation node={_node.NodeId} index={dropped.Index} dropped: {reason}");
        _intent = null;
        _selectionRequested = _focusRequested = false;
    }

    private void OnRejected(string property)
    {
        if (property is "selectedIndices" or "focusedIndex") Cancel($"{property} was rejected");
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
        if (!_flushQueued) Cancel("the dispatcher refused the continuation");
    }

    private void Flush()
    {
        if (_disposed || _intent is not { } intent) return;
        if (_applyingCanonical()) { QueueFlush(); return; }
        if (!_node.ItemActivationSupported || !_node.Enabled || !_node.Visible || !_allows("activateItem") ||
            ListViewActivationIntentPolicy.Decide(intent, _node.ItemNativeIds,
                _node.SelectedIndices, _node.FocusedIndex) == ListViewActivationDecision.Drop)
        {
            Cancel("the list or the item it named changed");
            return;
        }
        var selected = _node.SelectedIndices.Contains(intent.Index);
        var focused = _node.FocusedIndex == intent.Index;
        if ((_selectionRequested && !_node.HasPending("selectedIndices") && !selected) ||
            (_focusRequested && !_node.HasPending("focusedIndex") && !focused))
        {
            Cancel("the native list did not select or focus the item");
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
            Cancel("selection or focus could not be requested");
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
