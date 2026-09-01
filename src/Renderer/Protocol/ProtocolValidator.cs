using System.Text;
using System.Text.Json;

namespace FluentShell.Renderer.Protocol;

/// <summary>
/// Protocol admission checks, kept separate from serialization so the wire
/// contract can be read as a list of rules rather than as one long method.
///
/// Every message crosses three gates in order:
///   1. <see cref="ValidateRequiredFields"/> - the JSON has the properties this
///      frame type promises, before any typed binding happens.
///   2. <see cref="ValidateCommon"/> - session identity and canonical naming.
///   3. <see cref="ValidateSemanticCaps"/> - per-message caps, ranges, and shape.
/// A rule that fails throws <see cref="ProtocolException"/>; nothing is repaired
/// or defaulted, because a malformed frame means the peer is out of contract.
/// </summary>
internal static class ProtocolValidator
{
    public static void ValidateCommon(IProtocolMessage message)
    {
        if (MessageTypeNames.ToFrameType(message.MessageType) is var type &&
            !string.Equals(message.MessageType, MessageTypeNames.FromFrameType(type), StringComparison.Ordinal))
        {
            throw new ProtocolException("Non-canonical messageType.");
        }
        if (string.IsNullOrEmpty(message.SessionNonce) ||
            message.SessionNonce.Length != 32 || !message.SessionNonce.All(Uri.IsHexDigit))
        {
            throw new ProtocolException("sessionNonce must contain exactly 32 hexadecimal characters.");
        }
    }

    public static void ValidateSemanticCaps(IProtocolMessage message)
    {
        switch (message)
        {
            case HelloMessage hello: ValidateHello(hello); break;
            case WindowOpenMessage open: ValidateSnapshot(open.Window); break;
            case WindowPatchMessage patch: ValidatePatch(patch); break;
            case ActionInvokeMessage action: ValidateActionInvoke(action); break;
            case ActionResultMessage result: ValidateActionResult(result); break;
            case SurfaceReadyMessage ready: ValidateSurfaceReady(ready); break;
            case SurfaceCommitMessage commit: RequireSurface(commit.SurfaceId, "surface.commit"); ParseUInt64(commit.Revision, "revision"); break;
            case WindowCloseMessage close: ValidateWindowClose(close); break;
            case ShutdownMessage shutdown: RequireText(shutdown.Reason, "Shutdown reason is required."); break;
            case HeartbeatMessage heartbeat: ParseUInt64(heartbeat.SentAt, "sentAt"); break;
            case ErrorMessage error: ValidateError(error); break;
        }
    }

    private static void ValidateHello(HelloMessage hello)
    {
        if (hello.ProcessId == 0 || hello.ProtocolMajor != ProtocolConstants.Major ||
            hello.Role is not ("bridge" or "renderer"))
        {
            throw new ProtocolException("Invalid hello identity or protocol fields.");
        }
        ParseUInt64(hello.ProcessCreated, "processCreated");
    }

    private static void ValidatePatch(WindowPatchMessage patch)
    {
        RequireSurface(patch.SurfaceId, "window.patch");
        var baseRevision = ParseUInt64(patch.BaseRevision, "baseRevision");
        var revision = ParseUInt64(patch.Revision, "revision");
        if (revision < baseRevision)
            throw new ProtocolException("window.patch revision precedes baseRevision.");
        if (patch.EventId is not null) ParseUInt64(patch.EventId, "eventId");
        if (patch.Operations.Count > ProtocolConstants.MaxPatchOperations)
            throw new ProtocolException("Patch operation count exceeds the protocol cap.");
        if (patch.Snapshot is not null && patch.Operations.Count != 0)
            throw new ProtocolException("A full-resync patch must have an empty operations array.");
        if (patch.Snapshot is not null)
        {
            ValidateSnapshot(patch.Snapshot);
            if (patch.Snapshot.SurfaceId != patch.SurfaceId ||
                ParseUInt64(patch.Snapshot.Revision, "snapshot.revision") != revision)
                throw new ProtocolException("Full-resync snapshot identity or revision differs from window.patch.");
        }
        foreach (var operation in patch.Operations)
        {
            if (operation.Op is not ("replace" or "add" or "remove") ||
                string.IsNullOrWhiteSpace(operation.Property))
                throw new ProtocolException("Invalid patch operation.");
            if (operation.NodeId is not null) ParseUInt64(operation.NodeId, "nodeId");
            if (operation.EventId is not null) ParseUInt64(operation.EventId, "eventId");
        }
    }

    private static void ValidateActionResult(ActionResultMessage result)
    {
        RequireSurface(result.SurfaceId, "action.result");
        ParseUInt64(result.EventId, "eventId");
        var revision = ParseUInt64(result.Revision, "revision");
        if (result.Status is not ("accepted" or "rejected" or "stale" or "closeRejected"))
            throw new ProtocolException("Unknown action.result status.");
        if (result.Snapshot is null) return;
        ValidateSnapshot(result.Snapshot);
        if (result.Snapshot.SurfaceId != result.SurfaceId ||
            ParseUInt64(result.Snapshot.Revision, "snapshot.revision") != revision)
            throw new ProtocolException("action.result snapshot identity or revision differs from its result.");
    }

    private static void ValidateSurfaceReady(SurfaceReadyMessage ready)
    {
        RequireSurface(ready.SurfaceId, "surface.ready");
        ParseUInt64(ready.Revision, "revision");
        ParseHex64(ready.ProxyHwnd, "proxyHwnd");
        ValidateRect(ready.Bounds, "bounds");
        if (ready.NodeCount is < 0 or > ProtocolConstants.MaxNodes)
            throw new ProtocolException("surface.ready nodeCount is outside the protocol cap.");
    }

    private static void ValidateWindowClose(WindowCloseMessage close)
    {
        RequireSurface(close.SurfaceId, "window.close");
        if (close.Reason is not ("nativeDestroyed" or "unsupported" or "restore" or "shutdown"))
            throw new ProtocolException("Unknown window close reason.");
    }

    private static void ValidateError(ErrorMessage error)
    {
        if (string.IsNullOrWhiteSpace(error.Code) || error.Code.Length > 64)
            throw new ProtocolException("error code is required and must fit the protocol cap.");
        if (error.Detail is null)
            throw new ProtocolException("error detail is required.");
        if (error.SurfaceId == Guid.Empty)
            throw new ProtocolException("error surfaceId cannot be empty when supplied.");
    }

    // Actions that address a control; every other action addresses the window.
    private static readonly HashSet<string> NodeAddressedActions =
        new(StringComparer.Ordinal)
        {
            "invoke", "setText", "setCheck", "select", "setSelection", "setItemCheck", "toolbarCommand",
        };

    // Actions that carry no value at all.
    private static readonly HashSet<string> NullValueActions =
        new(StringComparer.Ordinal)
        {
            "activate", "invoke", "minimize", "maximize", "restore", "close",
        };

