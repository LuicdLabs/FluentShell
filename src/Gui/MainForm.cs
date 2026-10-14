using System.Globalization;

namespace FluentShell.Gui;

// The FluentShell entry point.  It turns global apply on and off, lists what the watcher sees, and rolls
// back single windows.  Everything it shows comes from the watcher's status lines, the consent and opt-out
// settings in the user's registry, and the rollback events the Bridges publish.
internal sealed class MainForm : Form {
    private readonly string _injectorPath;
    private readonly WatcherProcess _watcher = new();
    private readonly System.Windows.Forms.Timer _refresh = new() { Interval = 1000 };

    private readonly Label _status = new() {
        Dock = DockStyle.Top,
        AutoSize = false,
        Height = 34,
        Font = new Font(SystemFonts.DefaultFont, FontStyle.Bold),
        TextAlign = ContentAlignment.MiddleLeft,
        Padding = new Padding(0, 0, 0, 0),
    };
    private readonly Button _start = new() { Text = "Start global apply", AutoSize = true };
    private readonly Button _stop = new() { Text = "Stop and restore all windows", AutoSize = true };
    private readonly Label _hint = new() {
        Text = "Ctrl+Alt+Shift+F12 does the same from any window. Elevated windows are skipped. Nothing starts at logon.",
        AutoSize = true,
        Padding = new Padding(8, 8, 0, 0),
    };

    private readonly ListView _targets = NewList(
        ("Application", 300), ("PID", 70), ("State", 90), ("Detail", 420));
    private readonly ListView _windows = NewList(
        ("Window", 420), ("PID", 70), ("Projected", 90), ("Handle", 120));
    private readonly Button _rollback = new() { Text = "Roll back selected window", AutoSize = true, Enabled = false };
    private readonly TextBox _log = new() {
        Multiline = true,
        ReadOnly = true,
        ScrollBars = ScrollBars.Vertical,
        Dock = DockStyle.Fill,
        Font = new Font(FontFamily.GenericMonospace, 8.5f),
    };

    private readonly ContextMenuStrip _targetMenu = new();
    private readonly ToolStripMenuItem _turnOff = new("Turn this application off, and roll back its windows");
    private readonly ToolStripMenuItem _turnOn = new("Turn this application back on");

    private sealed record TargetInfo(uint ProcessId, string Image);
    private sealed record WindowInfo(uint ProcessId, long Handle);

    public MainForm(string injectorPath) {
        _injectorPath = injectorPath;
        Text = "FluentShell";
        ClientSize = new Size(1000, 680);
        MinimumSize = new Size(760, 520);
        StartPosition = FormStartPosition.CenterScreen;

        var buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight, AutoSize = false };
        buttons.Controls.Add(_start);
        buttons.Controls.Add(_stop);
        buttons.Controls.Add(_hint);
        // Docking follows the order of Controls: edge-docked controls are added first and the Fill control
        // last, so Fill takes only the space the edges leave.
        var header = new Panel { Dock = DockStyle.Top, Height = 82, Padding = new Padding(10, 6, 10, 0) };
        header.Controls.Add(_status);
        header.Controls.Add(buttons);

        var targetsPanel = new Panel { Dock = DockStyle.Fill };
        targetsPanel.Controls.Add(new Label { Text = "Applications the watcher has seen", Dock = DockStyle.Top, AutoSize = true, Padding = new Padding(10, 6, 0, 4) });
        targetsPanel.Controls.Add(_targets);

