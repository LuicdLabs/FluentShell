using System.Text;
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
            Window = TestData.Snapshot(),
        });
        var json = Encoding.UTF8.GetString(payload);
        json = json[..^1];
        payload = Encoding.UTF8.GetBytes(json + ",\"futureMinorField\":true}");

        var message = Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen, payload));

        Assert.Equal("messageBox", message.Window.SurfaceKind);
        Assert.Equal("warning", message.Window.Icon);
        Assert.Single(message.Window.Nodes);
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
}
