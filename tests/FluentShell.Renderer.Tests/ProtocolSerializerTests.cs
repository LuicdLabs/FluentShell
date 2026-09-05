using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

public sealed class ProtocolSerializerTests
{
    [Fact]
    public void DeserializesWindowSnapshotAndIgnoresCompatibleUnknownField()
    {
        var payload = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = TestData.DialogSnapshot(),
        });
        var json = Encoding.UTF8.GetString(payload);
        json = json[..^1];
        payload = Encoding.UTF8.GetBytes(json + ",\"futureMinorField\":true}");

        var message = Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, payload));

        // A translated MessageBox round trips as a virtual surface: its nodes carry
        // no native HWND because no native dialog was ever created.
        Assert.Equal("messageBox", message.Window.SurfaceKind);
        Assert.Equal("warning", message.Window.Icon);
        Assert.All(message.Window.Nodes, node => Assert.Null(node.NativeHwnd));
        Assert.Equal(2, message.Window.Nodes.Count);
    }

    [Fact]
    public void SemanticViolationNamingOneSurfaceIsAttributedToIt()
    {
        // Admission runs inside deserialization, so a page the renderer refuses has
        // to name its surface here.  Without that the read loop can only treat it as
        // a session fault, and one bad page would send every other window in the
        // target process back to native with it.
        var snapshot = TestData.Snapshot() with { Revision = "0" };
        var scoped = Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen,
            ProtocolSerializer.Serialize(new WindowOpenMessage
            {
                SessionNonce = TestData.Nonce,
                Window = snapshot,
            })));
        Assert.Equal(snapshot.SurfaceId, scoped.SurfaceScope);
        Assert.Equal(TestData.Nonce, scoped.ScopeNonce);
        // The rule that refused the page stays reachable for the log.
        Assert.NotNull(scoped.InnerException);

        // A session-scoped frame names no surface, so its violation stays fatal.
        var fatal = Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.Heartbeat,
            ProtocolSerializer.Serialize(new HeartbeatMessage
            {
                SessionNonce = TestData.Nonce,
                SentAt = "-1",
            })));
        Assert.Null(fatal.SurfaceScope);
        Assert.Null(fatal.ScopeNonce);
    }

    [Fact]
    public void RejectsFrameAndPayloadTypeMismatch()
    {
        var payload = ProtocolSerializer.Serialize(new HeartbeatMessage { SessionNonce = TestData.Nonce, SentAt = "1" });
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.Error, payload));
    }

    [Fact]
    public void RejectsOversizedString()
    {
        var payload = Encoding.UTF8.GetBytes($"{{\"messageType\":\"shutdown\",\"sessionNonce\":\"{TestData.Nonce}\",\"reason\":\"{new string('x', ProtocolConstants.MaxStringChars + 1)}\"}}");
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.Shutdown, payload));
    }

    [Fact]
    public void DeserializesWindowPatchEventOwnership()
    {
        var payload = ProtocolSerializer.Serialize(new WindowPatchMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = TestData.Snapshot().SurfaceId,
            BaseRevision = "7",
            Revision = "8",
            EventId = "18",
            Snapshot = TestData.Snapshot() with { Revision = "8" },
        });

        var patch = Assert.IsType<WindowPatchMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.WindowPatch, payload));

        Assert.Equal("18", patch.EventId);
    }

    [Fact]
    public void RejectsMissingRequiredSurfaceCommitField()
    {
        var payload = ProtocolSerializer.Serialize(new SurfaceCommitMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = TestData.Snapshot().SurfaceId,
            Revision = "8",
            Show = true,
        });
        var json = JsonNode.Parse(payload)!.AsObject();
        json.Remove("show");

        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.SurfaceCommit, Encoding.UTF8.GetBytes(json.ToJsonString())));
    }

    [Fact]
    public void SurfaceCommitInteractiveGateIsExplicitAndBackwardCompatible()
    {
        var gatedPayload = ProtocolSerializer.Serialize(new SurfaceCommitMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = TestData.Snapshot().SurfaceId,
            Revision = "8",
            Show = true,
            Interactive = false,
        });
        var gated = Assert.IsType<SurfaceCommitMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.SurfaceCommit, gatedPayload));
        Assert.False(gated.Interactive);

        var legacyJson = JsonNode.Parse(gatedPayload)!.AsObject();
        legacyJson.Remove("interactive");
        var legacy = Assert.IsType<SurfaceCommitMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.SurfaceCommit,
                Encoding.UTF8.GetBytes(legacyJson.ToJsonString())));
        Assert.True(legacy.Interactive);
    }

    [Fact]
    public void RejectsMissingRequiredSnapshotBoolean()
    {
        var payload = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = TestData.Snapshot(),
        });
        var json = JsonNode.Parse(payload)!.AsObject();
        json["window"]!.AsObject().Remove("enabled");

        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, Encoding.UTF8.GetBytes(json.ToJsonString())));
    }

    [Fact]
    public void RejectsUnknownActionResultStatus()
    {
        var payload = ProtocolSerializer.Serialize(new ActionResultMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = TestData.Snapshot().SurfaceId,
            EventId = "1",
            Status = "accepted",
            Revision = "8",
        });
        var json = JsonNode.Parse(payload)!.AsObject();
        json["status"] = "bogus";

        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.ActionResult, Encoding.UTF8.GetBytes(json.ToJsonString())));
    }

    [Fact]
    public void RejectsDuplicateNativeTabIndexes()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes.Add(snapshot.Nodes[0] with
        {
            NodeId = "11",
            NativeHwnd = "0x5679",
        });
        var payload = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = snapshot,
        });

        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, payload));
    }

    [Fact]
    public void AcceptsSemanticGroupAndProgressNodes()
    {
        var snapshot = TestData.Snapshot();
        snapshot = snapshot with { Nodes =
        [
            snapshot.Nodes[0] with
            {
                Kind = "groupBox",
                TabIndex = -1,
                TabStop = false,
                Text = "Status",
            },
            snapshot.Nodes[0] with
            {
                NodeId = "11",
                NativeHwnd = "0x5679",
                Kind = "progressBar",
                ZIndex = 1,
                TabIndex = -1,
                TabStop = false,
                Text = string.Empty,
                Minimum = -10,
                Maximum = 30,
                Position = 12,
                Indeterminate = true,
            },
        ] };

        var payload = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = snapshot,
        });
        var message = Assert.IsType<WindowOpenMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, payload));

        Assert.Equal("groupBox", message.Window.Nodes[0].Kind);
        Assert.Equal(12, message.Window.Nodes[1].Position);
        Assert.True(message.Window.Nodes[1].Indeterminate);
    }

    [Theory]
    [InlineData(null, 100, 50)]
    [InlineData(0, 0, 0)]
    [InlineData(0, 100, 101)]
    public void RejectsInvalidProgressState(int? minimum, int? maximum, int? position)
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with
        {
            Kind = "progressBar",
            TabIndex = -1,
            TabStop = false,
            Minimum = minimum,
            Maximum = maximum,
            Position = position,
            Indeterminate = false,
        };
        var payload = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = snapshot,
        });

        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, payload));
    }

    [Fact]
    public void PreservesEditableComboBoxState()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with
        {
            Kind = "comboBox",
            Editable = true,
            Text = "custom",
            SelectedIndex = 1,
            Items = ["one", "two"],
        };

        var payload = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = snapshot,
        });
        var message = Assert.IsType<WindowOpenMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, payload));

        Assert.True(message.Window.Nodes[0].Editable);
        Assert.Equal("custom", message.Window.Nodes[0].Text);
        Assert.Equal(1, message.Window.Nodes[0].SelectedIndex);
        Assert.Equal(["one", "two"], message.Window.Nodes[0].Items);
    }

    [Fact]
    public void RejectsMissingEditableState()
    {
        var payload = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = TestData.Snapshot(),
        });
        var json = JsonNode.Parse(payload)!.AsObject();
        json["window"]!["nodes"]![0]!.AsObject().Remove("editable");

        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, Encoding.UTF8.GetBytes(json.ToJsonString())));
    }

    [Fact]
    public void RejectsEditableNonComboBoxAndInvalidComboSelection()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with { Editable = true };
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen,
            ProtocolSerializer.Serialize(new WindowOpenMessage { SessionNonce = TestData.Nonce, Window = snapshot })));

        snapshot.Nodes[0] = snapshot.Nodes[0] with
        {
            Kind = "comboBox",
            Editable = true,
            SelectedIndex = 1,
            Items = ["only"],
        };
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen,
            ProtocolSerializer.Serialize(new WindowOpenMessage { SessionNonce = TestData.Nonce, Window = snapshot })));
    }

    [Fact]
    public void PreservesBoundedSysLinkAndListViewState()
    {
        var source = TestData.Snapshot().Nodes[0];
        var snapshot = TestData.Snapshot() with
        {
            Nodes =
            [
                source with
                {
                    Kind = "sysLink",
                    Text = "Read the privacy statement.",
                    Items = ["privacy statement"],
                },
                BoundedListView(source) with
                {
                    NodeId = "11",
                    NativeHwnd = "0x5679",
                    ZIndex = 1,
                    TabIndex = 1,
                },
            ],
        };

        var decoded = Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen,
            ProtocolSerializer.Serialize(new WindowOpenMessage
            {
                SessionNonce = TestData.Nonce,
                Window = snapshot,
            })));

        Assert.Equal("privacy statement", decoded.Window.Nodes[0].Items.Single());
        var listView = decoded.Window.Nodes[1];
        Assert.Equal(["Name", "Status"], listView.Columns);
        Assert.Equal([160, 80], listView.ColumnWidths);
        Assert.Equal(["Drive C", "Ready"], listView.Rows[0]);
        Assert.True(listView.CheckBoxes);
        Assert.Equal([0], listView.CheckedIndices);
        Assert.Equal([1], listView.SelectedIndices);
        Assert.Equal(1, listView.FocusedIndex);
        Assert.False(listView.MultiSelect);
        Assert.False(listView.ColumnHeadersVisible);
    }

    [Theory]
    [InlineData("Read Help, then Help again.", "Help")]
    [InlineData("No matching label", "Help")]
    [InlineData("Empty label", "")]
    public void RejectsNonCanonicalSysLinkLabel(string text, string label)
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with
        {
            Kind = "sysLink",
            Text = text,
            Items = [label],
        };

        AssertSnapshotRejected(snapshot);
    }

    [Fact]
    public void RejectsSysLinkWithMoreThanOneLink()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with
        {
            Kind = "sysLink",
            Text = "Read Help or Support.",
            Items = ["Help", "Support"],
        };

        AssertSnapshotRejected(snapshot);
    }

    [Fact]
    public void RejectsNonCanonicalListViewSelection()
    {
        var source = TestData.Snapshot().Nodes[0];
        foreach (var selectedIndices in new[]
                 {
                     new List<int> { 1, 0 },
                     new List<int> { 0, 0 },
                     new List<int> { 2 },
                     new List<int> { 0, 1 },
                 })
        {
            var snapshot = TestData.Snapshot();
            snapshot.Nodes[0] = BoundedListView(source) with { SelectedIndices = selectedIndices };
            AssertSnapshotRejected(snapshot);
        }
    }

    [Fact]
    public void RejectsMalformedListViewColumnsWidthsAndRows()
    {
        var source = TestData.Snapshot().Nodes[0];
        var malformed = new[]
        {
            BoundedListView(source) with { ColumnWidths = [160] },
            BoundedListView(source) with { ColumnWidths = [160, -1] },
            BoundedListView(source) with { Rows = [["missing status"]] },
            BoundedListView(source) with { Columns = [] , ColumnWidths = [], Rows = [] },
            BoundedListView(source) with { FocusedIndex = 2 },
        };
        foreach (var node in malformed)
        {
            var snapshot = TestData.Snapshot();
            snapshot.Nodes[0] = node;
            AssertSnapshotRejected(snapshot);
        }
    }

    [Fact]
    public void RejectsNullListViewRows()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = BoundedListView(snapshot.Nodes[0]);
        var payload = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = snapshot,
        });
        var json = JsonNode.Parse(payload)!.AsObject();
        json["window"]!["nodes"]![0]!["rows"] = null;

        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, Encoding.UTF8.GetBytes(json.ToJsonString())));
    }

    [Theory]
    [InlineData("selectedIndices")]
    [InlineData("focusedIndex")]
    [InlineData("multiSelect")]
    [InlineData("columns")]
    [InlineData("columnWidths")]
    [InlineData("rows")]
    [InlineData("columnHeadersVisible")]
    [InlineData("checkBoxes")]
    [InlineData("checkedIndices")]
    public void RejectsMissingCanonicalListViewField(string property)
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = BoundedListView(snapshot.Nodes[0]);
        var payload = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = snapshot,
        });
        var json = JsonNode.Parse(payload)!.AsObject();
        json["window"]!["nodes"]![0]!.AsObject().Remove(property);

        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, Encoding.UTF8.GetBytes(json.ToJsonString())));
    }

    [Fact]
    public void RejectsColumnHeaderVisibilityOnNonListView()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with { ColumnHeadersVisible = true };

        AssertSnapshotRejected(snapshot);
    }

    [Fact]
    public void RejectsNonBooleanListViewColumnHeaderVisibility()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = BoundedListView(snapshot.Nodes[0]);
        var payload = ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce,
            Window = snapshot,
        });
        var json = JsonNode.Parse(payload)!.AsObject();
        json["window"]!["nodes"]![0]!["columnHeadersVisible"] = "false";

        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen, Encoding.UTF8.GetBytes(json.ToJsonString())));
    }

    [Fact]
    public void RejectsInvalidCanonicalListViewCheckboxState()
    {
        var source = TestData.Snapshot().Nodes[0];
        foreach (var node in new[]
                 {
                     BoundedListView(source) with { CheckedIndices = [1, 0] },
                     BoundedListView(source) with { CheckedIndices = [0, 0] },
                     BoundedListView(source) with { CheckedIndices = [2] },
                     BoundedListView(source) with { CheckBoxes = false, CheckedIndices = [0] },
                 })
        {
            var snapshot = TestData.Snapshot();
            snapshot.Nodes[0] = node;
            AssertSnapshotRejected(snapshot);
        }
    }

    [Fact]
    public void AcceptsCanonicalSetSelectionAction()
    {
        var action = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = TestData.Snapshot().SurfaceId,
            NodeId = "10",
            EventId = "1",
            ExpectedRevision = "7",
            Action = "setSelection",
            Value = JsonSerializer.SerializeToElement(new[] { 0, 2 }),
        };

        var decoded = Assert.IsType<ActionInvokeMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(action)));

        Assert.Equal([0, 2], decoded.Value.EnumerateArray().Select(value => value.GetInt32()));
    }

    [Fact]
    public void PreservesCanonicalSetItemCheckAction()
    {
        var action = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = TestData.Snapshot().SurfaceId,
            NodeId = "10",
            EventId = "2",
            ExpectedRevision = "7",
            Action = "setItemCheck",
            Value = JsonSerializer.SerializeToElement(new ListViewCheckActionValue
            {
                Index = 1,
                Checked = true,
            }),
        };

        var decoded = Assert.IsType<ActionInvokeMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(action)));

        Assert.Equal(1, decoded.Value.GetProperty("index").GetInt32());
        Assert.True(decoded.Value.GetProperty("checked").GetBoolean());
    }

    [Theory]
    [InlineData("[1,1]")]
    [InlineData("[2,1]")]
    [InlineData("[-1]")]
    [InlineData("[4096]")]
    [InlineData("[\"1\"]")]
    [InlineData("{}")]
    public void RejectsNonCanonicalSetSelectionAction(string valueJson)
    {
        var action = new ActionInvokeMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = TestData.Snapshot().SurfaceId,
            NodeId = "10",
            EventId = "1",
            ExpectedRevision = "7",
            Action = "setSelection",
            Value = JsonDocument.Parse(valueJson).RootElement.Clone(),
        };

        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.ActionInvoke, ProtocolSerializer.Serialize(action)));
    }

    [Fact]
    public void PreservesBoundedReadOnlyStatusBarParts()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with
        {
            Kind = "statusBar",
            TabStop = false,
            TabIndex = -1,
            Text = string.Empty,
            Items = ["Ready", "Line 1"],
            ColumnWidths = [240, 80],
        };

        var decoded = Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen,
            ProtocolSerializer.Serialize(new WindowOpenMessage
            {
                SessionNonce = TestData.Nonce,
                Window = snapshot,
            })));

        Assert.Equal(["Ready", "Line 1"], decoded.Window.Nodes[0].Items);
        Assert.Equal([240, 80], decoded.Window.Nodes[0].ColumnWidths);
    }

    [Fact]
    public void RejectsStatusBarBeyondPartCapOrWithAmbiguousWidths()
    {
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with
        {
            Kind = "statusBar",
            TabStop = false,
            TabIndex = -1,
            Items = Enumerable.Range(0, ProtocolConstants.MaxColumns + 1)
                .Select(index => index.ToString()).ToList(),
        };
        AssertSnapshotRejected(snapshot);

        snapshot.Nodes[0] = snapshot.Nodes[0] with
        {
            Items = ["Ready", "Line 1"],
            ColumnWidths = [240],
        };
        AssertSnapshotRejected(snapshot);
    }

    [Fact]
    public void PreservesTreeHierarchyAndTrackbarStateAcrossTheWire()
    {
        // Round-tripping matters here beyond the typed rules: source-generated
        // binding leaves an omitted collection null, so a node that carries no
        // hierarchy must still be admissible next to one that does.
        var snapshot = TestData.Snapshot();
        snapshot.Nodes[0] = snapshot.Nodes[0] with
        {
            Kind = "treeView",
            Text = string.Empty,
            AutomationName = string.Empty,
            Items = ["Console Root", "Services", "Local"],
            ItemDepths = [0, 1, 2],
            ItemExpanded = [true, false, false],
            ItemHasChildren = [true, true, false],
            SelectedIndex = 2,
            ImageList = [],
            ItemImages = [-1, -1, -1],
            ItemSelectedImages = [-1, -1, -1],
            EditableLabels = false,
            EditingIndex = -1,
        };
        snapshot.Nodes.Add(snapshot.Nodes[0] with
        {
            NodeId = "11",
            Kind = "slider",
            ZIndex = 1,
            TabIndex = 1,
            Items = [],
            ItemDepths = null,
            ItemExpanded = null,
            ItemHasChildren = null,
            ItemSelectedImages = null,
            ImageList = null,
            ItemImages = null,
            EditableLabels = null,
            EditingIndex = null,
            SelectedIndex = -1,
            Minimum = 0,
            Maximum = 20,
            Position = 7,
            SmallChange = 1,
            LargeChange = 5,
        });

        var decoded = Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen,
            ProtocolSerializer.Serialize(new WindowOpenMessage
            {
                SessionNonce = TestData.Nonce,
                Window = snapshot,
            })));

        var tree = decoded.Window.Nodes[0];
        Assert.Equal([0, 1, 2], tree.ItemDepths);
        Assert.Equal([true, false, false], tree.ItemExpanded);
        Assert.Equal([true, true, false], tree.ItemHasChildren);
        Assert.Equal(2, tree.SelectedIndex);
        var slider = decoded.Window.Nodes[1];
        Assert.Equal(20, slider.Maximum);
        Assert.Equal(7, slider.Position);
        Assert.Equal(5, slider.LargeChange);
    }

    private static ControlNode BoundedListView(ControlNode source) => source with
    {
        Kind = "listView",
        Text = string.Empty,
        Items = ["Drive C", "Drive D"],
        Columns = ["Name", "Status"],
        ColumnWidths = [160, 80],
        ColumnOrder = [0, 1],
        Rows = [["Drive C", "Ready"], ["Drive D", "Running"]],
        SelectedIndices = [1],
        FocusedIndex = 1,
        MultiSelect = false,
        ColumnHeadersVisible = false,
        CheckBoxes = true,
        CheckedIndices = [0],
        ImageList = [],
        ItemImages = [-1, -1],
        EditableLabels = false,
        EditingIndex = -1,
    };

    private static void AssertSnapshotRejected(WindowSnapshot snapshot) =>
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(
            FrameMessageType.WindowOpen,
            ProtocolSerializer.Serialize(new WindowOpenMessage
            {
                SessionNonce = TestData.Nonce,
                Window = snapshot,
            })));
}