    private static void ValidateActionInvoke(ActionInvokeMessage action)
    {
        RequireSurface(action.SurfaceId, "action.invoke");
        ParseUInt64(action.EventId, "eventId");
        ParseUInt64(action.ExpectedRevision, "expectedRevision");
        if (action.NodeId is not null) ParseUInt64(action.NodeId, "nodeId");
        if (NodeAddressedActions.Contains(action.Action) != (action.NodeId is not null))
            throw new ProtocolException("action.invoke nodeId does not match action semantics.");
        ValidateActionValue(action.Action, action.Value);
    }

    private static void ValidateActionValue(string action, JsonElement value)
    {
        switch (action)
        {
            case "setText":
                if (value.ValueKind != JsonValueKind.String)
                    throw new ProtocolException("setText requires a string value.");
                return;
            case "setCheck":
            case "select":
            case "toolbarCommand":
                if (value.ValueKind != JsonValueKind.Number || !value.TryGetInt32(out _))
                    throw new ProtocolException("setCheck/select/toolbarCommand requires an integer value.");
                if (action == "toolbarCommand" && value.GetInt32() is <= 0 or > 0xffff)
                    throw new ProtocolException("toolbarCommand requires a standard WM_COMMAND ID.");
                return;
            case "setSelection":
                ValidateCanonicalIndices(value, "setSelection");
                return;
            case "setItemCheck":
                var names = value.ValueKind == JsonValueKind.Object
                    ? value.EnumerateObject().Select(property => property.Name)
                        .Order(StringComparer.Ordinal).ToArray()
                    : [];
                if (names is not ["checked", "index"] ||
                    !value.TryGetProperty("index", out var index) ||
                    !index.TryGetInt32(out var indexValue) ||
                    indexValue is < 0 or >= ProtocolConstants.MaxItems ||
                    !value.TryGetProperty("checked", out var isChecked) ||
                    isChecked.ValueKind is not (JsonValueKind.True or JsonValueKind.False))
                    throw new ProtocolException("setItemCheck requires exactly a nonnegative integer index and boolean checked.");
                return;
            case "menuCommand":
                if (value.ValueKind != JsonValueKind.Number ||
                    !value.TryGetInt32(out var commandId) || commandId is <= 0 or > 0xffff)
                    throw new ProtocolException("menuCommand requires a standard WM_COMMAND ID.");
                return;
            case "move":
            case "resize":
                ValidateBoundsValue(value);
                return;
            default:
                if (!NullValueActions.Contains(action))
                    throw new ProtocolException("Unknown action.invoke action.");
                if (value.ValueKind != JsonValueKind.Null)
                    throw new ProtocolException("Request action requires a null value.");
                return;
        }
    }

    private static void ValidateBoundsValue(JsonElement value)
    {
        if (value.ValueKind != JsonValueKind.Object ||
            !value.TryGetProperty("x", out var x) || !x.TryGetInt32(out _) ||
            !value.TryGetProperty("y", out var y) || !y.TryGetInt32(out _) ||
            !value.TryGetProperty("width", out var width) ||
            !width.TryGetInt32(out var widthValue) || widthValue < 0 ||
            !value.TryGetProperty("height", out var height) ||
            !height.TryGetInt32(out var heightValue) || heightValue < 0)
        {
            throw new ProtocolException("move/resize requires complete integer bounds.");
        }
    }

    // ---- Snapshot ----------------------------------------------------------

    private static readonly HashSet<string> SurfaceKinds =
        new(StringComparer.Ordinal) { "window", "messageBox", "taskDialog" };

    private static readonly HashSet<string> SurfaceIcons =
        new(StringComparer.Ordinal) { "none", "warning", "error", "info", "question", "shield" };

    private static readonly HashSet<string> WindowStates =
        new(StringComparer.Ordinal) { "normal", "minimized", "maximized" };

    public static void ValidateSnapshot(WindowSnapshot snapshot)
    {
        if (snapshot.SurfaceId == Guid.Empty) throw new ProtocolException("surfaceId cannot be empty.");
        if (snapshot.Dpi is < 48 or > 960) throw new ProtocolException("Snapshot DPI is outside the protocol range.");
        if (!SurfaceKinds.Contains(snapshot.SurfaceKind)) throw new ProtocolException("Unknown surface kind.");
        if (!SurfaceIcons.Contains(snapshot.Icon)) throw new ProtocolException("Unknown surface icon.");
        if (!WindowStates.Contains(snapshot.State)) throw new ProtocolException("Unknown window state.");
        ValidateRect(snapshot.Bounds, "bounds");
        ValidateRect(snapshot.ClientBounds, "clientBounds");
        if (snapshot.Nodes is null || snapshot.Nodes.Count > ProtocolConstants.MaxNodes)
            throw new ProtocolException("Node count exceeds the protocol cap or nodes is null.");
        if (ParseUInt64(snapshot.Generation, "generation") == 0 ||
            ParseUInt64(snapshot.Revision, "revision") == 0)
            throw new ProtocolException("Snapshot generation and revision must be nonzero.");
        ParseHex64(snapshot.NativeHwnd, "nativeHwnd");
        if (snapshot.OwnerHwnd is not null) ParseHex64(snapshot.OwnerHwnd, "ownerHwnd");
        ParseHex64(snapshot.WindowStyle, "windowStyle");
        ParseHex64(snapshot.WindowExStyle, "windowExStyle");
        ValidateMenu(snapshot.Menu);
        ValidateApplicationAdapter(snapshot);

        // Node IDs and focus order are cross-node invariants, so they are tracked
        // across the whole tree rather than inside the per-node rules.
        var nodeIds = new HashSet<string>(StringComparer.Ordinal);
        var nodeKinds = new Dictionary<string, string>(StringComparer.Ordinal);
        var nodeZIndexes = new Dictionary<string, int>(StringComparer.Ordinal);
        var zIndexes = new HashSet<int>();
        var tabIndexes = new HashSet<int>();
        foreach (var node in snapshot.Nodes)
        {
            if (ParseUInt64(node.NodeId, "nodeId") == 0 ||
                ParseUInt64(node.Generation, "generation") == 0 ||
                !nodeIds.Add(node.NodeId) || !zIndexes.Add(node.ZIndex))
                throw new ProtocolException("Control node IDs and generations must be unique and nonzero.");
            ValidateNode(node);
            if (node.ParentNodeId is not null &&
                (!nodeKinds.TryGetValue(node.ParentNodeId, out var parentKind) ||
                 parentKind != "dialogContainer" ||
                 nodeZIndexes[node.ParentNodeId] >= node.ZIndex))
                throw new ProtocolException("Control parent must be a preceding dialogContainer node.");
            nodeKinds.Add(node.NodeId, node.Kind);
            nodeZIndexes.Add(node.NodeId, node.ZIndex);
            ValidateNodeTabOrder(node, tabIndexes);
        }
    }

    // ---- Control nodes -----------------------------------------------------

