using FluentShell.Renderer.ViewModels;

namespace FluentShell.Renderer.Windows;

/// <summary>
/// One action the renderer has sent and not yet seen answered.  The action name and
/// value travel with it so a refusal the Bridge attributes to a revision race can be
/// re-sent unchanged.
/// </summary>
internal readonly record struct PendingNodeAction(
    ControlNodeViewModel? Node,
    string Property,
    string Action,
    object? Value,
    int RetryCount);

/// <summary>
/// A node property action the Bridge refused as <c>stale</c>, waiting for the patch
/// that carries the revision which made it stale.  The node is resolved again by id at
/// replay time, because a structural patch can replace the node collection.
/// </summary>
internal readonly record struct NodeActionReplay(
    string NodeId,
    string Property,
    string Action,
    object? Value,
    int RetryCount);

/// <summary>
/// A menu-bar command the Bridge refused as <c>stale</c>, as the user saw its item: where
/// it sits in the menu, its command and its label.  A menu has no node to look up again,
/// so the item itself is the identity the replay is checked against.
/// </summary>
internal readonly record struct MenuActionReplay(
    string ItemId,
    int CommandId,
    string Text,
    int RetryCount);

internal static class MenuActionReplayPolicy
{
    /// <summary>
    /// The item a refused menu click may be re-sent for, or null.  A reconcile that lands
    /// between the click and its admission only moves the revision; the user still chose
    /// that item.  The click is therefore kept when the menu that replaced the one the
    /// user saw still has the same enabled command, under the same path, with the same
    /// label -- the Bridge then validates it against that menu like any other.  A menu
    /// that was rebuilt around a different item is a different request, and the user's
    /// native state stays the truth.
    /// </summary>
    public static MenuItemViewModel? Resolve(IEnumerable<MenuItemViewModel> menu, MenuActionReplay replay)
    {
        foreach (var item in menu)
        {
            if (item.ItemId == replay.ItemId)
            {
                return item.Kind == "command" && item.Enabled && item.CommandId == replay.CommandId &&
                    item.Text == replay.Text
                    ? item
                    : null;
            }
            if (Resolve(item.Items, replay) is { } nested) return nested;
        }
        return null;
    }
}

internal static class NodeActionReplayPolicy
{
    /// <summary>
    /// One retry.  A revision race is a single missed window between the user's
    /// gesture and the roughly one-per-second reconcile; a second failure means
    /// something else is moving the control, and the native state stays the truth.
    /// </summary>
    public const int MaxStaleRetries = 1;

    /// <summary>
    /// Whether a property action refused as <c>stale</c> may be re-sent.
    ///
    /// The rule is not which control it belongs to but what the action means. Each of
    /// these carries the absolute state the user asked for -- expanded, checked,
    /// selected -- so re-sending it against a newer revision converges on that state
    /// no matter what the reconcile in between observed. A toggle or a one-shot
    /// command has no such property: replaying <c>invoke</c> would press a button
    /// twice, and replaying <c>text</c> would overwrite whatever normalization the
    /// application applied to the field, which is exactly the ownership the projection
    /// hands back to it.
    /// </summary>
    public static bool IsReplayableAfterStale(string property) =>
        property is "itemExpanded" or "checked" or "selectedIndex" or "selectedIndices"
            or "checkedIndices" or "focusedIndex";
}
