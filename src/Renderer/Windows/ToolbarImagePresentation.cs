using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Windows;

internal enum ToolbarImageState { Normal, Hot, Disabled, Pressed }

internal static class ToolbarImagePresentation
{
    // Input state belongs to the proxy; pixel choices belong to the native toolbar.
    // Native comctl32 uses normal pixels when no pressed image exists, even over
    // a hot item. A latched button uses hot pixels independently of hover style.
    // Disabled buttons never select interaction pixels during an in-flight click.
    internal static ToolbarImageState Select(ToolbarItemSnapshot item,
        bool enabled, bool pointerOver, bool pressed, bool isChecked, bool hotTracking)
    {
        if (!enabled) return item.DisabledImage is null ? ToolbarImageState.Normal : ToolbarImageState.Disabled;
        if (pressed) return item.PressedImage is null ? ToolbarImageState.Normal : ToolbarImageState.Pressed;
        if ((isChecked || (hotTracking && pointerOver)) && item.HotImage is not null) return ToolbarImageState.Hot;
        return ToolbarImageState.Normal;
    }
}
