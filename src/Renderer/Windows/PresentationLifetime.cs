using System.ComponentModel;

namespace FluentShell.Renderer.Windows;

// A rebuilt tree may still have UIA peers or queued WinUI events holding its
// controls. Retire its actions before detaching its model subscriptions; the
// controls' own events can then finish without driving the replacement tree.
internal sealed class PresentationLifetime : IDisposable
{
    private readonly List<(INotifyPropertyChanged Source, PropertyChangedEventHandler Handler)> _subscriptions = [];
    private readonly List<Action> _cleanup = [];

    internal bool IsActive { get; private set; } = true;

    internal void Subscribe(INotifyPropertyChanged source, PropertyChangedEventHandler handler)
    {
        if (!IsActive) return;
        PropertyChangedEventHandler guarded = (sender, args) =>
        {
            if (IsActive) handler(sender, args);
        };
        source.PropertyChanged += guarded;
        _subscriptions.Add((source, guarded));
    }

    internal void Invoke(Action action)
    {
        if (IsActive) action();
    }

    internal void OnDispose(Action cleanup)
    {
        if (IsActive) _cleanup.Add(cleanup);
        else cleanup();
    }

    public void Dispose()
    {
        if (!IsActive) return;
        IsActive = false;
        foreach (var (source, handler) in _subscriptions)
            source.PropertyChanged -= handler;
        _subscriptions.Clear();
        var cleanup = _cleanup.ToArray();
        _cleanup.Clear();
        foreach (var release in cleanup) release();
    }
}
