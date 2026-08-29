using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Runtime;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Media;
using Windows.System;

namespace FluentShell.Renderer.Windows;

internal sealed class ControlFactory
{
    private const double NativeFontSize = 12;
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
            "groupBox" => CreateGroupBox(viewModel),
            "progressBar" => CreateProgressBar(viewModel),
            "sysLink" => CreateSysLink(viewModel),
            "listView" => CreateListView(viewModel),
            "statusBar" => CreateStatusBar(viewModel),
            _ => throw new InvalidOperationException($"Unsupported translated control kind '{viewModel.Kind}'."),
        };

        element.DataContext = viewModel;
        element.IsTabStop = viewModel.Kind == "sysLink" ? false : viewModel.TabStop;
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

    private static ContentControl CreateStatic(ControlNodeViewModel viewModel)
    {
        var text = new TextBlock
        {
            FontSize = NativeFontSize,
            TextWrapping = TextWrapping.Wrap,
            VerticalAlignment = VerticalAlignment.Center,
        };
        Bind(text, TextBlock.TextProperty, nameof(viewModel.Text), BindingMode.OneWay);
        AutomationProperties.SetAccessibilityView(text, AccessibilityView.Raw);
        return new SemanticStaticTextControl
        {
            Content = text,
            MinWidth = 0,
            MinHeight = 0,
            Padding = new Thickness(0),
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch,
            IsHitTestVisible = false,
        };
    }

    private static ContentControl CreateGroupBox(ControlNodeViewModel viewModel)
    {
        var border = new Border
        {
            Margin = new Thickness(0, 8, 0, 0),
            BorderBrush = new SolidColorBrush(Microsoft.UI.Colors.Gray),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(4),
        };
        var caption = new TextBlock
        {
            FontSize = NativeFontSize,
            Margin = new Thickness(10, 0, 0, 0),
            Padding = new Thickness(4, 0, 4, 0),
            VerticalAlignment = VerticalAlignment.Top,
        };
        Bind(caption, TextBlock.TextProperty, nameof(viewModel.Text), BindingMode.OneWay, MnemonicTextConverter);
        var layout = new Grid();
        layout.Children.Add(border);
        layout.Children.Add(caption);
        return new SemanticGroupControl
        {
            Content = layout,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch,
            IsHitTestVisible = false,
        };
    }

    private static ProgressBar CreateProgressBar(ControlNodeViewModel viewModel)
    {
        var control = new ProgressBar { IsIndeterminate = false };
        void ApplyNativeState()
        {
            if (viewModel.Minimum > control.Maximum)
            {
                control.Maximum = viewModel.Maximum;
                control.Minimum = viewModel.Minimum;
            }
            else
            {
                control.Minimum = viewModel.Minimum;
                control.Maximum = viewModel.Maximum;
            }
            control.Value = viewModel.Position;
        }
        ApplyNativeState();
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Minimum) or nameof(viewModel.Maximum) or nameof(viewModel.Position))
                ApplyNativeState();
        };
        return control;
    }

    private Button CreateButton(ControlNodeViewModel viewModel)
    {
        var control = new Button
        {
            FontSize = NativeFontSize,
            MinWidth = 0,
            MinHeight = 0,
            Padding = new Thickness(4, 0, 4, 0),
            HorizontalContentAlignment = HorizontalAlignment.Center,
        };
        Bind(control, ContentControl.ContentProperty, nameof(viewModel.Text), BindingMode.OneWay, MnemonicTextConverter);
        control.Click += (_, _) =>
        {
            if (!_isApplyingCanonical()) _action(viewModel, "invoke", null);
        };
        return control;
    }

    private CheckBox CreateCheckBox(ControlNodeViewModel viewModel, bool threeState)
    {
        var control = new CheckBox
        {
            FontSize = NativeFontSize,
            MinHeight = 0,
            IsThreeState = threeState,
            IsChecked = ToNullableBool(viewModel.Checked),
        };
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
            FontSize = NativeFontSize,
            MinWidth = 0,
            MinHeight = 0,
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
            FontSize = NativeFontSize,
            MinHeight = 0,
            Padding = new Thickness(4, 0, 4, 0),
            AcceptsReturn = AcceptsReturnFor(viewModel),
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
            RendererDiagnostics.Log($"textBox draft changed length={control.Text.Length}");
            timer.Stop();
            timer.Start();
        };
        timer.Tick += (_, _) => CommitDraft();
        control.LostFocus += (_, _) => CommitDraft();
        control.KeyDown += (_, args) =>
        {
            RendererDiagnostics.Log($"textBox keyDown key={args.Key}");
            if (!viewModel.Multiline && args.Key == VirtualKey.Enter) CommitDraft();
        };
        control.GotFocus += (_, _) => RendererDiagnostics.Log("textBox got focus");
        control.Loaded += (_, _) => RendererDiagnostics.Log(
            $"textBox loaded readOnly={viewModel.ReadOnly} multiline={viewModel.Multiline} " +
            $"dialogCode=0x{viewModel.DialogCode:X} enabled={viewModel.Enabled}");
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
        var control = new PasswordBox
        {
            FontSize = NativeFontSize,
            MinHeight = 0,
            Padding = new Thickness(4, 0, 4, 0),
            Password = viewModel.DraftText,
        };
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
        var control = new ComboBox
        {
            FontSize = NativeFontSize,
            MinHeight = 0,
            ItemsSource = viewModel.Items,
            SelectedIndex = viewModel.SelectedIndex,
            IsEditable = viewModel.Editable,
            Text = viewModel.DraftText,
        };
        Bind(control, ComboBox.TextProperty, nameof(viewModel.DraftText), BindingMode.TwoWay);
        var retryTimer = control.DispatcherQueue.CreateTimer();
        retryTimer.Interval = TimeSpan.FromMilliseconds(100);
        retryTimer.IsRepeating = false;
        string? pendingText = null;
        void CommitDraft()
        {
            retryTimer.Stop();
            if (!viewModel.Editable || _isApplyingCanonical() ||
                control.Text == viewModel.Text || control.Text == pendingText) return;
            if (_isImeComposing())
            {
                retryTimer.Start();
                return;
            }
            viewModel.DraftText = control.Text;
            pendingText = control.Text;
            _action(viewModel, "setText", control.Text);
        }
        retryTimer.Tick += (_, _) => CommitDraft();
        control.TextSubmitted += (_, _) => CommitDraft();
        control.LostFocus += (_, _) => CommitDraft();
        control.SelectionChanged += (_, _) =>
        {
            if (!_isApplyingCanonical() && control.SelectedIndex != viewModel.SelectedIndex)
            {
                retryTimer.Stop();
                if (viewModel.Editable && control.SelectedIndex == -1 &&
                    control.Text != viewModel.Text)
                {
                    retryTimer.Start();
                    return;
                }
                _action(viewModel, "select", control.SelectedIndex);
            }
        };
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.SelectedIndex)) control.SelectedIndex = viewModel.SelectedIndex;
            if (args.PropertyName == nameof(viewModel.Editable)) control.IsEditable = viewModel.Editable;
            if (args.PropertyName == nameof(viewModel.DraftText) && control.Text != viewModel.DraftText)
                control.Text = viewModel.DraftText;
            if (args.PropertyName == nameof(viewModel.Text)) pendingText = null;
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
            FontSize = NativeFontSize,
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

    private ContentControl CreateSysLink(ControlNodeViewModel viewModel)
    {
        var text = new RichTextBlock
        {
            FontSize = NativeFontSize,
            TextWrapping = TextWrapping.Wrap,
            VerticalAlignment = VerticalAlignment.Center,
        };
        var control = new SemanticSysLinkControl
        {
            Content = text,
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch,
        };
        void RebuildText()
        {
            if (viewModel.Items.Count != 1 ||
                !TrySplitSysLinkText(viewModel.Text, viewModel.Items[0], out var segments)) return;
            var paragraph = new Paragraph();
            if (segments.Prefix.Length != 0) paragraph.Inlines.Add(new Run { Text = segments.Prefix });
            var link = new Hyperlink
            {
                IsTabStop = viewModel.TabStop,
                TabIndex = Math.Max(0, viewModel.TabIndex),
            };
            link.Inlines.Add(new Run { Text = segments.Label });
            AutomationProperties.SetName(link, segments.Label);
            link.Click += (_, _) =>
            {
                if (!_isApplyingCanonical()) _action(viewModel, "invoke", null);
            };
            paragraph.Inlines.Add(link);
            if (segments.Suffix.Length != 0) paragraph.Inlines.Add(new Run { Text = segments.Suffix });
            text.Blocks.Clear();
            text.Blocks.Add(paragraph);
        }
        RebuildText();
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Text) or nameof(viewModel.Items)) RebuildText();
        };
        return control;
    }

    private ListView CreateListView(ControlNodeViewModel viewModel)
    {
        var itemStyle = new Style(typeof(ListViewItem));
        itemStyle.Setters.Add(new Setter(Control.MinHeightProperty, 22d));
        itemStyle.Setters.Add(new Setter(Control.PaddingProperty, new Thickness(0)));
        itemStyle.Setters.Add(new Setter(Control.HorizontalContentAlignmentProperty, HorizontalAlignment.Left));
        itemStyle.Setters.Add(new Setter(Control.VerticalContentAlignmentProperty, VerticalAlignment.Center));
        var control = new ListView
        {
            ItemContainerStyle = itemStyle,
            SelectionMode = SelectionModeFor(viewModel.MultiSelect),
            HorizontalContentAlignment = HorizontalAlignment.Left,
        };
        var applyingSelection = false;

        Grid BuildCells(IReadOnlyList<string> cells, bool header)
        {
            var grid = new Grid { HorizontalAlignment = HorizontalAlignment.Left };
            for (var index = 0; index < viewModel.Columns.Count; index++)
            {
                grid.ColumnDefinitions.Add(new ColumnDefinition
                {
                    Width = new GridLength(viewModel.ColumnWidths[index] * _scale),
                });
                var content = new TextBlock
                {
                    FontSize = NativeFontSize,
                    Text = cells[index],
                    Margin = new Thickness(6, 0, 6, 0),
                    VerticalAlignment = VerticalAlignment.Center,
                    TextTrimming = TextTrimming.CharacterEllipsis,
                    FontWeight = new global::Windows.UI.Text.FontWeight
                    {
                        Weight = header ? (ushort)600 : (ushort)400,
                    },
                };
                Grid.SetColumn(content, index);
                grid.Children.Add(content);
            }
            AutomationProperties.SetName(grid, string.Join(" ", cells));
            return grid;
        }

        void ApplyCanonicalSelection()
        {
            applyingSelection = true;
            try
            {
                if (viewModel.MultiSelect)
                {
                    control.SelectedItems.Clear();
                    foreach (var index in viewModel.SelectedIndices)
                    {
                        if (index >= 0 && index < control.Items.Count)
                            control.SelectedItems.Add(control.Items[index]);
                    }
                }
                else
                    control.SelectedIndex = viewModel.SelectedIndices.FirstOrDefault(-1);
            }
            finally
            {
                applyingSelection = false;
            }
        }

        void RebuildRows()
        {
            if (!HasRenderableListViewShape(viewModel)) return;
            applyingSelection = true;
            try
            {
                control.Header = BuildCells(viewModel.Columns, header: true);
                control.Items.Clear();
                foreach (var row in viewModel.Rows) control.Items.Add(BuildCells(row, header: false));
            }
            finally
            {
                applyingSelection = false;
            }
            ApplyCanonicalSelection();
        }

        control.SelectionChanged += (_, _) =>
        {
            if (applyingSelection || _isApplyingCanonical()) return;
            var selection = CanonicalSelectionIndices(control.SelectedItems
                .Cast<object>()
                .Select(item => control.Items.IndexOf(item)));
            if (!viewModel.SelectedIndices.SequenceEqual(selection))
                _action(viewModel, "setSelection", selection);
        };
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Columns) or nameof(viewModel.ColumnWidths) or nameof(viewModel.Rows))
                RebuildRows();
            else if (args.PropertyName == nameof(viewModel.MultiSelect))
            {
                control.SelectionMode = SelectionModeFor(viewModel.MultiSelect);
                ApplyCanonicalSelection();
            }
            else if (args.PropertyName == nameof(viewModel.SelectedIndices))
                ApplyCanonicalSelection();
        };
        RebuildRows();
        return control;
    }

    private ContentControl CreateStatusBar(ControlNodeViewModel viewModel)
    {
        var grid = new Grid();
        var control = new SemanticStatusBarControl
        {
            Content = grid,
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch,
        };
        void RebuildParts()
        {
            grid.ColumnDefinitions.Clear();
            grid.Children.Clear();
            var hasWidths = viewModel.ColumnWidths.Count == viewModel.Items.Count;
            for (var index = 0; index < viewModel.Items.Count; index++)
            {
                grid.ColumnDefinitions.Add(new ColumnDefinition
                {
                    Width = hasWidths
                        ? new GridLength(viewModel.ColumnWidths[index] * _scale)
                        : new GridLength(1, GridUnitType.Star),
                });
                var part = new Border
                {
                    BorderBrush = new SolidColorBrush(Microsoft.UI.Colors.Gray),
                    BorderThickness = index + 1 < viewModel.Items.Count
                        ? new Thickness(0, 0, 1, 0)
                        : new Thickness(0),
                    Child = new TextBlock
                    {
                        FontSize = NativeFontSize,
                        Text = viewModel.Items[index],
                        Margin = new Thickness(6, 0, 6, 0),
                        VerticalAlignment = VerticalAlignment.Center,
                        TextTrimming = TextTrimming.CharacterEllipsis,
                    },
                };
                AutomationProperties.SetName(part, viewModel.Items[index]);
                Grid.SetColumn(part, index);
                grid.Children.Add(part);
            }
        }
        RebuildParts();
        control.Loaded += (_, _) => RendererDiagnostics.Log(
            $"statusBar layout requested={viewModel.Rect.Width}x{viewModel.Rect.Height} " +
            $"scale={_scale:F3} owner={control.ActualWidth:F1}x{control.ActualHeight:F1} " +
            $"grid={grid.ActualWidth:F1}x{grid.ActualHeight:F1} parts={viewModel.Items.Count}");
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Items) or nameof(viewModel.ColumnWidths)) RebuildParts();
        };
        return control;
    }

    internal static SysLinkSegments SplitSysLinkText(string text, string label)
    {
        if (!TrySplitSysLinkText(text, label, out var segments))
            throw new ArgumentException("SysLink label must occur exactly once in its text.", nameof(label));
        return segments;
    }

    private static bool TrySplitSysLinkText(string text, string label, out SysLinkSegments segments)
    {
        segments = default;
        if (string.IsNullOrEmpty(label)) return false;
        var start = text.IndexOf(label, StringComparison.Ordinal);
        if (start < 0 || text.IndexOf(label, start + label.Length, StringComparison.Ordinal) >= 0) return false;
        segments = new SysLinkSegments(text[..start], label, text[(start + label.Length)..]);
        return true;
    }

    internal static int[] CanonicalSelectionIndices(IEnumerable<int> indices) =>
        indices.Where(index => index >= 0).Distinct().Order().ToArray();

    internal static ListViewSelectionMode SelectionModeFor(bool multiSelect) =>
        multiSelect ? ListViewSelectionMode.Extended : ListViewSelectionMode.Single;

    internal static bool HasRenderableListViewShape(ControlNodeViewModel viewModel) =>
        viewModel.Columns.Count != 0 &&
        viewModel.ColumnWidths.Count == viewModel.Columns.Count &&
        viewModel.Rows.All(row => row.Count == viewModel.Columns.Count);

    internal static bool AcceptsReturnFor(ControlNodeViewModel viewModel) =>
        viewModel.Multiline && (viewModel.DialogCode & 0x0004u) != 0;

    internal static AutomationControlType AutomationControlTypeFor(string kind) => kind switch
    {
        "static" => AutomationControlType.Text,
        "sysLink" => AutomationControlType.Pane,
        "listView" => AutomationControlType.List,
        "statusBar" => AutomationControlType.StatusBar,
        _ => AutomationControlType.Custom,
    };

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

