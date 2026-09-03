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

        // One page that exercises the whole capability-derived lane: virtual
        // provider text, a native command link, a native check box, a two-route
        // editable combo box, a two-route checkbox ListView, an inert read-only
        // text box that is still a traversal stop, owned BitmapDisplayClass
        // pixels, an unnamed status bar, a tool bar that routes commands without
        // being a tab stop of its own, and a BitmapSwitchClass radio that is
        // legitimately actionable while its group's tab stop sits elsewhere.
        ControlNode Semantic(string kind, int index, string key) => NodeOfKind(kind) with
        {
            NodeId = (100 + index).ToString(),
            ZIndex = index,
            TabIndex = -1,
            TabStop = false,
            AdapterId = adapter,
            PageId = page,
            SemanticKey = key,
            SourceKind = "nativeBacking",
            PresentationVariant = "standard",
            SupportedActions = [],
            HelpText = string.Empty,
            AccessKey = string.Empty,
            AutomationName = "Localized " + key,
        };
        var text = Semantic("static", 0, "pageText") with
            { NativeHwnd = null, SourceKind = "uiaVirtual" };
        var button = Semantic("button", 1, "nextbutton") with
        {
            PresentationVariant = "commandLink", TabStop = true, TabIndex = 0,
            SupportedActions = ["invoke"],
        };
        var checkBox = Semantic("checkBox", 2, "backupSystemFiles") with
            { Checked = 0, TabStop = true, TabIndex = 1, SupportedActions = ["setCheck"] };
        var combo = Semantic("comboBox", 3, "driveList") with
        {
            Editable = true, TabStop = true, TabIndex = 2,
            SupportedActions = ["select", "setText"],
        };
        var listView = Semantic("listView", 4, "volumes") with
            { TabStop = true, TabIndex = 3, SupportedActions = ["setSelection", "setItemCheck"] };
        var readOnly = Semantic("edit", 5, "summary") with
            { ReadOnly = true, TabStop = true, TabIndex = 4 };
        var icon = Semantic("staticIcon", 6, "preview") with
            { PresentationVariant = "bitmapDisplay" };
        var status = Semantic("statusBar", 7, "status") with { AutomationName = string.Empty };
        var toolbar = Semantic("toolbar", 8, "commands") with
            { SupportedActions = ["toolbarCommand"] };
        var bitmapRadio = Semantic("radioButton", 9, "orientation") with
        {
            PresentationVariant = "bitmapSwitch", Checked = 1, SupportedActions = ["setCheck"],
            ImageWidth = 1, ImageHeight = 1, ImageFormat = "bgra8-premultiplied",
            ImageData = Convert.ToBase64String([0x10, 0x20, 0x30, 0x40]),
        };
        var snapshot = TestData.Snapshot() with
        {
            SurfaceKind = "window", CanCancel = false, AdapterId = adapter, PageId = page,
            Nodes =
            [
                text, button, checkBox, combo, listView, readOnly, icon, status, toolbar,
                bitmapRadio,
            ],
        };
        ProtocolValidator.ValidateSnapshot(snapshot);

        // Semantic identity is the node's name on this page, so it must be
        // unique and canonical.
        snapshot.Nodes[2] = checkBox with { SemanticKey = button.SemanticKey };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[2] = checkBox with { SemanticKey = "localized key" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        // A route the role never offers, and a source shape that contradicts
        // itself, are both refused.
        snapshot.Nodes[2] = checkBox with { SupportedActions = ["invoke"] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[2] = checkBox with { NativeHwnd = null };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[2] = checkBox;

        // Only provider text, a provider separator, and a property-sheet button
        // may arrive without a native backing control.
        snapshot.Nodes[4] = listView with { SourceKind = "uiaVirtual", NativeHwnd = null };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        // Both second routes are conditional on that revision's own typed state.
        snapshot.Nodes[4] = listView with { CheckBoxes = false, CheckedIndices = [] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[4] = listView;
        snapshot.Nodes[3] = combo with { Editable = false };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[3] = combo;
        snapshot.Nodes[5] = readOnly with { SupportedActions = ["setText"] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[5] = readOnly;

        // A disabled slot offers nothing and an actionable one must be reachable.
        snapshot.Nodes[1] = button with { Enabled = false };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[1] = button with { TabStop = false, TabIndex = -1 };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[1] = button;

        // Focus is the provider's to declare and does not follow from having a
        // route: inside its own host DirectUI owns traversal, so a wizard's static
        // page text and an unnamed status bar can both be stops the native page
        // really has.  The renderer refuses only the contradiction it can see by
        // itself -- a traversal stop on a slot that is disabled -- and says which
        // slot it refused.
        snapshot.Nodes[0] = text with { TabStop = true, TabIndex = 5 };
        snapshot.Nodes[7] = status with { TabStop = true, TabIndex = 6 };
        ProtocolValidator.ValidateSnapshot(snapshot);
        snapshot.Nodes[7] = status with { TabStop = true, TabIndex = 6, Enabled = false };
        var refused = Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        Assert.Contains("traversal stop", refused.Message, StringComparison.Ordinal);
        Assert.Contains("kind=statusBar", refused.Message, StringComparison.Ordinal);
        Assert.Contains("key=status", refused.Message, StringComparison.Ordinal);
        snapshot.Nodes[0] = text;
        snapshot.Nodes[7] = status;

        // Pixel-only and label-borrowing roles carry the accessible name the
        // native control cannot supply, so an empty one rejects the surface.
        snapshot.Nodes[6] = icon with { AutomationName = string.Empty };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[6] = icon;
        snapshot.Nodes[3] = combo with { AutomationName = string.Empty };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[3] = combo;

        // A kind outside the lane, and a page id the engine never publishes,
        // both reject the whole surface rather than one node.
        snapshot.Nodes[8] = toolbar with { Kind = "treeView", ToolbarItems = null };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[8] = toolbar;
        ProtocolValidator.ValidateSnapshot(snapshot);
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
