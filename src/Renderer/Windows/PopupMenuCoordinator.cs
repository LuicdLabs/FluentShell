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
        if (node is { Kind: "diskView", Visible: true, DiskView: { } view } &&
            node.NodeId == popup.NodeId && popup.ItemIndex >= 0 && popup.ItemIndex < view.Items.Count)
        {
            var native = view.Items[popup.ItemIndex].Rect;
            return native with { X = native.X + view.ClientRect.X, Y = native.Y + view.ClientRect.Y };
        }
        // A toolbar button's menu opens under the button, whose rectangle is in the toolbar's own
        // client coordinates, which is the origin the toolbar's element has.
        if (node is { Kind: "toolbar", Visible: true } && node.NodeId == popup.NodeId &&
            popup.ItemIndex >= 0 && popup.ItemIndex < node.ToolbarItems.Count)
        {
            var button = node.ToolbarItems[popup.ItemIndex];
            return button.DropDown == true && button.Rect.Width > 0 && button.Rect.Height > 0 ? button.Rect : null;
        }
        // A split button's menu opens under the whole button, in the button's own coordinates.
        if (node is { Kind: "button", Visible: true, PresentationVariant: "splitButton" } &&
            node.NodeId == popup.NodeId && popup.ItemIndex == 0)
            return node.Rect.Width > 0 && node.Rect.Height > 0
                ? new PixelRect { Width = node.Rect.Width, Height = node.Rect.Height } : null;
        if (node is null || node.NodeId != popup.NodeId || node.Kind != "accessibleIsland" ||
            !node.Visible || popup.ItemIndex < 0 || popup.ItemIndex >= node.IslandItems.Count)
            return null;
        var item = node.IslandItems[popup.ItemIndex];
        // A provider commonly disables the action row during native tracking.
        return item.DropDown && item.Rect.Width > 0 && item.Rect.Height > 0 ? item.Rect : null;
    }
}
