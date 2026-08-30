using System.Text.Json;
using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

/// <summary>
/// The admission rules are table-driven, so these cases pin the tables: every
/// control kind the Bridge can emit has to be admissible, every action has to
/// agree with its value shape, and nothing outside the tables may pass.
/// </summary>
public class ProtocolValidatorTests
{
    // Mirrors ControlKind in src/Bridge/Translation/WindowSnapshot.h for the kinds
    // the bounded adapters can currently produce.
    public static TheoryData<string> ProjectedKinds() =>
    [
        "static", "separator", "button", "checkBox", "threeState", "radioButton",
        "edit", "password", "comboBox", "listBox", "groupBox", "progressBar",
        "sysLink", "listView", "statusBar",
    ];

    [Theory]
    [MemberData(nameof(ProjectedKinds))]
    public void EveryProjectedKindIsAdmissible(string kind)
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = NodeOfKind(kind);
        ProtocolValidator.ValidateSnapshot(snapshot);
    }

    [Fact]
    public void UnregisteredKindIsRejected()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = NodeOfKind("treeView");
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Theory]
    [InlineData("setText", "\"text\"", true)]
    [InlineData("setText", "3", false)]
    [InlineData("setCheck", "1", true)]
    [InlineData("setCheck", "\"1\"", false)]
    [InlineData("select", "0", true)]
    [InlineData("setSelection", "[0,2]", true)]
    [InlineData("setSelection", "[2,0]", false)]
    [InlineData("setSelection", "[0,0]", false)]
    [InlineData("invoke", "null", true)]
    [InlineData("invoke", "1", false)]
    public void NodeActionValueShapeIsEnforced(string action, string value, bool valid)
    {
        Validate(NodeAction(action, value), valid);
    }

    [Theory]
    [InlineData("menuCommand", "40001", true)]
    [InlineData("menuCommand", "0", false)]
    [InlineData("menuCommand", "70000", false)]
    [InlineData("move", """{"x":1,"y":2,"width":3,"height":4}""", true)]
    [InlineData("move", """{"x":1,"y":2,"width":-3,"height":4}""", false)]
    [InlineData("resize", """{"x":1,"y":2,"width":3}""", false)]
    [InlineData("activate", "null", true)]
    [InlineData("close", "null", true)]
    [InlineData("minimize", "null", true)]
    [InlineData("teleport", "null", false)]
    public void WindowActionValueShapeIsEnforced(string action, string value, bool valid)
    {
        Validate(WindowAction(action, value), valid);
    }

    [Fact]
    public void NodeAddressingMustMatchActionSemantics()
    {
        // A control action without a node, and a window action with one.
        Validate(WindowAction("setCheck", "1"), false);
        Validate(NodeAction("activate", "null"), false);
    }

    private static void Validate(ActionInvokeMessage action, bool valid)
    {
        if (valid) ProtocolValidator.ValidateSemanticCaps(action);
        else Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSemanticCaps(action));
    }

    private static ActionInvokeMessage NodeAction(string action, string value) =>
        BaseAction(action, value) with { NodeId = "10" };

    private static ActionInvokeMessage WindowAction(string action, string value) =>
        BaseAction(action, value);

    private static ActionInvokeMessage BaseAction(string action, string value) => new()
    {
        SessionNonce = TestData.Nonce,
        SurfaceId = Guid.Parse("11111111-2222-3333-4444-555555555555"),
        EventId = "5",
        ExpectedRevision = "7",
        Action = action,
        Value = JsonDocument.Parse(value).RootElement.Clone(),
    };

    // A node that satisfies every rule its kind adds, so a failure can only come
    // from the kind itself being unknown to the table.
    private static ControlNode NodeOfKind(string kind)
    {
        var node = new ControlNode
        {
            NodeId = "10",
            Generation = "1",
            NativeHwnd = "0x5678",
            Kind = kind,
            ControlId = 100,
            ZIndex = 0,
            TabIndex = -1,
            Rect = new PixelRect { X = 20, Y = 30, Width = 200, Height = 30 },
            Visible = true,
            Enabled = true,
            TabStop = false,
            DialogCode = 0,
            Text = "Label",
            Items = [],
        };
        return kind switch
        {
            "progressBar" => node with { Minimum = 0, Maximum = 100, Position = 40 },
            "comboBox" => node with { Items = ["one", "two"], SelectedIndex = 1 },
            "sysLink" => node with { Text = "Open the report now", Items = ["the report"] },
            "listView" => node with
            {
                Columns = ["Drive", "Status"],
                ColumnWidths = [180, 260],
                Rows = [["C:", "OK"]],
                SelectedIndices = [0],
                FocusedIndex = 0,
                MultiSelect = false,
            },
            "statusBar" => node with { Items = ["Ln 1", "100%"], ColumnWidths = [200, 80] },
            _ => node,
        };
    }
}
