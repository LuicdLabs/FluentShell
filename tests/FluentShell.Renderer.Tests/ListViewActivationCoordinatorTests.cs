using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class ListViewActivationCoordinatorTests
{
    private sealed record Emission(string Action, object? Value, string EventId);

    // Exercise the production coordinator against the real view model's pending
    // dictionary and full-snapshot ordering. The queue models a WinUI dispatcher
    // turn: it cannot observe a half-applied snapshot or the gap before a replay.
    private sealed class Harness : IDisposable
    {
        internal ControlNode Snapshot = ListViewModeProtocolTests.IconList() with
        {
            SelectedIndices = [], FocusedIndex = -1,
        };
        internal readonly ControlNodeViewModel Node;
        internal readonly ListViewActivationCoordinator Activation;
        internal readonly List<Emission> Sent = [];
        internal bool Applying;
        internal bool BlockEmission;
        private readonly Queue<Action> _queue = [];
        private int _nextEvent;

        internal Harness()
        {
            Node = ControlNodeViewModel.FromSnapshot(Snapshot);
            Activation = new(Node, Send, _ => true, () => Applying, callback =>
            {
                _queue.Enqueue(callback);
                return true;
            });
        }

        internal void Send(string action, object? value)
        {
            if (BlockEmission) return;
            var eventId = (++_nextEvent).ToString();
            Node.RegisterPending(TranslatedWindow.PropertyForNodeAction(action), eventId);
            Sent.Add(new(action, value, eventId));
        }

        internal Emission Last(string action) => Sent.Last(value => value.Action == action);

        internal void Patch(ControlNode snapshot, string? eventId)
        {
            Snapshot = snapshot;
            Node.ApplySnapshot(snapshot, preserveTransient: true, eventId: eventId);
        }

        internal void Canonical(Action update)
        {
            Applying = true;
            try { update(); }
            finally { Applying = false; }
            Drain();
        }

        internal void Reject(Emission action, bool replay = false) => Canonical(() =>
            Node.RejectPending(TranslatedWindow.PropertyForNodeAction(action.Action), action.EventId, replay));

        internal void Drain()
        {
            var remaining = 30;
            while (_queue.TryDequeue(out var callback))
            {
                Assert.True(--remaining > 0, "Activation kept enqueueing without pending work.");
                callback();
            }
        }

        internal void AssertNoActivation() => Assert.DoesNotContain(Sent, value => value.Action == "activateItem");
        public void Dispose() => Activation.Dispose();
    }

    [Theory]
    [InlineData("setSelection")]
    [InlineData("setFocusedIndex")]
    public void DefinitivePrerequisiteRejectionCannotBeRevivedByLaterCanonicalSelection(string action)
    {
        using var host = new Harness();
        host.Activation.Request(1);
        host.Reject(host.Last(action));
        Assert.False(host.Activation.HasPending);
        host.Canonical(() => host.Patch(host.Snapshot with { SelectedIndices = [1], FocusedIndex = 1 }, null));
        host.AssertNoActivation();
    }

    [Theory]
    [InlineData("setSelection")]
    [InlineData("setFocusedIndex")]
    public void AcceptedUnchangedPrerequisiteConsumesGestureEvenWithoutPropertyChange(string action)
    {
        using var host = new Harness();
        host.Activation.Request(1);
        var request = host.Last(action);
        host.Canonical(() => host.Patch(host.Snapshot, request.EventId));
        Assert.False(host.Activation.HasPending);
        host.Canonical(() => host.Patch(host.Snapshot with { SelectedIndices = [1], FocusedIndex = 1 }, null));
        host.AssertNoActivation();
    }

    [Fact]
    public void StaleReplayGapWaitsUntilItsNewEventPublishesCanonicalFocus()
    {
        using var host = new Harness();
        host.Activation.Request(1);
        var selection = host.Last("setSelection");
        var focus = host.Last("setFocusedIndex");
        host.Reject(focus, replay: true);
        Assert.True(host.Node.HasPending("focusedIndex"));
        Assert.True(host.Activation.HasPending);
        host.Canonical(() => host.Patch(host.Snapshot with { SelectedIndices = [1] }, selection.EventId));
        host.Canonical(() =>
        {
            host.Patch(host.Snapshot, focus.EventId);
            Assert.False(host.Node.HasPending("focusedIndex"));
            // TranslatedWindow emits the queued replay synchronously after the
            // resync patch, before the dispatcher can flush the gesture.
            host.Send("setFocusedIndex", 1);
        });
        host.AssertNoActivation();
        Assert.True(host.Activation.HasPending);
        host.Canonical(() => host.Patch(host.Snapshot with { FocusedIndex = 1 }, host.Last("setFocusedIndex").EventId));
        var activation = Assert.Single(host.Sent, value => value.Action == "activateItem");
        Assert.Equal(1, activation.Value);
        Assert.False(host.Activation.HasPending);
        host.Drain();
        Assert.Single(host.Sent, value => value.Action == "activateItem");
    }

    [Fact]
    public void ExistingPointerPreludeRequestsCanCompleteTheFollowingDoubleClick()
    {
        using var host = new Harness();
        host.Send("setSelection", new[] { 1 });
        host.Send("setFocusedIndex", 1);
        host.Activation.Request(1);
        Assert.Equal(2, host.Sent.Count);
        host.Canonical(() => host.Patch(host.Snapshot with { SelectedIndices = [1] }, host.Last("setSelection").EventId));
        host.Canonical(() => host.Patch(host.Snapshot with { FocusedIndex = 1 }, host.Last("setFocusedIndex").EventId));
        Assert.Single(host.Sent, value => value.Action == "activateItem");
    }

    [Fact]
    public void JoinedOlderRequestForAnotherRowDoesNotLeaveAnImmortalGesture()
    {
        using var host = new Harness();
        host.Send("setSelection", new[] { 0 });
        host.Activation.Request(1);
        host.Canonical(() => host.Patch(host.Snapshot with { SelectedIndices = [0] }, host.Last("setSelection").EventId));
        Assert.False(host.Activation.HasPending);
        host.Canonical(() => host.Patch(host.Snapshot with { SelectedIndices = [1], FocusedIndex = 1 }, null));
        host.AssertNoActivation();
    }

    [Fact]
    public void SubsequentSingleClickCancelsIntentBeforeLatePrerequisiteAcknowledgements()
    {
        using var host = new Harness();
        host.Activation.Request(1);
        var selection = host.Last("setSelection");
        var focus = host.Last("setFocusedIndex");
        host.Activation.Cancel(); // The real PointerPressed/selection/focus handlers use this operation.
        host.Canonical(() => host.Patch(host.Snapshot with { SelectedIndices = [1] }, selection.EventId));
        host.Canonical(() => host.Patch(host.Snapshot with { FocusedIndex = 1 }, focus.EventId));
        host.AssertNoActivation();
        host.Activation.Request(1);
        Assert.Single(host.Sent, value => value.Action == "activateItem");
    }

    [Fact]
    public void LateOldRejectionsDoNotCancelNewGestureBoundToNewPendingEvents()
    {
        using var host = new Harness();
        host.Activation.Request(1);
        var oldSelection = host.Last("setSelection");
        var oldFocus = host.Last("setFocusedIndex");
        host.Activation.Cancel();
        host.Send("setSelection", new[] { 1 });
        host.Send("setFocusedIndex", 1);
        host.Activation.Request(1);
        host.Reject(oldSelection);
        host.Reject(oldFocus);
        Assert.True(host.Activation.HasPending);
        host.Canonical(() => host.Patch(host.Snapshot with { SelectedIndices = [1] }, host.Last("setSelection").EventId));
        host.Canonical(() => host.Patch(host.Snapshot with { FocusedIndex = 1 }, host.Last("setFocusedIndex").EventId));
        Assert.Single(host.Sent, value => value.Action == "activateItem");
    }

    [Fact]
    public void FullSnapshotRebindingAndDisposalCancelQueuedFlushes()
    {
        using var host = new Harness();
        host.Activation.Request(1);
        host.Canonical(() => host.Patch(host.Snapshot with
        {
            ItemNativeIds = ["0", "8"], SelectedIndices = [1], FocusedIndex = 1,
        }, host.Last("setFocusedIndex").EventId));
        Assert.False(host.Activation.HasPending);
        host.AssertNoActivation();

        host.Activation.Request(1);
        host.Activation.Dispose();
        host.Canonical(() => host.Patch(host.Snapshot, host.Last("setSelection").EventId));
        host.AssertNoActivation();
    }

    [Fact]
    public void GatedPrerequisiteEmissionDoesNotLeaveAWaitingGesture()
    {
        using var host = new Harness { BlockEmission = true };
        host.Activation.Request(1);
        Assert.False(host.Activation.HasPending);
        host.BlockEmission = false;
        host.Canonical(() => host.Patch(host.Snapshot with { SelectedIndices = [1], FocusedIndex = 1 }, null));
        host.AssertNoActivation();
    }
}