    // Kind -> the extra rules that kind adds.  A projected control appears here
    // exactly once, so adding an adapter means adding one entry, not editing a
    // chain of kind comparisons.  Kinds with no extra rules map to null.
    private static readonly Dictionary<string, Action<ControlNode>?> KindRules =
        new(StringComparer.Ordinal)
        {
            ["static"] = null,
            ["staticIcon"] = ValidateStaticIcon,
            ["separator"] = null,
            ["button"] = null,
            ["checkBox"] = null,
            ["threeState"] = null,
            ["radioButton"] = null,
            ["edit"] = null,
            ["password"] = null,
            ["groupBox"] = null,
            ["listBox"] = null,
            ["comboBox"] = ValidateComboBox,
            ["progressBar"] = ValidateProgress,
            ["sysLink"] = ValidateSysLink,
            ["listView"] = ValidateListView,
            ["tabControl"] = ValidateTabControl,
            ["dialogContainer"] = ValidateDialogContainer,
            ["statusBar"] = ValidateStatusBar,
            ["toolbar"] = ValidateToolbar,
        };

    private static void ValidateNode(ControlNode node)
    {
        if (node.NativeHwnd is not null) ParseHex64(node.NativeHwnd, "node.nativeHwnd");
        if (node.ParentNodeId is not null) ParseUInt64(node.ParentNodeId, "parentNodeId");
        if (node.Style is not null) ParseHex64(node.Style, "node.style");
        if (node.ExStyle is not null) ParseHex64(node.ExStyle, "node.exStyle");
        ValidateNodeCollections(node);
        ValidateRect(node.Rect, "node.rect");
        if (!KindRules.TryGetValue(node.Kind, out var extraRules))
            throw new ProtocolException("Unknown control node kind.");
        if (node.ZIndex is < 0 or >= ProtocolConstants.MaxNodes ||
            node.TabIndex is < -1 or >= ProtocolConstants.MaxNodes ||
            node.Checked is < 0 or > 2 || node.SelectedIndex is < -1 or > 4095 ||
            node.FocusedIndex is < -1 or >= ProtocolConstants.MaxItems ||
            node.SelectionStart is < 0 || node.SelectionLength is < 0)
            throw new ProtocolException("Control node state is outside the protocol range.");
        if (node.Editable && node.Kind != "comboBox")
            throw new ProtocolException("Only ComboBox nodes can be editable.");
        if (node.Kind != "staticIcon" &&
            (node.ImageWidth is not null || node.ImageHeight is not null ||
             node.ImageFormat is not null || node.ImageData is not null))
            throw new ProtocolException("Only Static icon nodes can carry image fields.");
        if (node.Kind != "listView" && node.ColumnHeadersVisible is not null)
            throw new ProtocolException("Only ListView nodes can carry columnHeadersVisible.");
        if (node.Kind != "listView" && (node.CheckBoxes is not null || node.CheckedIndices is not null))
            throw new ProtocolException("Only ListView nodes can carry checkbox row state.");
        if (node.Kind != "tabControl" && node.ItemRects is not null)
            throw new ProtocolException("Only TabControl nodes can carry itemRects.");
        if (node.Kind != "toolbar" && node.ToolbarItems is not null)
            throw new ProtocolException("Only Toolbar nodes can carry toolbarItems.");
        if (node.Kind != "progressBar" && node.Indeterminate is not null)
            throw new ProtocolException("Only ProgressBar nodes can carry indeterminate state.");
        extraRules?.Invoke(node);
    }

    // The closed set of admitted DirectUI application pages. Each entry pins
    // the exact semantic order the Bridge engine emits from its profile slots,
    // so an unknown or reshaped adapter page can never reach the controls.
    // Action codes: 0 = invoke, 1 = setCheck.
    private sealed record ApplicationPageProfile(
        string PageId,
        string[] Keys,
        string[] Kinds,
        string[] Variants,
        string[] SourceKinds,
        int[][] Actions,
        int IconIndex = -1,
        int IconWidth = 0,
        int IconHeight = 0,
        int[]? DisabledSlots = null);

    private static readonly Dictionary<string, ApplicationPageProfile> ApplicationPages = new()
    {
        ["microsoft.mdsched.directui"] = new(
            "initial",
            ["MainIcon", "MainInstruction", "ContentText", "CommandLink.0", "CommandLink.1", "Cancel"],
            ["staticIcon", "static", "static", "button", "button", "button"],
            ["mainIcon", "instruction", "content", "commandLink", "commandLink", "standard"],
            ["uiaVirtual", "uiaVirtual", "uiaVirtual", "nativeBacking", "nativeBacking", "nativeBacking"],
            [[], [], [], [0], [0], [0]],
            IconIndex: 0, IconWidth: 32, IconHeight: 32),
        ["microsoft.recoverydrive.directui"] = new(
            "first",
            ["backbutton", "wizardtitle", "headertitle", "pageText", "backupSystemFiles", "nextbutton", "cancelbutton"],
            ["button", "static", "static", "static", "checkBox", "button", "button"],
            ["standard", "wizardTitle", "header", "content", "standard", "standard", "standard"],
            ["uiaVirtual", "uiaVirtual", "uiaVirtual", "nativeBacking", "nativeBacking", "nativeBacking", "nativeBacking"],
            [[], [], [], [], [1], [0], [0]],
            DisabledSlots: [0]),
    };

    private static void ValidateApplicationAdapter(WindowSnapshot snapshot)
    {
        if (snapshot.AdapterId is null && snapshot.PageId is null)
        {
            if (snapshot.Nodes.Any(node => node.AdapterId is not null || node.PageId is not null ||
                node.SemanticKey is not null || node.SourceKind is not null ||
                node.PresentationVariant is not null || node.SupportedActions is not null ||
                node.HelpText is not null || node.AccessKey is not null || node.NativeHwnd is null))
                throw new ProtocolException("Generic surface carries application-adapter node fields.");
            return;
        }
        if (snapshot.AdapterId == ProtocolConstants.GenericDirectUiAdapterId)
        {
            ValidateGenericDirectUiAdapter(snapshot);
            return;
        }
        if (snapshot.AdapterId is null || snapshot.PageId is null ||
            !ApplicationPages.TryGetValue(snapshot.AdapterId, out var profile) ||
            profile.PageId != snapshot.PageId || snapshot.SurfaceKind != "window" ||
            snapshot.Nodes.Count != profile.Keys.Length || snapshot.Menu.Count != 0 ||
            !snapshot.CanCancel)
            throw new ProtocolException("Unknown or malformed application-adapter page.");
        var actionNames = new[] { "invoke", "setCheck" };
        for (var index = 0; index < snapshot.Nodes.Count; ++index)
        {
            var node = snapshot.Nodes[index];
            var virtualSource = profile.SourceKinds[index] == "uiaVirtual";
            var expectedActions = profile.Actions[index].Select(code => actionNames[code]).ToArray();
            if (node.AdapterId != snapshot.AdapterId || node.PageId != snapshot.PageId ||
                node.SemanticKey != profile.Keys[index] || node.Kind != profile.Kinds[index] ||
                node.PresentationVariant != profile.Variants[index] ||
                node.SourceKind != profile.SourceKinds[index] ||
                node.SupportedActions is null ||
                !node.SupportedActions.SequenceEqual(expectedActions) ||
                node.HelpText is null || node.AccessKey is null ||
                (virtualSource ? node.NativeHwnd is not null : node.NativeHwnd is null))
                throw new ProtocolException("Application-adapter semantic node shape is invalid.");
            if (expectedActions.Length != 0 && (!node.Enabled || !node.TabStop))
                throw new ProtocolException("Application-adapter actionable slot is not focusable.");
        }
        if (profile.IconIndex >= 0)
        {
            var icon = snapshot.Nodes[profile.IconIndex];
            if (icon.ImageWidth != profile.IconWidth || icon.ImageHeight != profile.IconHeight ||
                icon.Rect.Width != profile.IconWidth || icon.Rect.Height != profile.IconHeight)
                throw new ProtocolException("Application-adapter icon contract is invalid.");
        }
        if (profile.DisabledSlots is not null)
        {
            foreach (var index in profile.DisabledSlots)
            {
                if (snapshot.Nodes[index].Enabled)
                    throw new ProtocolException("Application-adapter inert slot is enabled.");
            }
        }
    }

