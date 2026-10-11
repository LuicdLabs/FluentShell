using System.Text.Json;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

// Minor 34: a double-click on an MMC result item (a virtual list row, a snap-in tree's leaf)
// opens it the way the control answers a double-click. The Bridge offers itemDefaultActivation;
// the projection asks for openItem, which selects the item and sends its control the Enter key.
public sealed class DefaultActivationTests
{
    private static ControlNode List(bool? defaultActivation) =>
        TestData.Snapshot().Nodes[0] with
        {
            Kind = "listView", ListViewMode = "report",
            Columns = ["Name"], ColumnWidths = [100], ColumnOrder = [0],
            Items = ["Service"], Rows = [["Service"]],
            ColumnHeadersVisible = true, CheckBoxes = false, CheckedIndices = [],
            ItemActivationSupported = false, ItemNativeIds = [], ItemDefaultActivation = defaultActivation,
            SelectedIndices = [0], FocusedIndex = 0, MultiSelect = false,
            ImageList = [], ItemImages = [-1], EditableLabels = false, EditingIndex = -1,
        };

    private static ControlNode Tree(bool? defaultActivation) =>
        TestData.Snapshot().Nodes[0] with
        {
            Kind = "treeView", Text = string.Empty, AutomationName = string.Empty,
            Items = ["Computer", "Device"], ItemDepths = [0, 1],
            ItemExpanded = [true, false], ItemHasChildren = [true, false],
            SelectedIndex = 1, ImageList = [], ItemImages = [-1, -1], ItemSelectedImages = [-1, -1],
            StateImageList = [], ItemStateImages = [-1, -1],
            EditableLabels = false, EditingIndex = -1, ItemDefaultActivation = defaultActivation,
        };

    private static void Validate(ControlNode node) =>
        ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] });

    [Fact]
    public void ListAndTreeNodesMayOfferDefaultActivation()
    {
        Validate(List(true));
        Validate(List(false));
        Validate(List(null));
        Validate(Tree(true));
        Validate(Tree(null));
    }

    [Fact]
    public void OfferRoundTripsAcrossTheWireAndReachesTheViewModel()
    {
        foreach (var node in new[] { List(true), Tree(true) })
        {
            var bytes = ProtocolSerializer.Serialize(new WindowOpenMessage
            {
                SessionNonce = TestData.Nonce, Window = TestData.Snapshot() with { Nodes = [node] },
            });
            var received = Assert.Single(Assert.IsType<WindowOpenMessage>(
                ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, bytes)).Window.Nodes);
            Assert.True(received.ItemDefaultActivation);
            Assert.True(ControlNodeViewModel.FromSnapshot(received).ItemDefaultActivation);
        }
        Assert.False(ControlNodeViewModel.FromSnapshot(List(null)).ItemDefaultActivation);
    }

    [Fact]
    public void NoOtherKindMayCarryIt()
    {
        var button = TestData.Snapshot().Nodes[0] with { Kind = "button", ItemDefaultActivation = true };
        Assert.Throws<ProtocolException>(() => Validate(button));
    }

    [Fact]
    public void SharedFixtureCarriesTheItemIndexAndItsLabelWitness()
    {
        var action = Assert.IsType<ActionInvokeMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.ActionInvoke, Fixture("action.invoke.open-item.json")));
        Assert.Equal("openItem", action.Action);
        Assert.Equal("3", action.NodeId);
        Assert.Equal(2, action.Value.GetProperty("index").GetInt32());
        Assert.Equal("Leaf", action.Value.GetProperty("text").GetString());
        Assert.Equal("openItem", TranslatedWindow.PropertyForNodeAction("openItem"));
    }

    // A windowed virtual list names its rows absolutely, past the per-array item cap.
    [Theory]
    [InlineData(0)]
    [InlineData(4095)]
    [InlineData(4096)]
    [InlineData(16_777_215)]
    public void AnAbsoluteRowIsAdmittedAndAnEmptyListLabelIsAWitness(int index)
    {
        Validate(JsonSerializer.SerializeToElement(new ItemTextActionValue { Index = index, Text = "Row" }));
        Validate(JsonSerializer.SerializeToElement(new ItemTextActionValue { Index = index, Text = "" }));
    }

    [Theory]
    [InlineData("2")]
    [InlineData("{\"index\":2}")]
    [InlineData("{\"text\":\"Leaf\"}")]
    [InlineData("{\"index\":-1,\"text\":\"Leaf\"}")]
    [InlineData("{\"index\":16777216,\"text\":\"Leaf\"}")]
    [InlineData("{\"index\":2,\"text\":\"Leaf\",\"extra\":true}")]
    [InlineData("{\"index\":2,\"text\":7}")]
    public void MalformedRequestsAreRejected(string value)
    {
        Assert.Throws<ProtocolException>(() => Validate(JsonDocument.Parse(value).RootElement));
    }

    [Fact]
    public void RequestMustAddressANode()
    {
        Assert.Throws<ProtocolException>(() => Validate(
            JsonSerializer.SerializeToElement(new ItemTextActionValue { Index = 0, Text = "Row" }), nodeId: null));
    }

    [Fact]
    public void SecondPressOnTheSameItemWithinTheDoubleClickTimeCompletesTheGesture()
    {
        var presses = new ItemPressTracker();
        Assert.False(presses.Note("8", 1, 1_000, 500));
        Assert.True(presses.Note("8", 1, 1_400, 500));
        // The gesture is consumed: a third press starts a new one.
        Assert.False(presses.Note("8", 1, 1_450, 500));
    }

    [Fact]
    public void PressesOnDifferentItemsNodesOrTooFarApartAreNotADoubleClick()
    {
        var presses = new ItemPressTracker();
        Assert.False(presses.Note("8", 0, 1_000, 500));
        Assert.False(presses.Note("8", 1, 1_100, 500));
        Assert.False(presses.Note("9", 1, 1_200, 500));
        Assert.False(presses.Note("9", 1, 1_800, 500));
        Assert.False(presses.Note("9", -1, 1_900, 500));
        Assert.False(presses.Note("9", -1, 2_000, 500));
    }

    private static void Validate(JsonElement value, string? nodeId = "3")
    {
        var action = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = TestData.Snapshot().SurfaceId,
            EventId = "1", ExpectedRevision = "8", Action = "openItem", NodeId = nodeId, Value = value,
        };
        ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(action));
    }

    private static byte[] Fixture(string name) =>
        File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "ProtocolFixtures", name));
}
