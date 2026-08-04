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
}