    private static void ValidateGenericDirectUiAdapter(WindowSnapshot snapshot)
    {
        if (snapshot.PageId != ProtocolConstants.GenericDirectUiPageId ||
            snapshot.SurfaceKind != "window" || snapshot.Nodes.Count == 0 ||
            snapshot.Menu.Count != 0)
            throw new ProtocolException("Malformed generic DirectUI semantic page.");

        var semanticKeys = new HashSet<string>(StringComparer.Ordinal);
        foreach (var node in snapshot.Nodes)
        {
            if (node.AdapterId != snapshot.AdapterId || node.PageId != snapshot.PageId ||
                node.SemanticKey is null || node.SemanticKey.Length is < 1 or > 256 ||
                !node.SemanticKey.All(character =>
                    char.IsAsciiLetterOrDigit(character) || character is '.' or '_' or ':' or '-') ||
                !semanticKeys.Add(node.SemanticKey) ||
                node.SourceKind is not ("uiaVirtual" or "nativeBacking") ||
                node.PresentationVariant is null || node.SupportedActions is null ||
                node.HelpText is null || node.AccessKey is null ||
                (node.SourceKind == "uiaVirtual" ? node.NativeHwnd is not null : node.NativeHwnd is null))
                throw new ProtocolException("Generic DirectUI semantic identity or source shape is invalid.");

            string? expectedAction;
            string[] variants;
            switch (node.Kind)
            {
                case "static":
                    expectedAction = null;
                    variants = ["standard", "instruction", "content", "wizardTitle", "header"];
                    break;
                case "separator":
                    expectedAction = null;
                    variants = ["standard"];
                    break;
                case "button":
                    expectedAction = node.Enabled ? "invoke" : null;
                    variants = ["standard", "commandLink"];
                    break;
                case "checkBox":
                    if (node.Checked is not 0 and not 1)
                        throw new ProtocolException("Generic DirectUI checkbox state is not binary.");
                    expectedAction = node.Enabled ? "setCheck" : null;
                    variants = ["standard"];
                    break;
                case "progressBar":
                    expectedAction = null;
                    variants = ["standard"];
                    break;
                default:
                    throw new ProtocolException("Generic DirectUI semantic kind is not admitted.");
            }
            var expectedActions = expectedAction is null ? Array.Empty<string>() : [expectedAction];
            if (!variants.Contains(node.PresentationVariant, StringComparer.Ordinal) ||
                !node.SupportedActions.SequenceEqual(expectedActions) ||
                (expectedAction is not null && (!node.Visible || !node.Enabled || !node.TabStop)) ||
                (expectedAction is null && node.TabStop) ||
                (node.Visible && string.IsNullOrEmpty(node.AutomationName)))
                throw new ProtocolException("Generic DirectUI role, action, or focus contract is invalid.");
        }
    }

    // A focusable control must have a unique nonnegative order; everything else
    // must be explicitly out of the tab order.
    private static void ValidateNodeTabOrder(ControlNode node, HashSet<int> tabIndexes)
    {
        if (node.TabIndex is not { } tabIndex) return;
        if (node.TabStop && node.Enabled)
        {
            if (tabIndex < 0)
                throw new ProtocolException($"Focusable control '{node.NodeId}' has negative tabIndex {tabIndex}.");
            if (!tabIndexes.Add(tabIndex))
                throw new ProtocolException($"Focusable control '{node.NodeId}' duplicates tabIndex {tabIndex}.");
        }
        else if (!node.TabStop && tabIndex != -1)
        {
            throw new ProtocolException("Non-tab-stop control must use tabIndex -1.");
        }
    }

    private static void ValidateProgress(ControlNode node)
    {
        if (node.Minimum is null || node.Maximum is null || node.Position is null ||
            node.Indeterminate is null ||
            node.Maximum <= node.Minimum || node.Position < node.Minimum ||
            node.Position > node.Maximum)
            throw new ProtocolException("Progress node range or position is missing or invalid.");
    }

    private static void ValidateStaticIcon(ControlNode node)
    {
        if (node.TabStop || node.TabIndex is not null and not -1)
            throw new ProtocolException("Static icon must not be a tab stop.");
        if (node.ImageWidth is not { } width || node.ImageHeight is not { } height ||
            width is <= 0 or > ProtocolConstants.MaxImageDimension ||
            height is <= 0 or > ProtocolConstants.MaxImageDimension ||
            node.ImageFormat != "bgra8-premultiplied" || node.ImageData is null ||
            node.ImageData.Length > ProtocolConstants.MaxImageBase64Chars)
            throw new ProtocolException("Static icon metadata is missing or outside the protocol cap.");
        byte[] decoded;
        try
        {
            decoded = Convert.FromBase64String(node.ImageData);
        }
        catch (FormatException exception)
        {
            throw new ProtocolException($"Static icon imageData is not base64: {exception.Message}");
        }
        if (decoded.Length != checked(width * height * 4) ||
            decoded.Length > ProtocolConstants.MaxImageBytes ||
            Convert.ToBase64String(decoded) != node.ImageData)
            throw new ProtocolException("Static icon imageData is non-canonical or has the wrong decoded length.");
        for (var offset = 0; offset < decoded.Length; offset += 4)
        {
            var alpha = decoded[offset + 3];
            if (decoded[offset] > alpha || decoded[offset + 1] > alpha || decoded[offset + 2] > alpha)
                throw new ProtocolException("Static icon pixels are not premultiplied BGRA.");
        }
    }

