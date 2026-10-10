namespace FluentShell.Renderer.Windows;

/// <summary>
/// Recognizes a double-click from raw presses. The first press of a double-click can rebuild
/// the presenter (and even replace the node's view model) under the pointer, which cancels
/// a double-tap the presenter had recognized; the window outlives both, so it counts presses.
/// </summary>
internal sealed class ItemPressTracker
{
    private string? _nodeId;
    private int _index = -1;
    private long _tick;

    /// <summary>
    /// Notes a press of item `index` of node `nodeId` (-1: no item). True when it completes a
    /// double-click begun by the previous press: the same item, within the double-click time.
    /// The gesture is consumed, so a third press begins a new one.
    /// </summary>
    public bool Note(string nodeId, int index, long now, long doubleClickMilliseconds)
    {
        var completes = index >= 0 && index == _index && _nodeId == nodeId && now - _tick <= doubleClickMilliseconds;
        if (completes)
        {
            _nodeId = null;
            _index = -1;
            _tick = 0;
        }
        else
        {
            _nodeId = nodeId;
            _index = index;
            _tick = now;
        }
        return completes;
    }
}
