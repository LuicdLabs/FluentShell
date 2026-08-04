using FluentShell.Renderer.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Media;
using Windows.System;

namespace FluentShell.Renderer.Windows;

internal sealed class ControlFactory
{
    private static readonly BooleanToVisibilityConverter VisibilityConverter = new();
    private static readonly Win32MnemonicTextConverter MnemonicTextConverter = new();
    private readonly double _scale;
    private readonly Action<ControlNodeViewModel, string, object?> _action;
    private readonly Func<bool> _isApplyingCanonical;
    private readonly Func<bool> _isImeComposing;
    private readonly IReadOnlyDictionary<string, string> _radioGroups;

    public ControlFactory(
        int dpi,
        IEnumerable<ControlNodeViewModel> nodes,
        Action<ControlNodeViewModel, string, object?> action,
        Func<bool> isApplyingCanonical,
        Func<bool> isImeComposing)
    {
        _scale = 96.0 / dpi;
        _action = action;
        _isApplyingCanonical = isApplyingCanonical;
        _isImeComposing = isImeComposing;
        _radioGroups = BuildRadioGroups(nodes);
    }

    public FrameworkElement Create(ControlNodeViewModel viewModel)
    {
        FrameworkElement element = viewModel.Kind switch
        {
            "static" => CreateStatic(viewModel),
            "separator" => new Border { Height = 1, Background = new SolidColorBrush(Microsoft.UI.Colors.Gray) },
            "button" => CreateButton(viewModel),
            "checkBox" => CreateCheckBox(viewModel, false),
            "threeState" => CreateCheckBox(viewModel, true),
            "radioButton" => CreateRadioButton(viewModel),
            "edit" => CreateTextBox(viewModel),
            "password" => CreatePasswordBox(viewModel),
            "comboBox" => CreateComboBox(viewModel),
            "listBox" => CreateListBox(viewModel),
            _ => throw new InvalidOperationException($"Unsupported translated control kind '{viewModel.Kind}'."),
        };

        element.DataContext = viewModel;
        element.IsTabStop = viewModel.TabStop;
        element.TabIndex = Math.Max(0, viewModel.TabIndex);
        ApplyAutomationAndAccessKey(element, viewModel);
        Bind(element, UIElement.VisibilityProperty, nameof(viewModel.Visible), BindingMode.OneWay, VisibilityConverter);
        if (element is Control)
        {
            Bind(element, Control.IsEnabledProperty, nameof(viewModel.Enabled), BindingMode.OneWay);
        }
        else
        {
            Bind(element, UIElement.IsHitTestVisibleProperty, nameof(viewModel.Enabled), BindingMode.OneWay);
        }
        ApplyBounds(element, viewModel);
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.Rect)) ApplyBounds(element, viewModel);
            if (args.PropertyName == nameof(viewModel.AutomationName) ||
                args.PropertyName == nameof(viewModel.Text))
            {
                ApplyAutomationAndAccessKey(element, viewModel);
            }
        };
        Canvas.SetZIndex(element, viewModel.ZIndex);
        return element;
    }

    private static TextBlock CreateStatic(ControlNodeViewModel viewModel)
    {
        var control = new TextBlock { TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center };
        Bind(control, TextBlock.TextProperty, nameof(viewModel.Text), BindingMode.OneWay);
        return control;
    }

    private Button CreateButton(ControlNodeViewModel viewModel)
    {
        var control = new Button { HorizontalContentAlignment = HorizontalAlignment.Center };
        Bind(control, ContentControl.ContentProperty, nameof(viewModel.Text), BindingMode.OneWay, MnemonicTextConverter);
        control.Click += (_, _) =>
        {
            if (!_isApplyingCanonical()) _action(viewModel, "invoke", null);
        };
        return control;
    }

    private CheckBox CreateCheckBox(ControlNodeViewModel viewModel, bool threeState)
    {
        var control = new CheckBox { IsThreeState = threeState, IsChecked = ToNullableBool(viewModel.Checked) };
        Bind(control, ContentControl.ContentProperty, nameof(viewModel.Text), BindingMode.OneWay, MnemonicTextConverter);
        RoutedEventHandler changed = (_, _) =>
        {
            var value = control.IsChecked is null ? 2 : control.IsChecked.Value ? 1 : 0;
            if (!_isApplyingCanonical() && value != viewModel.Checked) _action(viewModel, "setCheck", value);
        };
        control.Checked += changed;
        control.Unchecked += changed;
        control.Indeterminate += changed;
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.Checked)) control.IsChecked = ToNullableBool(viewModel.Checked);
        };
        return control;
    }

    private RadioButton CreateRadioButton(ControlNodeViewModel viewModel)
    {
        var control = new RadioButton
        {
            IsChecked = viewModel.Checked == 1,
            GroupName = _radioGroups[viewModel.NodeId],
        };
        Bind(control, ContentControl.ContentProperty, nameof(viewModel.Text), BindingMode.OneWay, MnemonicTextConverter);
        control.Checked += (_, _) =>
        {
            if (!_isApplyingCanonical() && viewModel.Checked != 1) _action(viewModel, "setCheck", 1);
        };
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.Checked)) control.IsChecked = viewModel.Checked == 1;
        };
        return control;
    }

    private TextBox CreateTextBox(ControlNodeViewModel viewModel)
    {
        var control = new TextBox
        {
            AcceptsReturn = viewModel.Multiline,
            TextWrapping = viewModel.Multiline ? TextWrapping.Wrap : TextWrapping.NoWrap,
            IsReadOnly = viewModel.ReadOnly,
        };
        Bind(control, TextBox.TextProperty, nameof(viewModel.DraftText), BindingMode.TwoWay);
        var timer = control.DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(300);
        timer.IsRepeating = false;
        string? pendingText = null;
        void CommitDraft()
        {
            timer.Stop();
            if (_isImeComposing())
            {
                timer.Start();
                return;
            }
            if (_isApplyingCanonical() || control.Text == viewModel.Text || control.Text == pendingText) return;
            pendingText = control.Text;
            _action(viewModel, "setText", control.Text);
        }
        control.TextChanged += (_, _) =>
        {
            if (_isApplyingCanonical()) return;
            timer.Stop();
            timer.Start();
        };
        timer.Tick += (_, _) => CommitDraft();
        control.LostFocus += (_, _) => CommitDraft();
        control.KeyDown += (_, args) =>
        {
            if (!viewModel.Multiline && args.Key == VirtualKey.Enter) CommitDraft();
        };
        void ApplySelection()
        {
            var start = Math.Clamp(viewModel.SelectionStart, 0, control.Text.Length);
            var length = Math.Clamp(viewModel.SelectionLength, 0, control.Text.Length - start);
            control.Select(start, length);
        }
        control.Loaded += (_, _) => ApplySelection();
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.Text)) pendingText = null;
            if (args.PropertyName is nameof(viewModel.SelectionStart) or nameof(viewModel.SelectionLength))
                ApplySelection();
        };
        return control;
    }

    private PasswordBox CreatePasswordBox(ControlNodeViewModel viewModel)
    {
        var control = new PasswordBox { Password = viewModel.DraftText };
        control.PasswordChanged += (_, _) =>
        {
            viewModel.DraftText = control.Password;
            if (!_isApplyingCanonical() && control.Password != viewModel.Text) _action(viewModel, "setText", control.Password);
        };
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.DraftText) && control.Password != viewModel.DraftText) control.Password = viewModel.DraftText;
        };
        return control;
    }

    private ComboBox CreateComboBox(ControlNodeViewModel viewModel)
    {
        var control = new ComboBox { ItemsSource = viewModel.Items, SelectedIndex = viewModel.SelectedIndex };
        control.SelectionChanged += (_, _) =>
        {
            if (!_isApplyingCanonical() && control.SelectedIndex != viewModel.SelectedIndex) _action(viewModel, "select", control.SelectedIndex);
        };
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.SelectedIndex)) control.SelectedIndex = viewModel.SelectedIndex;
        };
        return control;
    }

    private ListBox CreateListBox(ControlNodeViewModel viewModel)
    {
        var itemStyle = new Style(typeof(ListBoxItem));
        itemStyle.Setters.Add(new Setter(Control.MinHeightProperty, 22d));
        itemStyle.Setters.Add(new Setter(Control.PaddingProperty, new Thickness(4, 0, 4, 0)));
        itemStyle.Setters.Add(new Setter(Control.VerticalContentAlignmentProperty, VerticalAlignment.Center));
        var control = new ListBox
        {
            ItemsSource = viewModel.Items,
            SelectedIndex = viewModel.SelectedIndex,
            ItemContainerStyle = itemStyle,
        };
        control.SelectionChanged += (_, _) =>
        {
            if (!_isApplyingCanonical() && control.SelectedIndex != viewModel.SelectedIndex) _action(viewModel, "select", control.SelectedIndex);
        };
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.SelectedIndex)) control.SelectedIndex = viewModel.SelectedIndex;
        };
        return control;
    }

    private void ApplyBounds(FrameworkElement element, ControlNodeViewModel viewModel)
    {
        Canvas.SetLeft(element, viewModel.Rect.X * _scale);
        Canvas.SetTop(element, viewModel.Rect.Y * _scale);
        element.Width = Math.Max(0, viewModel.Rect.Width * _scale);
        element.Height = Math.Max(0, viewModel.Rect.Height * _scale);
    }

    private static void ApplyAutomationAndAccessKey(
        FrameworkElement element,
        ControlNodeViewModel viewModel)
    {
        AutomationProperties.SetName(element, AutomationNameFor(viewModel));
        AutomationProperties.SetAutomationId(
            element, $"FluentShell.Node.{viewModel.NodeId}.{viewModel.Generation}");
        if (element is Button or CheckBox or RadioButton)
        {
            element.AccessKey = Win32Mnemonic.AccessKey(viewModel.Text);
        }
    }

    internal static string AutomationNameFor(ControlNodeViewModel viewModel) =>
        viewModel.Kind == "password"
            ? "Password edit"
            : Win32Mnemonic.DisplayText(viewModel.AutomationName);

    private static bool? ToNullableBool(int value) => value == 2 ? null : value == 1;

    internal static IReadOnlyDictionary<string, string> BuildRadioGroups(
        IEnumerable<ControlNodeViewModel> nodes)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var siblings in nodes.OrderBy(node => node.ZIndex)
                     .GroupBy(node => node.ParentNodeId ?? "root", StringComparer.Ordinal))
        {
            var group = 0;
            foreach (var node in siblings)
            {
                if (node.GroupStart) ++group;
                if (node.Kind == "radioButton")
                    result[node.NodeId] = $"native-{siblings.Key}-{group}";
            }
        }
        return result;
    }

    private static void Bind(FrameworkElement target, DependencyProperty property, string path, BindingMode mode, IValueConverter? converter = null) =>
        target.SetBinding(property, new Binding { Path = new PropertyPath(path), Mode = mode, Converter = converter });
}
