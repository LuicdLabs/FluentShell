using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

/// <summary>
/// Windows gates some native controls on the window being the foreground one:
/// Explorer's "Open File - Security Warning" runs a file only while its dialog is
/// in the foreground. The proxy holds the foreground, so it grants the right to
/// take it to the injected process before the actions that press a native
/// control reach it.
/// </summary>
public sealed class ForegroundGrantTests
{
    [Theory]
    [InlineData("invoke")]
    [InlineData("setCheck")]
    [InlineData("activateItem")]
    [InlineData("islandInvoke")]
    [InlineData("contextMenu")]
    [InlineData("openItem")]
    [InlineData("menuCommand")]
    [InlineData("popupCommand")]
    [InlineData("toolbarCommand")]
    [InlineData("toolbarDropDown")]
    [InlineData("splitDropDown")]
    public void PressesAskForTheForeground(string action)
    {
        Assert.True(WindowRegistry.NativePressNeedsForeground(action));
    }

    [Theory]
    [InlineData("setText")]
    [InlineData("setValue")]
    [InlineData("select")]
    [InlineData("setSelection")]
    [InlineData("focus")]
    [InlineData("scrollBy")]
    [InlineData("close")]
    [InlineData("state")]
    [InlineData("bounds")]
    [InlineData("htmlScroll")]
    [InlineData("diskScroll")]
    public void StateChangesLeaveTheForegroundAlone(string action)
    {
        Assert.False(WindowRegistry.NativePressNeedsForeground(action));
    }

    [Fact]
    public void AnUnknownActionAsksForNothing()
    {
        Assert.False(WindowRegistry.NativePressNeedsForeground("Invoke"));
        Assert.False(WindowRegistry.NativePressNeedsForeground(string.Empty));
    }
}
