using System.Globalization;
using System.Text.RegularExpressions;

namespace FluentShell.Gui;

// One status line from `FluentShell.Injector.exe watch`: "FSW|kind|field|field...".  The injector escapes
// every field to ASCII (\uXXXX), so a field never contains a raw '|' and the split is unambiguous.
internal sealed record WatchEvent(string Kind, IReadOnlyList<string> Fields);

internal static class WatchProtocol {
    private const string Prefix = "FSW";
    private static readonly Regex Escape = new(@"\\u([0-9a-fA-F]{4})", RegexOptions.Compiled);

    public static bool TryParse(string line, out WatchEvent? watchEvent) {
        watchEvent = null;
        var parts = line.Split('|');
        if (parts.Length < 2 || parts[0] != Prefix) return false;
        var fields = parts.Skip(2).Select(Unescape).ToArray();
        watchEvent = new WatchEvent(parts[1], fields);
        return true;
    }

    public static string Unescape(string text) =>
        Escape.Replace(text, match => ((char)int.Parse(match.Groups[1].Value, NumberStyles.HexNumber)).ToString());

    // Parses the hex HWND the injector writes ("%llx").  Returns false on anything else.
    public static bool TryParseHandle(string text, out long handle) =>
        long.TryParse(text, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out handle);
}
