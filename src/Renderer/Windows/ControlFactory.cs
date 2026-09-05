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
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Input;
using System.Diagnostics;
using System.Runtime.InteropServices.WindowsRuntime;
using Windows.Foundation;
using Windows.System;

namespace FluentShell.Renderer.Windows;

internal sealed class ControlFactory
{
    private const double NativeFontSize = 12;
    // The floor for a label fitted into the width the native control committed to.  A
    // menu bar rendered a fraction smaller is readable; one that reads "Favorit" is not.
    private const double MinimumFittedFontSize = 9;
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
            "radioButton" => viewModel.PresentationVariant == "bitmapSwitch"
                ? CreateBitmapSwitch(viewModel) : CreateRadioButton(viewModel),
            "edit" => CreateTextBox(viewModel),
            "password" => CreatePasswordBox(viewModel),
            "comboBox" => CreateComboBox(viewModel),
            "listBox" => CreateListBox(viewModel),
            "groupBox" => CreateGroupBox(viewModel),
            "progressBar" => CreateProgressBar(viewModel),
            "sysLink" => CreateSysLink(viewModel),
            "listView" => CreateListView(viewModel),
            "treeView" => CreateTreeView(viewModel),
            "tabControl" => CreateTabControl(viewModel),
            "slider" => CreateSlider(viewModel),
            "dialogContainer" => new SemanticDialogContainer(),
            "mdiClient" => new SemanticMdiClient(),
            "paneContainer" => CreatePaneContainer(viewModel),
            "accessibleIsland" => CreateAccessibleIsland(viewModel),
            "mdiChild" => CreateMdiChild(viewModel),
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
        // Native child enumeration is front to back, so the first node of a sibling
        // group is the one in front.  That only carries meaning where siblings
        // genuinely overlap and the order is semantic: MDI children, whose front
        // one is the activated window.  Every other sibling group keeps the
        // enumeration order it was captured in.
        Canvas.SetZIndex(element, viewModel.Kind == "mdiChild"
            ? -viewModel.ZIndex
            : viewModel.ZIndex);
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
        // A Static's label carries the same `&` mnemonic markup a Button's does: the
        // native control draws the letter underlined, not the ampersand.  Projecting the
        // raw string shows "&Number to dial:" where the application shows "Number to
        // dial:", and makes the label wider than the slot the control sized for it.
        Bind(text, TextBlock.TextProperty, nameof(viewModel.Text), BindingMode.OneWay,
            MnemonicTextConverter);
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

    // A Static icon is projected as a bare Image unless its variant needs the
    // stretched semantic wrapper. The wrapper republishes the node through its own
    // peer; the bare Image *is* the node, so the two cases differ in whether the
    // Image itself may leave the UIA control view.
    internal static bool StaticIconUsesSemanticWrapper(string presentationVariant) =>
        presentationVariant is "bitmapDisplay" or "monitorPalette";

