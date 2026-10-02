using FluentShell.Renderer.Protocol;
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

internal readonly record struct ListViewActivationIntent(int Index, string Identity);

internal enum ListViewActivationDecision
{
    Wait,
    Activate,
    Drop,
}

internal static class ListViewActivationIntentPolicy
{
    // The intention names the item by the label it had when the user asked.
    // A later snapshot may carry that same label at the same index; a different
    // label there is a different item, and activating it would retarget the gesture.
    public static ListViewActivationDecision Decide(
        ListViewActivationIntent intent,
        IReadOnlyList<string> items,
        IReadOnlyList<int> selected,
        int focused)
    {
        if (intent.Index < 0 || intent.Index >= items.Count ||
            !string.Equals(items[intent.Index], intent.Identity, StringComparison.Ordinal))
            return ListViewActivationDecision.Drop;
        return focused == intent.Index && selected.Contains(intent.Index)
            ? ListViewActivationDecision.Activate
            : ListViewActivationDecision.Wait;
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