    private static void ValidateDialogContainer(ControlNode node)
    {
        if (node.TabStop || node.TabIndex is not null and not -1)
            throw new ProtocolException("Dialog container must not be a tab stop.");
        if (node.Style is null || node.ExStyle is null)
            throw new ProtocolException("Dialog container requires native style evidence.");
        const ulong wsChild = 0x40000000;
        const ulong dsControl = 0x00000400;
        const ulong wsExControlParent = 0x00010000;
        const ulong unsupportedStyle = 0x00C00000 | 0x00040000 | 0x00800000 |
            0x00400000 | 0x00100000 | 0x00200000 | 0x00080000 | 0x00000080;
        const ulong unsupportedExStyle = 0x00000001 | 0x00000100 | 0x00000200 |
            0x00020000 | 0x00080000;
        var style = ParseHex64(node.Style, "dialogContainer.style");
        var exStyle = ParseHex64(node.ExStyle, "dialogContainer.exStyle");
        if ((style & (wsChild | dsControl)) != (wsChild | dsControl) ||
            (exStyle & wsExControlParent) == 0 ||
            (style & unsupportedStyle) != 0 || (exStyle & unsupportedExStyle) != 0)
            throw new ProtocolException("Dialog container lacks required DS_CONTROL shape.");
    }

    private static void ValidateComboBox(ControlNode node)
    {
        if (node.SelectedIndex is not { } selectedIndex ||
            selectedIndex < -1 || selectedIndex >= node.Items.Count)
            throw new ProtocolException("ComboBox selectedIndex is outside its item range.");
    }

    private static void ValidateNodeCollections(ControlNode node)
    {
        if (node.Items is null || node.Items.Count > ProtocolConstants.MaxItems)
            throw new ProtocolException("Item count exceeds the protocol cap or items is null.");
        if (node.SelectedIndices is null || node.SelectedIndices.Count > ProtocolConstants.MaxItems)
            throw new ProtocolException("selectedIndices exceeds the protocol cap or is null.");
        if (node.Columns is null || node.Columns.Count > ProtocolConstants.MaxColumns ||
            node.Columns.Any(column => column is null))
            throw new ProtocolException("Column count exceeds the protocol cap or columns is null.");
        if (node.ColumnWidths is null || node.ColumnWidths.Count > ProtocolConstants.MaxColumns ||
            node.ColumnWidths.Any(width => width < 0))
            throw new ProtocolException("columnWidths is null, exceeds the protocol cap, or contains a negative width.");
        if (node.Rows is null || node.Rows.Count > ProtocolConstants.MaxItems)
            throw new ProtocolException("Row count exceeds the protocol cap or rows is null.");
        if (node.Rows.Any(row => row is null || row.Count > ProtocolConstants.MaxColumns ||
                row.Any(cell => cell is null)))
            throw new ProtocolException("A ListView row is null or exceeds the column cap.");
        ValidateCanonicalIndices(node.SelectedIndices, "selectedIndices");
    }

    private static void ValidateSysLink(ControlNode node)
    {
        if (node.Items.Count != 1 || string.IsNullOrEmpty(node.Items[0]) || node.Text is null)
            throw new ProtocolException("SysLink requires exactly one nonempty link label.");
        var label = node.Items[0];
        var first = node.Text.IndexOf(label, StringComparison.Ordinal);
        if (first < 0 || node.Text.IndexOf(label, first + label.Length, StringComparison.Ordinal) >= 0)
            throw new ProtocolException("SysLink label must occur exactly once in text.");
    }

    private static void ValidateListView(ControlNode node)
    {
        if (node.ColumnHeadersVisible is null)
            throw new ProtocolException("ListView columnHeadersVisible is missing.");
        if (node.CheckBoxes is null || node.CheckedIndices is null ||
            node.CheckedIndices.Count > ProtocolConstants.MaxItems)
            throw new ProtocolException("ListView checkbox state is missing or exceeds the item cap.");
        if (node.Columns.Count == 0 || node.ColumnWidths.Count != node.Columns.Count)
            throw new ProtocolException("ListView requires one width for each nonempty column set.");
        if (node.Rows.Any(row => row.Count != node.Columns.Count))
            throw new ProtocolException("Every ListView row must contain exactly one cell per column.");
        if (node.SelectedIndices.Any(index => index >= node.Rows.Count))
            throw new ProtocolException("ListView selectedIndices contains an index outside its rows.");
        ValidateCanonicalIndices(node.CheckedIndices, "checkedIndices");
        if (node.CheckedIndices.Any(index => index >= node.Rows.Count))
            throw new ProtocolException("ListView checkedIndices contains an index outside its rows.");
        if (!node.CheckBoxes.Value && node.CheckedIndices.Count != 0)
            throw new ProtocolException("ListView without checkBoxes cannot carry checkedIndices.");
        if (!node.MultiSelect && node.SelectedIndices.Count > 1)
            throw new ProtocolException("A single-select ListView cannot contain multiple selectedIndices.");
        if (node.FocusedIndex is not { } focusedIndex ||
            focusedIndex < -1 || focusedIndex >= node.Rows.Count)
            throw new ProtocolException("ListView focusedIndex is missing or outside its rows.");
    }

    private static void ValidateTabControl(ControlNode node)
    {
        if (node.Style is null || node.ExStyle is null || node.ItemRects is null ||
            node.Items.Count is <= 0 or > ProtocolConstants.MaxTabItems ||
            node.ItemRects.Count != node.Items.Count ||
            node.Items.Any(string.IsNullOrEmpty) ||
            node.SelectedIndex is not { } selected || selected < 0 || selected >= node.Items.Count)
            throw new ProtocolException("TabControl requires bounded labels, itemRects, style evidence, and selection.");
        const ulong acceptedTabStyles = 0x0240;
        if ((ParseHex64(node.Style, "tabControl.style") & 0xffffUL & ~acceptedTabStyles) != 0 ||
            (ParseHex64(node.ExStyle, "tabControl.exStyle") & ~0x00000004UL) != 0)
            throw new ProtocolException("TabControl style evidence is outside the supported top-tab shape.");
        for (var index = 0; index < node.ItemRects.Count; ++index)
        {
            var rect = node.ItemRects[index];
            ValidateRect(rect, $"tabControl.itemRects[{index}]");
            if (rect.Width <= 0 || rect.Height <= 0 || rect.X < 0 || rect.Y < 0 ||
                (long)rect.X + rect.Width > node.Rect.Width ||
                (long)rect.Y + rect.Height > node.Rect.Height)
                throw new ProtocolException("TabControl item rectangle is malformed or outside local bounds.");
            for (var previous = 0; previous < index; ++previous)
            {
                var other = node.ItemRects[previous];
                if (rect.X < other.X + other.Width && rect.X + rect.Width > other.X &&
                    rect.Y < other.Y + other.Height && rect.Y + rect.Height > other.Y)
                    throw new ProtocolException("TabControl item rectangles overlap.");
            }
        }
        try
        {
            _ = Windows.ControlFactory.GroupTabHeaderRows(node.ItemRects);
        }
        catch (ArgumentException exception)
        {
            throw new ProtocolException(exception.Message);
        }
    }