    private static FrameworkElement CreateStaticIcon(ControlNodeViewModel viewModel)
    {
        var wrapped = StaticIconUsesSemanticWrapper(viewModel.PresentationVariant);
        var image = new Image
        {
            Stretch = wrapped ? Stretch.Fill : Stretch.None,
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
        // The unwrapped variant projects this Image as the node element itself, so it
        // must stay in the UIA control view: the Bridge's committed gate enumerates
        // with the control-view condition and a Raw element is simply absent there.
        // Only the wrapped variants may hide their inner Image, because the wrapper's
        // own peer republishes the node.
        if (!wrapped) return image;
        AutomationProperties.SetAccessibilityView(image, AccessibilityView.Raw);
        return new SemanticStaticIconControl
        {
            Content = image,
            MinWidth = 0,
            MinHeight = 0,
            Padding = new Thickness(0),
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch,
            IsHitTestVisible = false,
        };
    }

    internal static byte[] DecodeImagePixels(ControlNodeViewModel viewModel)
    {
        var maxDimension = viewModel.PresentationVariant is "bitmapDisplay" or "bitmapSwitch"
            or "monitorPalette"
            ? ProtocolConstants.MaxDirectUiBitmapDimension
            : ProtocolConstants.MaxImageDimension;
        if (viewModel.ImageFormat != "bgra8-premultiplied" ||
            viewModel.ImageWidth is <= 0 || viewModel.ImageHeight is <= 0 ||
            viewModel.ImageWidth > maxDimension || viewModel.ImageHeight > maxDimension)
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
            if (!_isApplyingCanonical() && viewModel.Checked != 1 &&
                AllowsAction(viewModel, "setCheck"))
                _action(viewModel, "setCheck", 1);
        };
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.Checked)) control.IsChecked = viewModel.Checked == 1;
        };
        return control;
    }

    private FrameworkElement CreateBitmapSwitch(ControlNodeViewModel viewModel)
    {
        var image = new Image
        {
            Stretch = Stretch.Fill,
            IsHitTestVisible = false,
        };
        AutomationProperties.SetAccessibilityView(image, AccessibilityView.Raw);
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
        var control = new SemanticBitmapSwitchControl
        {
            Content = image,
            MinWidth = 0,
            MinHeight = 0,
            Padding = new Thickness(0),
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch,
            GroupName = _radioGroups[viewModel.NodeId],
            IsChecked = viewModel.Checked == 1,
        };
        control.Checked += (_, _) =>
        {
            if (!_isApplyingCanonical() && viewModel.Checked != 1 &&
                AllowsAction(viewModel, "setCheck"))
                _action(viewModel, "setCheck", 1);
        };
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.Checked))
                control.IsChecked = viewModel.Checked == 1;
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
            if (_isApplyingCanonical() || !AllowsAction(viewModel, "setText") ||
                control.Text == viewModel.Text || control.Text == pendingText) return;
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
            if (!_isApplyingCanonical() && AllowsAction(viewModel, "setText") &&
                control.Password != viewModel.Text) _action(viewModel, "setText", control.Password);
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
            if (!viewModel.Editable || _isApplyingCanonical() || !AllowsAction(viewModel, "setText") ||
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
            if (!_isApplyingCanonical() && AllowsAction(viewModel, "select") &&
                control.SelectedIndex != viewModel.SelectedIndex)
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
            if (!_isApplyingCanonical() && AllowsAction(viewModel, "select") &&
                control.SelectedIndex != viewModel.SelectedIndex) _action(viewModel, "select", control.SelectedIndex);
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
                if (!_isApplyingCanonical() && AllowsAction(viewModel, "invoke")) _action(viewModel, "invoke", null);
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
        var rowContents = new List<ProjectedItemContent>();

        // Columns travel in the application's logical order and the header shows them
        // in its display order, so the projection permutes for presentation only.
        IReadOnlyList<int> DisplayOrder()
        {
            var order = viewModel.ColumnOrder;
            if (order.Count == viewModel.Columns.Count && order.Count != 0) return order;
            return Enumerable.Range(0, viewModel.Columns.Count).ToArray();
        }

        // A header drag reorders the columns for real: the drop position is turned
        // into a permutation and the native header applies it.
        void RequestColumnMove(int logical, double dropX)
        {
            var order = DisplayOrder().ToList();
            var from = order.IndexOf(logical);
            if (from < 0 || order.Count < 2) return;
            var edge = 0.0;
            var to = order.Count - 1;
            for (var position = 0; position < order.Count; ++position)
            {
                var width = viewModel.ColumnWidths[order[position]] * _scale;
                if (dropX < edge + width / 2)
                {
                    to = position;
                    break;
                }
                edge += width;
            }
            if (to == from) return;
            order.RemoveAt(from);
            order.Insert(Math.Clamp(to, 0, order.Count), logical);
            if (!AllowsAction(viewModel, "setColumnOrder") || _isApplyingCanonical()) return;
            _action(viewModel, "setColumnOrder", order);
        }

        Grid BuildCells(IReadOnlyList<string> cells, bool header, int rowIndex = -1,
            ImageSource? icon = null)
        {
            var grid = new Grid { HorizontalAlignment = HorizontalAlignment.Left };
            var display = DisplayOrder();
            for (var position = 0; position < display.Count; position++)
            {
                var index = display[position];
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
                // The first column carries the row's icon and, where the native list
                // allows it, the in-place editor a rename opens.
                if (!header && index == 0)
                {
                    var item = new ProjectedItemContent(requested =>
                    {
                        if (_isApplyingCanonical() || !viewModel.EditableLabels ||
                            rowIndex < 0 || rowIndex >= viewModel.Rows.Count ||
                            requested.Length == 0 || requested == cells[0] ||
                            !AllowsAction(viewModel, "setItemText")) return;
                        _action(viewModel, "setItemText", new ItemTextActionValue
                        {
                            Index = rowIndex,
                            Text = requested,
                        });
                    })
                    {
                        Margin = new Thickness(6, 0, 6, 0),
                        Editable = viewModel.EditableLabels,
                    };
                    item.Apply(cells[0], icon);
                    rowContents.Add(item);
                    content = item;
                }
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
                        if (viewModel.CheckedIndices.Contains(rowIndex) != requested &&
                            AllowsAction(viewModel, "setItemCheck"))
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
                    cell.Children.Add(content);
                    content = cell;
                }
                Grid.SetColumn(content, position);
                if (header)
                {
                    // The whole cell is the drag handle, so the gesture matches the
                    // native header's own reordering.
                    var handle = new Border
                    {
                        Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
                        Child = content,
                    };
                    var dragged = index;
                    var dragging = false;
                    handle.PointerPressed += (_, args) =>
                    {
                        dragging = handle.CapturePointer(args.Pointer);
                    };
                    handle.PointerReleased += (_, args) =>
                    {
                        if (!dragging) return;
                        dragging = false;
                        handle.ReleasePointerCapture(args.Pointer);
                        RequestColumnMove(dragged, args.GetCurrentPoint(grid).Position.X);
                    };
                    handle.PointerCaptureLost += (_, _) => dragging = false;
                    content = handle;
                    Grid.SetColumn(content, position);
                }
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
                rowContents.Clear();
                var icons = ItemIcons(viewModel, viewModel.Rows.Count);
                for (var index = 0; index < viewModel.Rows.Count; ++index)
                    control.Items.Add(BuildCells(
                        viewModel.Rows[index], header: false, index, icons[index]));
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
            if (!viewModel.SelectedIndices.SequenceEqual(selection) &&
                AllowsAction(viewModel, "setSelection"))
                _action(viewModel, "setSelection", selection);
        };
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Columns) or nameof(viewModel.ColumnWidths) or
                nameof(viewModel.ColumnOrder) or
                nameof(viewModel.Rows) or nameof(viewModel.ColumnHeadersVisible) or
                nameof(viewModel.CheckBoxes) or nameof(viewModel.ItemImages) or
                nameof(viewModel.EditableLabels))
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
            // F2 is the native in-place rename gesture; Space toggles the checkbox
            // column exactly as the native list does.
            if (args.Key == VirtualKey.F2 && viewModel.EditableLabels &&
                control.SelectedIndex >= 0 && control.SelectedIndex < rowContents.Count)
            {
                args.Handled = true;
                rowContents[control.SelectedIndex].BeginEdit();
                return;
            }
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
            MinWidth = 0,
            MinHeight = 0,
            Padding = new Thickness(0),
            BorderThickness = new Thickness(0),
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
                // Set for a text-only button, so the label can be fitted into the width
                // the native control committed to once that width is known.
                TextBlock? label = null;
                if (item.Kind == "separator")
                {
                    element = new SemanticToolbarSeparatorControl();
                }
                else
                {
                    // The icon is optional: a text-only toolbar owns no image list, and then
                    // the button shows the label the control publishes instead.
                    var face = BitmapForToolbarItem(item);
                    label = face is not null ? null : new TextBlock
                    {
                        Text = Win32Mnemonic.DisplayText(item.Text),
                        FontSize = NativeFontSize,
                        VerticalAlignment = VerticalAlignment.Center,
                        // The button is exactly as wide as the native control made it, so
                        // the label is stretched into that width: trimming can only report
                        // a shortened label if it knows how much room it has.
                        HorizontalAlignment = HorizontalAlignment.Stretch,
                        TextAlignment = TextAlignment.Center,
                        TextTrimming = TextTrimming.CharacterEllipsis,
                        TextWrapping = TextWrapping.NoWrap,
                    };
                    FrameworkElement content = face is not null
                        ? new Image { Source = face, Stretch = Stretch.None }
                        : label!;
                    if (item.DropDown == true)
                    {
                        // The button carries an arrow that asks its owner for a menu, so the
                        // projection draws the same affordance rather than promising a
                        // command that completes in place.
                        var row = new StackPanel
                        {
                            Orientation = Orientation.Horizontal,
                            VerticalAlignment = VerticalAlignment.Center,
                        };
                        row.Children.Add(content);
                        row.Children.Add(new FontIcon
                        {
                            Glyph = "\uE70D",
                            FontSize = 8,
                            Margin = new Thickness(2, 0, 0, 0),
                            VerticalAlignment = VerticalAlignment.Center,
                        });
                        content = row;
                    }
                    var commandId = item.CommandId;
                    void Emit()
                    {
                        if (!_isApplyingCanonical() && AllowsAction(viewModel, "toolbarCommand"))
                            _action(viewModel, "toolbarCommand", commandId);
                    }
                    if (item.Kind == "toggleButton" || item.Checked == true)
                    {
                        // A latched button keeps its state between clicks, and the control
                        // owns that state: the projection posts the same command and shows
                        // whatever the next capture reports.
                        var toggle = new ToggleButton
                        {
                            Content = content,
                            MinWidth = 0,
                            MinHeight = 0,
                            Padding = new Thickness(0),
                            HorizontalContentAlignment = HorizontalAlignment.Stretch,
                            VerticalContentAlignment = VerticalAlignment.Stretch,
                            IsEnabled = item.Enabled,
                            IsTabStop = item.Enabled && !item.Hidden,
                            IsChecked = item.Checked == true,
                        };
                        AutomationProperties.SetName(toggle, Win32Mnemonic.DisplayText(item.Text));
                        toggle.Click += (_, _) => Emit();
                        element = toggle;
                    }
                    else
                    {
                        var button = new Button
                        {
                            Content = content,
                            MinWidth = 0,
                            MinHeight = 0,
                            Padding = new Thickness(0),
                            HorizontalContentAlignment = HorizontalAlignment.Stretch,
                            VerticalContentAlignment = VerticalAlignment.Stretch,
                            IsEnabled = item.Enabled,
                            IsTabStop = item.Enabled && !item.Hidden,
                        };
                        AutomationProperties.SetName(button, Win32Mnemonic.DisplayText(item.Text));
                        button.Click += (_, _) => Emit();
                        element = button;
                    }
                }
                element.Visibility = item.Hidden ? Visibility.Collapsed : Visibility.Visible;
                Canvas.SetLeft(element, item.Rect.X * _scale);
                Canvas.SetTop(element, item.Rect.Y * _scale);
                element.Width = item.Rect.Width * _scale;
                element.Height = item.Rect.Height * _scale;
                if (label is not null) FitLabelToWidth(label, element.Width);
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

    // The icon a toolbar item draws, or null when the control owns no image list and
    // the button is text-only.
    private static WriteableBitmap? BitmapForToolbarItem(ToolbarItemSnapshot item)
    {
        if (item.ImageWidth is null && item.ImageHeight is null &&
            item.ImageFormat is null && item.ImageData is null)
            return null;
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

    // A projected MDI child is the native frame's caption contract drawn in Fluent
    // chrome over the native client band: the caption occupies exactly the inset
    // the native frame reports, so the child's own controls keep their native
    // offsets inside it.  Every caption button exists only when the native style
    // bit for it does, and each one routes back through the native system command.
    private SemanticMdiChildControl CreateMdiChild(ControlNodeViewModel viewModel)
    {
        const ulong wsMaximizeBox = 0x00010000;
        const ulong wsMinimizeBox = 0x00020000;
        const ulong wsSysMenu = 0x00080000;
        var control = new SemanticMdiChildControl(verb =>
        {
            if (!_isApplyingCanonical() && AllowsAction(viewModel, "mdiCommand"))
                _action(viewModel, "mdiCommand", verb);
        });
        void Apply()
        {
            control.Apply(new MdiChildPresentation(
                Title: viewModel.Text,
                Active: viewModel.Active,
                WindowState: viewModel.WindowState,
                CaptionHeight: Math.Max(0, viewModel.ClientRect.Y * _scale),
                ClientOffset: new global::Windows.Foundation.Point(
                    viewModel.ClientRect.X * _scale, viewModel.ClientRect.Y * _scale),
                ClientSize: new global::Windows.Foundation.Size(
                    Math.Max(0, viewModel.ClientRect.Width * _scale),
                    Math.Max(0, viewModel.ClientRect.Height * _scale)),
                CanMinimize: (viewModel.Style & wsMinimizeBox) != 0,
                CanMaximize: (viewModel.Style & wsMaximizeBox) != 0,
                CanClose: (viewModel.Style & wsSysMenu) != 0));
        }
        Apply();
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Text) or nameof(viewModel.Active) or
                nameof(viewModel.WindowState) or nameof(viewModel.ClientRect) or
                nameof(viewModel.Rect))
                Apply();
        };
        return control;
    }

    // A private container becomes a real Fluent surface with real splitters: the
    // panes stay where the native geometry puts them, and dragging a splitter
    // resizes exactly the two panes it divides through the native windows
    // themselves.
    private SemanticPaneContainer CreatePaneContainer(ControlNodeViewModel viewModel)
    {
        var control = new SemanticPaneContainer((index, position) =>
        {
            if (_isApplyingCanonical() || !AllowsAction(viewModel, "setSplit")) return;
            _action(viewModel, "setSplit", new SplitActionValue
            {
                Index = index,
                Position = position,
            });
        });
        void Apply()
        {
            control.ApplySplits(viewModel.Splits, _scale);
            control.ApplyChrome(viewModel.ChromeRegions, _scale);
        }
        Apply();
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Splits) or nameof(viewModel.Rect) or
                nameof(viewModel.ChromeRegions))
                Apply();
        };
        return control;
    }

    // An accessible island's elements own no HWND, so the projection renders a real
    // Fluent control for each one and drives it by asking the provider to perform that
    // element's own default action.  The action string is also the accessible
    // description, so what the projection promises is what the provider named.
    private SemanticAccessibleIsland CreateAccessibleIsland(ControlNodeViewModel viewModel)
    {
        var control = new SemanticAccessibleIsland(index =>
        {
            if (_isApplyingCanonical() || !AllowsAction(viewModel, "islandInvoke")) return;
            _action(viewModel, "islandInvoke", index);
        });
        void Apply() => control.ApplyItems(viewModel.IslandItems, _scale);
        Apply();
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.IslandItems) or nameof(viewModel.Rect))
                Apply();
        };
        return control;
    }

    private SemanticTabControl CreateTabControl(ControlNodeViewModel viewModel)
    {
        var control = new SemanticTabControl(index =>
        {
            if (!_isApplyingCanonical() && AllowsAction(viewModel, "select") &&
                index != viewModel.SelectedIndex)
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

    // The native tree owns hierarchy, selection, and expansion, so the projection
    // routes both gestures back to it and rebuilds from whatever the next canonical
    // revision reports.  Expansion is a real route rather than a local view state:
    // a lazily populated tree only inserts its children when the control itself
    // raises TVN_ITEMEXPANDING.
    private SemanticTreeViewControl CreateTreeView(ControlNodeViewModel viewModel)
    {
        var control = new SemanticTreeViewControl(
            index =>
            {
                if (!_isApplyingCanonical() && index != viewModel.SelectedIndex &&
                    AllowsAction(viewModel, "select"))
                    _action(viewModel, "select", index);
            },
            (index, expanded) =>
            {
                if (_isApplyingCanonical() || index < 0 ||
                    index >= viewModel.ItemExpanded.Count ||
                    viewModel.ItemExpanded[index] == expanded ||
                    !viewModel.ItemHasChildren[index] ||
                    !AllowsAction(viewModel, "setExpand")) return;
                _action(viewModel, "setExpand", new TreeExpandActionValue
                {
                    Index = index,
                    Expanded = expanded,
                });
            },
            (index, text) =>
            {
                if (_isApplyingCanonical() || !viewModel.EditableLabels ||
                    index < 0 || index >= viewModel.Items.Count ||
                    text.Length == 0 || text == viewModel.Items[index] ||
                    !AllowsAction(viewModel, "setItemText")) return;
                _action(viewModel, "setItemText", new ItemTextActionValue
                {
                    Index = index,
                    Text = text,
                });
            });

        void Rebuild()
        {
            if (!HasRenderableTreeShape(viewModel)) return;
            control.EditableLabels = viewModel.EditableLabels;
            control.Rebuild(viewModel.Items, viewModel.ItemDepths,
                viewModel.ItemExpanded, viewModel.ItemHasChildren,
                ItemIcons(viewModel, viewModel.Items.Count, viewModel.SelectedIndex),
                viewModel.SelectedIndex);
        }
        Rebuild();
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Items) or nameof(viewModel.ItemDepths) or
                nameof(viewModel.ItemImages) or nameof(viewModel.EditableLabels))
                Rebuild();
            else if (args.PropertyName == nameof(viewModel.SelectedIndex))
            {
                control.ApplySelection(viewModel.SelectedIndex);
                // A native tree draws the selected-state icon for the selected item,
                // so a selection move can change two rows' icons.
                if (viewModel.ItemSelectedImages.Count > 0)
                    control.ApplyIcons(ItemIcons(
                        viewModel, viewModel.Items.Count, viewModel.SelectedIndex));
            }
        };
        return control;
    }

    // One decoded icon per item, or null where the item draws none.  The image list
    // is decoded once per rebuild rather than once per item, because a tree or list
    // normally repeats a handful of icons across every row.
    // One decoded icon per item, or null where the item draws none.  The image list
    // is decoded once per rebuild rather than once per item, because a tree or list
    // normally repeats a handful of icons across every row.  The selected item uses
    // its selected-state index when the control captured one, which is how a native
    // tree shows an open folder for the current selection.
    private static IReadOnlyList<ImageSource?> ItemIcons(
        ControlNodeViewModel viewModel, int itemCount, int selectedIndex = -1)
    {
        var decoded = new WriteableBitmap?[viewModel.ImageList.Count];
        var icons = new ImageSource?[itemCount];
        for (var index = 0; index < itemCount; ++index)
        {
            var image = ImageIndexForItem(viewModel, index, selectedIndex);
            if (image < 0 || image >= decoded.Length) continue;
            decoded[image] ??= BitmapForImageListEntry(viewModel.ImageList[image]);
            icons[index] = decoded[image];
        }
        return icons;
    }

    // The image list index an item draws, or -1 for no icon.  The selected item
    // prefers its selected-state index, which is how a native tree shows an open
    // folder for the current selection while its siblings stay closed.
    internal static int ImageIndexForItem(
        ControlNodeViewModel viewModel, int index, int selectedIndex)
    {
        if (index < 0 || index >= viewModel.ItemImages.Count) return -1;
        return index == selectedIndex && index < viewModel.ItemSelectedImages.Count
            ? viewModel.ItemSelectedImages[index]
            : viewModel.ItemImages[index];
    }

    internal static WriteableBitmap BitmapForImageListEntry(ImageListEntry entry)
    {
        var pixels = Convert.FromBase64String(entry.ImageData);
        if (pixels.Length != checked(entry.ImageWidth * entry.ImageHeight * 4))
            throw new InvalidOperationException("Validated image list pixel count changed.");
        var bitmap = new WriteableBitmap(entry.ImageWidth, entry.ImageHeight);
        using var stream = bitmap.PixelBuffer.AsStream();
        stream.Write(pixels, 0, pixels.Length);
        bitmap.Invalidate();
        return bitmap;
    }

    // A container's own painted band, reproduced from the pixels the native window
    // drew.  The projection cannot re-render what it never parsed, so it publishes
    // exactly those pixels rather than approximating the band.
    internal static WriteableBitmap BitmapForChromeRegion(ChromeRegion region)
    {
        var pixels = Convert.FromBase64String(region.ImageData);
        if (pixels.Length != checked(region.ImageWidth * region.ImageHeight * 4))
            throw new InvalidOperationException("Validated chrome region pixel count changed.");
        var bitmap = new WriteableBitmap(region.ImageWidth, region.ImageHeight);
        using var stream = bitmap.PixelBuffer.AsStream();
        stream.Write(pixels, 0, pixels.Length);
        bitmap.Invalidate();
        return bitmap;
    }

    // A trackbar is projected as a stock Slider: integer steps, no invented ticks,
    // and no thumb tooltip, because the adapter captures a position and a range and
    // nothing else.  A vertical native trackbar counts downward from its top, which
    // is the reverse of a WinUI vertical Slider, so orientation and the native
    // TBS_REVERSED hint are combined into one direction.
    private Slider CreateSlider(ControlNodeViewModel viewModel)
    {
        var control = new Slider
        {
            Orientation = viewModel.Vertical ? Orientation.Vertical : Orientation.Horizontal,
            IsDirectionReversed = viewModel.Reversed ^ viewModel.Vertical,
            IsThumbToolTipEnabled = false,
            TickPlacement = TickPlacement.None,
            StepFrequency = 1,
            MinWidth = 0,
            MinHeight = 0,
            Padding = new Thickness(0),
        };
        var applyingCanonical = false;
        void ApplyCanonicalRange()
        {
            applyingCanonical = true;
            try
            {
                // Assign the bound that moves outward first so the control never
                // clamps a canonical value against a stale opposite bound.
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
                control.SmallChange = Math.Max(0, viewModel.SmallChange);
                control.LargeChange = Math.Max(0, viewModel.LargeChange);
                control.Value = viewModel.Position;
            }
            finally
            {
                applyingCanonical = false;
            }
        }
        ApplyCanonicalRange();
        control.ValueChanged += (_, args) =>
        {
            if (applyingCanonical || _isApplyingCanonical()) return;
            var requested = (int)Math.Round(args.NewValue);
            if (requested != viewModel.Position && AllowsAction(viewModel, "setValue"))
                _action(viewModel, "setValue", requested);
        };
        viewModel.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Minimum) or nameof(viewModel.Maximum) or
                nameof(viewModel.Position) or nameof(viewModel.SmallChange) or
                nameof(viewModel.LargeChange))
                ApplyCanonicalRange();
            else if (args.PropertyName == nameof(viewModel.Vertical) ||
                     args.PropertyName == nameof(viewModel.Reversed))
            {
                control.Orientation = viewModel.Vertical ? Orientation.Vertical : Orientation.Horizontal;
                control.IsDirectionReversed = viewModel.Reversed ^ viewModel.Vertical;
            }
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

    // The three tree arrays are one hierarchy.  A partially replaced set of them
    // would describe a different tree, so the projection waits for a complete one
    // instead of rendering an intermediate shape.
    internal static bool HasRenderableTreeShape(ControlNodeViewModel viewModel) =>
        viewModel.Items.Count is > 0 and <= ProtocolConstants.MaxItems &&
        viewModel.ItemDepths.Count == viewModel.Items.Count &&
        viewModel.ItemExpanded.Count == viewModel.Items.Count &&
        viewModel.ItemHasChildren.Count == viewModel.Items.Count &&
        viewModel.ItemDepths[0] == 0 &&
        viewModel.SelectedIndex >= -1 && viewModel.SelectedIndex < viewModel.Items.Count;

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

    // Theme brush keys the projection resolves lazily.  The first lookup loads the
    // framework's theme dictionary on the UI thread, and doing that while a
    // surface is first rasterizing competes with the Bridge's bounded UIA
    // validation, so the renderer pays for it once at startup instead.
    private static readonly string[] ProjectedThemeBrushKeys =
    [
        "SolidBackgroundFillColorSecondaryBrush",
        "CardStrokeColorDefaultBrush",
        "AccentFillColorDefaultBrush",
        "TextOnAccentFillColorPrimaryBrush",
        "TextFillColorPrimaryBrush",
    ];

    internal static void WarmThemeResources()
    {
        // A warm-up is an optimization, never a contract: a framework that refuses
        // the lookup must not take the renderer down with it.
        try
        {
            if (Application.Current?.Resources is not { } resources) return;
            foreach (var key in ProjectedThemeBrushKeys) resources.TryGetValue(key, out _);
        }
        catch (Exception exception)
        {
            RendererDiagnostics.Log("theme resource warm-up skipped: " + exception.Message);
        }
    }

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
        "treeView" => AutomationControlType.Tree,
        "tabControl" => AutomationControlType.Tab,
        "slider" => AutomationControlType.Slider,
        "dialogContainer" => AutomationControlType.Pane,
        "mdiClient" => AutomationControlType.Pane,
        "paneContainer" => AutomationControlType.Pane,
        "accessibleIsland" => AutomationControlType.Group,
        "mdiChild" => AutomationControlType.Window,
        "statusBar" => AutomationControlType.StatusBar,
        "toolbar" => AutomationControlType.ToolBar,
        _ => AutomationControlType.Custom,
    };

    // WinUI can report a ContentControl's visual in DIPs, or collapse a Canvas
    // with no fill onto the union of its children, while leaving the origin in
    // physical screen pixels.  The Bridge's committed gate compares physical HWND
    // rectangles, so a projected node that owns a layout slot reports that slot
    // converted through the XamlRoot rasterization scale.
    internal static global::Windows.Foundation.Rect PhysicalLayoutBounds(
        FrameworkElement owner,
        global::Windows.Foundation.Rect reported)
    {
        var scale = owner.XamlRoot?.RasterizationScale ?? 0;
        if (scale <= 0 || owner.ActualWidth <= 0 || owner.ActualHeight <= 0)
            return reported;
        return new global::Windows.Foundation.Rect(
            reported.X,
            reported.Y,
            owner.ActualWidth * scale,
            owner.ActualHeight * scale);
    }

    // The native control chose each button's width by measuring its label with GDI, and
    // WinUI's text metrics are not GDI's -- the same string at the same point size is
    // wider here.  The label is fitted into the width the control already committed to,
    // down to a floor, because the projection may not move the button and may not slice
    // glyphs in half either.
    internal static void FitLabelToWidth(TextBlock label, double available)
    {
        if (available <= 0 || label.Text.Length == 0) return;
        label.Measure(new global::Windows.Foundation.Size(
            double.PositiveInfinity, double.PositiveInfinity));
        var desired = label.DesiredSize.Width;
        if (desired <= available || desired <= 0) return;
        label.FontSize = Math.Max(MinimumFittedFontSize, label.FontSize * available / desired);
    }

    private void ApplyBounds(FrameworkElement element, ControlNodeViewModel viewModel)
    {
        var rect = RelativeRectFor(viewModel, _nodes);
        Canvas.SetLeft(element, rect.X * _scale);
        Canvas.SetTop(element, rect.Y * _scale);
        element.Width = Math.Max(0, viewModel.Rect.Width * _scale);
        element.Height = Math.Max(0, viewModel.Rect.Height * _scale);
        if (viewModel.Kind is "dialogContainer" or "tabControl" or "toolbar" or "treeView"
            or "mdiClient" or "mdiChild" or "paneContainer" or "accessibleIsland"
            or "statusBar")
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
        // An MDI child's controls sit in its client band, which starts below the
        // caption the projection draws, so their offsets are relative to that band
        // rather than to the frame.
        var originX = parent.Rect.X + (parent.Kind == "mdiChild" ? parent.ClientRect.X : 0);
        var originY = parent.Rect.Y + (parent.Kind == "mdiChild" ? parent.ClientRect.Y : 0);
        return node.Rect with { X = node.Rect.X - originX, Y = node.Rect.Y - originY };
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

internal sealed class SemanticBitmapSwitchControl : RadioButton
{
    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticBitmapSwitchAutomationPeer(this);
}

internal sealed class SemanticBitmapSwitchAutomationPeer(SemanticBitmapSwitchControl owner) :
    RadioButtonAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        AutomationControlType.RadioButton;
    protected override string GetClassNameCore() => "RadioButton";
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

internal sealed class SemanticStaticIconControl : ContentControl
{
    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticStaticIconAutomationPeer(this);
}

internal sealed class SemanticStaticIconAutomationPeer(SemanticStaticIconControl owner) :
    FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        ControlFactory.AutomationControlTypeFor("staticIcon");
    protected override string GetClassNameCore() => "Image";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => true;
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
    protected override global::Windows.Foundation.Rect GetBoundingRectangleCore()
    {
        var reported = base.GetBoundingRectangleCore();
        if (owner.ActualWidth <= 0 || owner.ActualHeight <= 0 || reported.Width <= 0)
            return reported;
        var scale = reported.Width / owner.ActualWidth;
        var semanticWidth = owner.ActualWidth * scale;
        var semanticHeight = owner.ActualHeight * scale;
        return new global::Windows.Foundation.Rect(
            reported.X - (semanticWidth - reported.Width) / 2,
            reported.Y - (semanticHeight - reported.Height) / 2,
            semanticWidth,
            semanticHeight);
    }
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

// A projected tree is the native tree's hierarchy, selection, and expansion, and
// nothing else.  The stock WinUI TreeView is hosted rather than subclassed --
// a C#-derived WinRT control cannot receive the base type's default style -- and
// the wrapper republishes it through a peer that pins the Tree + Selection
// contract the Bridge gate requires.  The inner control keeps its own peer, which
// is what leaves the realized TreeItems reachable underneath.
// One projected item's content: its icon, its label, and the in-place editor a
// rename opens.  Both the tree and the report list use it, so an item looks and
// renames the same way in either.
internal sealed class ProjectedItemContent : Grid
{
    private const double LabelFontSize = 12;
    private readonly Image _icon = new() { Stretch = Stretch.None, IsHitTestVisible = false };
    private readonly TextBlock _label = new()
    {
        FontSize = LabelFontSize,
        VerticalAlignment = VerticalAlignment.Center,
        TextTrimming = TextTrimming.CharacterEllipsis,
    };
    private readonly Action<string> _commit;
    private TextBox? _editor;

    public ProjectedItemContent(Action<string> commit)
    {
        _commit = commit;
        ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        Grid.SetColumn(_icon, 0);
        Grid.SetColumn(_label, 1);
        Children.Add(_icon);
        Children.Add(_label);
        AutomationProperties.SetAccessibilityView(_icon, AccessibilityView.Raw);
        AutomationProperties.SetAccessibilityView(_label, AccessibilityView.Raw);
    }

    public string Text { get; private set; } = string.Empty;
    public bool IsEditing => _editor is not null;

    // Whether the native control admits in-place renaming.  It gates both the F2
    // gesture and the UIA Value pattern, which is how assistive technology renames
    // an item natively.
    public bool Editable { get; set; }

    // TreeView binds nodes as data, so the item container derives its accessible
    // name from the data object.  This element *is* that object.
    public override string ToString() => Text;

    public void Apply(string text, ImageSource? icon)
    {
        Text = text;
        _label.Text = text;
        AutomationProperties.SetName(this, text);
        ApplyIcon(icon);
        if (_editor is not null) _editor.Text = text;
    }

    public void ApplyIcon(ImageSource? icon)
    {
        _icon.Source = icon;
        _icon.Visibility = icon is null ? Visibility.Collapsed : Visibility.Visible;
        _icon.Margin = icon is null ? new Thickness(0) : new Thickness(0, 0, 6, 0);
    }

    // Opens the projected editor.  Nothing native happens yet: the native control's
    // own label session runs once, at commit, so a projected edit never depends on
    // the cloaked window keeping keyboard focus while the user types.
    public void BeginEdit()
    {
        if (_editor is not null) return;
        _editor = new TextBox
        {
            Text = Text,
            FontSize = LabelFontSize,
            MinWidth = 0,
            MinHeight = 0,
            Padding = new Thickness(2, 0, 2, 0),
            VerticalAlignment = VerticalAlignment.Center,
        };
        AutomationProperties.SetName(_editor, Text);
        _editor.KeyDown += (_, args) =>
        {
            if (args.Key == VirtualKey.Enter)
            {
                args.Handled = true;
                EndEdit(commit: true);
            }
            else if (args.Key == VirtualKey.Escape)
            {
                args.Handled = true;
                EndEdit(commit: false);
            }
        };
        _editor.LostFocus += (_, _) => EndEdit(commit: true);
        _label.Visibility = Visibility.Collapsed;
        Grid.SetColumn(_editor, 1);
        Children.Add(_editor);
        _editor.SelectAll();
        _editor.Focus(FocusState.Programmatic);
    }

    public void EndEdit(bool commit)
    {
        if (_editor is not { } editor) return;
        var requested = editor.Text;
        _editor = null;
        Children.Remove(editor);
        _label.Visibility = Visibility.Visible;
        // An empty label is how native in-place editing signals a cancel, so it is
        // never sent as a rename.
        if (commit && requested.Length != 0 && requested != Text) _commit(requested);
    }

    // The UIA rename path: assistive technology sets an item's value rather than
    // opening an editor, and it lands on the same native label session.
    internal void RenameFromAutomation(string text)
    {
        if (!Editable || text.Length == 0 || text == Text) return;
        _commit(text);
    }

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new ProjectedItemContentAutomationPeer(this);
}

internal sealed class ProjectedItemContentAutomationPeer(ProjectedItemContent owner) :
    FrameworkElementAutomationPeer(owner), IValueProvider
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        AutomationControlType.Text;
    protected override string GetClassNameCore() => "ProjectedItemLabel";
    protected override string GetNameCore() => owner.Text;
    // A read-only label adds nothing its item does not already say, so it stays out
    // of the control view until the native control admits renaming.
    protected override bool IsControlElementCore() => owner.Editable;
    protected override object GetPatternCore(PatternInterface patternInterface) =>
        patternInterface == PatternInterface.Value ? this : base.GetPatternCore(patternInterface);

    public bool IsReadOnly => !owner.Editable;
    public string Value => owner.Text;
    public void SetValue(string value) => owner.RenameFromAutomation(value ?? string.Empty);
}

