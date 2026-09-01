using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Runtime;
using FluentShell.Renderer.Protocol;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Microsoft.UI.Xaml.Automation.Provider;
using System.Runtime.InteropServices.WindowsRuntime;
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
    private readonly IReadOnlyDictionary<string, ControlNodeViewModel> _nodes;

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
        var nodeArray = nodes.ToArray();
        _radioGroups = BuildRadioGroups(nodeArray);
        _nodes = nodeArray.ToDictionary(node => node.NodeId, StringComparer.Ordinal);
    }

    public FrameworkElement Create(ControlNodeViewModel viewModel)
    {
        FrameworkElement element = viewModel.Kind switch
        {
            "static" => CreateStatic(viewModel),
            "staticIcon" => CreateStaticIcon(viewModel),
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
            "tabControl" => CreateTabControl(viewModel),
            "dialogContainer" => new SemanticDialogContainer(),
            "statusBar" => CreateStatusBar(viewModel),
            "toolbar" => CreateToolbar(viewModel),
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
        if (viewModel.ParentNodeId is { } parentNodeId && _nodes.TryGetValue(parentNodeId, out var parent))
        {
            parent.PropertyChanged += (_, args) =>
            {
                if (args.PropertyName == nameof(parent.Rect)) ApplyBounds(element, viewModel);
            };
        }
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

    private static Image CreateStaticIcon(ControlNodeViewModel viewModel)
    {
        var image = new Image
        {
            // The HICON has already resolved native resource sizing. Do not
            // resample it merely because the Static HWND bounds are larger.
            Stretch = Stretch.None,
            IsHitTestVisible = false,
        };
        void ApplyPixels()
        {
            var pixels = DecodeImagePixels(viewModel);
            var bitmap = new WriteableBitmap(viewModel.ImageWidth, viewModel.ImageHeight);
            using var stream = bitmap.PixelBuffer.AsStream();
            stream.Write(pixels, 0, pixels.Length);
            bitmap.Invalidate();
            image.Source = bitmap;
        }
        ApplyPixels();
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.ImageData)) ApplyPixels();
        };
        return image;
    }

    internal static byte[] DecodeImagePixels(ControlNodeViewModel viewModel)
    {
        if (viewModel.ImageFormat != "bgra8-premultiplied" ||
            viewModel.ImageWidth is <= 0 or > ProtocolConstants.MaxImageDimension ||
            viewModel.ImageHeight is <= 0 or > ProtocolConstants.MaxImageDimension)
            throw new InvalidOperationException("Validated Static icon metadata is unavailable.");
        var pixels = Convert.FromBase64String(viewModel.ImageData);
        if (pixels.Length != checked(viewModel.ImageWidth * viewModel.ImageHeight * 4))
            throw new InvalidOperationException("Validated Static icon pixel count changed.");
        return pixels;
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

    private static SemanticProgressBarControl CreateProgressBar(ControlNodeViewModel viewModel)
    {
        var control = new SemanticProgressBarControl
            { IsIndeterminate = viewModel.Indeterminate };
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
            if (args.PropertyName == nameof(viewModel.Indeterminate))
                control.IsIndeterminate = viewModel.Indeterminate;
            else if (args.PropertyName is nameof(viewModel.Minimum) or nameof(viewModel.Maximum) or nameof(viewModel.Position))
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
        if (viewModel.PresentationVariant == "commandLink")
        {
            var content = new StackPanel { Orientation = Orientation.Vertical };
            content.Children.Add(new TextBlock
            {
                Text = Win32Mnemonic.DisplayText(viewModel.Text),
                FontWeight = new global::Windows.UI.Text.FontWeight { Weight = 600 },
                TextWrapping = TextWrapping.Wrap,
            });
            if (!string.IsNullOrEmpty(viewModel.HelpText))
            {
                content.Children.Add(new TextBlock
                {
                    Text = viewModel.HelpText,
                    Opacity = 0.75,
                    TextWrapping = TextWrapping.Wrap,
                });
            }
            control.Content = content;
            control.HorizontalContentAlignment = HorizontalAlignment.Left;
            control.Padding = new Thickness(10, 4, 10, 4);
        }
        else
        {
            Bind(control, ContentControl.ContentProperty, nameof(viewModel.Text), BindingMode.OneWay, MnemonicTextConverter);
        }
        control.Click += (_, _) =>
        {
            // Adapter-declared slots are the only interactive ones; a projected
            // but inert slot (a disabled native back button) never emits.
            if (!_isApplyingCanonical() &&
                AllowsAction(viewModel, "invoke"))
                _action(viewModel, "invoke", null);
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
            if (!_isApplyingCanonical() && value != viewModel.Checked &&
                AllowsAction(viewModel, "setCheck"))
            {
                RendererDiagnostics.Log($"checkBox action value={value} canonical={viewModel.Checked} semantic={viewModel.SemanticKey}");
                _action(viewModel, "setCheck", value);
            }
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

    internal static bool AllowsAction(ControlNodeViewModel viewModel, string action) =>
        string.IsNullOrEmpty(viewModel.AdapterId) || viewModel.SupportedActions.Contains(action);

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
        var rowChecks = new List<CheckBox>();

        Grid BuildCells(IReadOnlyList<string> cells, bool header, int rowIndex = -1)
        {
            var grid = new Grid { HorizontalAlignment = HorizontalAlignment.Left };
            for (var index = 0; index < viewModel.Columns.Count; index++)
            {
                grid.ColumnDefinitions.Add(new ColumnDefinition
                {
                    Width = new GridLength(viewModel.ColumnWidths[index] * _scale),
                });
                var text = new TextBlock
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
                FrameworkElement content = text;
                if (!header && index == 0 && viewModel.CheckBoxes)
                {
                    var checkBox = new CheckBox
                    {
                        IsChecked = viewModel.CheckedIndices.Contains(rowIndex),
                        IsTabStop = false,
                        MinWidth = 0,
                        MinHeight = 0,
                        Padding = new Thickness(0),
                        VerticalAlignment = VerticalAlignment.Center,
                    };
                    AutomationProperties.SetName(checkBox, string.Join(" ", cells));
                    RoutedEventHandler changed = (_, _) =>
                    {
                        if (applyingSelection || _isApplyingCanonical()) return;
                        var requested = checkBox.IsChecked == true;
                        if (viewModel.CheckedIndices.Contains(rowIndex) != requested)
                        {
                            _action(viewModel, "setItemCheck", new ListViewCheckActionValue
                            {
                                Index = rowIndex,
                                Checked = requested,
                            });
                        }
                    };
                    checkBox.Checked += changed;
                    checkBox.Unchecked += changed;
                    rowChecks.Add(checkBox);
                    var cell = new StackPanel
                    {
                        Orientation = Orientation.Horizontal,
                        VerticalAlignment = VerticalAlignment.Center,
                    };
                    cell.Children.Add(checkBox);
                    cell.Children.Add(text);
                    content = cell;
                }
                Grid.SetColumn(content, index);
                grid.Children.Add(content);
            }
            AutomationProperties.SetName(grid, string.Join(" ", cells));
            return grid;
        }

        void ApplyCanonicalChecks()
        {
            applyingSelection = true;
            try
            {
                for (var index = 0; index < rowChecks.Count; ++index)
                    rowChecks[index].IsChecked = viewModel.CheckedIndices.Contains(index);
            }
            finally
            {
                applyingSelection = false;
            }
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
                control.Header = ShouldRenderListViewHeader(viewModel)
                    ? BuildCells(viewModel.Columns, header: true)
                    : null;
                control.Items.Clear();
                rowChecks.Clear();
                for (var index = 0; index < viewModel.Rows.Count; ++index)
                    control.Items.Add(BuildCells(viewModel.Rows[index], header: false, index));
            }
            finally
            {
                applyingSelection = false;
            }
            ApplyCanonicalSelection();
            ApplyCanonicalChecks();
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
            if (args.PropertyName is nameof(viewModel.Columns) or nameof(viewModel.ColumnWidths) or
                nameof(viewModel.Rows) or nameof(viewModel.ColumnHeadersVisible) or
                nameof(viewModel.CheckBoxes))
                RebuildRows();
            else if (args.PropertyName == nameof(viewModel.MultiSelect))
            {
                control.SelectionMode = SelectionModeFor(viewModel.MultiSelect);
                ApplyCanonicalSelection();
            }
            else if (args.PropertyName == nameof(viewModel.SelectedIndices))
                ApplyCanonicalSelection();
            else if (args.PropertyName == nameof(viewModel.CheckedIndices))
                ApplyCanonicalChecks();
        };
        control.KeyDown += (_, args) =>
        {
            if (!viewModel.CheckBoxes || args.Key != VirtualKey.Space ||
                control.SelectedIndex is < 0 || control.SelectedIndex >= rowChecks.Count) return;
            rowChecks[control.SelectedIndex].IsChecked =
                rowChecks[control.SelectedIndex].IsChecked != true;
            args.Handled = true;
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

    private SemanticToolbarControl CreateToolbar(ControlNodeViewModel viewModel)
    {
        var control = new SemanticToolbarControl();
        void Rebuild()
        {
            control.ClearItems();
            foreach (var item in viewModel.ToolbarItems)
            {
                FrameworkElement element;
                if (item.Kind == "separator")
                {
                    element = new AppBarSeparator
                    {
                        IsHitTestVisible = false,
                        IsTabStop = false,
                    };
                }
                else
                {
                    var image = new Image
                    {
                        Source = BitmapForToolbarItem(item),
                        Stretch = Stretch.None,
                    };
                    var button = new Button
                    {
                        Content = image,
                        MinWidth = 0,
                        MinHeight = 0,
                        Padding = new Thickness(0),
                        IsEnabled = item.Enabled,
                        IsTabStop = item.Enabled && !item.Hidden,
                    };
                    AutomationProperties.SetName(button, Win32Mnemonic.DisplayText(item.Text));
                    var commandId = item.CommandId;
                    button.Click += (_, _) =>
                    {
                        if (!_isApplyingCanonical()) _action(viewModel, "toolbarCommand", commandId);
                    };
                    element = button;
                }
                element.Visibility = item.Hidden ? Visibility.Collapsed : Visibility.Visible;
                Canvas.SetLeft(element, item.Rect.X * _scale);
                Canvas.SetTop(element, item.Rect.Y * _scale);
                element.Width = item.Rect.Width * _scale;
                element.Height = item.Rect.Height * _scale;
                control.AddItem(element);
            }
        }
        Rebuild();
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.ToolbarItems)) Rebuild();
        };
        return control;
    }

    private static WriteableBitmap BitmapForToolbarItem(ToolbarItemSnapshot item)
    {
        if (item.ImageWidth is not { } width || item.ImageHeight is not { } height ||
            item.ImageFormat != "bgra8-premultiplied" || item.ImageData is null)
            throw new InvalidOperationException("Validated Toolbar icon metadata is unavailable.");
        var pixels = Convert.FromBase64String(item.ImageData);
        if (pixels.Length != checked(width * height * 4))
            throw new InvalidOperationException("Validated Toolbar icon pixel count changed.");
        var bitmap = new WriteableBitmap(width, height);
        using var stream = bitmap.PixelBuffer.AsStream();
        stream.Write(pixels, 0, pixels.Length);
        bitmap.Invalidate();
        return bitmap;
    }

    private SemanticTabControl CreateTabControl(ControlNodeViewModel viewModel)
    {
        var control = new SemanticTabControl(index =>
        {
            if (!_isApplyingCanonical() && index != viewModel.SelectedIndex)
                _action(viewModel, "select", index);
        });

        void RebuildHeaders()
        {
            if (viewModel.Items.Count == viewModel.ItemRects.Count)
                control.Rebuild(viewModel.Items, viewModel.ItemRects, viewModel.SelectedIndex, _scale);
        }
        RebuildHeaders();
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Items) or nameof(viewModel.ItemRects))
                RebuildHeaders();
            else if (args.PropertyName == nameof(viewModel.SelectedIndex))
                control.ApplySelection(viewModel.SelectedIndex);
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

    internal static bool ShouldRenderListViewHeader(ControlNodeViewModel viewModel) =>
        viewModel.ColumnHeadersVisible;

    internal static bool IsListViewRowChecked(ControlNodeViewModel viewModel, int index) =>
        viewModel.CheckBoxes && viewModel.CheckedIndices.Contains(index);

    internal static bool HasRenderableTabShape(ControlNodeViewModel viewModel) =>
        viewModel.Items.Count is > 0 and <= ProtocolConstants.MaxTabItems &&
        viewModel.ItemRects.Count == viewModel.Items.Count &&
        viewModel.SelectedIndex >= 0 && viewModel.SelectedIndex < viewModel.Items.Count;

    internal static IReadOnlyList<TabHeaderRow> GroupTabHeaderRows(IReadOnlyList<PixelRect> rects)
    {
        var rows = new List<TabHeaderRow>();
        foreach (var group in rects
                     .Select((rect, index) => new TabHeaderPlacement(index, rect))
                     .GroupBy(item => item.Rect.Y)
                     .OrderBy(group => group.Key))
        {
            var items = group.OrderBy(item => item.Rect.X).ToArray();
            var height = items[0].Rect.Height;
            if (items.Any(item => item.Rect.Height != height))
                throw new ArgumentException("Tab items sharing a row must have identical vertical geometry.", nameof(rects));
            for (var index = 1; index < items.Length; ++index)
            {
                if ((long)items[index - 1].Rect.X + items[index - 1].Rect.Width > items[index].Rect.X)
                    throw new ArgumentException("Tab items in a row must be ordered without overlap.", nameof(rects));
            }
            var left = items[0].Rect.X;
            var right = items.Max(item => (long)item.Rect.X + item.Rect.Width);
            rows.Add(new TabHeaderRow(
                new PixelRect { X = left, Y = group.Key, Width = checked((int)(right - left)), Height = height },
                items));
        }
        for (var index = 1; index < rows.Count; ++index)
        {
            if ((long)rows[index - 1].Bounds.Y + rows[index - 1].Bounds.Height > rows[index].Bounds.Y)
                throw new ArgumentException("Tab header row bands must be ordered without overlap.", nameof(rects));
        }
        return rows;
    }

    internal static AutomationControlType TabItemAutomationControlType() => AutomationControlType.TabItem;

    internal static int LocalTabSelectionIndex(TabHeaderRow row, int selectedIndex)
    {
        for (var index = 0; index < row.Items.Count; ++index)
        {
            if (row.Items[index].Index == selectedIndex) return index;
        }
        return -1;
    }

    internal static bool AcceptsReturnFor(ControlNodeViewModel viewModel) =>
        viewModel.Multiline && (viewModel.DialogCode & 0x0004u) != 0;

    internal static AutomationControlType AutomationControlTypeFor(string kind) => kind switch
    {
        "static" => AutomationControlType.Text,
        "staticIcon" => AutomationControlType.Image,
        "sysLink" => AutomationControlType.Pane,
        "listView" => AutomationControlType.List,
        "tabControl" => AutomationControlType.Tab,
        "dialogContainer" => AutomationControlType.Pane,
        "statusBar" => AutomationControlType.StatusBar,
        "toolbar" => AutomationControlType.ToolBar,
        _ => AutomationControlType.Custom,
    };

    private void ApplyBounds(FrameworkElement element, ControlNodeViewModel viewModel)
    {
        var rect = RelativeRectFor(viewModel, _nodes);
        Canvas.SetLeft(element, rect.X * _scale);
        Canvas.SetTop(element, rect.Y * _scale);
        element.Width = Math.Max(0, viewModel.Rect.Width * _scale);
        element.Height = Math.Max(0, viewModel.Rect.Height * _scale);
        if (viewModel.Kind is "dialogContainer" or "tabControl" or "toolbar")
        {
            element.Clip = new RectangleGeometry
            {
                Rect = new global::Windows.Foundation.Rect(0, 0, element.Width, element.Height),
            };
        }
    }

    internal static PixelRect RelativeRectFor(
        ControlNodeViewModel node,
        IReadOnlyDictionary<string, ControlNodeViewModel> nodes)
    {
        if (node.ParentNodeId is null || !nodes.TryGetValue(node.ParentNodeId, out var parent))
            return node.Rect;
        return node.Rect with { X = node.Rect.X - parent.Rect.X, Y = node.Rect.Y - parent.Rect.Y };
    }

    private static void ApplyAutomationAndAccessKey(
        FrameworkElement element,
        ControlNodeViewModel viewModel)
    {
        AutomationProperties.SetName(element, AutomationNameFor(viewModel));
        AutomationProperties.SetAutomationId(
            element, $"FluentShell.Node.{viewModel.NodeId}.{viewModel.Generation}");
        AutomationProperties.SetHelpText(element, viewModel.HelpText);
        AutomationProperties.SetAccessKey(element, viewModel.AccessKey);
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

internal sealed class SemanticProgressBarControl : ContentControl
{
    private readonly ProgressBar _progress = new();

    public SemanticProgressBarControl()
    {
        IsTabStop = false;
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        VerticalContentAlignment = VerticalAlignment.Stretch;
        Content = _progress;
        AutomationProperties.SetAccessibilityView(_progress, AccessibilityView.Raw);
    }

    public double Minimum { get => _progress.Minimum; set => _progress.Minimum = value; }
    public double Maximum { get => _progress.Maximum; set => _progress.Maximum = value; }
    public double Value { get => _progress.Value; set => _progress.Value = value; }
    public bool IsIndeterminate
        { get => _progress.IsIndeterminate; set => _progress.IsIndeterminate = value; }

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticProgressBarAutomationPeer(this);
}

internal sealed class SemanticProgressBarAutomationPeer(SemanticProgressBarControl owner) :
    FrameworkElementAutomationPeer(owner), IRangeValueProvider
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        AutomationControlType.ProgressBar;
    protected override string GetClassNameCore() => "ProgressBar";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => true;
    protected override object GetPatternCore(PatternInterface patternInterface) =>
        patternInterface == PatternInterface.RangeValue && !owner.IsIndeterminate
            ? this : base.GetPatternCore(patternInterface);
    protected override IList<AutomationPeer> GetChildrenCore() => [];
    protected override global::Windows.Foundation.Rect GetBoundingRectangleCore()
    {
        var reported = base.GetBoundingRectangleCore();
        if (owner.ActualWidth <= 0 || owner.ActualHeight <= 0 || reported.Width <= 0)
            return reported;
        var scale = reported.Width / owner.ActualWidth;
        var semanticHeight = owner.ActualHeight * scale;
        return new global::Windows.Foundation.Rect(
            reported.X,
            reported.Y - (semanticHeight - reported.Height) / 2,
            reported.Width,
            semanticHeight);
    }

    public bool IsReadOnly => true;
    public double LargeChange => 0;
    public double Maximum => owner.Maximum;
    public double Minimum => owner.Minimum;
    public double SmallChange => 0;
    public double Value => owner.Value;
    public void SetValue(double value) =>
        throw new InvalidOperationException("Projected ProgressBar state is read-only.");
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

internal sealed class SemanticTabControl : ContentControl
{
    private readonly Action<int> _selectionRequested;
    private readonly List<TabViewItem> _headers = [];
    private readonly List<TabView> _rows = [];
    private readonly Dictionary<TabView, TabHeaderRow> _rowGeometry = [];
    private readonly Dictionary<TabViewItem, int> _indices = [];
    private readonly Canvas _headerCanvas = new() { Background = null };
    private int _selectedIndex = -1;
    private bool _applyingSelection;

    public SemanticTabControl(Action<int> selectionRequested)
    {
        _selectionRequested = selectionRequested;
        Content = _headerCanvas;
        Background = null;
        BorderThickness = new Thickness(0);
        Padding = new Thickness(0);
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        VerticalContentAlignment = VerticalAlignment.Stretch;
        GotFocus += (_, _) =>
        {
            if (_selectedIndex >= 0 && _selectedIndex < _headers.Count &&
                !_headers[_selectedIndex].FocusState.Equals(FocusState.Keyboard))
                _headers[_selectedIndex].Focus(FocusState.Keyboard);
        };
    }

    public IReadOnlyList<TabViewItem> Headers => _headers;
    public IReadOnlyList<TabView> Rows => _rows;
    public UIElementCollection Children => _headerCanvas.Children;
    public int SelectedIndex => _selectedIndex;
    public global::Windows.Foundation.Rect HeaderUnionBounds { get; private set; }

    internal static TabViewPolicy Policy { get; } = new(
        IsAddTabButtonVisible: false,
        CanDragTabs: false,
        CanReorderTabs: false,
        AreItemsClosable: false,
        TabWidthMode: TabViewWidthMode.SizeToContent);

    public void Rebuild(
        IReadOnlyList<string> labels,
        IReadOnlyList<PixelRect> rects,
        int selectedIndex,
        double scale)
    {
        Children.Clear();
        _headers.Clear();
        _rows.Clear();
        _rowGeometry.Clear();
        _indices.Clear();
        if (labels.Count != rects.Count)
            throw new ArgumentException("Tab labels and rectangles must have matching counts.", nameof(rects));
        var unionLeft = rects.Min(rect => rect.X);
        var unionTop = rects.Min(rect => rect.Y);
        var unionRight = rects.Max(rect => rect.X + rect.Width);
        var unionBottom = rects.Max(rect => rect.Y + rect.Height);
        HeaderUnionBounds = new global::Windows.Foundation.Rect(
            unionLeft * scale,
            unionTop * scale,
            (unionRight - unionLeft) * scale,
            (unionBottom - unionTop) * scale);
        _headers.AddRange(Enumerable.Repeat<TabViewItem>(null!, labels.Count));
        foreach (var row in ControlFactory.GroupTabHeaderRows(rects))
        {
            var tabView = new TabView
            {
                IsAddTabButtonVisible = Policy.IsAddTabButtonVisible,
                CanDragTabs = Policy.CanDragTabs,
                CanReorderTabs = Policy.CanReorderTabs,
                TabWidthMode = Policy.TabWidthMode,
                Background = null,
                MinWidth = 0,
                MinHeight = 0,
                Padding = new Thickness(0),
                Width = row.Bounds.Width * scale,
                Height = row.Bounds.Height * scale,
            };
            var previousRight = row.Bounds.X;
            foreach (var placement in row.Items)
            {
                var header = new TabViewItem
                {
                    Header = Win32Mnemonic.DisplayText(labels[placement.Index]),
                    IsClosable = Policy.AreItemsClosable,
                    FontSize = NativeTabFontSize,
                    MinWidth = 0,
                    MinHeight = 0,
                    Padding = new Thickness(4, 0, 4, 0),
                    HorizontalContentAlignment = HorizontalAlignment.Center,
                    VerticalContentAlignment = VerticalAlignment.Center,
                    Width = placement.Rect.Width * scale,
                    Height = placement.Rect.Height * scale,
                    Margin = new Thickness((placement.Rect.X - previousRight) * scale, 0, 0, 0),
                };
                AutomationProperties.SetName(header, Win32Mnemonic.DisplayText(labels[placement.Index]));
                tabView.TabItems.Add(header);
                _headers[placement.Index] = header;
                _indices.Add(header, placement.Index);
                previousRight = placement.Rect.X + placement.Rect.Width;
            }
            tabView.SelectionChanged += OnRowSelectionChanged;
            Canvas.SetLeft(tabView, row.Bounds.X * scale);
            Canvas.SetTop(tabView, row.Bounds.Y * scale);
            Children.Add(tabView);
            _rows.Add(tabView);
            _rowGeometry.Add(tabView, row);
        }
        ApplySelection(selectedIndex);
    }

    public void ApplySelection(int selectedIndex)
    {
        _applyingSelection = true;
        try
        {
            _selectedIndex = selectedIndex;
            foreach (var row in _rows)
                row.SelectedIndex = ControlFactory.LocalTabSelectionIndex(_rowGeometry[row], selectedIndex);
        }
        finally { _applyingSelection = false; }
    }

    public void RequestSelection(int index)
    {
        if (index >= 0 && index < _headers.Count) _selectionRequested(index);
    }

    private void OnRowSelectionChanged(object sender, SelectionChangedEventArgs args)
    {
        if (_applyingSelection || sender is not TabView selectedRow ||
            selectedRow.SelectedItem is not TabViewItem selected || !_indices.TryGetValue(selected, out var index))
            return;
        _applyingSelection = true;
        try
        {
            _selectedIndex = index;
            foreach (var row in _rows)
            {
                if (!ReferenceEquals(row, selectedRow)) row.SelectedIndex = -1;
            }
        }
        finally { _applyingSelection = false; }
        RequestSelection(index);
    }

    protected override AutomationPeer OnCreateAutomationPeer() => new SemanticTabControlAutomationPeer(this);

    private const double NativeTabFontSize = 12;
}

internal sealed class SemanticTabControlAutomationPeer(SemanticTabControl owner) :
    FrameworkElementAutomationPeer(owner), ISelectionProvider
{
    protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Tab;
    protected override string GetClassNameCore() => "TabControl";
    protected override bool IsControlElementCore() => true;
    protected override object GetPatternCore(PatternInterface patternInterface) =>
        patternInterface == PatternInterface.Selection ? this : base.GetPatternCore(patternInterface);
    protected override IList<AutomationPeer> GetChildrenCore() => owner.Headers
        .Select(CreatePeerForElement)
        .Where(peer => peer is not null)
        .Cast<AutomationPeer>()
        .ToList();
    protected override global::Windows.Foundation.Rect GetBoundingRectangleCore()
    {
        var reported = base.GetBoundingRectangleCore();
        return ExpandHeaderUnionBounds(
            reported, owner.HeaderUnionBounds, owner.ActualWidth, owner.ActualHeight);
    }

    internal static global::Windows.Foundation.Rect ExpandHeaderUnionBounds(
        global::Windows.Foundation.Rect reported,
        global::Windows.Foundation.Rect headerBounds,
        double ownerWidth,
        double ownerHeight)
    {
        if (reported.Width <= 0 || headerBounds.Width <= 0 || ownerWidth <= 0 || ownerHeight <= 0)
            return reported;
        var scale = reported.Width / headerBounds.Width;
        return new global::Windows.Foundation.Rect(
            reported.X - headerBounds.X * scale,
            reported.Y - headerBounds.Y * scale,
            ownerWidth * scale,
            ownerHeight * scale);
    }

    public bool CanSelectMultiple => false;
    public bool IsSelectionRequired => true;
    public IRawElementProviderSimple[] GetSelection()
    {
        if (owner.SelectedIndex < 0 || owner.SelectedIndex >= owner.Headers.Count) return [];
        var peer = CreatePeerForElement(owner.Headers[owner.SelectedIndex]);
        return peer is null ? [] : [ProviderFromPeer(peer)];
    }
}

internal readonly record struct TabHeaderPlacement(int Index, PixelRect Rect);
internal readonly record struct TabHeaderRow(PixelRect Bounds, IReadOnlyList<TabHeaderPlacement> Items);
internal readonly record struct TabViewPolicy(
    bool IsAddTabButtonVisible,
    bool CanDragTabs,
    bool CanReorderTabs,
    bool AreItemsClosable,
    TabViewWidthMode TabWidthMode);

internal sealed class SemanticDialogContainer : Canvas
{
    public SemanticDialogContainer()
    {
        IsTabStop = false;
    }

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticDialogContainerAutomationPeer(this);
}

internal sealed class SemanticDialogContainerAutomationPeer(SemanticDialogContainer owner) : FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Pane;
    protected override string GetClassNameCore() => "DialogContainer";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => false;
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

internal sealed class SemanticToolbarControl : ContentControl
{
    private readonly Canvas _canvas = new() { Background = null };
    private readonly List<FrameworkElement> _items = [];

    public SemanticToolbarControl()
    {
        Content = _canvas;
        Padding = new Thickness(0);
        BorderThickness = new Thickness(0);
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        VerticalContentAlignment = VerticalAlignment.Stretch;
    }

    public IReadOnlyList<FrameworkElement> Items => _items;

    public void ClearItems()
    {
        _canvas.Children.Clear();
        _items.Clear();
    }

    public void AddItem(FrameworkElement item)
    {
        _items.Add(item);
        _canvas.Children.Add(item);
    }

    protected override AutomationPeer OnCreateAutomationPeer() => new SemanticToolbarAutomationPeer(this);
}

internal sealed class SemanticToolbarAutomationPeer(SemanticToolbarControl owner) : FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.ToolBar;
    protected override string GetClassNameCore() => "ToolBar";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => false;
    protected override IList<AutomationPeer> GetChildrenCore() => owner.Items
        .Where(item => item.Visibility == Visibility.Visible)
        .Select(CreatePeerForElement)
        .Where(peer => peer is not null)
        .Cast<AutomationPeer>()
        .ToList();
}
