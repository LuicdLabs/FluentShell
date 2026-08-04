using System.Diagnostics;
using System.IO.Pipes;
using System.Text.Json;
using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Runtime;

public sealed class PipeClient : IAsyncDisposable
{
    private readonly RendererOptions _options;
    private readonly NamedPipeClientStream _pipe;
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private readonly CancellationTokenSource _lifetime = new();
    private Process? _parent;
    private long _outgoingSequence;
    private ulong _incomingSequence;
    private Task? _readTask;
    private Task? _heartbeatTask;
    private Task? _parentTask;
    private long _lastInboundTick;
    private int _closedSignaled;

    public Func<IProtocolMessage, ulong, Task>? MessageReceived { get; set; }
    public Action<Exception?>? Closed { get; set; }
    public Func<CancellationToken, Task>? LivenessProbe { get; set; }

    public PipeClient(RendererOptions options)
    {
        _options = options;
        _pipe = new NamedPipeClientStream(".", options.PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
    }

    public async Task ConnectAndHandshakeAsync(CancellationToken cancellationToken = default)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _lifetime.Token);
        timeout.CancelAfter(TimeSpan.FromSeconds(5));
        await _pipe.ConnectAsync(timeout.Token).ConfigureAwait(false);

        var serverPid = ProcessIdentity.GetPipeServerProcessId(_pipe.SafePipeHandle);
        if (serverPid != _options.ParentProcessId)
        {
            throw new ProtocolException($"Pipe server PID {serverPid} does not match parent PID {_options.ParentProcessId}.");
        }
        _parent = Process.GetProcessById(checked((int)serverPid));
        if (ProcessIdentity.GetCreationFileTime(_parent) != _options.ParentCreated)
        {
            throw new ProtocolException("Pipe server creation time does not match the launched parent identity.");
        }

        var first = await FrameCodec.ReadAsync(_pipe, timeout.Token).ConfigureAwait(false);
        ValidateIncomingSequence(first.Header.Sequence);
        if (first.Header.Revision != 0)
        {
            throw new ProtocolException("Bridge hello frame must have revision zero.");
        }
        var message = ProtocolSerializer.Deserialize(first.Header.MessageType, first.Payload);
        if (message is not HelloMessage hello || hello.Role != "bridge")
        {
            throw new ProtocolException("Bridge hello must be the first inbound message.");
        }
        ValidateNonce(hello);
        if (hello.ProcessId != serverPid ||
            ProtocolSerializer.ParseCanonicalUInt64(hello.ProcessCreated, "processCreated") != _options.ParentCreated ||
            hello.ProtocolMajor != ProtocolConstants.Major)
        {
            throw new ProtocolException("Bridge hello identity or protocol version is invalid.");
        }
        Volatile.Write(ref _lastInboundTick, Environment.TickCount64);

        using var current = Process.GetCurrentProcess();
        await SendAsync(new HelloMessage
        {
            SessionNonce = _options.Nonce,
            Role = "renderer",
            ProcessId = checked((uint)Environment.ProcessId),
            ProcessCreated = ProcessIdentity.GetCreationFileTime(current).ToString(System.Globalization.CultureInfo.InvariantCulture),
            ProtocolMajor = ProtocolConstants.Major,
            ProtocolMinor = ProtocolConstants.Minor,
        }, 0, timeout.Token).ConfigureAwait(false);