internal sealed class SemanticTreeViewControl : ContentControl
{
    // TreeView binds its nodes as data, so the projected item element reaches the
    // container through this template rather than being stringified by the default
    // one.
    private static readonly DataTemplate? ProjectedItemTemplate =
        Application.Current?.Resources.TryGetValue("ProjectedItemTemplate", out var template) == true
            ? template as DataTemplate
            : null;

    private readonly Action<int> _selectionRequested;
    private readonly Action<int, bool> _expansionRequested;
    private readonly Action<int, string> _renameRequested;
    private readonly TreeView _tree = new()
    {
        SelectionMode = TreeViewSelectionMode.Single,
        CanDragItems = false,
        CanReorderItems = false,
        Background = null,
        Padding = new Thickness(0),
        ItemTemplate = ProjectedItemTemplate,
    };
    private readonly List<TreeViewNode> _nodes = [];
    private readonly Dictionary<TreeViewNode, int> _indices = [];
    private readonly List<ProjectedItemContent> _contents = [];
    private int _selectedIndex = -1;
    private bool _applyingCanonical;

    public SemanticTreeViewControl(
        Action<int> selectionRequested,
        Action<int, bool> expansionRequested,
        Action<int, string> renameRequested)
    {
        _selectionRequested = selectionRequested;
        _expansionRequested = expansionRequested;
        _renameRequested = renameRequested;
        Content = _tree;
        Background = null;
        BorderThickness = new Thickness(0);
        Padding = new Thickness(0);
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        VerticalContentAlignment = VerticalAlignment.Stretch;
        _tree.SelectionChanged += OnSelectionChanged;
        _tree.Expanding += OnExpanding;
        _tree.Collapsed += OnCollapsed;
        GotFocus += (_, _) =>
        {
            if (SelectedContainer is Control container &&
                !container.FocusState.Equals(FocusState.Keyboard))
                container.Focus(FocusState.Keyboard);
        };
        // F2 is the native in-place rename gesture, so the projection keeps it.
        KeyDown += (_, args) =>
        {
            if (args.Key != VirtualKey.F2 || !EditableLabels) return;
            if (_selectedIndex < 0 || _selectedIndex >= _contents.Count) return;
            args.Handled = true;
            _contents[_selectedIndex].BeginEdit();
        };
    }

