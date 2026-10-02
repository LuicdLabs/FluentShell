using FluentShell.Renderer.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace FluentShell.Renderer.Windows;

internal sealed class MenuProjectionFactory(
    Action<MenuItemViewModel> invoke, PresentationLifetime lifetime, string? popupId = null)
{
    public MenuFlyout CreateFlyout(IEnumerable<MenuItemViewModel> items)
    {
        var flyout = new MenuFlyout();
        foreach (var item in items) flyout.Items.Add(CreateItem(item));
        return flyout;
    }

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
            lifetime.Subscribe(item, (_, args) =>
            {
                if (args.PropertyName == nameof(item.Text))
                {
                    root.Title = Win32Mnemonic.DisplayText(item.Text);
                    root.AccessKey = Win32Mnemonic.AccessKey(item.Text);
                    ApplyAutomation(root, item);
                }
                else if (args.PropertyName == nameof(item.Enabled)) root.IsEnabled = item.Enabled;
                else if (args.PropertyName == nameof(item.IsDefault)) ApplyAutomation(root, item);
            });
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
            lifetime.Subscribe(item, (_, args) =>
            {
                if (args.PropertyName == nameof(item.Text))
                {
                    popup.Text = Win32Mnemonic.DisplayText(item.Text);
                    popup.AccessKey = Win32Mnemonic.AccessKey(item.Text);
                    ApplyAutomation(popup, item);
                }
                else if (args.PropertyName == nameof(item.Enabled)) popup.IsEnabled = item.Enabled;
                else if (args.PropertyName == nameof(item.IsDefault)) ApplyAutomation(popup, item);
            });
            foreach (var child in item.Items) popup.Items.Add(CreateItem(child));
            return popup;
        }
        MenuFlyoutItemBase command = item.Radio
            ? new RadioMenuFlyoutItem
            {
                Text = Win32Mnemonic.DisplayText(item.Text),
                IsChecked = item.Checked,
                GroupName = RadioGroupName(item.ItemId, popupId),
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
        lifetime.Subscribe(item, (_, args) =>
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
        });
        if (command is MenuFlyoutItem flyout)
            flyout.Click += (_, _) => lifetime.Invoke(() => invoke(item));
        else if (command is ToggleMenuFlyoutItem toggle)
            toggle.Click += (_, _) => lifetime.Invoke(() => invoke(item));
        else if (command is RadioMenuFlyoutItem radio)
            radio.Click += (_, _) => lifetime.Invoke(() => invoke(item));
        return command;
    }

    private void ApplyAutomation(DependencyObject element, MenuItemViewModel item)
    {
        AutomationProperties.SetAutomationId(element, popupId is null
            ? AutomationId(item.ItemId) : PopupAutomationId(popupId, item.ItemId));
        AutomationProperties.SetName(element, Win32Mnemonic.DisplayText(item.Text));
        AutomationProperties.SetItemStatus(element, item.IsDefault ? "Default" : string.Empty);
        if (element is Control control)
        {
            if (item.IsDefault)
                control.FontWeight = new global::Windows.UI.Text.FontWeight { Weight = 700 };
            else
                control.ClearValue(Control.FontWeightProperty);
        }
    }

    internal static string AutomationId(string itemId) => $"FluentShell.Menu.{itemId}";

    internal static string PopupAutomationId(string popupId, string itemId) =>
        $"FluentShell.Popup.{popupId}.Item.{itemId}";

    internal static string RadioGroupName(string itemId, string? popupId = null)
    {
        var separator = itemId.LastIndexOf('.');
        var parent = separator < 0 ? "root" : itemId[..separator];
        return $"native-menu-{popupId ?? "bar"}-{parent}";
    }
}