        _readTask = ReadLoopAsync(_lifetime.Token);
        _heartbeatTask = HeartbeatLoopAsync(_lifetime.Token);
        _parentTask = WatchParentAsync(_lifetime.Token);
    }

    public async Task SendAsync(IProtocolMessage message, ulong revision, CancellationToken cancellationToken = default)
    {
        ValidateNonce(message);
        var payload = ProtocolSerializer.Serialize(message);
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, _lifetime.Token);
        timeout.CancelAfter(TimeSpan.FromSeconds(2));
        await _writeLock.WaitAsync(timeout.Token).ConfigureAwait(false);
        try
        {
            var sequence = checked((ulong)Interlocked.Increment(ref _outgoingSequence));
            var frame = new ProtocolFrame(new FrameHeader(
                ProtocolConstants.Major,
                ProtocolConstants.Minor,
                MessageTypeNames.ToFrameType(message.MessageType),
                0,
                checked((uint)payload.Length),
                sequence,
                revision), payload);
            await FrameCodec.WriteAsync(_pipe, frame, timeout.Token).ConfigureAwait(false);
        }
        finally
        {
            _writeLock.Release();
        }
    }

    private async Task ReadLoopAsync(CancellationToken cancellationToken)
    {
        Exception? failure = null;
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var frame = await FrameCodec.ReadAsync(_pipe, cancellationToken).ConfigureAwait(false);
                ValidateIncomingSequence(frame.Header.Sequence);
                var message = ProtocolSerializer.Deserialize(frame.Header.MessageType, frame.Payload);
                ValidateNonce(message);
                if (message is HelloMessage) throw new ProtocolException("Duplicate hello message.");
                Volatile.Write(ref _lastInboundTick, Environment.TickCount64);
                if (MessageReceived is not null) await MessageReceived(message, frame.Header.Revision).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { }
        catch (Exception exception)
        {
            failure = exception;
            try
            {
                await SendAsync(new ErrorMessage
                {
                    SessionNonce = _options.Nonce,
                    Code = "protocol_fault",
                    Detail = exception.ToString(),
                    Fatal = true,
                }, 0, CancellationToken.None).ConfigureAwait(false);
            }
            catch { }
        }
        finally
        {
            _lifetime.Cancel();
            SignalClosed(failure);
        }
    }

    private async Task HeartbeatLoopAsync(CancellationToken cancellationToken)
    {
        var probeTracker = new HeartbeatProbeTracker();
        try
        {
            using var timer = new PeriodicTimer(TimeSpan.FromSeconds(1));
            while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
            {
                if (Environment.TickCount64 - Volatile.Read(ref _lastInboundTick) > 3000)
                    throw new ProtocolException("Bridge heartbeat expired.");
                if (LivenessProbe is not null)
                {
                    using var probeTimeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                    probeTimeout.CancelAfter(TimeSpan.FromMilliseconds(900));
                    try
                    {
                        await LivenessProbe(probeTimeout.Token).ConfigureAwait(false);
                        probeTracker.RecordSuccess();
                    }
                    catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
                    {
                        if (probeTracker.RecordMiss())
                        {
                            RendererDiagnostics.Log($"renderer dispatcher liveness probe missed ({probeTracker.ConsecutiveMisses}/{HeartbeatProbeTracker.MaxConsecutiveMisses})");
                            continue;
                        }

                        throw new ProtocolException("Renderer dispatcher liveness failed after three consecutive heartbeat misses.");
                    }
                    catch (TimeoutException) when (!cancellationToken.IsCancellationRequested)
                    {
                        if (probeTracker.RecordMiss())
                        {
                            RendererDiagnostics.Log($"renderer dispatcher liveness probe timed out ({probeTracker.ConsecutiveMisses}/{HeartbeatProbeTracker.MaxConsecutiveMisses})");
                            continue;
                        }

                        throw new ProtocolException("Renderer dispatcher liveness failed after three consecutive heartbeat misses.");
                    }
                    catch (Exception exception) when (!cancellationToken.IsCancellationRequested)
                    {
                        if (probeTracker.RecordMiss())
                        {
                            RendererDiagnostics.Log($"renderer dispatcher liveness probe failed ({probeTracker.ConsecutiveMisses}/{HeartbeatProbeTracker.MaxConsecutiveMisses}): {exception.Message}");
                            continue;
                        }

                        throw new ProtocolException($"Renderer dispatcher liveness failed after three consecutive heartbeat misses. Last probe failure: {exception.Message}");
                    }
                }
                await SendAsync(new HeartbeatMessage
                {
                    SessionNonce = _options.Nonce,
                    SentAt = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds().ToString(System.Globalization.CultureInfo.InvariantCulture),
                }, 0, cancellationToken).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { }
        catch (Exception exception)
        {
            _lifetime.Cancel();
            SignalClosed(exception);
        }
    }

    private async Task WatchParentAsync(CancellationToken cancellationToken)
    {
        if (_parent is null) return;
        try
        {
            await _parent.WaitForExitAsync(cancellationToken).ConfigureAwait(false);
            _lifetime.Cancel();
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { }
    }

    private void ValidateIncomingSequence(ulong sequence)
    {
        var expected = checked(_incomingSequence + 1);
        if (sequence != expected) throw new ProtocolException($"Inbound sequence {sequence} is not expected sequence {expected}.");
        _incomingSequence = sequence;
    }

    private void ValidateNonce(IProtocolMessage message)
    {
        if (!string.Equals(message.SessionNonce, _options.Nonce, StringComparison.OrdinalIgnoreCase))
        {
            throw new ProtocolException("Session nonce mismatch.");
        }
    }

    private void SignalClosed(Exception? failure)
    {
        if (Interlocked.Exchange(ref _closedSignaled, 1) == 0) Closed?.Invoke(failure);
    }

    public async ValueTask DisposeAsync()
    {
        _lifetime.Cancel();
        var tasks = new[] { _readTask, _heartbeatTask, _parentTask }.Where(task => task is not null).Cast<Task>().ToArray();
        try { await Task.WhenAll(tasks).ConfigureAwait(false); } catch { }
        _pipe.Dispose();
        _parent?.Dispose();
        _writeLock.Dispose();
        _lifetime.Dispose();
    }
}
