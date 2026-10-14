using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

// Minor 41: a TVS_CHECKBOXES tree (Performance Options' DEP list) carries checkBoxes: true with its
// checkedIndices, drawn as real check boxes; setItemCheck asks the native tree to toggle one.
public sealed class TreeViewCheckBoxProtocolTests
{
    private static ControlNode Tree(bool? checkBoxes = true, List<int>? checkedIndices = null) =>
        TestData.Snapshot().Nodes[0] with
        {
            Kind = "treeView", Text = string.Empty, AutomationName = string.Empty,
            Items = ["Alpha.exe", "Beta.exe"], ItemDepths = [0, 0],
            ItemExpanded = [false, false], ItemHasChildren = [false, false],
            SelectedIndex = 0, ImageList = [], ItemImages = [-1, -1], ItemSelectedImages = [-1, -1],
            StateImageList = [], ItemStateImages = [-1, -1],
            EditableLabels = false, EditingIndex = -1,
            CheckBoxes = checkBoxes, CheckedIndices = checkBoxes is null ? null : checkedIndices ?? [1],
        };

    private static readonly ImageListEntry Icon = new()
    {
        ImageWidth = 1, ImageHeight = 1, ImageFormat = "bgra8-premultiplied",
        ImageData = Convert.ToBase64String(new byte[] { 10, 20, 30, 255 }),
    };

    private static void Validate(ControlNode node) =>
        ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] });

    private static byte[] Wire(ControlNode node) => ProtocolSerializer.Serialize(new WindowOpenMessage
    {
        SessionNonce = TestData.Nonce, Window = TestData.Snapshot() with { Nodes = [node] },
    });

    [Fact]
    public void ACheckBoxTreeIsAdmittedAndReachesTheViewModel()
    {
        Validate(Tree());
        Validate(Tree(checkedIndices: []));
        Validate(Tree(checkBoxes: null));
        var received = Assert.Single(Assert.IsType<WindowOpenMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, Wire(Tree()))).Window.Nodes);
        var model = ControlNodeViewModel.FromSnapshot(received);
        Assert.True(model.CheckBoxes);
        Assert.Equal([false, true], ControlFactory.ItemChecks(model));
        Assert.Null(ControlFactory.ItemChecks(ControlNodeViewModel.FromSnapshot(Tree(checkBoxes: null))));
    }

    [Fact]
    public void MalformedTreeCheckStateIsRejected()
    {
        foreach (var node in new[]
        {
            Tree(checkBoxes: false, checkedIndices: []),
            Tree() with { CheckedIndices = null },
            Tree(checkBoxes: null) with { CheckedIndices = [0] },
            Tree(checkedIndices: [2]),
            Tree(checkedIndices: [1, 0]),
            // The boxes are the projection's own check boxes, never also state glyphs.
            Tree() with { ItemStateImages = [0, -1], StateImageList = [Icon] },
        }) Assert.Throws<ProtocolException>(() => Validate(node));
        var button = TestData.Snapshot().Nodes[0] with { Kind = "button", CheckBoxes = true, CheckedIndices = [] };
        Assert.Throws<ProtocolException>(() => Validate(button));
    }

    [Fact]
    public void TheRawWireNeedsBothFieldsAndATrueFlag()
    {
        foreach (var edit in new Action<JsonObject>[]
        {
            node => node["checkBoxes"] = false,
            node => node.Remove("checkedIndices"),
            node => node["checkedIndices"] = 1,
        })
        {
            var json = JsonNode.Parse(Wire(Tree()))!;
            edit(json["window"]!["nodes"]![0]!.AsObject());
            Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
                System.Text.Encoding.UTF8.GetBytes(json.ToJsonString())));
        }
    }

    [Fact]
    public void TheToggleIsTheListsSetItemCheck()
    {
        var action = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = TestData.Snapshot().SurfaceId,
            EventId = "1", ExpectedRevision = "8", Action = "setItemCheck", NodeId = "2",
            Value = System.Text.Json.JsonSerializer.SerializeToElement(new ListViewCheckActionValue { Index = 1, Checked = true }),
        };
        ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(action));
        Assert.Equal("checkedIndices", TranslatedWindow.PropertyForNodeAction("setItemCheck"));
    }
}
