using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32;

namespace FluentShell.Gui;

// Global apply's state, kept in the user's registry hive (HKCU).  The watcher reads the same values, so
// the GUI and the watcher agree on consent and opt-outs without a shared process.
internal static class WatchSettings {
    private const string WatchKeyPath = @"Software\FluentShell\Watch";
    private const string OptOutKeyPath = @"Software\FluentShell\Watch\OptOut";
    private const string EnabledValue = "Enabled";
    private const string AcceptedHashValue = "AcceptedRiskSha256";

    public static bool IsAccepted(string currentWarningHash) {
        using var key = Registry.CurrentUser.OpenSubKey(WatchKeyPath, writable: false);
        if (key?.GetValue(EnabledValue) is not int enabled || enabled != 1) return false;
        return key.GetValue(AcceptedHashValue) is string accepted &&
               string.Equals(accepted, currentWarningHash, StringComparison.OrdinalIgnoreCase);
    }

    public static void RecordAcceptance(string warningHash) {
        using var key = Registry.CurrentUser.CreateSubKey(WatchKeyPath, writable: true);
        key.SetValue(EnabledValue, 1, RegistryValueKind.DWord);
        key.SetValue(AcceptedHashValue, warningHash, RegistryValueKind.String);
    }

    // The applications the user has turned off, by canonical image path (lowercase).
    public static IReadOnlySet<string> OptedOut() {
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using var key = Registry.CurrentUser.OpenSubKey(OptOutKeyPath, writable: false);
        if (key is null) return paths;
        foreach (var name in key.GetValueNames()) paths.Add(name);
        return paths;
    }

    public static void SetOptedOut(string imagePath, bool optedOut) {
        var name = imagePath.ToLowerInvariant();
        if (optedOut) {
            using var key = Registry.CurrentUser.CreateSubKey(OptOutKeyPath, writable: true);
            key.SetValue(name, 1, RegistryValueKind.DWord);
        } else {
            using var key = Registry.CurrentUser.OpenSubKey(OptOutKeyPath, writable: true);
            key?.DeleteValue(name, throwOnMissingValue: false);
        }
    }
}

// The warning the watcher prints.  The bytes are hashed as printed, so the hash the user accepts is the
// hash the watcher checks.
internal static class RiskWarning {
    public static (string Text, string Sha256) Load(string injectorPath) {
        var startInfo = new ProcessStartInfo(injectorPath, "watch-warning") {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            CreateNoWindow = true,
        };
        using var process = Process.Start(startInfo) ?? throw new InvalidOperationException("The injector did not start.");
        using var buffer = new MemoryStream();
        process.StandardOutput.BaseStream.CopyTo(buffer);
        process.WaitForExit();
        if (process.ExitCode != 0) throw new InvalidOperationException($"The risk warning could not be read (exit {process.ExitCode}).");

        var bytes = buffer.ToArray();
        return (Encoding.UTF8.GetString(bytes), Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant());
    }
}
