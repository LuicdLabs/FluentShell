namespace FluentShell.Gui;

internal static class Program {
    [STAThread]
    private static void Main() {
        // The GUI lives in the Gui folder beside Renderer\; the injector that runs the watcher is one level up.
        var injector = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "FluentShell.Injector.exe"));
        if (!File.Exists(injector)) {
            MessageBox.Show(
                $"FluentShell.Injector.exe was not found at:\n{injector}\n\n" +
                "Run FluentShell.Gui.exe from the Gui folder of the production output " +
                @"(build\bin\x64\Release\Gui), after .\build.ps1 has run.",
                "FluentShell", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }

        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new MainForm(injector));
    }
}
