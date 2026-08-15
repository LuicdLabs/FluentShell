using FluentShell.Renderer.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace FluentShell.Renderer.Windows;

internal sealed class MenuProjectionFactory(Action<MenuItemViewModel> invoke)
{
    public MenuBar Create(IEnumerable<MenuItemViewModel> items)
    {
        var bar = new MenuBar();
        foreach (var item in items)
        {
            if (item.Kind != "popup")
                throw new InvalidOperationException("A projected menu bar can contain only popup roots.");
            var root = new MenuBarItem
            {
                Title = Win32Mnemonic.DisplayText(item.Text),
                AccessKey = Win32Mnemonic.AccessKey(item.Text),
                IsEnabled = item.Enabled,
            };
            ApplyAutomation(root, item);
            item.PropertyChanged += (_, args) =>
            {
                if (args.PropertyName == nameof(item.Text))
                {
                    root.Title = Win32Mnemonic.DisplayText(item.Text);
                    root.AccessKey = Win32Mnemonic.AccessKey(item.Text);
                    ApplyAutomation(root, item);
                }
                else if (args.PropertyName == nameof(item.Enabled)) root.IsEnabled = item.Enabled;
                else if (args.PropertyName == nameof(item.IsDefault)) ApplyAutomation(root, item);
            };
            foreach (var child in item.Items) root.Items.Add(CreateItem(child));
            bar.Items.Add(root);
        }
        return bar;
    }

    private MenuFlyoutItemBase CreateItem(MenuItemViewModel item)
    {
        if (item.Kind == "separator")
        {
            var separator = new MenuFlyoutSeparator();
            ApplyAutomation(separator, item);
            return separator;
        }
        if (item.Kind == "popup")
        {
            var popup = new MenuFlyoutSubItem
            {
                Text = Win32Mnemonic.DisplayText(item.Text),
                AccessKey = Win32Mnemonic.AccessKey(item.Text),
                IsEnabled = item.Enabled,
            };
            ApplyAutomation(popup, item);
            foreach (var child in item.Items) popup.Items.Add(CreateItem(child));
            return popup;
        }
        MenuFlyoutItemBase command = item.Radio
            ? new RadioMenuFlyoutItem
            {
                Text = Win32Mnemonic.DisplayText(item.Text),
                IsChecked = item.Checked,
                GroupName = $"native-menu-{item.ItemId[..item.ItemId.LastIndexOf('.')]}",
            }
            : item.Checked
            ? new ToggleMenuFlyoutItem
            {
                Text = Win32Mnemonic.DisplayText(item.Text),
                IsChecked = item.Checked,
            }
            : new MenuFlyoutItem { Text = Win32Mnemonic.DisplayText(item.Text) };
        command.AccessKey = Win32Mnemonic.AccessKey(item.Text);
        command.IsEnabled = item.Enabled;
        ApplyAutomation(command, item);
        item.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(item.Text))
            {
                switch (command)
                {
                    case RadioMenuFlyoutItem radio: radio.Text = Win32Mnemonic.DisplayText(item.Text); break;
                    case ToggleMenuFlyoutItem toggle: toggle.Text = Win32Mnemonic.DisplayText(item.Text); break;
                    case MenuFlyoutItem flyout: flyout.Text = Win32Mnemonic.DisplayText(item.Text); break;
                }
                command.AccessKey = Win32Mnemonic.AccessKey(item.Text);
                ApplyAutomation(command, item);
            }
            else if (args.PropertyName == nameof(item.Enabled)) command.IsEnabled = item.Enabled;
            else if (args.PropertyName == nameof(item.Checked))
            {
                if (command is ToggleMenuFlyoutItem toggle) toggle.IsChecked = item.Checked;
                if (command is RadioMenuFlyoutItem radio) radio.IsChecked = item.Checked;
            }
            else if (args.PropertyName == nameof(item.IsDefault)) ApplyAutomation(command, item);
        };
        if (command is MenuFlyoutItem flyout)
            flyout.Click += (_, _) => invoke(item);
        else if (command is ToggleMenuFlyoutItem toggle)
            toggle.Click += (_, _) => invoke(item);
        else if (command is RadioMenuFlyoutItem radio)
            radio.Click += (_, _) => invoke(item);
        return command;
    }

    private static void ApplyAutomation(DependencyObject element, MenuItemViewModel item)
    {
        AutomationProperties.SetAutomationId(element, AutomationId(item.ItemId));
        AutomationProperties.SetName(element, Win32Mnemonic.DisplayText(item.Text));
        AutomationProperties.SetItemStatus(element, item.IsDefault ? "Default" : string.Empty);
        if (item.IsDefault && element is Control control)
            control.FontWeight = new global::Windows.UI.Text.FontWeight { Weight = 700 };
    }

    internal static string AutomationId(string itemId) => $"FluentShell.Menu.{itemId}";
}
