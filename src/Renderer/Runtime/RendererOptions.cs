using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Runtime;

public sealed record RendererOptions(string PipeName, string Nonce, uint ParentProcessId, ulong ParentCreated)
{
    public static RendererOptions Parse(IReadOnlyList<string> arguments)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        for (var index = 0; index < arguments.Count; index += 2)
        {
            if (index + 1 >= arguments.Count || !arguments[index].StartsWith("--", StringComparison.Ordinal))
            {
                throw new ArgumentException("Expected --pipe, --nonce, --parent-pid and --parent-created value pairs.");
            }
            if (!values.TryAdd(arguments[index], arguments[index + 1]))
            {
                throw new ArgumentException($"Duplicate renderer option '{arguments[index]}'.");
            }
        }

        var pipe = Required(values, "--pipe");
        const string pipePrefix = @"\\.\pipe\";
        if (pipe.StartsWith(pipePrefix, StringComparison.OrdinalIgnoreCase)) pipe = pipe[pipePrefix.Length..];
        if (string.IsNullOrWhiteSpace(pipe) || pipe.Length > 240 || pipe.Contains('\0'))
        {
            throw new ArgumentException("Invalid named-pipe name.");
        }
        var nonce = Required(values, "--nonce");
        if (nonce.Length != 32 || !nonce.All(Uri.IsHexDigit)) throw new ArgumentException("Invalid session nonce.");
        if (!uint.TryParse(Required(values, "--parent-pid"), out var parentPid) || parentPid == 0)
        {
            throw new ArgumentException("Invalid parent process ID.");
        }
        var parentCreated = ProtocolSerializer.ParseCanonicalUInt64(Required(values, "--parent-created"), "parent-created");
        return new RendererOptions(pipe, nonce.ToUpperInvariant(), parentPid, parentCreated);
    }

    private static string Required(Dictionary<string, string> values, string name) =>
        values.TryGetValue(name, out var value) ? value : throw new ArgumentException($"Missing renderer option '{name}'.");
}
