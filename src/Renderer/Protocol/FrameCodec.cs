using System.Buffers.Binary;

namespace FluentShell.Renderer.Protocol;

public readonly record struct FrameHeader(
    ushort Major,
    ushort Minor,
    FrameMessageType MessageType,
    ushort Flags,
    uint PayloadLength,
    ulong Sequence,
    ulong Revision);

public sealed record ProtocolFrame(FrameHeader Header, byte[] Payload);

public static class FrameCodec
{
    public static async ValueTask WriteAsync(Stream stream, ProtocolFrame frame, CancellationToken cancellationToken = default)
    {
        ValidateHeader(frame.Header);
        if (frame.Payload.Length != frame.Header.PayloadLength)
        {
            throw new ProtocolException("Frame payload length does not match its header.");
        }

        var header = new byte[ProtocolConstants.HeaderSize];
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(0, 4), ProtocolConstants.Magic);
        BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(4, 2), frame.Header.Major);
        BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(6, 2), frame.Header.Minor);
        BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(8, 2), (ushort)frame.Header.MessageType);
        BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(10, 2), frame.Header.Flags);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(12, 4), frame.Header.PayloadLength);
        BinaryPrimitives.WriteUInt64LittleEndian(header.AsSpan(16, 8), frame.Header.Sequence);
        BinaryPrimitives.WriteUInt64LittleEndian(header.AsSpan(24, 8), frame.Header.Revision);
        await stream.WriteAsync(header, cancellationToken).ConfigureAwait(false);
        await stream.WriteAsync(frame.Payload, cancellationToken).ConfigureAwait(false);
    }

    public static async ValueTask<ProtocolFrame> ReadAsync(Stream stream, CancellationToken cancellationToken = default)
    {
        var bytes = new byte[ProtocolConstants.HeaderSize];
        await ReadExactlyAsync(stream, bytes, cancellationToken).ConfigureAwait(false);
        if (BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(0, 4)) != ProtocolConstants.Magic)
        {
            throw new ProtocolException("Invalid FLSH frame magic.");
        }

        var rawType = BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(8, 2));
        if (!Enum.IsDefined(typeof(FrameMessageType), rawType))
        {
            throw new ProtocolException($"Unknown frame message type {rawType}.");
        }

        var header = new FrameHeader(
            BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(4, 2)),
            BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(6, 2)),
            (FrameMessageType)rawType,
            BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(10, 2)),
            BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(12, 4)),
            BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(16, 8)),
            BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(24, 8)));
        ValidateHeader(header);

        var payload = GC.AllocateUninitializedArray<byte>((int)header.PayloadLength);
        await ReadExactlyAsync(stream, payload, cancellationToken).ConfigureAwait(false);
        return new ProtocolFrame(header, payload);
    }

    private static void ValidateHeader(FrameHeader header)
    {
        if (header.Major != ProtocolConstants.Major)
        {
            throw new ProtocolException($"Unsupported protocol major {header.Major}.");
        }
        if (header.Flags != 0)
        {
            throw new ProtocolException($"Unsupported frame flags 0x{header.Flags:X4}.");
        }
        if (header.PayloadLength > ProtocolConstants.MaxPayloadBytes)
        {
            throw new ProtocolException($"Frame payload exceeds {ProtocolConstants.MaxPayloadBytes} bytes.");
        }
        _ = MessageTypeNames.FromFrameType(header.MessageType);
    }

    private static async ValueTask ReadExactlyAsync(Stream stream, Memory<byte> destination, CancellationToken cancellationToken)
    {
        var read = 0;
        while (read < destination.Length)
        {
            var count = await stream.ReadAsync(destination[read..], cancellationToken).ConfigureAwait(false);
            if (count == 0)
            {
                throw new EndOfStreamException("The pipe closed in the middle of an FLSH frame.");
            }
            read += count;
        }
    }
}