    public bool EditableLabels { get; set; }

    public TreeView Tree => _tree;
    public IReadOnlyList<TreeViewNode> Nodes => _nodes;
    public int SelectedIndex => _selectedIndex;

    public UIElement? SelectedContainer =>
        _selectedIndex >= 0 && _selectedIndex < _nodes.Count
            ? _tree.ContainerFromNode(_nodes[_selectedIndex]) as UIElement
            : null;

    // Labels are native item text, not menu text: an ampersand in a tree item is
    // a literal character, so no mnemonic collapsing happens here.
    public void Rebuild(
        IReadOnlyList<string> labels,
        IReadOnlyList<int> depths,
        IReadOnlyList<bool> expanded,
        IReadOnlyList<bool> hasChildren,
        IReadOnlyList<ImageSource?> icons,
        int selectedIndex)
    {
        if (labels.Count != depths.Count || labels.Count != expanded.Count ||
            labels.Count != hasChildren.Count || labels.Count != icons.Count)
            throw new ArgumentException(
                "Tree labels, depths, expansion, child flags, and icons must have matching counts.",
                nameof(depths));
        _applyingCanonical = true;
        try
        {
            _tree.RootNodes.Clear();
            _nodes.Clear();
            _indices.Clear();
            _contents.Clear();
            var ancestors = new List<TreeViewNode>();
            for (var index = 0; index < labels.Count; ++index)
            {
                var depth = depths[index];
                if (depth < 0 || depth > ancestors.Count)
                    throw new ArgumentException(
                        "Tree item depth does not describe a tree.", nameof(depths));
                var position = index;
                var content = new ProjectedItemContent(
                    text => _renameRequested(position, text))
                {
                    Editable = EditableLabels,
                };
                content.Apply(labels[index], icons[index]);
                var node = new TreeViewNode { Content = content };
                if (depth == 0) _tree.RootNodes.Add(node);
                else ancestors[depth - 1].Children.Add(node);
                ancestors.RemoveRange(depth, ancestors.Count - depth);
                ancestors.Add(node);
                _nodes.Add(node);
                _contents.Add(content);
                _indices.Add(node, index);
            }
            // Expansion is applied only once the whole hierarchy exists, so a
            // parent that opens already has its captured children in place.
            for (var index = 0; index < _nodes.Count; ++index)
            {
                _nodes[index].HasUnrealizedChildren =
                    hasChildren[index] && _nodes[index].Children.Count == 0;
                _nodes[index].IsExpanded = expanded[index];
            }
            ApplySelectionCore(selectedIndex);
        }
        finally
        {
            _applyingCanonical = false;
        }
    }

