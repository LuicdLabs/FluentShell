using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

public sealed class ProtocolFixtureTests
{
    private static byte[] Fixture(string name) =>
        File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "ProtocolFixtures", name));

    [Fact]
    public void SharedRendererHelloFixtureParses()
    {
        var hello = Assert.IsType<HelloMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.Hello, Fixture("hello.renderer.json")));
        Assert.Equal("renderer", hello.Role);
        Assert.Equal(4242U, hello.ProcessId);
        Assert.Equal((ushort)1, hello.ProtocolMajor);
    }

    [Fact]
    public void SharedFutureMinorHelloFixtureParsesAndIgnoresUnknownFields()
    {
        var hello = Assert.IsType<HelloMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.Hello, Fixture("hello.renderer.future-minor.json")));
        Assert.Equal("renderer", hello.Role);
        Assert.Equal((ushort)1, hello.ProtocolMajor);
        Assert.Equal((ushort)1, hello.ProtocolMinor);
    }

    [Fact]
    public void SharedActionFixtureParses()
    {
        var action = Assert.IsType<ActionInvokeMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke, Fixture("action.invoke.json")));
        Assert.Equal("invoke", action.Action);
        Assert.Equal("1", action.NodeId);
        Assert.Equal("18", action.EventId);
        Assert.Equal("7", action.ExpectedRevision);
    }

    [Fact]
    public void SharedUnicodeActionFixtureParsesAndIgnoresUnknownFields()
    {
        var action = Assert.IsType<ActionInvokeMessage>(
            ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke, Fixture("action.invoke.unicode.json")));
        Assert.Equal("setText", action.Action);
        Assert.Equal("9", action.NodeId);
        Assert.Equal("19", action.EventId);
        Assert.Equal("8", action.ExpectedRevision);
        Assert.Equal("繁體中文與 English 可以共存。", action.Value.GetString());
    }

    [Fact]
    public void UnknownFieldsRemainSubjectToStringCaps()
    {
        var payload = System.Text.Encoding.UTF8.GetBytes($"{{\"messageType\":\"shutdown\",\"sessionNonce\":\"{TestData.Nonce}\",\"reason\":\"ok\",\"futureMinorField\":\"{new string('x', ProtocolConstants.MaxStringChars + 1)}\"}}");
        Assert.Throws<ProtocolException>(() =>
            ProtocolSerializer.Deserialize(FrameMessageType.Shutdown, payload));
    }

    [Fact]
    public void RejectsPartialResizeBounds()
    {
        var payload = System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(new
        {
            messageType = "action.invoke",
            sessionNonce = TestData.Nonce,
            surfaceId = "11111111-2222-3333-4444-555555555555",
            eventId = "1",
            expectedRevision = "7",
            action = "resize",
            value = new { width = 10 },
        });
        Assert.Throws<ProtocolException>(() =>
            ProtocolSerializer.Deserialize(FrameMessageType.ActionInvoke, payload));
    }
}
