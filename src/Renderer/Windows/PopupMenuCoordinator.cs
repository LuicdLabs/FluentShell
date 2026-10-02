using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;

namespace FluentShell.Renderer.Windows;

// The native tracking call waits for exactly one terminal response. WinUI can raise
// Closed after Click, and canonical removal can close a flyout as well; neither is
// a second cancellation. Keep the terminal state until native retires this token.
internal sealed class PopupMenuCoordinator
{
    public string? PopupId { get; private set; }
    public bool HasResponse { get; private set; }
    public bool ResponseSent { get; private set; }
    private string? _itemId;

    public bool Observe(string? popupId)
    {
        if (PopupId == popupId) return false;
        PopupId = popupId;
        HasResponse = false;
        ResponseSent = false;
        _itemId = null;
        return true;
    }

    public bool Respond(string popupId, string? itemId)
    {
        if (PopupId != popupId || HasResponse) return false;
        HasResponse = true;
        _itemId = itemId;
        return true;
    }

    public PopupCommandActionValue? PendingResponse =>
        PopupId is { } id && HasResponse && !ResponseSent
            ? new PopupCommandActionValue { PopupId = id, ItemId = _itemId }
            : null;

    public void MarkSent(string popupId)
    {
        if (PopupId == popupId && HasResponse) ResponseSent = true;
    }
}

internal static class PopupMenuAnchor
{
    public static PixelRect? ItemRect(ControlNodeViewModel? node, PopupMenuSnapshot popup)
    {
        if (node is null || node.NodeId != popup.NodeId || node.Kind != "accessibleIsland" ||
            !node.Visible || popup.ItemIndex < 0 || popup.ItemIndex >= node.IslandItems.Count)
            return null;
        var item = node.IslandItems[popup.ItemIndex];
        // A provider commonly disables the action row during native tracking.
        return item.DropDown && item.Rect.Width > 0 && item.Rect.Height > 0 ? item.Rect : null;
    }
}
