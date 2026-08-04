using FluentShell.Renderer.Runtime;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace FluentShell.Renderer;

public static class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        try
        {
            RendererDiagnostics.Log("entry point started");
            WinRT.ComWrappersSupport.InitializeComWrappers();
            Application.Start(initialization =>
            {
                _ = initialization;
                var context = new DispatcherQueueSynchronizationContext(
                    DispatcherQueue.GetForCurrentThread());
                SynchronizationContext.SetSynchronizationContext(context);
                new App();
            });
            RendererDiagnostics.Log("application loop exited");
        }
        catch (Exception exception)
        {
            RendererDiagnostics.Log("fatal startup failure: " + exception);
            Environment.ExitCode = 2;
        }
    }
}
