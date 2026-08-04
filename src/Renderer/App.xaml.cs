using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.Runtime;
using FluentShell.Renderer.Windows;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace FluentShell.Renderer;

public sealed partial class App : Application
{
    private PipeClient? _pipe;
    private WindowRegistry? _windows;

    public App()
    {
        RendererDiagnostics.Log("constructing WinUI application");
        InitializeComponent();
        UnhandledException += (_, args) =>
        {
            System.Diagnostics.Debug.WriteLine(args.Exception);
            RendererDiagnostics.Log("application unhandled exception: " + args.Exception);
            args.Handled = false;
        };
    }

    protected override async void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            RendererDiagnostics.Log("launch activated; parsing renderer identity");
            var options = RendererOptions.Parse(Environment.GetCommandLineArgs().Skip(1).ToArray());
            _pipe = new PipeClient(options);
            _windows = new WindowRegistry(options.Nonce, (message, revision) => _pipe.SendAsync(message, revision));
            var dispatcher = DispatcherQueue.GetForCurrentThread();
            _pipe.LivenessProbe = cancellationToken =>
                dispatcher.EnqueueAsync(() => Task.CompletedTask).WaitAsync(cancellationToken);
            _pipe.MessageReceived = (message, revision) => dispatcher.EnqueueAsync(async () =>
            {
                try
                {
                    if (_windows is null) return;
                    await _windows.HandleAsync(message, revision);
                    if (message is ShutdownMessage) Exit();
                }
                catch (Exception exception)
                {
                    RendererDiagnostics.Log("message dispatch failure: " + exception);
                    throw;
                }
            });
            _pipe.Closed = exception => dispatcher.TryEnqueue(() =>
            {
                if (exception is not null) RendererDiagnostics.Log("pipe closed: " + exception);
                _windows?.CloseAll();
                Exit();
            });
            RendererDiagnostics.Log("connecting to Bridge pipe");
            await _pipe.ConnectAndHandshakeAsync();
            RendererDiagnostics.Log("Bridge handshake complete");
        }
        catch (Exception exception)
        {
            System.Diagnostics.Debug.WriteLine(exception);
            RendererDiagnostics.Log("launch failure: " + exception);
            _windows?.CloseAll();
            Environment.ExitCode = 2;
            Exit();
        }
    }
}
