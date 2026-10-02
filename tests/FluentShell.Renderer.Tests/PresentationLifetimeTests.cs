using System.ComponentModel;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class PresentationLifetimeTests
{
    private sealed class Model : INotifyPropertyChanged
    {
        private PropertyChangedEventHandler? _changed;
        internal int SubscriberCount { get; private set; }
        public event PropertyChangedEventHandler? PropertyChanged
        {
            add { _changed += value; ++SubscriberCount; }
            remove { _changed -= value; --SubscriberCount; }
        }
        internal void Change() => _changed?.Invoke(this, new PropertyChangedEventArgs("Value"));
    }

    [Fact]
    public void ReplacementDetachesOldModelHandlersAndBlocksRetainedActions()
    {
        var model = new Model();
        using var previous = new PresentationLifetime();
        var oldUpdates = 0;
        var oldActions = 0;
        previous.Subscribe(model, (_, _) => ++oldUpdates);
        Action retainedPeer = () => previous.Invoke(() => ++oldActions);
        model.Change();
        retainedPeer();
        Assert.Equal(1, oldUpdates);
        Assert.Equal(1, oldActions);

        previous.Dispose();
        Assert.Equal(0, model.SubscriberCount);
        using var current = new PresentationLifetime();
        var newUpdates = 0;
        var newActions = 0;
        current.Subscribe(model, (_, _) => ++newUpdates);
        model.Change();
        retainedPeer();
        current.Invoke(() => ++newActions);
        Assert.Equal(1, oldUpdates);
        Assert.Equal(1, oldActions);
        Assert.Equal(1, newUpdates);
        Assert.Equal(1, newActions);
        Assert.Equal(1, model.SubscriberCount);
    }

    [Fact]
    public void ReentrantRetirementSkipsHandlersAlreadyCopiedByTheEventSource()
    {
        var model = new Model();
        using var lifetime = new PresentationLifetime();
        var staleUpdates = 0;
        lifetime.Subscribe(model, (_, _) => lifetime.Dispose());
        lifetime.Subscribe(model, (_, _) => ++staleUpdates);
        model.Change();
        Assert.Equal(0, staleUpdates);
        Assert.Equal(0, model.SubscriberCount);
        lifetime.Subscribe(model, (_, _) => ++staleUpdates);
        lifetime.Dispose();
        Assert.Equal(0, model.SubscriberCount);
    }

    [Fact]
    public void UnbindingRunsAfterActionAndSubscriptionRetirementEvenWhenItRaisesEvents()
    {
        var model = new Model();
        using var lifetime = new PresentationLifetime();
        var updates = 0;
        var actions = 0;
        var cleanups = 0;
        lifetime.Subscribe(model, (_, _) => ++updates);
        lifetime.OnDispose(() =>
        {
            ++cleanups;
            Assert.False(lifetime.IsActive);
            Assert.Equal(0, model.SubscriberCount);
            // Clearing a XAML binding can synchronously raise control events.
            model.Change();
            lifetime.Invoke(() => ++actions);
        });
        lifetime.Dispose();
        lifetime.Dispose();
        Assert.Equal(1, cleanups);
        Assert.Equal(0, updates);
        Assert.Equal(0, actions);
    }

    [Fact]
    public void PopupLifetimeSurvivesControlRebuildAndStopsOnlyWhenDismissed()
    {
        using var controls = new PresentationLifetime();
        using var popup = new PresentationLifetime();
        var selections = 0;
        controls.Dispose();
        popup.Invoke(() => ++selections);
        Assert.Equal(1, selections);
        popup.Dispose();
        popup.Invoke(() => ++selections);
        Assert.Equal(1, selections);
    }
}