    private static void ValidateStatusBar(ControlNode node)
    {
        if (node.Items.Count > ProtocolConstants.MaxColumns || node.Items.Any(item => item is null))
            throw new ProtocolException("StatusBar item count exceeds the part cap or contains null text.");
        if (node.ColumnWidths.Count != 0 && node.ColumnWidths.Count != node.Items.Count)
            throw new ProtocolException("StatusBar columnWidths must be empty or contain one width per part.");
    }

    private static void ValidateToolbar(ControlNode node)
    {
        if (node.Style is null || node.ExStyle is null || node.ToolbarItems is null ||
            node.ToolbarItems.Count is <= 0 or > ProtocolConstants.MaxToolbarItems)
            throw new ProtocolException("Toolbar requires style evidence and a bounded item collection.");
        const ulong acceptedStyles = 0x8b01;
        if ((ParseHex64(node.Style, "toolbar.style") & 0xffffUL & ~acceptedStyles) != 0 ||
            ParseHex64(node.ExStyle, "toolbar.exStyle") != 0)
            throw new ProtocolException("Toolbar style evidence is outside the supported one-row shape.");
        var commands = new HashSet<int>();
        int? top = null;
        int? bottom = null;
        long previousRight = 0;
        foreach (var item in node.ToolbarItems)
        {
            if (item is null) throw new ProtocolException("Toolbar item is null.");
            ValidateRect(item.Rect, "toolbar.item.rect");
            if (item.Hidden)
            {
                if (item.Rect.Width != 0 || item.Rect.Height != 0)
                    throw new ProtocolException("Hidden Toolbar item must have empty geometry.");
            }
            else
            {
                if (item.Rect.X < 0 || item.Rect.Y < 0 || item.Rect.Width <= 0 || item.Rect.Height <= 0 ||
                    (long)item.Rect.X + item.Rect.Width > node.Rect.Width ||
                    (long)item.Rect.Y + item.Rect.Height > node.Rect.Height || item.Rect.X < previousRight)
                    throw new ProtocolException("Toolbar item geometry is malformed, overlapping, or outside local bounds.");
                top ??= item.Rect.Y;
                bottom ??= item.Rect.Y + item.Rect.Height;
                if (item.Rect.Y != top || item.Rect.Y + item.Rect.Height != bottom)
                    throw new ProtocolException("Toolbar item geometry is not one row.");
                previousRight = (long)item.Rect.X + item.Rect.Width;
            }
            if (item.Kind == "separator")
            {
                if (item.CommandId != 0 || !string.IsNullOrEmpty(item.Text) || item.ImageWidth is not null ||
                    item.ImageHeight is not null || item.ImageFormat is not null || item.ImageData is not null)
                    throw new ProtocolException("Toolbar separator carries push-button semantics.");
                continue;
            }
            if (item.Kind != "pushButton" || item.CommandId is <= 0 or > 0xffff ||
                !commands.Add(item.CommandId) || string.IsNullOrEmpty(item.Text))
                throw new ProtocolException("Toolbar push button identity or direct label is invalid.");
            ValidateToolbarImage(item);
        }
        if (top is null) throw new ProtocolException("Toolbar requires visible one-row geometry.");
    }

    private static void ValidateToolbarImage(ToolbarItemSnapshot item)
    {
        if (item.ImageWidth is not { } width || item.ImageHeight is not { } height ||
            width is <= 0 or > ProtocolConstants.MaxImageDimension ||
            height is <= 0 or > ProtocolConstants.MaxImageDimension ||
            item.ImageFormat != "bgra8-premultiplied" || item.ImageData is null)
            throw new ProtocolException("Toolbar push button image metadata is missing or outside the cap.");
        byte[] pixels;
        try { pixels = Convert.FromBase64String(item.ImageData); }
        catch (FormatException exception) { throw new ProtocolException($"Toolbar imageData is not base64: {exception.Message}"); }
        if (pixels.Length != checked(width * height * 4) || Convert.ToBase64String(pixels) != item.ImageData)
            throw new ProtocolException("Toolbar imageData is non-canonical or has the wrong length.");
        for (var offset = 0; offset < pixels.Length; offset += 4)
        {
            var alpha = pixels[offset + 3];
            if (pixels[offset] > alpha || pixels[offset + 1] > alpha || pixels[offset + 2] > alpha)
                throw new ProtocolException("Toolbar icon pixels are not premultiplied BGRA.");
        }
    }

    // ---- Menu --------------------------------------------------------------

    // Menu identity is positional: an item ID is its own index path, which is
    // what lets the Bridge resolve a projected click back to one HMENU item.
    private sealed class MenuScope
    {
        public int Count;
        public readonly HashSet<string> Ids = new(StringComparer.Ordinal);
        public readonly HashSet<int> Commands = new();
    }

    private static void ValidateMenu(List<MenuItemSnapshot>? menu)
    {
        if (menu is null) throw new ProtocolException("Snapshot menu is null.");
        ValidateMenuLevel(menu, new MenuScope(), depth: 1, topLevel: true, parentPath: string.Empty);
    }

    private static void ValidateMenuLevel(
        IReadOnlyList<MenuItemSnapshot> items,
        MenuScope scope,
        int depth,
        bool topLevel,
        string parentPath)
    {
        if (depth > ProtocolConstants.MaxMenuDepth)
            throw new ProtocolException("Menu exceeds the protocol depth cap.");
        for (var index = 0; index < items.Count; index++)
        {
            var item = items[index];
            var expectedPath = parentPath.Length == 0 ? index.ToString() : $"{parentPath}.{index}";
            if (++scope.Count > ProtocolConstants.MaxMenuItems || item.ItemId != expectedPath ||
                !scope.Ids.Add(item.ItemId) || item.Items is null)
                throw new ProtocolException("Menu identity or item count is invalid.");
            ValidateMenuItem(item, scope, depth, topLevel);
        }
    }

    private static void ValidateMenuItem(
        MenuItemSnapshot item, MenuScope scope, int depth, bool topLevel)
    {
        switch (item.Kind)
        {
            case "popup":
                if (string.IsNullOrEmpty(item.Text) || item.CommandId != 0 || item.Items.Count == 0)
                    throw new ProtocolException("Popup menu shape is invalid.");
                ValidateMenuLevel(item.Items, scope, depth + 1, topLevel: false, item.ItemId);
                return;
            case "command":
                if (topLevel || string.IsNullOrEmpty(item.Text) || item.Items.Count != 0 ||
                    item.CommandId is <= 0 or > 0xffff || !scope.Commands.Add(item.CommandId))
                    throw new ProtocolException("Menu command identity is invalid or ambiguous.");
                return;
            case "separator":
                if (topLevel || item.Text.Length != 0 || item.CommandId != 0 || item.Items.Count != 0)
                    throw new ProtocolException("Menu separator shape is invalid.");
                return;
            default:
                throw new ProtocolException("Unknown menu item kind.");
        }
    }

