using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace FluentShell.Renderer.Windows;

// WinUI's real ProgressBar remains the visual and the range-state owner. The native
// vertical variant fills upwards, so its horizontal visual is rotated by -90
// degrees inside the same HWND-sized layout slot.
internal sealed class SemanticProgressBarControl : ContentControl
{
    private readonly ProgressBar _progress = new()
    {
        MinWidth = 0,
        MinHeight = 0,
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Top,
        IsHitTestVisible = false,
    };
    private readonly MatrixTransform _transform = new();
    private bool _vertical;

    public SemanticProgressBarControl()
    {
        IsTabStop = false;
        MinWidth = 0;
        MinHeight = 0;
        Padding = new Thickness(0);
        BorderThickness = new Thickness(0);
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        VerticalContentAlignment = VerticalAlignment.Stretch;
        // A transparent full-size canvas gives the peer the layout slot's origin,
        // even when the framework's indicator occupies only its centered thin band.
        var canvas = new Canvas { Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent) };
        canvas.Children.Add(_progress);
        Content = canvas;
        _progress.RenderTransform = _transform;
        AutomationProperties.SetAccessibilityView(canvas, AccessibilityView.Raw);
        AutomationProperties.SetAccessibilityView(_progress, AccessibilityView.Raw);
        SizeChanged += (_, _) => ApplyLayout();
    }

    public bool Vertical
    {
        get => _vertical;
        set
        {
            if (_vertical == value) return;
            _vertical = value;
            ApplyLayout();
        }
    }

    public double Minimum { get => _progress.Minimum; set => _progress.Minimum = value; }
    public double Maximum { get => _progress.Maximum; set => _progress.Maximum = value; }
    public double Value { get => _progress.Value; set => _progress.Value = value; }
    public bool IsIndeterminate
        { get => _progress.IsIndeterminate; set => _progress.IsIndeterminate = value; }

    private void ApplyLayout()
    {
        var layout = ProgressBarLayout.For(ActualWidth, ActualHeight, Vertical);
        _progress.Width = layout.VisualWidth;
        _progress.Height = layout.VisualHeight;
        _transform.Matrix = new Matrix(
            layout.M11, layout.M12, layout.M21, layout.M22, layout.OffsetX, layout.OffsetY);
    }

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticProgressBarAutomationPeer(this);
}

internal readonly record struct ProgressBarLayout(
    double VisualWidth, double VisualHeight,
    double M11, double M12, double M21, double M22, double OffsetX, double OffsetY)
{
    internal static bool IsVerticalStyle(ulong style) => (style & 0x0004UL) != 0; // PBS_VERTICAL

    internal static ProgressBarLayout For(double width, double height, bool vertical)
    {
        width = double.IsFinite(width) ? Math.Max(0, width) : 0;
        height = double.IsFinite(height) ? Math.Max(0, height) : 0;
        return vertical
            ? new(height, width, 0, -1, 1, 0, 0, height)
            : new(width, height, 1, 0, 0, 1, 0, 0);
    }
}