    public void ApplySelection(int selectedIndex)
    {
        _applyingCanonical = true;
        try { ApplySelectionCore(selectedIndex); }
        finally { _applyingCanonical = false; }
    }

    // Only the icons change, so an in-progress projected edit and the existing UIA
    // elements both survive a selection move.
    public void ApplyIcons(IReadOnlyList<ImageSource?> icons)
    {
        for (var index = 0; index < _contents.Count && index < icons.Count; ++index)
            _contents[index].ApplyIcon(icons[index]);
    }

    private void ApplySelectionCore(int selectedIndex)
    {
        _selectedIndex = selectedIndex;
        _tree.SelectedNode = selectedIndex >= 0 && selectedIndex < _nodes.Count
            ? _nodes[selectedIndex]
            : null;
    }

    private void OnSelectionChanged(TreeView sender, TreeViewSelectionChangedEventArgs args)
    {
        if (_applyingCanonical) return;
        if (_tree.SelectedNode is { } node && _indices.TryGetValue(node, out var index) &&
            index != _selectedIndex)
            _selectionRequested(index);
    }

    private void OnExpanding(TreeView sender, TreeViewExpandingEventArgs args)
    {
        if (_applyingCanonical) return;
        if (_indices.TryGetValue(args.Node, out var index)) _expansionRequested(index, true);
    }

    private void OnCollapsed(TreeView sender, TreeViewCollapsedEventArgs args)
    {
        if (_applyingCanonical) return;
        if (_indices.TryGetValue(args.Node, out var index)) _expansionRequested(index, false);
    }

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticTreeViewAutomationPeer(this);
}

