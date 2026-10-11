using System.Text.Json;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;

namespace FluentShell.Renderer.Tests;

// A multi-select list box (LBS_MULTIPLESEL / LBS_EXTENDEDSEL: Performance Monitor's Add Counters
// lists) is chosen as an index list through setSelection, as a ListView's rows are.
public sealed class ListBoxSelectionTests
{
    private static ControlNode MultiSelectListBox(params int[] selected) =>
        TestData.Snapshot().Nodes[0] with
        {
            Kind = "listBox", MultiSelect = true, Items = ["a", "b", "c", "d"],
            SelectedIndices = [.. selected], SelectedIndex = selected.Length == 0 ? -1 : selected[0],
        };

    private static WindowSnapshot Snapshot(ControlNode node) => TestData.Snapshot() with { Nodes = [node] };

    [Fact]
    public void ASelectionOfSeveralItemsValidatesAndSurvivesTheWire()
    {
        ProtocolValidator.ValidateSnapshot(Snapshot(MultiSelectListBox(1, 3)));
        var received = Assert.Single(Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, ProtocolSerializer.Serialize(new WindowOpenMessage
            {
                SessionNonce = TestData.Nonce, Window = Snapshot(MultiSelectListBox(1, 3)),
            }))).Window.Nodes);
        Assert.True(received.MultiSelect);
        Assert.Equal([1, 3], received.SelectedIndices);
        Assert.Equal(1, received.SelectedIndex);
    }

    [Fact]
    public void TheViewModelFollowsTheCanonicalSelection()
    {
        var model = ControlNodeViewModel.FromSnapshot(MultiSelectListBox(1));
        var changed = new List<string?>();
        model.PropertyChanged += (_, args) => changed.Add(args.PropertyName);
        model.ApplyCanonical("selectedIndices", JsonSerializer.SerializeToElement(new[] { 0, 2, 3 }), null);
        Assert.Equal([0, 2, 3], model.SelectedIndices);
        Assert.Contains("SelectedIndices", changed);
        Assert.True(model.MultiSelect);
    }

    [Fact]
    public void ASelectionRequestIsAnIndexList()
    {
        var request = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = TestData.Snapshot().SurfaceId, EventId = "1",
            ExpectedRevision = "7", NodeId = "1", Action = "setSelection",
            Value = JsonSerializer.SerializeToElement(new[] { 0, 2 }),
        };
        static void Validate(ActionInvokeMessage action) => ProtocolSerializer.Deserialize(
            FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(action));
        Validate(request);
        // The list is canonical: ascending and without repeats.
        Assert.Throws<ProtocolException>(() => Validate(request with
        {
            Value = JsonSerializer.SerializeToElement(new[] { 2, 0 }),
        }));
    }
}
