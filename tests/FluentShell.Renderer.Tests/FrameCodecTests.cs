using System.Buffers.Binary;
using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Tests;

public sealed class FrameCodecTests
{
    [Fact]
    public async Task RoundTripUsesFixedLittleEndianHeader()
    {
        var payload = new byte[] { 1, 2, 3 };
        var expected = new FrameHeader(1, 0, FrameMessageType.WindowPatch, 0, 3, 42, 99);
        await using var stream = new MemoryStream();

        await FrameCodec.WriteAsync(stream, new ProtocolFrame(expected, payload));

        var bytes = stream.ToArray();
        Assert.Equal(new byte[] { (byte)'F', (byte)'L', (byte)'S', (byte)'H' }, bytes[..4]);
        Assert.Equal(ProtocolConstants.HeaderSize + payload.Length, bytes.Length);
        Assert.Equal((ushort)FrameMessageType.WindowPatch, BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(8, 2)));
        Assert.Equal(42UL, BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(16, 8)));
        stream.Position = 0;
        var actual = await FrameCodec.ReadAsync(stream);
        Assert.Equal(expected, actual.Header);
        Assert.Equal(payload, actual.Payload);
    }

    [Theory]
    [InlineData(2, 0, 1)]
    [InlineData(1, 1, 1)]
    [InlineData(1, 0, 99)]
    public async Task RejectsInvalidMajorFlagsAndType(ushort major, ushort flags, ushort type)
    {
        var bytes = new byte[ProtocolConstants.HeaderSize];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(0, 4), ProtocolConstants.Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4, 2), major);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(8, 2), type);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(10, 2), flags);
        await using var stream = new MemoryStream(bytes);
        await Assert.ThrowsAsync<ProtocolException>(async () => await FrameCodec.ReadAsync(stream));
    }

    [Fact]
    public async Task RejectsTruncatedPayload()
    {
        var bytes = new byte[ProtocolConstants.HeaderSize + 1];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(0, 4), ProtocolConstants.Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4, 2), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(8, 2), (ushort)FrameMessageType.Heartbeat);
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(12, 4), 2);
        await using var stream = new MemoryStream(bytes);
        await Assert.ThrowsAsync<EndOfStreamException>(async () => await FrameCodec.ReadAsync(stream));
    }

    [Fact]
    public async Task AcceptsNewerSameMajorMinorVersion()
    {
        var bytes = new byte[ProtocolConstants.HeaderSize];
        BinaryPrimitives.WriteUInt32LittleEndian(bytes.AsSpan(0, 4), ProtocolConstants.Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(4, 2), ProtocolConstants.Major);
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(6, 2), (ushort)(ProtocolConstants.Minor + 1));
        BinaryPrimitives.WriteUInt16LittleEndian(bytes.AsSpan(8, 2), (ushort)FrameMessageType.Heartbeat);
        BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(16, 8), 1);
        await using var stream = new MemoryStream(bytes);
        var frame = await FrameCodec.ReadAsync(stream);
        Assert.Equal((ushort)(ProtocolConstants.Minor + 1), frame.Header.Minor);
    }
}