        var windowsBar = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, Padding = new Padding(10, 4, 10, 4) };
        windowsBar.Controls.Add(new Label { Text = "Windows of applications FluentShell projects", AutoSize = true, Padding = new Padding(0, 6, 12, 0) });
        windowsBar.Controls.Add(_rollback);
        var windowsPanel = new Panel { Dock = DockStyle.Fill };
        windowsPanel.Controls.Add(windowsBar);
        windowsPanel.Controls.Add(_windows);

        var split = new SplitContainer { Dock = DockStyle.Fill, Orientation = Orientation.Horizontal, SplitterDistance = 300 };
        split.Panel1.Controls.Add(targetsPanel);
        split.Panel2.Controls.Add(windowsPanel);

        var logPanel = new Panel { Dock = DockStyle.Bottom, Height = 150, Padding = new Padding(10, 0, 10, 10) };
        logPanel.Controls.Add(new Label { Text = "Log", Dock = DockStyle.Top, AutoSize = true, Padding = new Padding(0, 4, 0, 2) });
        logPanel.Controls.Add(_log);

        Controls.Add(header);
        Controls.Add(logPanel);
        Controls.Add(split);

        _targetMenu.Items.Add(_turnOff);
        _targetMenu.Items.Add(_turnOn);
        _targets.ContextMenuStrip = _targetMenu;
        _targets.MouseDown += (_, e) => {
            if (e.Button != MouseButtons.Right) return;
            var item = _targets.GetItemAt(e.X, e.Y);
            if (item is not null) item.Selected = true;
        };
        _targetMenu.Opening += (_, e) => { if (_targets.SelectedItems.Count == 0) e.Cancel = true; };
        _turnOff.Click += (_, _) => SetOptedOut(optedOut: true);
        _turnOn.Click += (_, _) => SetOptedOut(optedOut: false);

        _start.Click += (_, _) => StartGlobalApply();
        _stop.Click += (_, _) => StopGlobalApply();
        _rollback.Click += (_, _) => RollbackSelectedWindow();
        _windows.SelectedIndexChanged += (_, _) => UpdateRollbackButton();

        _watcher.LineReceived += line => BeginInvoke(new Action(() => OnLine(line)));
        _watcher.Exited += code => BeginInvoke(new Action(() => OnWatcherExited(code)));

        _refresh.Tick += (_, _) => RefreshProjections();
        _refresh.Start();

        SetRunning(running: false, "off");
    }

    private static ListView NewList(params (string Name, int Width)[] columns) {
        var list = new ListView {
            View = View.Details,
            FullRowSelect = true,
            HideSelection = false,
            MultiSelect = false,
            Dock = DockStyle.Fill,
        };
        foreach (var (name, width) in columns) list.Columns.Add(name, width);
        return list;
    }

    private void StartGlobalApply() {
        try {
            var warning = RiskWarning.Load(_injectorPath);
            if (!WatchSettings.IsAccepted(warning.Sha256)) {
                using var consent = new ConsentForm(warning.Text);
                if (consent.ShowDialog(this) != DialogResult.OK) {
                    Log("Global apply stays off: the risk warning was not accepted.");
                    return;
                }
                WatchSettings.RecordAcceptance(warning.Sha256);
                Log("Risk warning accepted. The watcher will start only while this acceptance matches.");
            }
            Dormancy.Resume();
            _watcher.Start(_injectorPath);
            SetRunning(running: true, "starting");
        } catch (Exception ex) {
            Log("Global apply did not start: " + ex.Message);
            MessageBox.Show(this, ex.Message, "FluentShell", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    // Stop is also the panic path: the dormancy event makes every Bridge restore its windows, and the
    // watcher is stopped so nothing new is injected.
    private void StopGlobalApply() {
        try {
            Dormancy.SetDormant();
            _watcher.Stop();
            SetRunning(running: false, "stopped by you; every window is native");
        } catch (Exception ex) {
            Log("Stop failed: " + ex.Message);
        }
    }

    private void OnLine(string line) {
        Log(line);
        if (!WatchProtocol.TryParse(line, out var watchEvent) || watchEvent is null) return;
        switch (watchEvent.Kind) {
            case "started":
                SetRunning(running: true, "running");
                break;
            case "stopped":
                SetRunning(running: false, Describe(watchEvent.Fields.FirstOrDefault()));
                break;
            case "target":
                OnTarget(watchEvent.Fields);
                break;
            case "gone":
                if (TryPid(watchEvent.Fields, 0, out var gone)) RemoveProcess(gone);
                break;
            case "windows":
                if (TryPid(watchEvent.Fields, 0, out var replaced)) RemoveWindows(replaced);
                break;
            case "window":
                OnWindow(watchEvent.Fields);
                break;
        }
    }

    private void OnWatcherExited(int code) {
        if (!_watcher.IsRunning) {
            SetRunning(running: false, $"the watcher ended (exit {code}); every window is native");
            Log($"The watcher ended with exit code {code}.");
        }
    }

    private void OnTarget(IReadOnlyList<string> fields) {
        if (fields.Count < 5 || !TryPid(fields, 0, out var pid)) return;
        var image = fields[2];
        var key = pid.ToString(CultureInfo.InvariantCulture);
        if (!_targets.Items.ContainsKey(key)) {
            var item = new ListViewItem(key) { Name = key };
            item.SubItems.Add(key);
            item.SubItems.Add(string.Empty);
            item.SubItems.Add(string.Empty);
            _targets.Items.Add(item);
        }
        var row = _targets.Items[key]!;
        row.Tag = new TargetInfo(pid, image);
        row.Text = Path.GetFileName(image);
        row.SubItems[1].Text = key;
        row.SubItems[2].Text = fields[3];
        row.SubItems[3].Text = fields[4];
    }

    private void OnWindow(IReadOnlyList<string> fields) {
        if (fields.Count < 3 || !TryPid(fields, 0, out var pid) || !WatchProtocol.TryParseHandle(fields[1], out var handle)) return;
        var key = $"{pid}:{handle:x}";
        if (!_windows.Items.ContainsKey(key)) {
            var item = new ListViewItem(fields[2]) { Name = key };
            item.SubItems.Add(pid.ToString(CultureInfo.InvariantCulture));
            item.SubItems.Add(string.Empty);
            item.SubItems.Add($"{handle:x}");
            _windows.Items.Add(item);
        }
        var row = _windows.Items[key]!;
        row.Text = fields[2];
        row.Tag = new WindowInfo(pid, handle);
    }

    private void RemoveProcess(uint pid) {
        var key = pid.ToString(CultureInfo.InvariantCulture);
        if (_targets.Items.ContainsKey(key)) _targets.Items.RemoveByKey(key);
        RemoveWindows(pid);
    }

    private void RemoveWindows(uint pid) {
        for (var index = _windows.Items.Count - 1; index >= 0; index--) {
            if (_windows.Items[index].Tag is WindowInfo window && window.ProcessId == pid) {
                _windows.Items.RemoveAt(index);
            }
        }
    }

    private void RefreshProjections() {
        foreach (ListViewItem item in _windows.Items) {
            if (item.Tag is not WindowInfo window) continue;
            item.SubItems[2].Text = RollbackRequests.IsProjected(window.ProcessId, window.Handle) ? "yes" : "no";
        }
        UpdateRollbackButton();
    }

    private void UpdateRollbackButton() {
        _rollback.Enabled = _windows.SelectedItems.Count > 0 &&
                            _windows.SelectedItems[0].SubItems[2].Text == "yes";
    }

    private void RollbackSelectedWindow() {
        if (_windows.SelectedItems.Count == 0 || _windows.SelectedItems[0].Tag is not WindowInfo window) return;
        var item = _windows.SelectedItems[0];
        var requested = RollbackRequests.Request(window.ProcessId, window.Handle);
        Log(requested
            ? $"Rollback requested for \"{item.Text}\" (PID {window.ProcessId}). It stays native until the application restarts."
            : $"\"{item.Text}\" is not projected, so there is nothing to roll back.");
        RefreshProjections();
    }

    private void SetOptedOut(bool optedOut) {
        if (_targets.SelectedItems.Count == 0 || _targets.SelectedItems[0].Tag is not TargetInfo target) return;
        WatchSettings.SetOptedOut(target.Image, optedOut);
        if (!optedOut) {
            Log($"{Path.GetFileName(target.Image)} is back on. The watcher offers it its windows on the next pass.");
            return;
        }
        var rolledBack = 0;
        foreach (ListViewItem item in _windows.Items) {
            if (item.Tag is WindowInfo window && window.ProcessId == target.ProcessId &&
                RollbackRequests.Request(window.ProcessId, window.Handle)) {
                rolledBack++;
            }
        }
        Log($"{Path.GetFileName(target.Image)} is off. Rollback requested for {rolledBack} projected window(s).");
    }

    private void SetRunning(bool running, string detail) {
        _status.Text = $"Global apply: {(running ? "ON" : "off")} - {detail}";
        _start.Enabled = !running;
    }

    private static string Describe(string? reason) => reason switch {
        "panic" => "panic hotkey pressed; every window is native",
        "disabled" => "turned off; every window is native",
        null or "" => "stopped",
        _ => reason,
    };

    private static bool TryPid(IReadOnlyList<string> fields, int index, out uint pid) {
        pid = 0;
        return fields.Count > index && uint.TryParse(fields[index], NumberStyles.None, CultureInfo.InvariantCulture, out pid);
    }

    private void Log(string text) {
        _log.AppendText($"{DateTime.Now:HH:mm:ss}  {text}{Environment.NewLine}");
    }

    protected override void OnFormClosing(FormClosingEventArgs e) {
        // Closing FluentShell ends global apply, so every window goes back to native first.
        if (_watcher.IsRunning) Dormancy.SetDormant();
        _watcher.Dispose();
        _refresh.Stop();
        base.OnFormClosing(e);
    }
}
