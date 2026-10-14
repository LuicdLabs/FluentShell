using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace FluentShell.Gui;

// The global watcher as a child of this GUI.  Its lines come back on stdout.  It is put in a job object
// that kills it when this GUI exits, so closing FluentShell ends global apply the same way a crash would:
// the Bridges see the watcher's mutex go and panic to native.
internal sealed class WatcherProcess : IDisposable {
    private readonly IntPtr _job;
    private Process? _process;

    public WatcherProcess() {
        _job = NativeMethods.CreateKillOnCloseJob();
    }

    public event Action<string>? LineReceived;
    public event Action<int>? Exited;

    public bool IsRunning => _process is { HasExited: false };

    public void Start(string injectorPath) {
        if (IsRunning) return;
        var startInfo = new ProcessStartInfo(injectorPath, "watch") {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        var process = new Process { StartInfo = startInfo, EnableRaisingEvents = true };
        process.OutputDataReceived += (_, e) => { if (e.Data is not null) LineReceived?.Invoke(e.Data); };
        process.ErrorDataReceived += (_, e) => { if (e.Data is not null) LineReceived?.Invoke("stderr: " + e.Data); };
        process.Exited += (_, _) => Exited?.Invoke(SafeExitCode(process));
        if (!process.Start()) throw new InvalidOperationException("The watcher did not start.");
        if (!NativeMethods.AssignToJob(_job, process.Handle)) {
            process.Kill();
            throw new Win32Exception(Marshal.GetLastWin32Error(), "The watcher could not be bound to this GUI.");
        }
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        _process = process;
    }

    // There is no graceful channel to the watcher.  Killing it is safe: the Bridges' lease turns the
    // missing mutex into a panic to native, and the caller sets the dormancy event first as well.
    public void Stop() {
        if (_process is { HasExited: false } process) {
            try { process.Kill(); } catch (InvalidOperationException) { }
        }
    }

    public void Dispose() {
        Stop();
        _process?.Dispose();
        NativeMethods.CloseHandle(_job);
    }

    private static int SafeExitCode(Process process) {
        try { return process.ExitCode; } catch (InvalidOperationException) { return -1; }
    }
}
