using FluentShell.Renderer.ViewModels;

namespace FluentShell.Renderer.Windows;

// Coupled canonical fields arrive in one snapshot or patch. Rebuild a presenter
// once, synchronously after that transaction, so readiness never observes an
// intermediate item/row/icon tuple or waits for a dispatcher callback.
internal sealed class CanonicalPresentationUpdate : IDisposable
{
    private readonly ControlNodeViewModel _node;
    private readonly PresentationLifetime _lifetime;
    private readonly Action _apply;
    private bool _requested;
    private bool _disposed;

    internal CanonicalPresentationUpdate(ControlNodeViewModel node,
        PresentationLifetime lifetime, Action apply)
    {
        _node = node;
        _lifetime = lifetime;
        _apply = apply;
        node.CanonicalUpdateCompleted += Flush;
        lifetime.OnDispose(Dispose);
    }

    internal void Request()
    {
        if (_disposed || !_lifetime.IsActive) return;
        _requested = true;
        if (!_node.IsApplyingCanonicalUpdate) Flush();
    }

    private void Flush()
    {
        if (!_requested || _disposed || !_lifetime.IsActive) return;
        _requested = false;
        _apply();
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _requested = false;
        _node.CanonicalUpdateCompleted -= Flush;
    }
}
