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
        "sysLink", "listView", "treeView", "tabControl", "slider", "dialogContainer",
        "mdiClient", "statusBar", "toolbar", "paneContainer", "accessibleIsland",
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
    public void TranslatedDialogSurfacesAreVirtualAndWindowSurfacesAreHwndBacked()
    {
        // The Bridge answers MessageBox/TaskDialog calls itself, so a translated
        // dialog has no native dialog HWND tree and its nodes carry no HWND.  This
        // is the rule that decides whether a MessageBox projects at all: requiring
        // a per-node HWND here faulted every translated dialog the moment it opened.
        var dialog = TestData.DialogSnapshot();
        ProtocolValidator.ValidateSnapshot(dialog);

        // Still closed the other way: a dialog node that claims a native HWND is
        // mixing the two lanes, and an HWND-tree surface must not carry a virtual
        // node.
        var hwndBackedDialog = TestData.DialogSnapshot();
        hwndBackedDialog.Nodes[0] = hwndBackedDialog.Nodes[0] with { NativeHwnd = "0x5678" };
        Assert.Contains("nativeHwnd", Assert.Throws<ProtocolException>(
            () => ProtocolValidator.ValidateSnapshot(hwndBackedDialog)).Message);

        var virtualWindowNode = TestData.Snapshot();
        virtualWindowNode.Nodes[0] = virtualWindowNode.Nodes[0] with { NativeHwnd = null };
        Assert.Contains("nativeHwnd (missing)", Assert.Throws<ProtocolException>(
            () => ProtocolValidator.ValidateSnapshot(virtualWindowNode)).Message);

        // Application-adapter fields stay refused on both, and the rejection names
        // the node and the field so the log alone explains it.
        var adapterField = TestData.DialogSnapshot();
        adapterField.Nodes[1] = adapterField.Nodes[1] with { SemanticKey = "okbutton" };
        var message = Assert.Throws<ProtocolException>(
            () => ProtocolValidator.ValidateSnapshot(adapterField)).Message;
        Assert.Contains("node 2", message);
        Assert.Contains("semanticKey", message);
    }

    [Fact]
    public void AnEmptyTopLevelMenuIsAdmissibleOnlyWhileItIsDisabled()
    {
        // A menu bar an application draws with a toolbar can offer a top-level menu it
        // currently has nothing to show for -- MMC's Window slot on a console with no
        // snap-in windows.  The native bar still shows the title, so the projection shows
        // the title and leaves it unopenable, which is what a disabled menu is.
        var snapshot = TestData.Snapshot();
        var empty = new MenuItemSnapshot
        {
            ItemId = "0",
            Kind = "popup",
            Text = "Window",
            CommandId = 0,
            Enabled = false,
            Items = [],
        };
        snapshot.Menu.Clear();
        snapshot.Menu.Add(empty);
        ProtocolValidator.ValidateSnapshot(snapshot);

        // An enabled popup still has to have something in it: a menu the user can open
        // onto nothing is a projection that lost the application's content.
        snapshot.Menu[0] = empty with { Enabled = true };
        Assert.Contains("Popup menu shape", Assert.Throws<ProtocolException>(
            () => ProtocolValidator.ValidateSnapshot(snapshot)).Message);
    }

    [Fact]
    public void ContainerPanesOwnChildNodesAndOtherKindsDoNot()
    {
        // A private container pane frames other windows, so the projected graph has to
        // let a node name it as its parent -- that is how MMC's view window owns its
        // tree, list, and Actions pane.  Anything that is not a container still may
        // not own nodes, and the rejection names both sides.
        var snapshot = TestData.Snapshot();
        var pane = NodeOfKind("paneContainer") with
        {
            NodeId = "40",
            ZIndex = 40,
            Rect = new PixelRect { X = 0, Y = 0, Width = 300, Height = 200 },
        };
        var child = NodeOfKind("static") with
        {
            NodeId = "41",
            ZIndex = 41,
            ParentNodeId = "40",
            Rect = new PixelRect { X = 0, Y = 0, Width = 100, Height = 20 },
        };
        snapshot.Nodes[0] = pane;
        snapshot.Nodes.Insert(1, child);
        ProtocolValidator.ValidateSnapshot(snapshot);

        snapshot.Nodes[0] = pane with { Kind = "statusBar", Splits = null, ChromeRegions = null };
        var message = Assert.Throws<ProtocolException>(
            () => ProtocolValidator.ValidateSnapshot(snapshot)).Message;
        Assert.Contains("node 41", message);
        Assert.Contains("statusBar", message);
    }

    [Fact]
    public void ToolbarEvidenceCoversTheStylesAndFacesTheBridgeAdmits()
    {
        // MMC's own toolbars carry CCS_NORESIZE|CCS_NODIVIDER|TBSTYLE_LIST and
        // WS_EX_TOOLWINDOW|WS_EX_NOPARENTNOTIFY.  None of them changes what the
        // control paints or what a button does, so refusing them would keep the whole
        // window native for evidence the projection reproduces exactly.
        var snapshot = TestData.Snapshot();
        var toolbar = NodeOfKind("toolbar");
        snapshot.Nodes[0] = toolbar with { Style = "0x56009845", ExStyle = "0x84" };
        ProtocolValidator.ValidateSnapshot(snapshot);

        // A button whose face the control draws itself, or which has no icon at all,
        // travels without image metadata: an icon-only button names itself from the
        // accessible name the control publishes.
        snapshot.Nodes[0] = toolbar with
        {
            ToolbarItems =
            [
                toolbar.ToolbarItems![0] with
                {
                    ImageWidth = null,
                    ImageHeight = null,
                    ImageFormat = null,
                    ImageData = null,
                },
                toolbar.ToolbarItems![1],
            ],
        };
        ProtocolValidator.ValidateSnapshot(snapshot);

        // Still closed: an owner-draw or multi-row style bit, and half-declared image
        // metadata, are both refused.
        foreach (var broken in new[]
        {
            toolbar with { Style = "0x5600984D" },
            toolbar with { ExStyle = "0x100" },
            toolbar with
            {
                ToolbarItems = [toolbar.ToolbarItems![0] with { ImageData = null }, toolbar.ToolbarItems![1]],
            },
        })
        {
            snapshot.Nodes[0] = broken;
            Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        }
    }

    [Fact]
    public void UnregisteredKindIsRejected()
    {
        var snapshot = TestData.Snapshot();
        // RichEdit is a named Tranche D target with no adapter, so it stands in for
        // any kind the Bridge cannot produce yet.
        snapshot.Nodes[0] = NodeOfKind("richEdit");
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
    [InlineData("setItemText", "{\"index\":1,\"text\":\"Renamed\"}", true)]
    [InlineData("setItemText", "{\"index\":1,\"text\":\"\"}", false)]
    [InlineData("setItemText", "{\"index\":-1,\"text\":\"Renamed\"}", false)]
    [InlineData("setItemText", "\"Renamed\"", false)]
    [InlineData("setItemCheck", "{\"index\":-1,\"checked\":true}", false)]
    [InlineData("setItemCheck", "{\"index\":4096,\"checked\":true}", false)]
    [InlineData("setItemCheck", "{\"index\":1,\"checked\":1}", false)]
    [InlineData("setItemCheck", "{\"index\":1,\"checked\":true,\"extra\":0}", false)]
    [InlineData("toolbarCommand", "101", true)]
    [InlineData("toolbarCommand", "0", false)]
    [InlineData("setValue", "25", true)]
    [InlineData("setValue", "\"25\"", false)]
    [InlineData("setExpand", "{\"index\":1,\"expanded\":true}", true)]
    [InlineData("setExpand", "{\"index\":-1,\"expanded\":true}", false)]
    [InlineData("setExpand", "{\"index\":1,\"checked\":true}", false)]
    [InlineData("setExpand", "{\"index\":1,\"expanded\":1}", false)]
    [InlineData("mdiCommand", "\"maximize\"", true)]
    [InlineData("mdiCommand", "\"teleport\"", false)]
    [InlineData("mdiCommand", "3", false)]
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

    [Fact]
    public void ContainerPaneAdmitsItsSplitsAndItsOwnPaintedBands()
    {
        var pane = NodeOfKind("paneContainer") with
        {
            Rect = new PixelRect { X = 0, Y = 0, Width = 300, Height = 200 },
            Splits = [new PaneSplit
            {
                Vertical = true, Position = 148, Thickness = 3, Minimum = 24, Maximum = 273,
            }],
            ChromeRegions = [Band(300, 30)],
        };
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = pane;
        ProtocolValidator.ValidateSnapshot(snapshot);

        // A split has to name a real range inside the pane, and its position has to sit
        // inside that range: anything else would offer a drag the native panes cannot
        // follow.
        foreach (var broken in new[]
        {
            pane with { Splits = [pane.Splits![0] with { Position = 10 }] },
            pane with { Splits = [pane.Splits![0] with { Maximum = 10 }] },
            pane with { Splits = [pane.Splits![0] with { Position = 298, Maximum = 298 }] },
            pane with { Splits = [pane.Splits![0] with { Thickness = 0 }] },
        })
        {
            snapshot.Nodes[0] = broken;
            Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        }

        // A band's pixels have to describe exactly its rectangle, and the rectangle has
        // to sit inside the pane: otherwise the projection would paint something the
        // native window never drew.
        foreach (var broken in new[]
        {
            pane with { ChromeRegions = [Band(300, 30) with { ImageWidth = 299 }] },
            pane with { ChromeRegions = [Band(300, 30) with
            {
                Rect = new PixelRect { X = 0, Y = 190, Width = 300, Height = 30 },
            }] },
            pane with { ChromeRegions = [Band(300, 30) with { ImageFormat = "bgra8" }] },
            pane with { ChromeRegions = [Band(300, 30) with { ImageData = "not base64!" }] },
        })
        {
            snapshot.Nodes[0] = broken;
            Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        }

        // Container-only state stays out of every other kind.
        snapshot.Nodes[0] = NodeOfKind("button") with { Splits = [] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = NodeOfKind("button") with { ChromeRegions = [] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Fact]
    public void AccessibleIslandItemsMustMatchHowTheyWouldBeDrawn()
    {
        var island = NodeOfKind("accessibleIsland") with
        {
            IslandItems =
            [
                IslandItem("text", "Actions", string.Empty),
                IslandItem("button", "Computer Management (Local)", "Computer Management (Local)"),
                IslandItem("button", "More Actions", "More Actions") with { DropDown = true },
            ],
        };
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = island;
        ProtocolValidator.ValidateSnapshot(snapshot);

        // The provider's own default action is the whole contract for driving an
        // element, so an element the projection would draw as actionable has to carry
        // one, and an element that carries one may not be drawn as inert text.
        foreach (var broken in new[]
        {
            island with { IslandItems = [IslandItem("button", "No action", string.Empty)] },
            island with { IslandItems = [IslandItem("text", "Acts anyway", "Do it")] },
            island with { IslandItems = [IslandItem("text", "Menu", string.Empty) with { DropDown = true }] },
            island with { IslandItems = [IslandItem("button", string.Empty, "Unnamed")] },
            island with { IslandItems = [IslandItem("combo", "Wrong kind", "Act")] },
            island with { IslandItems = [] },
            island with { TabStop = true, TabIndex = 0 },
        })
        {
            snapshot.Nodes[0] = broken;
            Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        }

        // Island state stays out of every other kind, and the route only names an
        // island item index.
        snapshot.Nodes[0] = NodeOfKind("button") with { IslandItems = [] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        Validate(NodeAction("islandInvoke", "2"), valid: true);
        Validate(NodeAction("islandInvoke", "32"), valid: false);
        Validate(NodeAction("islandInvoke", "\"two\""), valid: false);
    }

    private static AccessibleIslandItem IslandItem(string kind, string name, string action) => new()
    {
        Kind = kind,
        Rect = new PixelRect { X = 0, Y = 0, Width = 200, Height = 32 },
        Name = name,
        Description = string.Empty,
        ActionName = action,
        Enabled = true,
    };

    // An opaque band of the requested size: the shape a container's own paint arrives
    // in.
    private static ChromeRegion Band(int width, int height) => new()
    {
        Rect = new PixelRect { X = 0, Y = 0, Width = width, Height = height },
        ImageWidth = width,
        ImageHeight = height,
        ImageFormat = "bgra8-premultiplied",
        ImageData = Convert.ToBase64String(
            Enumerable.Repeat<byte>(0x40, width * height * 4)
                .Select((value, index) => index % 4 == 3 ? (byte)0xFF : value).ToArray()),
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
                ColumnOrder = [0, 1],
                Rows = [["C:", "OK"]],
                SelectedIndices = [0],
                FocusedIndex = 0,
                MultiSelect = false,
                ColumnHeadersVisible = true,
                CheckBoxes = true,
                CheckedIndices = [0],
                ImageList = [],
                ItemImages = [-1],
                EditableLabels = false,
                EditingIndex = -1,
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
            "mdiClient" => node with { Text = string.Empty, AutomationName = string.Empty },
            "paneContainer" => node with
            {
                Text = string.Empty,
                AutomationName = string.Empty,
                Splits = [],
                ChromeRegions = [],
            },
            "accessibleIsland" => node with
            {
                Text = string.Empty,
                AutomationName = string.Empty,
                IslandItems = [IslandItem("button", "More Actions", "More Actions")],
            },
            "statusBar" => node with { Items = ["Ln 1", "100%"], ColumnWidths = [200, 80] },
            "treeView" => node with
            {
                Text = string.Empty,
                AutomationName = string.Empty,
                Items = ["Console Root", "Services", "Local"],
                ItemDepths = [0, 1, 2],
                ItemExpanded = [true, true, false],
                ItemHasChildren = [true, true, false],
                SelectedIndex = 1,
                ImageList =
                [
                    new ImageListEntry
                    {
                        ImageWidth = 1, ImageHeight = 1,
                        ImageFormat = "bgra8-premultiplied",
                        ImageData = Convert.ToBase64String([0x10, 0x20, 0x30, 0x40]),
                    },
                ],
                ItemImages = [0, 0, -1],
                ItemSelectedImages = [0, -1, -1],
                EditableLabels = true,
                EditingIndex = -1,
            },
            "slider" => node with
            {
                Minimum = 0,
                Maximum = 10,
                Position = 4,
                SmallChange = 1,
                LargeChange = 2,
            },
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

    [Fact]
    public void TreeViewHierarchyAndSelectionFailClosed()
    {
        var snapshot = TestData.Snapshot();
        var valid = NodeOfKind("treeView");
        snapshot.Nodes[0] = valid;
        ProtocolValidator.ValidateSnapshot(snapshot);

        // A depth that jumps two levels, a tree that does not start at a root, a
        // parent that denies the child it carries, an array that disagrees with the
        // labels, an empty label, and a selection outside the items.
        snapshot.Nodes[0] = valid with { ItemDepths = [0, 1, 3] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { ItemDepths = [1, 1, 2] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { ItemHasChildren = [true, false, false] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { ItemExpanded = [true, true] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { Items = ["Console Root", string.Empty, "Local"] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { SelectedIndex = 3 };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { MultiSelect = true };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));

        // Only a tree may carry per-item hierarchy state.
        snapshot.Nodes[0] = NodeOfKind("listBox") with { ItemDepths = [0] };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    [Fact]
    public void SliderRangeFailsClosed()
    {
        var snapshot = TestData.Snapshot();
        var valid = NodeOfKind("slider");
        snapshot.Nodes[0] = valid;
        ProtocolValidator.ValidateSnapshot(snapshot);

        snapshot.Nodes[0] = valid with { Maximum = 0 };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { Position = 11 };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { SmallChange = null };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[0] = valid with { LargeChange = -1 };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        // A step of zero is the native "no keyboard step" value and stays admissible.
        snapshot.Nodes[0] = valid with { SmallChange = 0, LargeChange = 0 };
        ProtocolValidator.ValidateSnapshot(snapshot);
    }

    [Fact]
    public void MdiFrameGraphIsClosedToNonContainerParents()
    {
        var snapshot = TestData.Snapshot() with { SurfaceKind = "window", Nodes = [] };
        var client = NodeOfKind("mdiClient") with { NodeId = "20", ZIndex = 0, TabIndex = -1 };
        var child = MdiChild() with { ParentNodeId = client.NodeId };
        var control = NodeOfKind("button") with
        {
            NodeId = "22", ZIndex = 2, TabIndex = -1, ParentNodeId = child.NodeId,
        };
        snapshot.Nodes.AddRange([client, child, control]);
        ProtocolValidator.ValidateSnapshot(snapshot);

        // An MDI child needs an MDI client as its parent, and no other kind may
        // own one.
        snapshot.Nodes[1] = child with { ParentNodeId = null };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[1] = child with { ParentNodeId = "22" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));

        // Caption state is required, bounded, and inside the frame.
        snapshot.Nodes[1] = child with { WindowState = "floating" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[1] = child with { Active = null };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[1] = child with
        {
            ClientRect = new PixelRect { X = 4, Y = 24, Width = 400, Height = 400 },
        };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
        snapshot.Nodes[1] = child with { ExStyle = "0x00000100" };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));

        // Only an MDI child carries caption state at all.
        snapshot.Nodes[1] = child;
        snapshot.Nodes[2] = control with { Active = true };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(snapshot));
    }

    private static ControlNode MdiChild() => new()
    {
        NodeId = "21",
        Generation = "1",
        NativeHwnd = "0x9abc",
        Kind = "mdiChild",
        ControlId = 0,
        ZIndex = 1,
        TabIndex = -1,
        Rect = new PixelRect { X = 20, Y = 20, Width = 340, Height = 200 },
        Style = "0x54CF0000",
        ExStyle = "0x00000140",
        Visible = true,
        Enabled = true,
        TabStop = false,
        DialogCode = 0,
        Text = "Document 1",
        AutomationName = "Document 1",
        Items = [],
        Active = true,
        WindowState = "normal",
        ClientRect = new PixelRect { X = 4, Y = 24, Width = 332, Height = 172 },
    };
}
