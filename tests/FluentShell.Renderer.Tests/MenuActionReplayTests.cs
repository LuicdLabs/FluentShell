using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

// A menu click that lost a race with a reconcile is refused as stale; it is re-sent only when
// the user's item is still there exactly as they saw it.
public sealed class MenuActionReplayTests
{
    private static MenuItemSnapshot Command(string path, int commandId, string text, bool enabled = true) => new()
    {
        ItemId = path,
        Kind = "command",
        Text = text,
        CommandId = commandId,
        Enabled = enabled,
    };

    private static MenuItemViewModel[] Menu(params MenuItemSnapshot[] roots) =>
        roots.Select(MenuItemViewModel.FromSnapshot).ToArray();

    private static MenuItemSnapshot File(params MenuItemSnapshot[] items) => new()
    {
        ItemId = "0",
        Kind = "popup",
        Text = "File",
        Enabled = true,
        Items = [.. items],
    };

    [Fact]
    public void ResolvesTheSameCommandUnderTheSamePath()
    {
        var menu = Menu(File(Command("0/0", 100, "New"), Command("0/1", 101, "Options...")));

        var item = MenuActionReplayPolicy.Resolve(menu, new MenuActionReplay("0/1", 101, "Options...", 1));

        Assert.NotNull(item);
        Assert.Equal(101, item.CommandId);
    }

    [Fact]
    public void ResolvesAnItemInANestedPopup()
    {
        var nested = new MenuItemSnapshot
        {
            ItemId = "0/2",
            Kind = "popup",
            Text = "Recent",
            Enabled = true,
            Items = [Command("0/2/0", 300, "One")],
        };
        var menu = Menu(File(Command("0/0", 100, "New"), Command("0/1", 101, "Open"), nested));

        Assert.NotNull(MenuActionReplayPolicy.Resolve(menu, new MenuActionReplay("0/2/0", 300, "One", 1)));
    }

    [Theory]
    [InlineData("0/1", 102, "Options...")] // the path now carries another command
    [InlineData("0/1", 101, "Settings...")] // renamed
    [InlineData("0/9", 101, "Options...")] // no such item any more
    [InlineData("0", 0, "File")] // a popup is not a command
    public void DropsAClickWhoseItemChanged(string path, int commandId, string text)
    {
        var menu = Menu(File(Command("0/0", 100, "New"), Command("0/1", 101, "Options...")));

        Assert.Null(MenuActionReplayPolicy.Resolve(menu, new MenuActionReplay(path, commandId, text, 1)));
    }

    [Fact]
    public void DropsAClickWhoseItemWasDisabledInTheMeantime()
    {
        var menu = Menu(File(Command("0/0", 100, "New", enabled: false)));

        Assert.Null(MenuActionReplayPolicy.Resolve(menu, new MenuActionReplay("0/0", 100, "New", 1)));
    }

    [Fact]
    public void ARefusedMenuClickIsRetriedOnce()
    {
        Assert.Equal(1, NodeActionReplayPolicy.MaxStaleRetries);
    }
}