// GetChildrenCore is deliberately not overridden: the default children are the
// hosted TreeView's peer, whose own TreeViewList peer publishes every realized
// TreeItem.  Republishing them here would freeze the first, still-unrealized
// answer instead.
internal sealed class SemanticTreeViewAutomationPeer(SemanticTreeViewControl owner) :
    FrameworkElementAutomationPeer(owner), ISelectionProvider
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        ControlFactory.AutomationControlTypeFor("treeView");
    protected override string GetClassNameCore() => "TreeView";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
    protected override bool IsControlElementCore() => true;
    protected override object GetPatternCore(PatternInterface patternInterface) =>
        patternInterface == PatternInterface.Selection ? this : base.GetPatternCore(patternInterface);

    public bool CanSelectMultiple => false;
    public bool IsSelectionRequired => false;
    public IRawElementProviderSimple[] GetSelection()
    {
        if (owner.SelectedContainer is not { } container) return [];
        var peer = CreatePeerForElement(container);
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

// Any projected container that owns other projected nodes.  TranslatedWindow
// places a child in its parent's collection through this, so adding a container
// kind never means editing the placement code.
internal interface ISemanticContainer
{
    UIElementCollection Children { get; }
}

internal sealed class SemanticDialogContainer : Canvas, ISemanticContainer
{
    public SemanticDialogContainer()
    {
        IsTabStop = false;
        // A Canvas with no Background renders nothing of its own, which collapses
        // its UIA bounding rectangle onto the union of its children.  The Bridge
        // compares this rectangle against the native container and clips its
        // children against it, so the fill is what keeps both truthful.
        Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
    }

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticDialogContainerAutomationPeer(this);
}

// The MDI client area: a plain region that owns the frame's child windows.
internal sealed class SemanticMdiClient : Canvas, ISemanticContainer
{
    public SemanticMdiClient()
    {
        IsTabStop = false;
        Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
    }

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticMdiClientAutomationPeer(this);
}

// A private container the Bridge admitted because its children tile it: the panes
// keep their native geometry and every thin strip between two full-height or
// full-width panes becomes a real splitter.  Dragging one resizes the two native
// panes it divides, so the native layout stays the truth.
internal sealed class SemanticPaneContainer : Canvas, ISemanticContainer
{
    private readonly Action<int, int> _splitRequested;
    private readonly List<SemanticPaneSplitter> _splitters = [];
    private readonly List<Image> _chrome = [];

    public SemanticPaneContainer(Action<int, int> splitRequested)
    {
        _splitRequested = splitRequested;
        IsTabStop = false;
        // A Canvas with no Background renders no visual at all, which collapses its
        // UIA bounding rectangle onto the union of its children and makes the
        // Bridge's geometry gate compare the wrong rectangle.
        Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        // The splitters span the container, and its own size arrives after the
        // factory has built them.
        SizeChanged += (_, _) => ApplyExtents();
    }

    private void ApplyExtents()
    {
        foreach (var splitter in _splitters) splitter.ApplyExtent(ActualWidth, ActualHeight);
        if (_splitters.Count != 0)
        {
            RendererDiagnostics.Log(
                $"paneContainer extent {ActualWidth:F0}x{ActualHeight:F0} splitters={_splitters.Count} " +
                string.Join(" ", _splitters.Select(splitter =>
                    $"[{splitter.Split.Position}+{splitter.Split.Thickness} {splitter.Width:F0}x{splitter.Height:F0}]")));
        }
    }

    public void ApplySplits(IReadOnlyList<PaneSplit> splits, double scale)
    {
        while (_splitters.Count > splits.Count)
        {
            var extra = _splitters[^1];
            _splitters.RemoveAt(_splitters.Count - 1);
            Children.Remove(extra);
        }
        for (var index = 0; index < splits.Count; ++index)
        {
            if (index == _splitters.Count)
            {
                var position = index;
                var created = new SemanticPaneSplitter(
                    value => _splitRequested(position, value));
                // Splitters sit above the panes so the strip between two panes stays
                // hittable no matter which order the panes were added in.
                Canvas.SetZIndex(created, 1000);
                _splitters.Add(created);
                Children.Add(created);
            }
            _splitters[index].Apply(splits[index], scale);
        }
        ApplyExtents();
    }

    // The bands the container paints itself.  They sit behind the panes and behind the
    // splitters, which is where the native window drew them.
    public void ApplyChrome(IReadOnlyList<ChromeRegion> regions, double scale)
    {
        while (_chrome.Count > regions.Count)
        {
            var extra = _chrome[^1];
            _chrome.RemoveAt(_chrome.Count - 1);
            Children.Remove(extra);
        }
        for (var index = 0; index < regions.Count; ++index)
        {
            if (index == _chrome.Count)
            {
                var created = new Image { Stretch = Stretch.Fill, IsHitTestVisible = false };
                Canvas.SetZIndex(created, -1);
                _chrome.Add(created);
                Children.Add(created);
            }
            var region = regions[index];
            var image = _chrome[index];
            image.Source = ControlFactory.BitmapForChromeRegion(region);
            image.Width = Math.Max(0, region.Rect.Width * scale);
            image.Height = Math.Max(0, region.Rect.Height * scale);
            Canvas.SetLeft(image, region.Rect.X * scale);
            Canvas.SetTop(image, region.Rect.Y * scale);
        }
    }

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticPaneContainerAutomationPeer(this);
}

internal sealed class SemanticPaneContainerAutomationPeer(SemanticPaneContainer owner)
    : FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        ControlFactory.AutomationControlTypeFor("paneContainer");
    protected override string GetClassNameCore() => "ProjectedPaneContainer";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => false;
    protected override global::Windows.Foundation.Rect GetBoundingRectangleCore() =>
        ControlFactory.PhysicalLayoutBounds(owner, base.GetBoundingRectangleCore());
}

// A host window whose content owns no HWND at all, rendered as real Fluent controls.
// Each element is placed at the rectangle the provider reported and invoked by asking
// the provider to perform that element's own default action, so the projection never
// guesses at what a click means.  An element that reported no action is inert text,
// exactly as the provider described it.
internal sealed class SemanticAccessibleIsland : Canvas
{
    private const double ItemFontSize = 12;

    private readonly Action<int> _invoked;

    public SemanticAccessibleIsland(Action<int> invoked)
    {
        _invoked = invoked;
        IsTabStop = false;
        // A Canvas with no Background renders no visual at all, which collapses its UIA
        // bounding rectangle onto the union of its children.
        Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
    }

    public void ApplyItems(IReadOnlyList<AccessibleIslandItem> items, double scale)
    {
        // The elements are rebuilt whenever the provider's description changes, which
        // is the same contract a projected Toolbar's buttons follow.
        Children.Clear();
        for (var index = 0; index < items.Count; ++index)
        {
            var item = items[index];
            var element = item.Kind == "text"
                ? (FrameworkElement)new TextBlock
                {
                    Text = item.Name,
                    FontSize = ItemFontSize,
                    VerticalAlignment = VerticalAlignment.Center,
                    TextTrimming = TextTrimming.CharacterEllipsis,
                }
                : BuildActionable(item, index);
            AutomationProperties.SetName(element, item.Name);
            // The provider's own action string is what the projection will perform, so it
            // is what an assistive client is told about.
            AutomationProperties.SetHelpText(element,
                item.Description.Length != 0 ? item.Description : item.ActionName);
            Canvas.SetLeft(element, item.Rect.X * scale);
            Canvas.SetTop(element, item.Rect.Y * scale);
            element.Width = Math.Max(0, item.Rect.Width * scale);
            element.Height = Math.Max(0, item.Rect.Height * scale);
            Children.Add(element);
        }
    }