    // ---- Required-field gate -----------------------------------------------

    // The properties each frame type must carry, checked against raw JSON before
    // any typed binding so a missing field is never silently defaulted.
    private static readonly Dictionary<FrameMessageType, string[]> RequiredByFrame = new()
    {
        [FrameMessageType.Hello] =
            ["role", "processId", "processCreated", "protocolMajor", "protocolMinor"],
        [FrameMessageType.WindowOpen] = ["window"],
        [FrameMessageType.WindowPatch] = ["surfaceId", "baseRevision", "revision", "operations"],
        [FrameMessageType.ActionInvoke] =
            ["surfaceId", "eventId", "expectedRevision", "action", "value"],
        [FrameMessageType.ActionResult] = ["surfaceId", "eventId", "status", "revision"],
        [FrameMessageType.SurfaceReady] =
            ["surfaceId", "revision", "proxyHwnd", "bounds", "nodeCount", "uiaReady"],
        [FrameMessageType.SurfaceCommit] = ["surfaceId", "revision", "show"],
        [FrameMessageType.WindowClose] = ["surfaceId", "reason"],
        [FrameMessageType.Heartbeat] = ["sentAt"],
        [FrameMessageType.Error] = ["code", "detail", "fatal"],
        [FrameMessageType.Shutdown] = ["reason"],
    };

    public static void ValidateRequiredFields(FrameMessageType frameType, JsonElement root)
    {
        RequireProperties(root, "message", "messageType", "sessionNonce");
        if (!RequiredByFrame.TryGetValue(frameType, out var required))
            throw new ProtocolException("Unknown frame message type.");
        var context = MessageTypeNames.FromFrameType(frameType);
        RequireProperties(root, context, required);

        // Nested shapes only some frames carry.
        switch (frameType)
        {
            case FrameMessageType.WindowOpen:
                ValidateRequiredSnapshotFields(root.GetProperty("window"), "window.open.window");
                break;
            case FrameMessageType.WindowPatch:
                var operations = RequireKind(
                    root.GetProperty("operations"), JsonValueKind.Array, "window.patch.operations");
                foreach (var operation in operations.EnumerateArray())
                    RequireProperties(operation, "window.patch.operation", "op", "property", "value");
                RequireOptionalSnapshot(root, "window.patch.snapshot");
                break;
            case FrameMessageType.ActionResult:
                RequireOptionalSnapshot(root, "action.result.snapshot");
                break;
            case FrameMessageType.SurfaceReady:
                ValidateRequiredRectFields(root.GetProperty("bounds"), "surface.ready.bounds");
                break;
        }
    }

    private static void RequireOptionalSnapshot(JsonElement root, string context)
    {
        if (root.TryGetProperty("snapshot", out var snapshot) &&
            snapshot.ValueKind != JsonValueKind.Null)
        {
            ValidateRequiredSnapshotFields(snapshot, context);
        }
    }

    private static readonly string[] RequiredSnapshotProperties =
    [
        "surfaceId", "surfaceKind", "modal", "canCancel", "icon", "generation", "revision",
        "nativeHwnd", "title", "dpi", "bounds", "clientBounds", "windowStyle", "windowExStyle",
        "visible", "enabled", "state", "showInTaskbar", "rtl", "nodes",
    ];

    private static readonly string[] RequiredNodeProperties =
    [
        "nodeId", "generation", "nativeHwnd", "kind", "controlId", "zIndex", "rect",
        "visible", "enabled", "tabStop", "dialogCode", "text", "editable", "items",
    ];

    private static readonly string[] RequiredAdapterNodeProperties =
    [
        "adapterId", "pageId", "semanticKey", "sourceKind", "presentationVariant",
        "supportedActions", "helpText", "accessKey",
    ];

    private static readonly string[] RequiredListViewProperties =
        ["selectedIndices", "focusedIndex", "multiSelect", "columns", "columnWidths", "rows",
         "columnHeadersVisible", "checkBoxes", "checkedIndices"];

    private static readonly string[] RequiredImageProperties =
        ["imageWidth", "imageHeight", "imageFormat", "imageData"];

    private static readonly string[] RequiredProgressProperties =
        ["minimum", "maximum", "position", "indeterminate"];

    private static readonly string[] RequiredTabControlProperties =
        ["selectedIndex", "itemRects"];

    private static readonly string[] RequiredToolbarItemProperties =
        ["kind", "commandId", "rect", "text", "enabled", "hidden"];

    private static readonly string[] RequiredMenuProperties =
    [
        "itemId", "kind", "text", "commandId", "enabled", "checked", "radio", "isDefault", "items",
    ];

    private static void ValidateRequiredSnapshotFields(JsonElement snapshot, string context)
    {
        RequireProperties(snapshot, context, RequiredSnapshotProperties);
        var hasAdapter = snapshot.TryGetProperty("adapterId", out var adapter) &&
            adapter.ValueKind != JsonValueKind.Null;
        var hasPage = snapshot.TryGetProperty("pageId", out var page) &&
            page.ValueKind != JsonValueKind.Null;
        if (hasAdapter != hasPage)
            throw new ProtocolException($"{context} must carry adapterId and pageId together.");
        ValidateRequiredRectFields(snapshot.GetProperty("bounds"), $"{context}.bounds");
        ValidateRequiredRectFields(snapshot.GetProperty("clientBounds"), $"{context}.clientBounds");
        var nodes = RequireKind(snapshot.GetProperty("nodes"), JsonValueKind.Array, $"{context}.nodes");
        if (snapshot.TryGetProperty("menu", out var menu))
        {
            ValidateRequiredMenuFields(
                RequireKind(menu, JsonValueKind.Array, $"{context}.menu"), $"{context}.menu", 1);
        }
        foreach (var node in nodes.EnumerateArray())
        {
            RequireProperties(node, $"{context}.node", RequiredNodeProperties);
            if (hasAdapter)
                RequireProperties(node, $"{context}.adapterNode", RequiredAdapterNodeProperties);
            ValidateRequiredRectFields(node.GetProperty("rect"), $"{context}.node.rect");
            RequireKind(node.GetProperty("items"), JsonValueKind.Array, $"{context}.node.items");
            var kind = RequireKind(
                node.GetProperty("kind"), JsonValueKind.String, $"{context}.node.kind").GetString();
            if (kind == "progressBar")
            {
                RequireProperties(node, $"{context}.progressBar", RequiredProgressProperties);
                var indeterminateKind = node.GetProperty("indeterminate").ValueKind;
                if (indeterminateKind is not JsonValueKind.True and not JsonValueKind.False)
                    throw new ProtocolException(
                        $"{context}.progressBar.indeterminate must be a JSON boolean.");
            }
            else if (node.TryGetProperty("indeterminate", out _))
            {
                throw new ProtocolException($"{context}.node carries ProgressBar-only indeterminate state.");
            }
            if (kind == "listView")
            {
                RequireProperties(node, $"{context}.listView", RequiredListViewProperties);
            }
            else if (node.TryGetProperty("columnHeadersVisible", out _) ||
                     node.TryGetProperty("checkBoxes", out _) ||
                     node.TryGetProperty("checkedIndices", out _))
            {
                throw new ProtocolException($"{context}.node carries ListView-only fields for a non-ListView kind.");
            }
            var imageFields = RequiredImageProperties.Count(name => node.TryGetProperty(name, out _));
            if (kind == "staticIcon")
            {
                RequireProperties(node, $"{context}.staticIcon", RequiredImageProperties);
            }
            else if (imageFields != 0)
            {
                throw new ProtocolException($"{context}.node carries image fields for a non-image kind.");
            }
            if (kind == "tabControl")
            {
                RequireProperties(node, $"{context}.tabControl", RequiredTabControlProperties);
                var itemRects = RequireKind(
                    node.GetProperty("itemRects"), JsonValueKind.Array, $"{context}.tabControl.itemRects");
                foreach (var rect in itemRects.EnumerateArray())
                    ValidateRequiredRectFields(rect, $"{context}.tabControl.itemRects");
            }
            else if (node.TryGetProperty("itemRects", out _))
            {
                throw new ProtocolException($"{context}.node carries TabControl-only itemRects.");
            }
            if (kind == "toolbar")
            {
                RequireProperties(node, $"{context}.toolbar", "toolbarItems");
                var toolbarItems = RequireKind(node.GetProperty("toolbarItems"), JsonValueKind.Array,
                    $"{context}.toolbar.toolbarItems");
                foreach (var item in toolbarItems.EnumerateArray())
                {
                    RequireProperties(item, $"{context}.toolbar.item", RequiredToolbarItemProperties);
                    ValidateRequiredRectFields(item.GetProperty("rect"), $"{context}.toolbar.item.rect");
                    if (item.GetProperty("kind").GetString() == "pushButton")
                        RequireProperties(item, $"{context}.toolbar.pushButton", RequiredImageProperties);
                }
            }
            else if (node.TryGetProperty("toolbarItems", out _))
            {
                throw new ProtocolException($"{context}.node carries Toolbar-only toolbarItems.");
            }
        }
    }

