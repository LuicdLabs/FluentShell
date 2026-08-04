using System.Globalization;
using System.Threading.Channels;

namespace FluentShell.Renderer.Runtime;

internal static class RendererDiagnostics
{
    private static readonly string PathName = Path.Combine(
        Path.GetTempPath(), "FluentShell.Renderer.log");
    private static readonly Channel<string> Queue = Channel.CreateBounded<string>(
        new BoundedChannelOptions(4096)
        {
            SingleReader = true,
            FullMode = BoundedChannelFullMode.DropOldest,
        });
    private static readonly Task Writer = Task.Run(WriteLoopAsync);

    public static void Log(string message)
    {
        try
        {
            var line = string.Create(
                CultureInfo.InvariantCulture,
                $"{DateTime.Now:HH:mm:ss.fff} [Renderer:{Environment.ProcessId}] {message}{Environment.NewLine}");
            _ = Writer;
            Queue.Writer.TryWrite(line);
        }
        catch
        {
        }
    }

    private static async Task WriteLoopAsync()
    {
        await foreach (var line in Queue.Reader.ReadAllAsync().ConfigureAwait(false))
        {
            try
            {
                await File.AppendAllTextAsync(PathName, line).ConfigureAwait(false);
            }
            catch
            {
            }
        }
    }
}