    private FrameworkElement BuildActionable(AccessibleIslandItem item, int index)
    {
        var label = new TextBlock
        {
            Text = item.Name,
            FontSize = ItemFontSize,
            VerticalAlignment = VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        FrameworkElement content = label;
        if (item.DropDown)
        {
            // The element completes by opening a menu of its own, so the projection draws
            // the same affordance rather than promising an in-place result.
            var row = new Grid();
            row.ColumnDefinitions.Add(
                new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            Grid.SetColumn(label, 0);
            var chevron = new FontIcon
            {
                Glyph = "\uE76C",
                FontSize = 10,
                Margin = new Thickness(8, 0, 0, 0),
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetColumn(chevron, 1);
            row.Children.Add(label);
            row.Children.Add(chevron);
            content = row;
        }
        if (item.Kind == "link")
        {
            var hyperlink = new HyperlinkButton
            {
                Content = content,
                IsEnabled = item.Enabled,
                Padding = new Thickness(8, 0, 8, 0),
                MinWidth = 0,
                MinHeight = 0,
            };
            hyperlink.Click += (_, _) => _invoked(index);
            return hyperlink;
        }
        var button = new Button
        {
            Content = content,
            IsEnabled = item.Enabled,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Center,
            Padding = new Thickness(8, 0, 8, 0),
            MinWidth = 0,
            MinHeight = 0,
        };
        button.Click += (_, _) => _invoked(index);
        return button;
    }

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticAccessibleIslandAutomationPeer(this);
}

internal sealed class SemanticAccessibleIslandAutomationPeer(SemanticAccessibleIsland owner)
    : FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        ControlFactory.AutomationControlTypeFor("accessibleIsland");
    protected override string GetClassNameCore() => "ProjectedAccessibleIsland";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => false;
    protected override global::Windows.Foundation.Rect GetBoundingRectangleCore() =>
        ControlFactory.PhysicalLayoutBounds(owner, base.GetBoundingRectangleCore());
}

// One splitter.  The native container leaves the gap empty, so the projection draws a
// Fluent divider inside exactly that gap and widens only the invisible grab target when
// the native strip is too thin to hit.  The position it reports is always in the
// container's own client pixels.
internal sealed class SemanticPaneSplitter : ContentControl
{
    // A drag emits at most one native resize per interval: the native panes are
    // resized for real, and every frame of a 60 Hz drag would be a source-thread
    // command plus a recapture.
    private const long EmitIntervalMilliseconds = 60;
    private const int KeyboardStep = 8;

    private readonly Action<int> _positionRequested;
    private readonly Stopwatch _sinceEmit = Stopwatch.StartNew();
    // The divider is a child rather than this control's own Background: a bare
    // ContentControl with only a brush renders no visual of its own, which collapses
    // its bounding rectangle and leaves nothing on screen to grab.
    private readonly Border _divider = new()
    {
        HorizontalAlignment = HorizontalAlignment.Stretch,
        VerticalAlignment = VerticalAlignment.Stretch,
        CornerRadius = new CornerRadius(1),
    };
    private PaneSplit _split = new();
    private double _scale = 1;
    private bool _dragging;
    private double _grabOffset;
    private int _lastEmitted = int.MinValue;

    public SemanticPaneSplitter(Action<int> positionRequested)
    {
        _positionRequested = positionRequested;
        IsTabStop = true;
        UseSystemFocusVisuals = true;
        Padding = new Thickness(0);
        BorderThickness = new Thickness(0);
        MinWidth = 0;
        MinHeight = 0;
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        VerticalContentAlignment = VerticalAlignment.Stretch;
        Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        Content = _divider;
        ApplyDividerBrush(false);
        PointerEntered += (_, _) => ApplyDividerBrush(true);
        PointerExited += (_, _) => { if (!_dragging) ApplyDividerBrush(false); };
        PointerPressed += OnPointerPressed;
        PointerMoved += OnPointerMoved;
        PointerReleased += OnPointerReleased;
        PointerCaptureLost += (_, _) =>
        {
            _dragging = false;
            ApplyDividerBrush(false);
        };
        KeyDown += OnKeyDown;
    }

    private void ApplyDividerBrush(bool hot) =>
        _divider.Background = new SolidColorBrush(hot
            ? Microsoft.UI.ColorHelper.FromArgb(0xFF, 0x60, 0xA0, 0xE0)
            : Microsoft.UI.ColorHelper.FromArgb(0x60, 0x80, 0x80, 0x80));

    public PaneSplit Split => _split;

    public void Apply(PaneSplit split, double scale)
    {
        _split = split;
        _scale = scale <= 0 ? 1 : scale;
        // The grab target is widened to a usable size only when the native gap is
        // thinner than one, and it is centred on the native gap so the panes on
        // either side keep their exact native edges.  The divider itself stays the
        // width of the gap the container actually left empty.
        var thickness = Math.Max(1, split.Thickness * _scale);
        var grab = Math.Max(thickness, 6);
        var overhang = (grab - thickness) / 2;
        if (split.Vertical)
        {
            Width = grab;
            _divider.Width = thickness;
            _divider.Height = double.NaN;
            Canvas.SetLeft(this, split.Position * _scale - overhang);
            Canvas.SetTop(this, 0);
            ProtectedCursor = InputSystemCursor.Create(InputSystemCursorShape.SizeWestEast);
        }
        else
        {
            Height = grab;
            _divider.Height = thickness;
            _divider.Width = double.NaN;
            Canvas.SetTop(this, split.Position * _scale - overhang);
            Canvas.SetLeft(this, 0);
            ProtectedCursor = InputSystemCursor.Create(InputSystemCursorShape.SizeNorthSouth);
        }
        AutomationProperties.SetName(this, split.Vertical ? "Vertical splitter" : "Horizontal splitter");
    }

    // A splitter divides the whole container, so it spans it across its own axis.
    public void ApplyExtent(double width, double height)
    {
        if (_split.Vertical) Height = Math.Max(0, height);
        else Width = Math.Max(0, width);
    }

    // Native client pixels for a point in this splitter's own coordinate space.
    private int PositionFor(Point point)
    {
        var container = Parent as FrameworkElement;
        var inContainer = container is null
            ? point
            : TransformToVisual(container).TransformPoint(point);
        var device = (_split.Vertical ? inContainer.X : inContainer.Y) - _grabOffset;
        var native = (int)Math.Round(device / _scale);
        return Math.Clamp(native, _split.Minimum, _split.Maximum);
    }

    private void Emit(int position, bool force)
    {
        if (position == _lastEmitted) return;
        if (!force && _sinceEmit.ElapsedMilliseconds < EmitIntervalMilliseconds) return;
        _lastEmitted = position;
        _sinceEmit.Restart();
        _positionRequested(position);
    }

    private void OnPointerPressed(object sender, PointerRoutedEventArgs args)
    {
        var point = args.GetCurrentPoint(this).Position;
        // Grabbing anywhere on the strip keeps the pointer on the same spot of the
        // splitter for the whole drag instead of snapping its edge to the cursor.
        _grabOffset = _split.Vertical ? point.X : point.Y;
        _dragging = CapturePointer(args.Pointer);
        Focus(FocusState.Pointer);
        args.Handled = true;
    }

    private void OnPointerMoved(object sender, PointerRoutedEventArgs args)
    {
        if (!_dragging) return;
        Emit(PositionFor(args.GetCurrentPoint(this).Position), force: false);
        args.Handled = true;
    }

    private void OnPointerReleased(object sender, PointerRoutedEventArgs args)
    {
        if (!_dragging) return;
        Emit(PositionFor(args.GetCurrentPoint(this).Position), force: true);
        ReleasePointerCapture(args.Pointer);
        _dragging = false;
        args.Handled = true;
    }

    private void OnKeyDown(object sender, KeyRoutedEventArgs args)
    {
        var step = args.Key switch
        {
            VirtualKey.Left or VirtualKey.Up => -KeyboardStep,
            VirtualKey.Right or VirtualKey.Down => KeyboardStep,
            _ => 0,
        };
        if (step == 0) return;
        args.Handled = true;
        RequestPosition(_split.Position + step);
    }

    public void RequestPosition(int position) =>
        Emit(Math.Clamp(position, _split.Minimum, _split.Maximum), force: true);

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticPaneSplitterAutomationPeer(this);
}

// A splitter is a thumb whose value is where it sits, which is exactly the UIA
// RangeValue contract and what lets an assistive client move it.
internal sealed class SemanticPaneSplitterAutomationPeer(SemanticPaneSplitter owner)
    : FrameworkElementAutomationPeer(owner), IRangeValueProvider
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        AutomationControlType.Thumb;
    protected override string GetClassNameCore() => "ProjectedPaneSplitter";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => false;
    protected override object GetPatternCore(PatternInterface patternInterface) =>
        patternInterface == PatternInterface.RangeValue
            ? this
            : base.GetPatternCore(patternInterface);

    public bool IsReadOnly => false;
    public double Maximum => owner.Split.Maximum;
    public double Minimum => owner.Split.Minimum;
    public double Value => owner.Split.Position;
    public double LargeChange => 32;
    public double SmallChange => 8;

    public void SetValue(double value) =>
        owner.RequestPosition((int)Math.Round(value));
}

internal sealed class SemanticMdiClientAutomationPeer(SemanticMdiClient owner) :
    FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        ControlFactory.AutomationControlTypeFor("mdiClient");
    protected override string GetClassNameCore() => "MDIClient";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => false;
}

internal readonly record struct MdiChildPresentation(
    string Title,
    bool Active,
    string WindowState,
    double CaptionHeight,
    global::Windows.Foundation.Point ClientOffset,
    global::Windows.Foundation.Size ClientSize,
    bool CanMinimize,
    bool CanMaximize,
    bool CanClose);

// One window inside the window.  The card is the native frame's rectangle, the
// caption band is exactly the native frame inset, and the content canvas is the
// native client band, so nothing about the child's own controls moves.
internal sealed class SemanticMdiChildControl : ContentControl, ISemanticContainer
{
    private const double CaptionFontSize = 12;
    private const double CaptionButtonWidth = 30;
    private readonly Action<string> _command;
    private readonly Canvas _root = new() { Background = null };
    private readonly Border _card = new() { CornerRadius = new CornerRadius(6), BorderThickness = new Thickness(1) };
    private readonly Border _caption = new();
    private readonly TextBlock _title = new()
    {
        FontSize = CaptionFontSize,
        VerticalAlignment = VerticalAlignment.Center,
        Margin = new Thickness(8, 0, 0, 0),
        TextTrimming = TextTrimming.CharacterEllipsis,
    };
    private readonly StackPanel _captionButtons = new()
    {
        Orientation = Orientation.Horizontal,
        HorizontalAlignment = HorizontalAlignment.Right,
        VerticalAlignment = VerticalAlignment.Stretch,
    };
    private readonly Canvas _client = new() { Background = null };
    private readonly Dictionary<string, Button> _captionCommands = new(StringComparer.Ordinal);
    private string _titleText = string.Empty;