    private static void ValidateRequiredMenuFields(JsonElement menu, string context, int depth)
    {
        if (depth > ProtocolConstants.MaxMenuDepth)
            throw new ProtocolException("Menu exceeds the protocol depth cap.");
        foreach (var item in menu.EnumerateArray())
        {
            RequireProperties(item, context, RequiredMenuProperties);
            var children = RequireKind(item.GetProperty("items"), JsonValueKind.Array, $"{context}.items");
            ValidateRequiredMenuFields(children, $"{context}.items", depth + 1);
        }
    }

    private static void ValidateRequiredRectFields(JsonElement rect, string context)
    {
        RequireKind(rect, JsonValueKind.Object, context);
        RequireProperties(rect, context, "x", "y", "width", "height");
    }

    // ---- Primitives --------------------------------------------------------

    private static void RequireSurface(Guid surfaceId, string context)
    {
        if (surfaceId == Guid.Empty)
            throw new ProtocolException($"{context} surfaceId cannot be empty.");
    }

    private static void RequireText(string? value, string message)
    {
        if (string.IsNullOrWhiteSpace(value)) throw new ProtocolException(message);
    }

    private static void ValidateRect(PixelRect rect, string field)
    {
        if (rect.Width < 0 || rect.Height < 0)
            throw new ProtocolException($"{field} dimensions cannot be negative.");
    }

    private static ulong ParseUInt64(string value, string field) =>
        ProtocolSerializer.ParseCanonicalUInt64(value, field);

    private static ulong ParseHex64(string value, string field)
    {
        if (string.IsNullOrEmpty(value) || value.Length is < 3 or > 18 ||
            !value.StartsWith("0x", StringComparison.Ordinal) ||
            !ulong.TryParse(value.AsSpan(2), System.Globalization.NumberStyles.AllowHexSpecifier,
                System.Globalization.CultureInfo.InvariantCulture, out var parsed))
        {
            throw new ProtocolException($"{field} is not a canonical hexadecimal value.");
        }
        return parsed;
    }

    // Selection indices are canonical: unique, strictly increasing, in range.
    // That is what lets the Bridge compare two selections without sorting them.
    private static void ValidateCanonicalIndices(IReadOnlyList<int> indices, string field)
    {
        if (indices.Count > ProtocolConstants.MaxItems)
            throw new ProtocolException($"{field} exceeds the item cap.");
        var previous = -1;
        foreach (var index in indices)
        {
            if (index is < 0 or >= ProtocolConstants.MaxItems || index <= previous)
                throw new ProtocolException($"{field} must contain unique, strictly increasing in-range indices.");
            previous = index;
        }
    }

    private static void ValidateCanonicalIndices(JsonElement value, string field)
    {
        if (value.ValueKind != JsonValueKind.Array)
            throw new ProtocolException($"{field} requires an integer array.");
        var indices = new List<int>();
        foreach (var entry in value.EnumerateArray())
        {
            if (entry.ValueKind != JsonValueKind.Number || !entry.TryGetInt32(out var index))
                throw new ProtocolException($"{field} requires integer indices.");
            indices.Add(index);
            if (indices.Count > ProtocolConstants.MaxItems)
                throw new ProtocolException($"{field} exceeds the item cap.");
        }
        ValidateCanonicalIndices(indices, field);
    }

    private static JsonElement RequireKind(JsonElement element, JsonValueKind kind, string context)
    {
        if (element.ValueKind != kind)
            throw new ProtocolException($"{context} must be a JSON {kind}.");
        return element;
    }

    private static void RequireProperties(JsonElement element, string context, params string[] names)
    {
        RequireKind(element, JsonValueKind.Object, context);
        foreach (var name in names)
        {
            if (!element.TryGetProperty(name, out _))
                throw new ProtocolException($"{context} is missing required property '{name}'.");
        }
    }

    // Depth is already capped by JsonDocumentOptions; this bounds the width of a
    // payload that parsed successfully but would still be unreasonable to bind.
    public static void ValidateJsonTree(JsonElement element)
    {
        switch (element.ValueKind)
        {
            case JsonValueKind.String:
                if ((element.GetString()?.Length ?? 0) > ProtocolConstants.MaxStringChars)
                    throw new ProtocolException("JSON string exceeds the protocol cap.");
                break;
            case JsonValueKind.Array:
                foreach (var child in element.EnumerateArray()) ValidateJsonTree(child);
                break;
            case JsonValueKind.Object:
                foreach (var property in element.EnumerateObject())
                {
                    if (Encoding.UTF8.GetByteCount(property.Name) > 256)
                        throw new ProtocolException("JSON property name is unreasonably long.");
                    ValidateJsonTree(property.Value);
                }
                break;
        }
    }
}
