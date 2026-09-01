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
        "static", "staticIcon", "separator", "button", "checkBox", "threeState", "radioButton",
        "edit", "password", "comboBox", "listBox", "groupBox", "progressBar",
        "sysLink", "listView", "tabControl", "dialogContainer", "statusBar", "toolbar",
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

    [Fact]
    public void AdapterPagesAreClosedToExtraSemantics()
    {
        // MdSched: three virtual presentation slots plus three handoff buttons.
        var snapshot = TestData.Snapshot() with
        {
            SurfaceKind = "window",
            AdapterId = "microsoft.mdsched.directui",
            PageId = "initial",
            Nodes = [],
        };
        string[] keys = ["MainIcon", "MainInstruction", "ContentText", "CommandLink.0", "CommandLink.1", "Cancel"];
        string[] kinds = ["staticIcon", "static", "static", "button", "button", "button"];
        string[] variants = ["mainIcon", "instruction", "content", "commandLink", "commandLink", "standard"];
        for (var index = 0; index < keys.Length; ++index)
        {
            var interactive = index >= 3;
            snapshot.Nodes.Add(new ControlNode
            {
                NodeId = (100 + index).ToString(), Generation = "1",
                NativeHwnd = interactive ? $"0x{200 + index:X}" : null,
                Kind = kinds[index], ZIndex = index, TabIndex = interactive ? index - 3 : -1,
                Rect = new PixelRect { X = 10, Y = 10 + index * 40, Width = index == 0 ? 32 : 200, Height = index == 0 ? 32 : 32 },
                Visible = true, Enabled = true, TabStop = interactive, Text = $"localized {index}",
                AutomationName = $"localized {index}", Items = [],
                ImageWidth = index == 0 ? 32 : null, ImageHeight = index == 0 ? 32 : null,
                ImageFormat = index == 0 ? "bgra8-premultiplied" : null,
                ImageData = index == 0 ? Convert.ToBase64String(new byte[32 * 32 * 4]) : null,
                AdapterId = snapshot.AdapterId, PageId = snapshot.PageId, SemanticKey = keys[index],
                SourceKind = interactive ? "nativeBacking" : "uiaVirtual",
                PresentationVariant = variants[index], SupportedActions = interactive ? ["invoke"] : [],
                HelpText = string.Empty, AccessKey = string.Empty,
            });
        }
        ProtocolValidator.ValidateSnapshot(snapshot);

        snapshot.Nodes[3] = snapshot.Nodes[3] with { SupportedActions = ["invoke", "close"] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[3] = snapshot.Nodes[3] with { SupportedActions = ["invoke"], SemanticKey = "Unexpected" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[3] = snapshot.Nodes[3] with { SemanticKey = "CommandLink.0", NativeHwnd = null };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));

        // RecoveryDrive: disabled virtual back button, three texts, a native
        // checkbox with setCheck, and two handoff buttons.
        var recovery = TestData.Snapshot() with
        {
            SurfaceKind = "window",
            AdapterId = "microsoft.recoverydrive.directui",
            PageId = "first",
            Nodes = [],
        };
        string[] rKeys = ["backbutton", "wizardtitle", "headertitle", "pageText", "backupSystemFiles", "nextbutton", "cancelbutton"];
        string[] rKinds = ["button", "static", "static", "static", "checkBox", "button", "button"];
        string[] rVariants = ["standard", "wizardTitle", "header", "content", "standard", "standard", "standard"];
        string[] rSources = ["uiaVirtual", "uiaVirtual", "uiaVirtual", "nativeBacking", "nativeBacking", "nativeBacking", "nativeBacking"];
        string[][] rActions = [[], [], [], [], ["setCheck"], ["invoke"], ["invoke"]];
        for (var index = 0; index < rKeys.Length; ++index)
        {
            var backing = rSources[index] == "nativeBacking";
            recovery.Nodes.Add(new ControlNode
            {
                NodeId = (200 + index).ToString(), Generation = "1",
                NativeHwnd = backing ? $"0x{300 + index:X}" : null,
                Kind = rKinds[index], ZIndex = index, TabIndex = index >= 4 ? index - 4 : -1,
                Rect = new PixelRect { X = 10, Y = 10 + index * 40, Width = 200, Height = 32 },
                Visible = true, Enabled = index != 0, TabStop = index >= 4, Text = $"localized {index}",
                AutomationName = $"localized {index}", Items = [],
                AdapterId = recovery.AdapterId, PageId = recovery.PageId, SemanticKey = rKeys[index],
                SourceKind = rSources[index],
                PresentationVariant = rVariants[index],
                SupportedActions = [.. rActions[index]],
                HelpText = string.Empty, AccessKey = string.Empty,
            });
        }
        ProtocolValidator.ValidateSnapshot(recovery);

        // The disabled back button must stay inert: enabling it violates the
        // exact page contract the Bridge profile admitted.
        recovery.Nodes[0] = recovery.Nodes[0] with { Enabled = true };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(recovery));
        recovery.Nodes[0] = recovery.Nodes[0] with { Enabled = false, SupportedActions = ["invoke"] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(recovery));
        recovery.Nodes[0] = recovery.Nodes[0] with { SupportedActions = [] };
        // An unknown adapter id is never admissible.
        var unknown = recovery with { AdapterId = "microsoft.unknown.directui" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(unknown));
    }

    [Fact]
    public void GenericDirectUiPageIsDynamicButCapabilityClosed()
    {
        var adapter = ProtocolConstants.GenericDirectUiAdapterId;
        var page = ProtocolConstants.GenericDirectUiPageId;
        var text = NodeOfKind("static") with
        {
            NodeId = "100", NativeHwnd = null, ZIndex = 0, TabIndex = -1,
            AutomationName = "Localized instruction", AdapterId = adapter, PageId = page,
            SemanticKey = "semantic.0.text", SourceKind = "uiaVirtual",
            PresentationVariant = "instruction", SupportedActions = [],
            HelpText = string.Empty, AccessKey = string.Empty,
        };
        var button = NodeOfKind("button") with
        {
            NodeId = "101", NativeHwnd = "0xABC", ZIndex = 1, TabIndex = 0,
            TabStop = true, AutomationName = "Localized action", AdapterId = adapter,
            PageId = page, SemanticKey = "semantic.1.button", SourceKind = "nativeBacking",
            PresentationVariant = "standard", SupportedActions = ["invoke"],
            HelpText = string.Empty, AccessKey = string.Empty,
        };
        var checkBox = NodeOfKind("checkBox") with
        {
            NodeId = "102", NativeHwnd = null, ZIndex = 2, TabIndex = 1,
            TabStop = true, Checked = 0, AutomationName = "Localized option",
            AdapterId = adapter, PageId = page, SemanticKey = "semantic.2.checkbox",
            SourceKind = "uiaVirtual", PresentationVariant = "standard",
            SupportedActions = ["setCheck"], HelpText = string.Empty, AccessKey = string.Empty,
        };
        var snapshot = TestData.Snapshot() with
        {
            SurfaceKind = "window", CanCancel = false, AdapterId = adapter, PageId = page,
            Nodes = [text, button, checkBox],
        };
        ProtocolValidator.ValidateSnapshot(snapshot);

        snapshot.Nodes[2] = checkBox with { SemanticKey = button.SemanticKey };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[2] = checkBox with { SemanticKey = "localized key" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[2] = checkBox with { SupportedActions = ["invoke"] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[2] = checkBox with { SourceKind = "nativeBacking", NativeHwnd = null };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[2] = checkBox;
        snapshot.Nodes[1] = button with { Kind = "radioButton" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[1] = button;
        var wrongPage = snapshot with { PageId = "future-semantic-page" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(wrongPage));
    }

    [Fact]
    public void ParentGraphRequiresPrecedingDialogContainers()
    {
        var snapshot = TestData.Snapshot();
        var child = snapshot.Nodes[0] with { NodeId = "11", ParentNodeId = "10", ZIndex = 1 };
        snapshot.Nodes.Clear();
        snapshot.Nodes.Add(NodeOfKind("dialogContainer") with { NodeId = "10", ZIndex = 0 });
        snapshot.Nodes.Add(child);
        ProtocolValidator.ValidateSnapshot(snapshot);

        snapshot.Nodes[0] = NodeOfKind("groupBox") with { NodeId = "10", ZIndex = 0 };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));

        snapshot.Nodes.Clear();
        snapshot.Nodes.Add(child);
        snapshot.Nodes.Add(NodeOfKind("dialogContainer") with { NodeId = "10", ZIndex = 0 });
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Fact]
    public void DialogContainerCannotBecomeFocusable()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = NodeOfKind("dialogContainer") with { TabStop = true, TabIndex = 0 };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Fact]
    public void DialogContainerMayCarryAnAccessibilityName()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = NodeOfKind("dialogContainer") with
        {
            Text = "Disk Cleanup page",
            AutomationName = "Disk Cleanup page",
        };
        ProtocolValidator.ValidateSnapshot(snapshot);
    }

    [Fact]
    public void StaticIconPayloadMustBeCanonicalBoundedPremultipliedBgra()
    {
        var snapshot = TestData.Snapshot();
        var valid = NodeOfKind("staticIcon");
        snapshot.Nodes[0] = valid;
        ProtocolValidator.ValidateSnapshot(snapshot);

        snapshot.Nodes[0] = valid with { ImageData = "AAAgQA= =" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { ImageData = Convert.ToBase64String([0, 0, 0]) };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { ImageData = Convert.ToBase64String([0, 0, 65, 64]) };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { ImageWidth = ProtocolConstants.MaxImageDimension + 1 };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Fact]
    public void ImageFieldsAreRejectedOnOtherKindsByRequiredFieldGate()
    {
        var snapshot = TestData.Snapshot();
        var json = JsonSerializer.SerializeToElement(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = snapshot with
            {
                Nodes = [snapshot.Nodes[0] with { ImageWidth = 1 }],
            },
        });

        Assert.Throws<ProtocolException>(() =>
            ProtocolValidator.ValidateRequiredFields(FrameMessageType.WindowOpen, json));
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
    [InlineData("setItemCheck", "{\"index\":1,\"checked\":true}", true)]
    [InlineData("setItemCheck", "{\"index\":-1,\"checked\":true}", false)]
    [InlineData("setItemCheck", "{\"index\":4096,\"checked\":true}", false)]
    [InlineData("setItemCheck", "{\"index\":1,\"checked\":1}", false)]
    [InlineData("setItemCheck", "{\"index\":1,\"checked\":true,\"extra\":0}", false)]
    [InlineData("toolbarCommand", "101", true)]
    [InlineData("toolbarCommand", "0", false)]
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
            "progressBar" => node with
                { Minimum = 0, Maximum = 100, Position = 40, Indeterminate = false },
            "staticIcon" => node with
            {
                Text = string.Empty,
                AutomationName = "Application icon",
                ImageWidth = 1,
                ImageHeight = 1,
                ImageFormat = "bgra8-premultiplied",
                ImageData = Convert.ToBase64String([0x10, 0x20, 0x30, 0x40]),
            },
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
                ColumnHeadersVisible = true,
                CheckBoxes = true,
                CheckedIndices = [0],
            },
            "tabControl" => node with
            {
                Style = "0x54030240",
                ExStyle = "0x00000004",
                Items = ["General", "Advanced"],
                ItemRects =
                [
                    new PixelRect { X = 0, Y = 0, Width = 80, Height = 24 },
                    new PixelRect { X = 80, Y = 0, Width = 100, Height = 24 },
                ],
                SelectedIndex = 1,
            },
            "dialogContainer" => node with
            {
                Style = "0x40000400",
                ExStyle = "0x00010000",
                Text = string.Empty,
                AutomationName = string.Empty,
            },
            "statusBar" => node with { Items = ["Ln 1", "100%"], ColumnWidths = [200, 80] },
            "toolbar" => node with
            {
                Style = "0x50008B01",
                ExStyle = "0x0",
                ToolbarItems =
                [
                    new ToolbarItemSnapshot
                    {
                        Kind = "pushButton", CommandId = 101, Text = "Open", Enabled = true,
                        Rect = new PixelRect { X = 0, Y = 0, Width = 30, Height = 30 },
                        ImageWidth = 1, ImageHeight = 1, ImageFormat = "bgra8-premultiplied",
                        ImageData = Convert.ToBase64String([0x10, 0x20, 0x30, 0x40]),
                    },
                    new ToolbarItemSnapshot
                    {
                        Kind = "separator", CommandId = 0, Text = string.Empty, Enabled = true,
                        Rect = new PixelRect { X = 30, Y = 0, Width = 8, Height = 30 },
                    },
                ],
            },
            _ => node,
        };
    }

    [Fact]
    public void TabControlGeometryAndStateFailClosed()
    {
        var snapshot = TestData.Snapshot();
        var valid = NodeOfKind("tabControl");
        snapshot.Nodes[0] = valid;
        ProtocolValidator.ValidateSnapshot(snapshot);

        snapshot.Nodes[0] = valid with { SelectedIndex = 2 };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { ItemRects = [valid.ItemRects![0]] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with
        {
            ItemRects =
            [
                valid.ItemRects![0],
                new PixelRect { X = 40, Y = 0, Width = 100, Height = 24 },
            ],
        };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with
        {
            ItemRects =
            [
                valid.ItemRects![0],
                new PixelRect { X = 80, Y = 0, Width = 100, Height = 25 },
            ],
        };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with
        {
            ItemRects =
            [
                valid.ItemRects![0],
                new PixelRect { X = 100, Y = 20, Width = 100, Height = 24 },
            ],
        };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { Style = "0x54030640" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }
}