    public SemanticMdiChildControl(Action<string> command)
    {
        _command = command;
        var captionGrid = new Grid();
        captionGrid.Children.Add(_title);
        captionGrid.Children.Add(_captionButtons);
        _caption.Child = captionGrid;
        foreach (var (verb, glyph, name) in new[]
        {
            (MdiCommands.Minimize, "\uE921", "Minimize"),
            (MdiCommands.Maximize, "\uE922", "Maximize"),
            (MdiCommands.Restore, "\uE923", "Restore"),
            (MdiCommands.Close, "\uE8BB", "Close"),
        })
        {
            // Caption buttons are created once and only shown or hidden per
            // revision: rebuilding them would replace the UIA elements a client is
            // holding, and take focus with them, on every native repaint.
            var button = new Button
            {
                Content = new FontIcon { Glyph = glyph, FontSize = 10 },
                Width = CaptionButtonWidth,
                MinWidth = 0,
                MinHeight = 0,
                Padding = new Thickness(0),
                Background = null,
                BorderThickness = new Thickness(0),
                IsTabStop = false,
                Visibility = Visibility.Collapsed,
            };
            AutomationProperties.SetName(button, name);
            button.Click += (_, _) => _command(verb);
            _captionCommands.Add(verb, button);
            _captionButtons.Children.Add(button);
        }
        _card.Child = new Canvas { Background = null };
        Content = _root;
        Background = null;
        BorderThickness = new Thickness(0);
        Padding = new Thickness(0);
        IsTabStop = false;
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        VerticalContentAlignment = VerticalAlignment.Stretch;
        _root.Children.Add(_card);
        _root.Children.Add(_caption);
        _root.Children.Add(_client);
        Canvas.SetZIndex(_card, 0);
        Canvas.SetZIndex(_caption, 1);
        Canvas.SetZIndex(_client, 2);
        _caption.Tapped += (_, args) =>
        {
            _command(MdiCommands.Activate);
            args.Handled = true;
        };
        // The factory sets this element's size after construction, so the card and
        // caption follow the arranged size rather than reading a not-yet-assigned
        // Width.  A card that does not fill the element would also collapse the
        // node's UIA bounding rectangle onto whatever happens to render inside it.
        SizeChanged += (_, args) => LayoutCard(args.NewSize.Width, args.NewSize.Height);
    }

    private void LayoutCard(double width, double height)
    {
        _card.Width = Math.Max(0, width);
        _card.Height = Math.Max(0, height);
        _caption.Width = Math.Max(0, width);
    }

    public UIElementCollection Children => _client.Children;

    public string CaptionTitle => _titleText;

    public void Apply(MdiChildPresentation presentation)
    {
        _titleText = presentation.Title;
        _title.Text = presentation.Title;
        AutomationProperties.SetName(_title, presentation.Title);
        _card.Background = ThemeBrush("SolidBackgroundFillColorSecondaryBrush", Microsoft.UI.Colors.White);
        _card.BorderBrush = ThemeBrush("CardStrokeColorDefaultBrush", Microsoft.UI.Colors.Gray);
        _caption.Background = presentation.Active
            ? ThemeBrush("AccentFillColorDefaultBrush", Microsoft.UI.Colors.SteelBlue)
            : ThemeBrush("CardStrokeColorDefaultBrush", Microsoft.UI.Colors.LightGray);
        _title.Foreground = presentation.Active
            ? ThemeBrush("TextOnAccentFillColorPrimaryBrush", Microsoft.UI.Colors.White)
            : ThemeBrush("TextFillColorPrimaryBrush", Microsoft.UI.Colors.Black);
        _caption.CornerRadius = new CornerRadius(6, 6, 0, 0);
        _caption.Height = presentation.CaptionHeight;
        LayoutCard(ActualWidth, ActualHeight);
        Canvas.SetLeft(_client, presentation.ClientOffset.X);
        Canvas.SetTop(_client, presentation.ClientOffset.Y);
        _client.Width = presentation.ClientSize.Width;
        _client.Height = presentation.ClientSize.Height;
        _client.Clip = new RectangleGeometry
        {
            Rect = new global::Windows.Foundation.Rect(
                0, 0, presentation.ClientSize.Width, presentation.ClientSize.Height),
        };
        RebuildCaptionButtons(presentation);
    }

    private void RebuildCaptionButtons(MdiChildPresentation presentation)
    {
        var minimized = presentation.WindowState == "minimized";
        var maximized = presentation.WindowState == "maximized";
        // Each verb is offered only when the native frame's own style and state
        // accept it, so the projection can never present a command the native
        // child would refuse.
        var offered = new Dictionary<string, bool>(StringComparer.Ordinal)
        {
            [MdiCommands.Minimize] = presentation.CanMinimize && !minimized,
            [MdiCommands.Maximize] = presentation.CanMaximize && !minimized && !maximized,
            [MdiCommands.Restore] = minimized || maximized,
            [MdiCommands.Close] = presentation.CanClose,
        };
        foreach (var (verb, button) in _captionCommands)
        {
            button.Visibility = offered[verb] ? Visibility.Visible : Visibility.Collapsed;
            button.Height = Math.Max(0, presentation.CaptionHeight);
            button.Foreground = _title.Foreground;
        }
    }

    private static Brush ThemeBrush(string key, global::Windows.UI.Color fallback) =>
        Application.Current?.Resources is { } resources &&
        resources.TryGetValue(key, out var value) && value is Brush brush
            ? brush
            : new SolidColorBrush(fallback);

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticMdiChildAutomationPeer(this);
}

internal sealed class SemanticMdiChildAutomationPeer(SemanticMdiChildControl owner) :
    FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        ControlFactory.AutomationControlTypeFor("mdiChild");
    protected override string GetClassNameCore() => "MDIChild";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => false;
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
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => true;
    protected override global::Windows.Foundation.Rect GetBoundingRectangleCore() =>
        ControlFactory.PhysicalLayoutBounds(owner, base.GetBoundingRectangleCore());
}

// A toolbar separator.  WinUI's AppBarSeparator publishes no automation peer of its
// own, so a bare one would vanish from the accessibility tree the moment it left a
// CommandBar -- the native control's separators would simply stop existing for a
// screen reader.  Its theme margin also collapses the rule to nothing inside a strip
// as narrow as the native gap, so the line is drawn directly and the wrapper supplies
// the Separator peer.
internal sealed class SemanticToolbarSeparatorControl : ContentControl
{
    public SemanticToolbarSeparatorControl()
    {
        Content = new Border
        {
            Width = 1,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Stretch,
            Margin = new Thickness(0, 3, 0, 3),
            Background = new SolidColorBrush(
                Microsoft.UI.ColorHelper.FromArgb(0x80, 0x80, 0x80, 0x80)),
            IsHitTestVisible = false,
        };
        Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        MinWidth = 0;
        MinHeight = 0;
        Padding = new Thickness(0);
        BorderThickness = new Thickness(0);
        IsTabStop = false;
        IsHitTestVisible = false;
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        VerticalContentAlignment = VerticalAlignment.Stretch;
    }

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticToolbarSeparatorAutomationPeer(this);
}

internal sealed class SemanticToolbarSeparatorAutomationPeer(SemanticToolbarSeparatorControl owner)
    : FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        AutomationControlType.Separator;
    protected override string GetClassNameCore() => "Separator";
    protected override string GetNameCore() => string.Empty;
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => false;
    protected override IList<AutomationPeer> GetChildrenCore() => [];
    protected override global::Windows.Foundation.Rect GetBoundingRectangleCore() =>
        ControlFactory.PhysicalLayoutBounds(owner, base.GetBoundingRectangleCore());
}

internal sealed class SemanticToolbarControl : ContentControl
{
    private readonly Canvas _canvas = new()
    {
        Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
    };
    private readonly List<FrameworkElement> _items = [];

    public SemanticToolbarControl()
    {
        Content = _canvas;
        Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        MinWidth = 0;
        MinHeight = 0;
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
    protected override global::Windows.Foundation.Rect GetBoundingRectangleCore() =>
        ControlFactory.PhysicalLayoutBounds(owner, base.GetBoundingRectangleCore());
    protected override IList<AutomationPeer> GetChildrenCore() => owner.Items
        .Where(item => item.Visibility == Visibility.Visible)
        .Select(CreatePeerForElement)
        .Where(peer => peer is not null)
        .Cast<AutomationPeer>()
        .ToList();
}