internal sealed class SemanticStaticTextControl : ContentControl
{
    protected override AutomationPeer OnCreateAutomationPeer() => new SemanticStaticTextAutomationPeer(this);
}

internal sealed class SemanticStaticTextAutomationPeer(SemanticStaticTextControl owner) : FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        ControlFactory.AutomationControlTypeFor("static");

    protected override string GetClassNameCore() => "TextBlock";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => true;
}

internal sealed class SemanticGroupControl : ContentControl
{
    protected override AutomationPeer OnCreateAutomationPeer() => new SemanticGroupAutomationPeer(this);
}

internal sealed class SemanticGroupAutomationPeer(SemanticGroupControl owner) : FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Group;
    protected override string GetClassNameCore() => "GroupBox";
}

internal readonly record struct SysLinkSegments(string Prefix, string Label, string Suffix);

internal sealed class SemanticSysLinkControl : ContentControl
{
    protected override AutomationPeer OnCreateAutomationPeer() => new SemanticSysLinkAutomationPeer(this);
}

internal sealed class SemanticSysLinkAutomationPeer(SemanticSysLinkControl owner) : FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        ControlFactory.AutomationControlTypeFor("sysLink");

    protected override string GetClassNameCore() => "SysLink";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
}

internal sealed class SemanticStatusBarControl : ContentControl
{
    protected override AutomationPeer OnCreateAutomationPeer() => new SemanticStatusBarAutomationPeer(this);
}

internal sealed class SemanticStatusBarAutomationPeer(SemanticStatusBarControl owner) : FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        ControlFactory.AutomationControlTypeFor("statusBar");

    protected override string GetClassNameCore() => "StatusBar";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
    protected override global::Windows.Foundation.Rect GetBoundingRectangleCore()
    {
        var reported = base.GetBoundingRectangleCore();
        if (owner.ActualWidth <= 0 || owner.ActualHeight <= 0 || reported.Width <= 0)
            return reported;
        var scale = reported.Width / owner.ActualWidth;
        return new global::Windows.Foundation.Rect(
            reported.X, reported.Y, reported.Width, owner.ActualHeight * scale);
    }
}
