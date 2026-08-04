using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class MnemonicTests
{
    [Theory]
    [InlineData("&MessageBox", "MessageBox", "M")]
    [InlineData("Do&n't Save", "Don't Save", "n")]
    [InlineData("Save && Close", "Save & Close", "")]
    [InlineData("Trailing&", "Trailing&", "")]
    public void ParsesWin32Mnemonic(string source, string display, string accessKey)
    {
        Assert.Equal(display, Win32Mnemonic.DisplayText(source));
        Assert.Equal(accessKey, Win32Mnemonic.AccessKey(source));
    }
}
