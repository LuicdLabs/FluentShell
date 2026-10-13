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
    internal const int PaneChromeZIndex = -1;
    internal const int DialogIconZIndex = ProtocolConstants.MaxNodes;
    internal const int PaneSplitterZIndex = 1000;
    private const double NativeFontSize = 12;
    private const double MainInstructionFontSize = 15;
    // The floor for a label fitted into the width the native control committed to.  A
    // menu bar rendered a fraction smaller is readable; one that reads "Favorit" is not.
    private const double MinimumFittedFontSize = 9;
    // The horizontal padding of a projected toolbar button around its content.
    private const double ToolbarLabelPadding = 16;
    private static readonly BooleanToVisibilityConverter VisibilityConverter = new();
    private static readonly Win32MnemonicTextConverter MnemonicTextConverter = new();
    private readonly double _scale;
    private readonly Action<ControlNodeViewModel, string, object?> _action;
    private readonly Func<bool> _isApplyingCanonical;
    private readonly Func<bool> _isImeComposing;
    private readonly PresentationLifetime _lifetime;
    private readonly IReadOnlyDictionary<string, string> _radioGroups;
    private readonly IReadOnlyDictionary<string, ControlNodeViewModel> _nodes;

    private readonly ItemPressTracker _itemPresses;

    public ControlFactory(
        int dpi,
        IEnumerable<ControlNodeViewModel> nodes,
        Action<ControlNodeViewModel, string, object?> action,
        Func<bool> isApplyingCanonical,
        Func<bool> isImeComposing,
        PresentationLifetime lifetime,
        ItemPressTracker? itemPresses = null)
    {
        _scale = 96.0 / dpi;
        _lifetime = lifetime;
        _itemPresses = itemPresses ?? new ItemPressTracker();
        _action = (node, kind, value) => lifetime.Invoke(() => action(node, kind, value));
        _isApplyingCanonical = () => !lifetime.IsActive || isApplyingCanonical();
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
            "staticIcon" or "staticBitmap" => CreateStaticIcon(viewModel),
            "upDown" => CreateUpDown(viewModel),
            "datePicker" => CreateDatePicker(viewModel),
            "timePicker" => CreateTimePicker(viewModel),
            "staticDecoration" => CreateStaticDecoration(viewModel),
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
            "nativeDocument" => CreateNativeDocument(viewModel),
            "diskView" => CreateDiskView(viewModel),
            "richText" => CreateRichText(viewModel),
            "performanceGraph" => CreatePerformanceGraph(viewModel),
            "performanceLegend" => CreatePerformanceLegend(viewModel),
            "performanceValueBar" => CreatePerformanceValueBar(viewModel),
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
        else if (viewModel.Kind != "staticDecoration")
        {
            Bind(element, UIElement.IsHitTestVisibleProperty, nameof(viewModel.Enabled), BindingMode.OneWay);
            // Containers report the native enabled state through their peers.
            _lifetime.Subscribe(viewModel, (_, args) =>
            {
                if (args.PropertyName == nameof(viewModel.Enabled) &&
                    FrameworkElementAutomationPeer.FromElement(element) is { } peer)
                    peer.RaisePropertyChangedEvent(AutomationElementIdentifiers.IsEnabledProperty,
                        !viewModel.Enabled, viewModel.Enabled);
            });
        }
        ApplyBounds(element, viewModel);
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.TabStop) or nameof(viewModel.TabIndex))
            {
                element.IsTabStop = viewModel.Kind == "sysLink" ? false : viewModel.TabStop;
                element.TabIndex = Math.Max(0, viewModel.TabIndex);
            }
            if (args.PropertyName == nameof(viewModel.Rect)) ApplyBounds(element, viewModel);
            if (args.PropertyName == nameof(viewModel.AutomationName) ||
                args.PropertyName == nameof(viewModel.Text))
            {
                ApplyAutomationAndAccessKey(element, viewModel);
            }
        });
        if (viewModel.ParentNodeId is { } parentNodeId && _nodes.TryGetValue(parentNodeId, out var parent))
        {
            _lifetime.Subscribe(parent, (_, args) =>
            {
                if (args.PropertyName == nameof(parent.Rect)) ApplyBounds(element, viewModel);
            });
        }
        Canvas.SetZIndex(element, ProjectionZIndexFor(viewModel));
        return element;
    }

    internal static int ProjectionZIndexFor(ControlNodeViewModel viewModel) =>
        // HWND children are captured front to back; XAML draws larger indexes
        // in front. Keep nodes in 0..511 so container chrome stays behind them.
        // Virtual dialogs and DirectUI semantic slots already carry paint order,
        // including their HWND-backed slots, and must not be reversed.
        viewModel.NativeHwnd is not null && viewModel.AdapterId.Length == 0
            ? ProtocolConstants.MaxNodes - 1 - viewModel.ZIndex
            : viewModel.ZIndex;

    private FrameworkElement CreateStaticDecoration(ControlNodeViewModel viewModel)
    {
        var surface = new Grid { IsHitTestVisible = false, IsTabStop = false };
        AutomationProperties.SetAccessibilityView(surface, AccessibilityView.Raw);
        void ApplyPresentation()
        {
            var presentation = StaticDecorationPresentation.FromStyles(viewModel.Style, viewModel.ExStyle);
            surface.Children.Clear();
            void Add(Border border)
            {
                border.IsHitTestVisible = false;
                border.IsTabStop = false;
                AutomationProperties.SetAccessibilityView(border, AccessibilityView.Raw);
                surface.Children.Add(border);
            }
            if (presentation.FillTone is { } fill)
            {
                Add(new Border
                {
                    Margin = new Thickness(presentation.FillInset * _scale),
                    Background = StaticDecorationBrush(fill),
                });
            }
            foreach (var stroke in presentation.Strokes)
            {
                var leading = (stroke.Edges & StaticDecorationEdges.TopLeft) != 0 ? _scale : 0;
                var trailing = (stroke.Edges & StaticDecorationEdges.BottomRight) != 0 ? _scale : 0;
                Add(new Border
                {
                    Margin = new Thickness(stroke.Inset * _scale),
                    BorderThickness = new Thickness(leading, leading, trailing, trailing),
                    BorderBrush = StaticDecorationBrush(stroke.Tone),
                    Background = null,
                });
            }
        }
        ApplyPresentation();
        surface.ActualThemeChanged += (_, _) => ApplyPresentation();
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Style) or nameof(viewModel.ExStyle))
                ApplyPresentation();
        });
        return surface;
    }

    private static Brush StaticDecorationBrush(StaticDecorationTone tone)
    {
        var (key, fallback) = tone switch
        {
            StaticDecorationTone.Dark => ("TextFillColorPrimaryBrush", Microsoft.UI.Colors.Black),
            StaticDecorationTone.Gray => ("TextFillColorSecondaryBrush", Microsoft.UI.Colors.Gray),
            StaticDecorationTone.Light => ("SolidBackgroundFillColorBaseBrush", Microsoft.UI.Colors.White),
            StaticDecorationTone.Shadow => ("ControlStrongStrokeColorDefaultBrush", Microsoft.UI.Colors.Gray),
            _ => ("ControlStrokeColorOnAccentSecondaryBrush", Microsoft.UI.Colors.White),
        };
        return Application.Current?.Resources is { } resources &&
            resources.TryGetValue(key, out var value) && value is Brush brush
                ? brush : new SolidColorBrush(fallback);
    }

    // A native Static draws its text with DrawText: left, centred or right as SS_LEFT / SS_CENTER /
    // SS_RIGHT say, wrapped at the control's width unless it is SS_SIMPLE or SS_LEFTNOWORDWRAP, from the
    // top of the rectangle unless SS_CENTERIMAGE centres it, with the `&` mnemonic markup stripped unless
    // SS_NOPREFIX, an ellipsis for the SS_*ELLIPSIS styles -- and clips whatever does not fit to the
    // control's own rectangle.  A label that is too narrow for its text therefore shows its first line,
    // where a centred, unclipped TextBlock showed the middle of two.
    private ContentControl CreateStatic(ControlNodeViewModel viewModel)
    {
        const ulong ssTypeMask = 0x1F, ssCenter = 1, ssRight = 2, ssSimple = 0x0B, ssLeftNoWordWrap = 0x0C;
        const ulong ssNoPrefix = 0x80, ssCenterImage = 0x200, ssEllipsisMask = 0xC000;
        var text = new TextBlock { FontSize = NativeFontSize };
        // A task dialog's main instruction is its heading (DialogSnapshots.cpp
        // measures it at the same size and weight).
        if (viewModel.PresentationVariant == "mainInstruction")
        {
            text.FontSize = MainInstructionFontSize;
            text.FontWeight = new global::Windows.UI.Text.FontWeight { Weight = 600 };
        }
        void ApplyStyle()
        {
            var type = viewModel.Style & ssTypeMask;
            var ellipsis = (viewModel.Style & ssEllipsisMask) != 0;
            text.TextAlignment = type switch
            {
                ssCenter => TextAlignment.Center,
                ssRight => TextAlignment.Right,
                _ => TextAlignment.Left,
            };
            text.TextWrapping = type is ssSimple or ssLeftNoWordWrap || ellipsis ? TextWrapping.NoWrap : TextWrapping.Wrap;
            text.TextTrimming = ellipsis ? TextTrimming.CharacterEllipsis : TextTrimming.None;
            // Stretch, not Top: a TextBlock draws its text from the top of a slot taller than it, and the
            // slot is what the automation peer reports as the control's bounds (it expands a centred
            // text's rectangle symmetrically to the control's size, which is wrong for a top-aligned one).
            text.VerticalAlignment = (viewModel.Style & ssCenterImage) != 0 ? VerticalAlignment.Center : VerticalAlignment.Stretch;
        }
        ApplyStyle();
        // A Static's label carries the same `&` mnemonic markup a Button's does: the
        // native control draws the letter underlined, not the ampersand.  Projecting the
        // raw string shows "&Number to dial:" where the application shows "Number to
        // dial:", and makes the label wider than the slot the control sized for it.
        if ((viewModel.Style & ssNoPrefix) != 0)
            Bind(text, TextBlock.TextProperty, nameof(viewModel.Text), BindingMode.OneWay);
        else
            Bind(text, TextBlock.TextProperty, nameof(viewModel.Text), BindingMode.OneWay,
                MnemonicTextConverter);
        AutomationProperties.SetAccessibilityView(text, AccessibilityView.Raw);
        var control = new SemanticStaticTextControl
        {
            Content = text,
            MinWidth = 0,
            MinHeight = 0,
            Padding = new Thickness(0),
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch,
            IsHitTestVisible = false,
        };
        // The rectangle is at least one line tall: WinUI's line box is taller than the native font's, so a
        // native label one native line high would lose the descenders of its only line.
        control.SizeChanged += (_, args) =>
            control.Clip = new RectangleGeometry
            {
                Rect = new Rect(0, 0, args.NewSize.Width, Math.Max(args.NewSize.Height, text.FontSize * 1.4)),
            };
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.Style)) ApplyStyle();
        });
        return control;
    }

    // DirectUI bitmap variants are drawn stretched to their slot; a classic SS_ICON
    // keeps its pixels at their own size.
    internal static Stretch StaticIconStretch(string presentationVariant) =>
        presentationVariant is "bitmapDisplay" or "monitorPalette" ? Stretch.Fill : Stretch.None;

    // A Static icon is an Image inside a semantic wrapper whose peer republishes the
    // node.  The wrapper is a Control, so it carries the native enabled state into UIA
    // -- a bare Image always reports itself enabled, which a disabled window (an owner
    // behind its modal dialog) contradicts.  The Image itself leaves the control view.
    private FrameworkElement CreateStaticIcon(ControlNodeViewModel viewModel)
    {
        var image = new Image
        {
            // A static bitmap's pixels already span its whole client area.
            Stretch = viewModel.Kind == "staticBitmap"
                ? Stretch.Fill : StaticIconStretch(viewModel.PresentationVariant),
            IsHitTestVisible = false,
        };
        void ApplyPixels() => image.Source = BitmapFromPixels(
            viewModel.ImageWidth, viewModel.ImageHeight, DecodeImagePixels(viewModel));
        ApplyPixels();
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.ImageData)) ApplyPixels();
        });
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
        var maxDimension = viewModel.Kind == "staticBitmap" ||
            viewModel.PresentationVariant is "bitmapDisplay" or "bitmapSwitch" or "monitorPalette"
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
        // The frame and caption take no input of their own: what lies under the frame's line
        // (a Win32 group box's controls are its siblings) is clicked through it, and a
        // Windows Forms GroupBox's controls are placed in the layer above.
        var border = new Border
        {
            Margin = new Thickness(0, 8, 0, 0),
            BorderBrush = new SolidColorBrush(Microsoft.UI.Colors.Gray),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(4),
            IsHitTestVisible = false,
        };
        var caption = new TextBlock
        {
            FontSize = NativeFontSize,
            Margin = new Thickness(10, 0, 0, 0),
            Padding = new Thickness(4, 0, 4, 0),
            VerticalAlignment = VerticalAlignment.Top,
            IsHitTestVisible = false,
        };
        Bind(caption, TextBlock.TextProperty, nameof(viewModel.Text), BindingMode.OneWay, MnemonicTextConverter);
        var children = new Canvas { Background = null };
        var layout = new Grid();
        layout.Children.Add(border);
        layout.Children.Add(caption);
        layout.Children.Add(children);
        return new SemanticGroupControl(children)
        {
            Content = layout,
            HorizontalContentAlignment = HorizontalAlignment.Stretch,
            VerticalContentAlignment = VerticalAlignment.Stretch,
        };
    }

    private SemanticProgressBarControl CreateProgressBar(ControlNodeViewModel viewModel)
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
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.Indeterminate))
                control.IsIndeterminate = viewModel.Indeterminate;
            else if (args.PropertyName is nameof(viewModel.Minimum) or nameof(viewModel.Maximum) or nameof(viewModel.Position))
                ApplyNativeState();
        });
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
        if (viewModel.PresentationVariant is "commandLink" or "commandLinkNoIcon" or "commandLinkElevated")
        {
            var content = new StackPanel { Orientation = Orientation.Vertical };
            var title = new TextBlock
            {
                FontWeight = new global::Windows.UI.Text.FontWeight { Weight = 600 },
                TextWrapping = TextWrapping.Wrap,
            };
            Bind(title, TextBlock.TextProperty, nameof(viewModel.Text), BindingMode.OneWay, MnemonicTextConverter);
            content.Children.Add(title);
            if (!string.IsNullOrEmpty(viewModel.HelpText))
            {
                content.Children.Add(new TextBlock
                {
                    Text = viewModel.HelpText,
                    Opacity = 0.75,
                    TextWrapping = TextWrapping.Wrap,
                });
            }
            // A command link leads with an arrow, as comctl32 draws one, or with the shield
            // when the command needs elevation.
            var glyph = viewModel.PresentationVariant switch
            {
                "commandLinkElevated" => "",
                "commandLinkNoIcon" => null,
                _ => "",
            };
            control.Content = glyph is null ? content : WithLeadingGlyph(content, glyph, 16, VerticalAlignment.Top);
            control.HorizontalContentAlignment = HorizontalAlignment.Stretch;
            control.VerticalContentAlignment = VerticalAlignment.Center;
            control.Padding = new Thickness(10, 6, 10, 6);
        }
        else if (viewModel.PresentationVariant is "elevated" or "expandoCollapsed" or "expandoExpanded")
        {
            var label = new TextBlock { VerticalAlignment = VerticalAlignment.Center };
            Bind(label, TextBlock.TextProperty, nameof(viewModel.Text), BindingMode.OneWay, MnemonicTextConverter);
            var glyph = viewModel.PresentationVariant switch
            {
                "elevated" => "",
                "expandoExpanded" => "",
                _ => "",
            };
            control.Content = WithLeadingGlyph(label, glyph, 12, VerticalAlignment.Center);
            if (viewModel.PresentationVariant != "elevated")
            {
                // The expando button is a disclosure toggle, not a command button.
                control.HorizontalContentAlignment = HorizontalAlignment.Left;
                control.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
                control.BorderBrush = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            }
        }
        else if (viewModel.PresentationVariant == "navigationBack")
        {
            // A wizard's Back is the arrow its header draws, not a labelled button; the
            // label ("上一步(B)") stays the accessible name and becomes the tooltip.
            var arrow = new FontIcon { Glyph = "", FontSize = 14 };
            AutomationProperties.SetAccessibilityView(arrow, AccessibilityView.Raw);
            control.Content = arrow;
            control.Padding = new Thickness(0);
            control.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            control.BorderBrush = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            ToolTipService.SetToolTip(control, Win32Mnemonic.DisplayText(viewModel.Text));
        }
        else if (!string.IsNullOrEmpty(viewModel.ImageData))
        {
            // A BS_ICON / BS_BITMAP button shows its picture, centred at its own size,
            // instead of its text; the text stays the accessible name.
            var picture = new Image { Stretch = Stretch.None, IsHitTestVisible = false };
            void ApplyPicture() => picture.Source = BitmapFromBase64(
                viewModel.ImageWidth, viewModel.ImageHeight, viewModel.ImageFormat, viewModel.ImageData);
            ApplyPicture();
            _lifetime.Subscribe(viewModel, (_, args) =>
            {
                if (args.PropertyName == nameof(viewModel.ImageData) &&
                    !string.IsNullOrEmpty(viewModel.ImageData)) ApplyPicture();
            });
            AutomationProperties.SetAccessibilityView(picture, AccessibilityView.Raw);
            control.Content = picture;
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

    // A glyph beside a button's content; the glyph is decoration and stays out of
    // the accessibility tree.
    private static Grid WithLeadingGlyph(UIElement content, string glyph, double size, VerticalAlignment alignment)
    {
        var icon = new FontIcon
        {
            Glyph = glyph,
            FontSize = size,
            Margin = new Thickness(0, alignment == VerticalAlignment.Top ? 2 : 0, 8, 0),
            VerticalAlignment = alignment,
        };
        AutomationProperties.SetAccessibilityView(icon, AccessibilityView.Raw);
        var layout = new Grid();
        layout.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        layout.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        layout.Children.Add(icon);
        Grid.SetColumn((FrameworkElement)content, 1);
        layout.Children.Add(content);
        return layout;
    }

    private CheckBox CreateCheckBox(ControlNodeViewModel viewModel, bool threeState)
    {
        // WinUI's CheckBox is 120 px wide at least: a narrower native one (a 65 px "Hidden" box)
        // would report bounds the native control does not have.
        var control = new CheckBox
        {
            Style = CompactControlStyles.CheckBox,
            FontSize = NativeFontSize,
            MinWidth = 0,
            MinHeight = 0,
            // The default 5 px of top padding (and a top-aligned label) push the text of a native-sized
            // box -- 13 to 16 px tall -- past the control's bottom edge, where it is clipped.
            Padding = CompactControlStyles.LabelPadding,
            VerticalContentAlignment = VerticalAlignment.Center,
            // A native check box answers a click anywhere in its rectangle; a template whose root has no
            // background answers only on its glyph and text.
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            IsThreeState = threeState,
            IsChecked = ToNullableBool(viewModel.Checked),
        };
        CompactControlStyles.WrapWhenTall(control);
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
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.Checked)) control.IsChecked = ToNullableBool(viewModel.Checked);
        });
        return control;
    }

    private RadioButton CreateRadioButton(ControlNodeViewModel viewModel)
    {
        var control = new RadioButton
        {
            Style = CompactControlStyles.RadioButton,
            FontSize = NativeFontSize,
            MinWidth = 0,
            MinHeight = 0,
            Padding = CompactControlStyles.LabelPadding,
            VerticalContentAlignment = VerticalAlignment.Center,
            // Hit-testable over the whole rectangle, like the native control (see CreateCheckBox).
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent),
            IsChecked = viewModel.Checked == 1,
            GroupName = _radioGroups[viewModel.NodeId],
        };
        CompactControlStyles.WrapWhenTall(control);
        Bind(control, ContentControl.ContentProperty, nameof(viewModel.Text), BindingMode.OneWay, MnemonicTextConverter);
        control.Checked += (_, _) =>
        {
            if (!_isApplyingCanonical() && viewModel.Checked != 1 &&
                AllowsAction(viewModel, "setCheck"))
                _action(viewModel, "setCheck", 1);
        };
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.Checked)) control.IsChecked = viewModel.Checked == 1;
        });
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
        void ApplyPixels() => image.Source = BitmapFromPixels(
            viewModel.ImageWidth, viewModel.ImageHeight, DecodeImagePixels(viewModel));
        ApplyPixels();
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.ImageData)) ApplyPixels();
        });
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
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.Checked))
                control.IsChecked = viewModel.Checked == 1;
        });
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
        BindCueBanner(viewModel, text => control.PlaceholderText = text);
        Bind(control, TextBox.TextProperty, nameof(viewModel.DraftText), BindingMode.TwoWay);
        // A retained Value peer can still change a detached control. Remove its
        // two-way source link before that edit can reach the replacement tree.
        _lifetime.OnDispose(() => control.ClearValue(TextBox.TextProperty));
        var timer = control.DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(300);
        timer.IsRepeating = false;
        _lifetime.OnDispose(timer.Stop);
        string? pendingText = null;
        void CommitDraft()
        {
            timer.Stop();
            if (_isApplyingCanonical() || !AllowsAction(viewModel, "setText") ||
                control.Text == viewModel.Text || control.Text == pendingText) return;
            if (_isImeComposing())
            {
                timer.Start();
                return;
            }
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
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.Text)) pendingText = null;
            if (args.PropertyName is nameof(viewModel.SelectionStart) or nameof(viewModel.SelectionLength))
                ApplySelection();
        });
        return control;
    }

    // A read-only rich edit control in its formats (minor 35): a scroller around a text block of
    // runs, so a long text scrolls and a selection copies the way the native control's does. It
    // reports as a pane (the scroller's role), named by the control's text.
    private ScrollViewer CreateRichText(ControlNodeViewModel viewModel)
    {
        var block = new RichTextBlock
        {
            IsTextSelectionEnabled = true,
            TextWrapping = TextWrapping.Wrap,
            FontSize = NativeFontSize,
            Padding = new Thickness(4, 0, 4, 0),
        };
        var control = new ScrollViewer
        {
            Content = block,
            MinWidth = 0,
            MinHeight = 0,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            VerticalScrollMode = ScrollMode.Auto,
            HorizontalScrollMode = ScrollMode.Disabled,
        };
        void Apply() => RichTextPresenter.Apply(block, viewModel.RichText, NativeFontSize);
        Apply();
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.RichText)) Apply();
        });
        return control;
    }

    // Performance Monitor's chart (minor 36): the graph, its legend and its value strip are drawn
    // from the data the Bridge read from the System Monitor control.
    private PerformanceGraphControl CreatePerformanceGraph(ControlNodeViewModel viewModel)
    {
        var control = new PerformanceGraphControl(_scale, NativeFontSize);
        control.Apply(viewModel.PerformanceGraph);
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.PerformanceGraph)) control.Apply(viewModel.PerformanceGraph);
        });
        return control;
    }

    private PerformanceLegendControl CreatePerformanceLegend(ControlNodeViewModel viewModel)
    {
        // A click on a row selects the counter, and the Show box toggles whether it is plotted: the
        // same select and setItemCheck requests a list makes, answered by the control behind it.
        var control = new PerformanceLegendControl(_scale, NativeFontSize,
            index =>
            {
                if (!_isApplyingCanonical() && AllowsAction(viewModel, "select")) _action(viewModel, "select", index);
            },
            (index, visible) =>
            {
                if (!_isApplyingCanonical() && AllowsAction(viewModel, "setItemCheck"))
                    _action(viewModel, "setItemCheck", new ListViewCheckActionValue { Index = index, Checked = visible });
            });
        control.Apply(viewModel.PerformanceLegend);
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.PerformanceLegend)) control.Apply(viewModel.PerformanceLegend);
        });
        return control;
    }

    private PerformanceValueBarControl CreatePerformanceValueBar(ControlNodeViewModel viewModel)
    {
        var control = new PerformanceValueBarControl(NativeFontSize);
        control.Apply(viewModel.PerformanceValueBar);
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.PerformanceValueBar)) control.Apply(viewModel.PerformanceValueBar);
        });
        return control;
    }

    // The grey hint the native box shows while it is empty (its cue banner), kept current.
    private void BindCueBanner(ControlNodeViewModel viewModel, Action<string> apply)
    {
        apply(viewModel.CueBanner);
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.CueBanner)) apply(viewModel.CueBanner);
        });
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
        BindCueBanner(viewModel, text => control.PlaceholderText = text);
        control.PasswordChanged += (_, _) =>
        {
            if (!_lifetime.IsActive) return;
            viewModel.DraftText = control.Password;
            if (!_isApplyingCanonical() && AllowsAction(viewModel, "setText") &&
                control.Password != viewModel.Text) _action(viewModel, "setText", control.Password);
        };
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.DraftText) && control.Password != viewModel.DraftText) control.Password = viewModel.DraftText;
        });
        return control;
    }

    private ComboBox CreateComboBox(ControlNodeViewModel viewModel)
    {
        var control = new OwnerDrawComboBoxControl
        {
            FontSize = NativeFontSize,
            MinHeight = 0,
            // The default 12,5,0,7 padding leaves a native 21 to 24 px combo 10 px for its 16 px line.
            Padding = new Thickness(8, 0, 0, 0),
            ItemsSource = viewModel.Items,
            SelectedIndex = viewModel.SelectedIndex,
            IsEditable = viewModel.Editable,
            Text = viewModel.DraftText,
        };
        BindCueBanner(viewModel, text => control.PlaceholderText = text);
        control.ApplyNativeFaces(viewModel, _scale);
        Bind(control, ComboBox.TextProperty, nameof(viewModel.DraftText), BindingMode.TwoWay);
        _lifetime.OnDispose(() => control.ClearValue(ComboBox.TextProperty));
        var retryTimer = control.DispatcherQueue.CreateTimer();
        retryTimer.Interval = TimeSpan.FromMilliseconds(100);
        retryTimer.IsRepeating = false;
        _lifetime.OnDispose(retryTimer.Stop);
        string? pendingText = null;
        void CommitDraft()
        {
            retryTimer.Stop();
            if (!_lifetime.IsActive || !viewModel.Editable || _isApplyingCanonical() || !AllowsAction(viewModel, "setText") ||
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
            if (_lifetime.IsActive && !_isApplyingCanonical() && AllowsAction(viewModel, "select") &&
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
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName == nameof(viewModel.SelectedIndex)) control.ApplyCanonicalSelection(viewModel.SelectedIndex);
            if (args.PropertyName == nameof(viewModel.Editable)) control.IsEditable = viewModel.Editable;
            if (args.PropertyName == nameof(viewModel.OwnerDrawCombo)) control.ApplyNativeFaces(viewModel, _scale);
            if (args.PropertyName == nameof(viewModel.DraftText) && control.Text != viewModel.DraftText)
                control.Text = viewModel.DraftText;
            if (args.PropertyName == nameof(viewModel.Text)) pendingText = null;
        });
        return control;
    }

    // A right-click, Shift+F10 or the Menu key on a tree/list item asks the native
    // application for that item's own context menu. The native menu then arrives as
    // the snapshot's popupMenu and is shown where the request was made.
    // `index` names one of the node's carried items; a windowed list adds its window
    // start, because the action names the row absolutely.
    private bool RequestContextMenu(ControlNodeViewModel viewModel, FrameworkElement element,
        FrameworkElement? item, int index, ContextRequestedEventArgs args, int indexOffset = 0)
    {
        if (_isApplyingCanonical() || !_lifetime.IsActive || index < 0 ||
            index >= viewModel.Items.Count || !AllowsAction(viewModel, "contextMenu")) return false;
        if (args.TryGetPosition(element, out var point))
            viewModel.ContextMenuAnchor = (point.X, point.Y);
        else if (item is not null)
        {
            // Keyboard requests have no pointer: open below the item's label.
            var corner = item.TransformToVisual(element).TransformPoint(
                new global::Windows.Foundation.Point(16, item.ActualHeight));
            viewModel.ContextMenuAnchor = (corner.X, corner.Y);
        }
        else viewModel.ContextMenuAnchor = (0, 0);
        // The label is the item's witness: the Bridge refuses the request if the
        // index has since come to name a different native item.
        _action(viewModel, "contextMenu", new ItemTextActionValue { Index = index + indexOffset, Text = viewModel.Items[index] });
        return true;
    }

    // Opens an item the way its control answers a double-click: the Bridge selects it and sends
    // the control the Enter key. The label is the witness, as for a context menu. False when
    // the node offers no such activation.
    private bool RequestOpenItem(ControlNodeViewModel viewModel, int index, int indexOffset = 0)
    {
        if (_isApplyingCanonical() || !_lifetime.IsActive || index < 0 || index >= viewModel.Items.Count ||
            !viewModel.ItemDefaultActivation || !AllowsAction(viewModel, "openItem")) return false;
        _action(viewModel, "openItem", new ItemTextActionValue { Index = index + indexOffset, Text = viewModel.Items[index] });
        return true;
    }

    private static T? AncestorOrSelf<T>(DependencyObject? source, DependencyObject stop) where T : DependencyObject
    {
        while (source is not null && source != stop && source is not T)
            source = VisualTreeHelper.GetParent(source);
        return source as T;
    }

    internal static bool AllowsAction(ControlNodeViewModel viewModel, string action) =>
        viewModel.Kind != "staticDecoration" &&
        (string.IsNullOrEmpty(viewModel.AdapterId) || viewModel.SupportedActions.Contains(action));

    private ListBox CreateListBox(ControlNodeViewModel viewModel)
    {
        var itemStyle = new Style(typeof(ListBoxItem));
        itemStyle.Setters.Add(new Setter(Control.MinHeightProperty, 22d));
        itemStyle.Setters.Add(new Setter(Control.PaddingProperty, new Thickness(4, 0, 4, 0)));
        itemStyle.Setters.Add(new Setter(Control.VerticalContentAlignmentProperty, VerticalAlignment.Center));
        // A multi-select list (LBS_EXTENDEDSEL takes ctrl and shift, LBS_MULTIPLESEL alone toggles each
        // click) is chosen as an index list; any other list has one current item.
        var multiple = viewModel.MultiSelect;
        var control = new ListBox
        {
            FontSize = NativeFontSize,
            ItemsSource = viewModel.Items,
            ItemContainerStyle = itemStyle,
            SelectionMode = !multiple ? SelectionMode.Single
                : (viewModel.Style & 0x0800) != 0 ? SelectionMode.Extended : SelectionMode.Multiple,
        };
        var applying = false;
        void ApplySelection()
        {
            applying = true;
            try
            {
                if (!multiple)
                {
                    control.SelectedIndex = viewModel.SelectedIndex;
                    return;
                }
                control.SelectedItems.Clear();
                foreach (var index in viewModel.SelectedIndices)
                    if (index >= 0 && index < viewModel.Items.Count) control.SelectedItems.Add(viewModel.Items[index]);
            }
            finally
            {
                applying = false;
            }
        }
        ApplySelection();
        control.SelectionChanged += (_, _) =>
        {
            if (applying || _isApplyingCanonical()) return;
            if (multiple)
            {
                var selection = CanonicalSelectionIndices(control.SelectedItems.Cast<object>().Select(control.Items.IndexOf));
                if (AllowsAction(viewModel, "setSelection") && !viewModel.SelectedIndices.SequenceEqual(selection))
                    _action(viewModel, "setSelection", selection);
            }
            else if (AllowsAction(viewModel, "select") && control.SelectedIndex != viewModel.SelectedIndex)
                _action(viewModel, "select", control.SelectedIndex);
        };
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.SelectedIndex) or nameof(viewModel.SelectedIndices)) ApplySelection();
        });
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
            if (!TrySplitSysLinkRuns(viewModel.Text, viewModel.Items, out var runs)) return;
            var paragraph = new Paragraph();
            var linkIndex = 0;
            foreach (var (value, isLink) in runs)
            {
                if (!isLink)
                {
                    paragraph.Inlines.Add(new Run { Text = value });
                    continue;
                }
                var index = linkIndex++;
                var link = new Hyperlink
                {
                    IsTabStop = viewModel.TabStop,
                    TabIndex = Math.Max(0, viewModel.TabIndex),
                };
                link.Inlines.Add(new Run { Text = value });
                AutomationProperties.SetName(link, value);
                // A native SysLink has one link and is invoked; a task dialog's text
                // names which of its links was clicked.
                link.Click += (_, _) =>
                {
                    if (_isApplyingCanonical()) return;
                    if (viewModel.Items.Count == 1)
                    {
                        if (AllowsAction(viewModel, "invoke")) _action(viewModel, "invoke", null);
                    }
                    else if (AllowsAction(viewModel, "activateItem"))
                    {
                        _action(viewModel, "activateItem", index);
                    }
                };
                paragraph.Inlines.Add(link);
            }
            text.Blocks.Clear();
            text.Blocks.Add(paragraph);
        }
        RebuildText();
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Text) or nameof(viewModel.Items) or
                nameof(viewModel.TabStop) or nameof(viewModel.TabIndex)) RebuildText();
        });
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
        ReportFocusEntry(control, viewModel);
        ItemsPanelTemplate? checkedReportPanel = null;
        var positionedPanel = (ItemsPanelTemplate)Microsoft.UI.Xaml.Markup.XamlReader.Load(
            "<ItemsPanelTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'><Canvas/></ItemsPanelTemplate>");
        var contentBounds = new PixelRect();
        var pendingScroll = false;
        var sentScrollX = 0;
        var sentScrollY = 0;
        bool Positioned() => viewModel.ListViewMode != "report" || viewModel.GroupViewEnabled;
        ScrollViewer? scrollHook = null;

        // Minor 33: a windowed virtual list shows the rows it carries between two spacers
        // standing for the rows it does not, so the scroll extent is the whole list's.
        // Scrolling past the carried rows asks the native list to move its top row there,
        // and the next snapshot carries the rows around it.  Indices on the wire stay
        // absolute; control.Items holds the window.
        // Fresh spacers on every rebuild: an element still parented by the old header or
        // footer presenter cannot be given to a new one (WinUI reports that as the
        // misleading COMException 0x800F1000, "no installed components were detected").
        var topSpacer = new Border { IsHitTestVisible = false };
        var bottomSpacer = new Border { IsHitTestVisible = false };
        FrameworkElement? columnHeader = null;
        var rowHeight = 22.0;
        var windowStart = 0;
        var endedNativeSession = -1;
        var windowPlaced = false;
        var requestedTop = -1;
        // Rebuilding the items resets a ListView's scroll offset, but the spacers keep
        // the whole list's extent, so the offset the user was at is still the right one
        // after a window move; it is put back once the new rows have a layout.
        double? restoreOffset = null;
        var restoreAttempts = 0;
        ScrollViewer? windowScroll = null;
        Microsoft.UI.Dispatching.DispatcherQueueTimer? windowTimer = null;
        int Absolute(int local) => local < 0 ? local : local + windowStart;
        void ApplyWindowSpacers()
        {
            if (!viewModel.IsWindowedList) return;
            var (above, below) = ListViewPresentation.SpacerHeights(
                windowStart, control.Items.Count, viewModel.ItemCount, rowHeight);
            topSpacer.Height = above;
            bottomSpacer.Height = below;
        }
        void RequestWindowForViewport()
        {
            if (!_lifetime.IsActive || !viewModel.IsWindowedList || _isApplyingCanonical() ||
                windowScroll is not { } scroll || scroll.ViewportHeight <= 0) return;
            // While an offset restore waits for its layout the scroll position is the
            // reset one (row 0), which is no evidence of where the user is looking.
            if (restoreOffset is not null)
            {
                if (++restoreAttempts > 40) restoreOffset = null;
                else
                {
                    ScheduleWindowCheck();
                    return;
                }
            }
            var first = ListViewPresentation.RowAtOffset(scroll.VerticalOffset,
                columnHeader?.ActualHeight ?? 0, rowHeight, viewModel.ItemCount);
            var visible = Math.Max(1, (int)Math.Ceiling(scroll.ViewportHeight / rowHeight));
            if (ListViewPresentation.WindowCovers(windowStart, control.Items.Count, first, visible,
                    viewModel.ItemCount, margin: 4) || !AllowsAction(viewModel, "setTopIndex")) return;
            // One request at a time: the native list scrolls and recaptures for each, so a
            // continuing scroll is answered by asking again, for where the viewport is
            // then, once the window that answers this request has arrived.
            if (viewModel.HasPending("setTopIndex") || first == requestedTop)
            {
                ScheduleWindowCheck();
                return;
            }
            requestedTop = first;
            RendererDiagnostics.Log($"windowed list node={viewModel.NodeId} asks for top row {first} " +
                $"(window {windowStart}+{control.Items.Count}, offset {scroll.VerticalOffset:F0}, row {rowHeight:F1}px)");
            _action(viewModel, "setTopIndex", first);
        }
        var rebuildQueued = false;
        void QueueWindowRebuild()
        {
            if (rebuildQueued) return;
            rebuildQueued = true;
            control.DispatcherQueue.TryEnqueue(() =>
            {
                rebuildQueued = false;
                if (_lifetime.IsActive) RebuildRows();
            });
        }
        void ScheduleWindowCheck()
        {
            if (!viewModel.IsWindowedList || !_lifetime.IsActive) return;
            if (windowTimer is null)
            {
                windowTimer = control.DispatcherQueue.CreateTimer();
                windowTimer.Interval = TimeSpan.FromMilliseconds(120);
                windowTimer.IsRepeating = false;
                windowTimer.Tick += (_, _) => RequestWindowForViewport();
                _lifetime.OnDispose(windowTimer.Stop);
            }
            if (!windowTimer.IsRunning) windowTimer.Start();
        }
        void OnWindowScroll(object? sender, ScrollViewerViewChangedEventArgs args)
        {
            if (!_isApplyingCanonical()) ScheduleWindowCheck();
        }
        void TrackWindowViewport()
        {
            if (!viewModel.IsWindowedList) return;
            var scroll = FindScrollViewer(control);
            if (!ReferenceEquals(scroll, windowScroll))
            {
                if (windowScroll is not null) windowScroll.ViewChanged -= OnWindowScroll;
                windowScroll = scroll;
                if (windowScroll is not null) windowScroll.ViewChanged += OnWindowScroll;
            }
            if (control.Items.Count > 0 && control.ContainerFromIndex(0) is FrameworkElement row &&
                row.ActualHeight > 1 && Math.Abs(row.ActualHeight - rowHeight) > 0.5)
            {
                rowHeight = row.ActualHeight;
                ApplyWindowSpacers();
            }
            if (restoreOffset is { } target && windowScroll is { ViewportHeight: > 0 } restoring)
            {
                if (Math.Abs(restoring.VerticalOffset - target) < 1 || ++restoreAttempts > 20)
                {
                    RendererDiagnostics.Log($"windowed list node={viewModel.NodeId} offset restored to " +
                        $"{restoring.VerticalOffset:F0} (wanted {target:F0}) after {restoreAttempts} layout(s)");
                    restoreOffset = null;
                }
                else restoring.ChangeView(null, target, null, true);
            }
            // The first window sits around the native list's top row; the projection
            // opens there instead of at row 0, which would ask to scroll the native list.
            if (!windowPlaced && windowScroll is { ViewportHeight: > 0 } placed)
            {
                windowPlaced = true;
                if (windowStart > 0)
                {
                    var perPage = (int)Math.Floor(placed.ViewportHeight / rowHeight);
                    placed.ChangeView(null, (columnHeader?.ActualHeight ?? 0) + (windowStart + perPage) * rowHeight,
                        null, true);
                }
            }
        }
        _lifetime.OnDispose(() =>
        {
            if (windowScroll is not null) windowScroll.ViewChanged -= OnWindowScroll;
        });
        void OnListScroll(object? sender, ScrollViewerViewChangedEventArgs args)
        {
            if (pendingScroll || _isApplyingCanonical() || !Positioned() ||
                sender is not ScrollViewer scroll || !_lifetime.IsActive) return;
            if (!ListViewPresentation.TryIncrementalScroll(
                    scroll.HorizontalOffset, scroll.VerticalOffset,
                    -contentBounds.X * _scale, -contentBounds.Y * _scale, _scale,
                    sentScrollX, sentScrollY, out var dx, out var dy) ||
                !AllowsAction(viewModel, "scrollBy")) return;
            sentScrollX += dx;
            sentScrollY += dy;
            _action(viewModel, "scrollBy", new ScrollByActionValue { Dx = dx, Dy = dy });
        }
        void ApplyItemLayout()
        {
            if (_lifetime.IsActive) TrackWindowViewport();
            if (!_lifetime.IsActive || !Positioned() || control.ItemsPanelRoot is not Canvas canvas) return;
            var scroll = FindScrollViewer(control);
            if (!ReferenceEquals(scroll, scrollHook))
            {
                if (scrollHook is not null) scrollHook.ViewChanged -= OnListScroll;
                scrollHook = scroll;
                if (scrollHook is not null) scrollHook.ViewChanged += OnListScroll;
            }
            var x = -contentBounds.X * _scale;
            var y = -contentBounds.Y * _scale;
            // Include the viewport after translating a negative native origin,
            // even when the item extent is smaller than the visible viewport.
            var width = Math.Max(contentBounds.Width * _scale, (scroll?.ViewportWidth ?? 0) + x);
            var height = Math.Max(contentBounds.Height * _scale, (scroll?.ViewportHeight ?? 0) + y);
            if (canvas.Width != width) canvas.Width = width;
            if (canvas.Height != height) canvas.Height = height;
            if (pendingScroll && scroll is not null)
            {
                // ChangeView can run before new canvas dimensions reach the
                // ScrollViewer. Wait for the requested offset, not its return flag.
                if (scroll.ViewportWidth <= 0 || scroll.ViewportHeight <= 0) return;
                if (Math.Abs(scroll.HorizontalOffset - x) < 0.5 &&
                    Math.Abs(scroll.VerticalOffset - y) < 0.5)
                    pendingScroll = false;
                else scroll.ChangeView(x, y, null, true);
            }
        }
        var panelLogged = false;
        control.Loaded += (_, _) =>
        {
            if (!panelLogged)
            {
                panelLogged = true;
                var panel = control.ItemsPanelRoot;
                var stack = panel as ItemsStackPanel;
                RendererDiagnostics.Log($"ListView panel loaded node={viewModel.NodeId} mode={viewModel.ListViewMode} " +
                    $"items={control.Items.Count} checkBoxes={viewModel.CheckBoxes} root={panel?.GetType().Name ?? "null"} " +
                    $"children={panel?.Children.Count ?? -1} cache={stack?.FirstCacheIndex ?? -1}..{stack?.LastCacheIndex ?? -1}");
            }
            ApplyItemLayout();
        };
        control.LayoutUpdated += (_, _) => ApplyItemLayout();
        var applyingSelection = false;
        var rowChecks = new List<CheckBox>();
        var rowContents = new List<ProjectedItemContent>();
        var selectionAnchor = -1;
        long itemPresentationGeneration = 0;
        var activation = new ListViewActivationCoordinator(viewModel,
            (action, value) => _action(viewModel, action, value),
            action => _lifetime.IsActive && AllowsAction(viewModel, action),
            _isApplyingCanonical, callback => control.DispatcherQueue.TryEnqueue(() => callback()));
        _lifetime.OnDispose(activation.Dispose);

        int FocusedItemIndex()
        {
            if (control.XamlRoot is null) return control.SelectedIndex;
            var focused = FocusManager.GetFocusedElement(control.XamlRoot) as DependencyObject;
            while (focused is not null && focused != control && focused is not ListViewItem)
                focused = VisualTreeHelper.GetParent(focused);
            return focused is ListViewItem item ? control.IndexFromContainer(item) : control.SelectedIndex;
        }

        // Native activation (stable identities and an accessible default action) when the row
        // has it; otherwise the control is sent Enter, where the node offers that.
        bool OffersActivation() =>
            (viewModel.ItemActivationSupported && AllowsAction(viewModel, "activateItem")) ||
            (viewModel.ItemDefaultActivation && AllowsAction(viewModel, "openItem"));

        void ActivateItem(int index)
        {
            if (!_lifetime.IsActive || _isApplyingCanonical() ||
                index < 0 || index >= control.Items.Count || index >= viewModel.Items.Count ||
                rowContents.Any(content => content.IsEditing))
            {
                RendererDiagnostics.Log($"list activation node={viewModel.NodeId} index={index} ignored " +
                    $"(active={_lifetime.IsActive} applying={_isApplyingCanonical()} items={control.Items.Count}/{viewModel.Items.Count})");
                return;
            }
            if (viewModel.ItemActivationSupported && index < viewModel.ItemNativeIds.Count &&
                AllowsAction(viewModel, "activateItem"))
            {
                activation.Request(index);
                return;
            }
            RequestOpenItem(viewModel, index, windowStart);
        }

        ActivatableListViewItem WrapItem(FrameworkElement content, int index, string name)
        {
            var generation = itemPresentationGeneration;
            void InvokeCurrentItem()
            {
                if (_lifetime.IsActive && generation == itemPresentationGeneration) ActivateItem(index);
            }
            var container = new ActivatableListViewItem
            {
                Content = content,
                Activate = OffersActivation() ? InvokeCurrentItem : null,
            };
            AutomationProperties.SetName(container, name);
            container.DoubleTapped += (_, args) =>
            {
                if (IsListItemEditor(args.OriginalSource as DependencyObject, container)) return;
                InvokeCurrentItem();
                args.Handled = OffersActivation();
            };
            return container;
        }

        FrameworkElement BuildIconItem(int index, ImageSource? icon)
        {
            var generation = itemPresentationGeneration;
            var label = viewModel.Items[index];
            var content = new ProjectedItemContent(requested =>
            {
                var native = EndsNativeSession(index);
                if (generation != itemPresentationGeneration || _isApplyingCanonical() || !viewModel.EditableLabels || requested.Length == 0 ||
                    (requested == label && !native) || !AllowsAction(viewModel, "setItemText")) return;
                _action(viewModel, "setItemText", new ItemTextActionValue { Index = index, Text = requested });
            }, largeIcon: viewModel.ListViewMode == "largeIcon") { Editable = viewModel.EditableLabels };
            content.Apply(label, icon);
            rowContents.Add(content);
            FrameworkElement visual = content;
            if (viewModel.CheckBoxes)
            {
                var check = new CheckBox
                {
                    IsChecked = viewModel.CheckedIndices.Contains(index), IsTabStop = false,
                    MinWidth = 0, MinHeight = 0, Padding = new Thickness(0),
                    VerticalAlignment = VerticalAlignment.Top,
                };
                AutomationProperties.SetName(check, label);
                AutomationProperties.SetAutomationId(check, "fls-item-check-" + index);
                RoutedEventHandler changed = (_, _) =>
                {
                    if (generation != itemPresentationGeneration || applyingSelection || _isApplyingCanonical() || !AllowsAction(viewModel, "setItemCheck")) return;
                    var requested = check.IsChecked == true;
                    if (viewModel.CheckedIndices.Contains(index) != requested)
                        _action(viewModel, "setItemCheck", new ListViewCheckActionValue { Index = index, Checked = requested });
                };
                check.Checked += changed;
                check.Unchecked += changed;
                rowChecks.Add(check);
                var grid = new Grid();
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
                grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
                grid.Children.Add(check);
                Grid.SetColumn(content, 1);
                grid.Children.Add(content);
                visual = grid;
            }
            var container = WrapItem(visual, index, label);
            var rect = viewModel.ItemRects[index];
            container.Visibility = rect.Width == 0 || rect.Height == 0 ? Visibility.Collapsed : Visibility.Visible;
            container.MinWidth = container.MinHeight = 0;
            container.Width = rect.Width * _scale;
            container.Height = rect.Height * _scale;
            container.Padding = container.Margin = new Thickness(0);
            container.HorizontalContentAlignment = HorizontalAlignment.Stretch;
            container.VerticalContentAlignment = VerticalAlignment.Stretch;
            Canvas.SetLeft(container, (rect.X - contentBounds.X) * _scale);
            Canvas.SetTop(container, (rect.Y - contentBounds.Y) * _scale);
            return container;
        }

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
            var generation = itemPresentationGeneration;
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
                        var native = EndsNativeSession(rowIndex);
                        if (generation != itemPresentationGeneration || _isApplyingCanonical() || !viewModel.EditableLabels ||
                            rowIndex < 0 || rowIndex >= viewModel.Rows.Count ||
                            requested.Length == 0 || (requested == cells[0] && !native) ||
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
                    AutomationProperties.SetAutomationId(checkBox, "fls-item-check-" + rowIndex);
                    RoutedEventHandler changed = (_, _) =>
                    {
                        if (generation != itemPresentationGeneration || applyingSelection || _isApplyingCanonical()) return;
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
                // Selection is absolute; only the carried rows show it.
                if (viewModel.MultiSelect)
                {
                    control.SelectedItems.Clear();
                    foreach (var index in viewModel.SelectedIndices)
                    {
                        var local = index - windowStart;
                        if (local >= 0 && local < control.Items.Count)
                            control.SelectedItems.Add(control.Items[local]);
                    }
                }
                else
                {
                    var local = viewModel.SelectedIndices.FirstOrDefault(-1) - windowStart;
                    control.SelectedIndex = local >= 0 && local < control.Items.Count ? local : -1;
                }
            }
            finally
            {
                applyingSelection = false;
            }
        }

        void RebuildRows()
        {
            if (!HasRenderableListViewShape(viewModel)) return;
            ++itemPresentationGeneration;
            // Indexes can denote different native items after a rebuild. Start
            // the next Shift gesture from its current focus instead of keeping
            // an anchor that may now refer to a removed item.
            selectionAnchor = -1;
            applyingSelection = true;
            try
            {
                var report = viewModel.ListViewMode == "report";
                if (Positioned())
                    control.ItemsPanel = positionedPanel;
                else if (viewModel.CheckBoxes)
                {
                    // The UIA checkbox gate currently maps enumeration order to
                    // canonical row indexes. Keep every checkbox realized until
                    // that contract carries the identity of a virtualized row.
                    checkedReportPanel ??= (ItemsPanelTemplate)Microsoft.UI.Xaml.Markup.XamlReader.Load(
                        "<ItemsPanelTemplate xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation'><StackPanel/></ItemsPanelTemplate>");
                    control.ItemsPanel = checkedReportPanel;
                }
                else
                {
                    // Reading ItemsPanel before this control enters the tree
                    // returns null. Writing that value back masks the themed
                    // ItemsStackPanel and makes all report rows nonvirtualized.
                    control.ClearValue(ItemsControl.ItemsPanelProperty);
                }
                ScrollViewer.SetHorizontalScrollMode(control, ScrollMode.Enabled);
                ScrollViewer.SetHorizontalScrollBarVisibility(control, ScrollBarVisibility.Auto);
                columnHeader = ShouldRenderListViewHeader(viewModel) && !viewModel.GroupViewEnabled
                    ? BuildCells(viewModel.Columns, header: true)
                    : null;
                // A restore still waiting for a layout keeps the offset it was taken at:
                // the offset read now is the reset one.
                if (viewModel.IsWindowedList && windowPlaced && windowScroll is { } current && restoreOffset is null)
                {
                    restoreOffset = current.VerticalOffset;
                    restoreAttempts = 0;
                    RendererDiagnostics.Log($"windowed list node={viewModel.NodeId} window {windowStart} -> " +
                        $"{viewModel.ItemWindowStart} ({viewModel.Rows.Count} rows of {viewModel.ItemCount}) " +
                        $"keeping offset {current.VerticalOffset:F0}");
                }
                windowStart = viewModel.IsWindowedList ? viewModel.ItemWindowStart : 0;
                // A new window answers the last request for one.
                requestedTop = -1;
                control.Footer = null;
                if (viewModel.IsWindowedList)
                {
                    topSpacer = new Border { IsHitTestVisible = false };
                    bottomSpacer = new Border { IsHitTestVisible = false };
                    var head = new StackPanel();
                    if (columnHeader is not null) head.Children.Add(columnHeader);
                    head.Children.Add(topSpacer);
                    control.Header = head;
                    control.Footer = bottomSpacer;
                }
                else control.Header = columnHeader;
                control.Items.Clear();
                rowChecks.Clear();
                rowContents.Clear();
                var count = report ? viewModel.Rows.Count : viewModel.Items.Count;
                var icons = ItemIcons(viewModel, count);
                // A Canvas needs the extent occupied by its items only. The
                // ScrollViewer supplies its own viewport minimum after chrome.
                if (Positioned()) contentBounds = ListViewPresentation.ContentBounds(
                    viewModel.ItemRects.Concat(viewModel.ListViewGroups.SelectMany(group => new[] { group.HeaderRect, group.FooterRect })).ToArray(), new PixelRect());
                void Position(FrameworkElement element, PixelRect rect)
                {
                    element.MinWidth = element.MinHeight = 0;
                    element.Width = rect.Width * _scale;
                    element.Height = rect.Height * _scale;
                    element.Margin = new Thickness(0);
                    element.Visibility = rect.Width == 0 || rect.Height == 0 ? Visibility.Collapsed : Visibility.Visible;
                    Canvas.SetLeft(element, (rect.X - contentBounds.X) * _scale);
                    Canvas.SetTop(element, (rect.Y - contentBounds.Y) * _scale);
                }
                // A list that indexes its realized rows (check boxes, in-place rename) keeps them all;
                // see ListViewPresentation.InBuildWindow for the rest.
                bool InBuildWindow(PixelRect rect) =>
                    viewModel.CheckBoxes || viewModel.EditableLabels ||
                    ListViewPresentation.InBuildWindow(rect, viewModel.Rect.Width, viewModel.Rect.Height);
                for (var index = 0; index < count; ++index)
                {
                    // A row the viewport will not show keeps its place in control.Items (selection and
                    // focus are indexes into it) as an empty, collapsed container.
                    if (Positioned() && !InBuildWindow(viewModel.ItemRects[index]))
                    {
                        control.Items.Add(new ListViewItem { Visibility = Visibility.Collapsed, IsTabStop = false, IsHitTestVisible = false });
                        continue;
                    }
                    var row = report
                        ? WrapItem(BuildCells(viewModel.Rows[index], header: false, index, icons[index]),
                            index, string.Join(" ", viewModel.Rows[index]))
                        : BuildIconItem(index, icons[index]);
                    if (report && viewModel.GroupViewEnabled) Position(row, viewModel.ItemRects[index]);
                    control.Items.Add(row);
                }
                if (viewModel.GroupViewEnabled)
                {
                    var generation = itemPresentationGeneration;
                    void Band(FrameworkElement content, PixelRect bounds, bool interactive)
                    {
                        var container = new ListViewItem
                        {
                            Content = content, IsTabStop = false, IsHitTestVisible = interactive,
                            Padding = new Thickness(0), HorizontalContentAlignment = HorizontalAlignment.Stretch,
                            VerticalContentAlignment = VerticalAlignment.Stretch,
                        };
                        // A band is structure around the rows, not a row: its content (the group heading,
                        // the footer text, the column titles) is the element, so the carrier stays out of
                        // the accessibility tree rather than doubling every band in it.
                        AutomationProperties.SetAccessibilityView(container, AccessibilityView.Raw);
                        Position(container, bounds);
                        control.Items.Add(container);
                    }
                    // Bands come after every row, so one the viewport will not show is simply not built.
                    foreach (var group in viewModel.ListViewGroups.Where(group => !group.Hidden))
                    {
                        if (InBuildWindow(group.HeaderRect))
                            Band(new ListViewGroupHeader(group, expanded =>
                            {
                                if (!_lifetime.IsActive || generation != itemPresentationGeneration || _isApplyingCanonical() ||
                                    !AllowsAction(viewModel, "setGroupExpanded")) return;
                                _action(viewModel, "setGroupExpanded", new ListViewGroupExpandActionValue
                                { GroupId = group.GroupId, Expanded = expanded });
                            }), group.HeaderRect, group.Collapsible);
                        if (group.Footer.Length != 0 && InBuildWindow(group.FooterRect)) Band(new TextBlock
                        {
                            Text = group.Footer, TextAlignment = ListViewGroupPresentation.Alignment(group.FooterAlignment),
                            VerticalAlignment = VerticalAlignment.Center,
                        }, group.FooterRect, false);
                    }
                    if (ShouldRenderListViewHeader(viewModel))
                        Band(BuildCells(viewModel.Columns, header: true), new PixelRect
                        {
                            Width = viewModel.ColumnWidths.Sum(), Height = 22,
                        }, true);
                }
                pendingScroll = Positioned();
                sentScrollX = 0;
                sentScrollY = 0;
                ApplyWindowSpacers();
                ApplyItemLayout();
            }
            finally
            {
                applyingSelection = false;
            }
            ApplyCanonicalSelection();
            ApplyCanonicalChecks();
            ApplyNativeEditSession();
            // The user may have kept scrolling while the window was on its way.
            ScheduleWindowCheck();
        }

        // A label session the application opened itself (its Rename command, from the menu or
        // a context menu) is shown as the projected editor on that row; it closes, without
        // sending anything, when the application ends the session on its own.
        void ApplyNativeEditSession()
        {
            var editing = viewModel.EditingIndex;
            if (editing != endedNativeSession) endedNativeSession = -1;
            var local = editing < 0 || editing == endedNativeSession ? -1 : editing - windowStart;
            for (var index = 0; index < rowContents.Count; ++index)
                if (index != local && rowContents[index].NativeSession) rowContents[index].AbandonEdit();
            if (!viewModel.EditableLabels || local < 0 || local >= rowContents.Count ||
                rowContents[local].IsEditing) return;
            if (local < control.Items.Count) control.ScrollIntoView(control.Items[local]);
            rowContents[local].BeginEdit(nativeSession: true);
        }

        // Whether a row's editor answers the application's own session. Its unchanged label
        // is then still sent (it ends that session without a rename), and a capture taken
        // before the answer landed does not open the editor again.
        bool EndsNativeSession(int local)
        {
            var absolute = Absolute(local);
            if (absolute < 0 || viewModel.EditingIndex != absolute) return false;
            endedNativeSession = absolute;
            return true;
        }

        control.SelectionChanged += (_, _) =>
        {
            if (applyingSelection || _isApplyingCanonical()) return;
            activation.Cancel("the selection was changed by the user");
            var selection = CanonicalSelectionIndices(control.SelectedItems
                .Cast<object>()
                .Select(item => control.Items.IndexOf(item)).Where(index => index < viewModel.Items.Count));
            if (control.SelectedItems.Cast<object>().Any(item => control.Items.IndexOf(item) >= viewModel.Items.Count))
            {
                ApplyCanonicalSelection();
                return;
            }
            if (viewModel.IsWindowedList)
            {
                // A windowed list's rows are named absolutely, and a multiple selection
                // keeps the selected rows the window does not carry.
                var carried = control.Items.Count;
                selection = CanonicalSelectionIndices(selection.Select(Absolute).Concat(viewModel.MultiSelect
                    ? viewModel.SelectedIndices.Where(index => index < windowStart || index >= windowStart + carried)
                    : []));
            }
            if (!viewModel.SelectedIndices.SequenceEqual(selection) &&
                AllowsAction(viewModel, "setSelection"))
                _action(viewModel, "setSelection", selection);
            var focus = Absolute(FocusedItemIndex());
            if (focus < 0 && selection.Length > 0) focus = selection[^1];
            if (focus >= 0 && focus != viewModel.FocusedIndex &&
                AllowsAction(viewModel, "setFocusedIndex"))
                _action(viewModel, "setFocusedIndex", focus);
        };
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Items) or nameof(viewModel.ItemRects) or
                nameof(viewModel.ListViewMode) or nameof(viewModel.ItemActivationSupported) or
                nameof(viewModel.ListViewGroups) or
                nameof(viewModel.ItemNativeIds) or
                nameof(viewModel.Columns) or nameof(viewModel.ColumnWidths) or
                nameof(viewModel.ColumnOrder) or
                nameof(viewModel.Rows) or nameof(viewModel.ColumnHeadersVisible) or
                nameof(viewModel.CheckBoxes) or nameof(viewModel.ItemImages) or
                nameof(viewModel.EditableLabels))
            {
                // One snapshot announces Items, ItemImages and Rows one after another, and a
                // window of rows is expensive to build, so a windowed list builds once, after
                // the snapshot has been applied.
                if (viewModel.IsWindowedList) QueueWindowRebuild();
                else RebuildRows();
            }
            else if (args.PropertyName == nameof(viewModel.MultiSelect))
            {
                control.SelectionMode = SelectionModeFor(viewModel.MultiSelect);
                ApplyCanonicalSelection();
            }
            else if (args.PropertyName == nameof(viewModel.SelectedIndices))
            {
                ApplyCanonicalSelection();
            }
            else if (args.PropertyName == nameof(viewModel.CheckedIndices))
                ApplyCanonicalChecks();
            else if (args.PropertyName == nameof(viewModel.EditingIndex))
                ApplyNativeEditSession();
        });
        // Both pointer presses of a double-click precede DoubleTapped, so they
        // cancel an older gesture without cancelling the new activation. A later
        // single-click or focus/navigation gesture always replaces the old intent.
        control.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler((_, _) =>
        {
            if (!_isApplyingCanonical()) activation.Cancel("a pointer press");
        }), true);
        control.GotFocus += (_, _) =>
        {
            if (!applyingSelection && !_isApplyingCanonical()) activation.Cancel("focus moved");
        };
        // The row container consumes Enter as part of its own invoke handling before it
        // bubbles here, so the activation gesture listens to handled events too.
        control.AddHandler(UIElement.KeyDownEvent, new KeyEventHandler((_, args) =>
        {
            if (!_lifetime.IsActive || args.Key != VirtualKey.Enter ||
                IsListItemEditor(args.OriginalSource as DependencyObject, control)) return;
            var index = FocusedItemIndex();
            ActivateItem(index);
            if (index >= 0 && OffersActivation()) args.Handled = true;
        }), true);
        control.KeyDown += (_, args) =>
        {
            if (!_lifetime.IsActive) return;
            if (args.Key is VirtualKey.Left or VirtualKey.Right or VirtualKey.Up or VirtualKey.Down or
                VirtualKey.Home or VirtualKey.End or VirtualKey.PageUp or VirtualKey.PageDown or
                VirtualKey.Tab or VirtualKey.Space or VirtualKey.F2 or VirtualKey.Escape)
                activation.Cancel("a navigation key");
            if (Positioned() && !args.Handled &&
                args.Key is VirtualKey.Left or VirtualKey.Right or VirtualKey.Up or VirtualKey.Down or VirtualKey.Home or VirtualKey.End &&
                !IsListItemEditor(args.OriginalSource as DependencyObject, control))
            {
                var current = FocusedItemIndex();
                var next = ListViewPresentation.Navigate(viewModel.ItemRects, current, args.Key);
                var visibleOrder = viewModel.GroupViewEnabled
                    ? ListViewGroupPresentation.VisibleOrder(viewModel.ListViewGroups, viewModel.ItemGroupIds)
                    : Enumerable.Range(0, viewModel.Items.Count).ToArray();
                if (args.Key == VirtualKey.Home) next = visibleOrder.FirstOrDefault(-1);
                if (args.Key == VirtualKey.End) next = visibleOrder.LastOrDefault(-1);
                if (next >= 0 && control.ContainerFromIndex(next) is ListViewItem destination)
                {
                    var shift = (InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift) &
                        global::Windows.UI.Core.CoreVirtualKeyStates.Down) != 0;
                    var ctrl = (InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Control) &
                        global::Windows.UI.Core.CoreVirtualKeyStates.Down) != 0;
                    destination.Focus(FocusState.Keyboard);
                    if (shift && viewModel.MultiSelect)
                    {
                        if (selectionAnchor < 0) selectionAnchor = Math.Max(0, current);
                        applyingSelection = true;
                        try
                        {
                            control.SelectedItems.Clear();
                            foreach (var position in ListViewPresentation.SelectionRange(visibleOrder.Length,
                                Array.IndexOf(visibleOrder, selectionAnchor), Array.IndexOf(visibleOrder, next)))
                            {
                                var index = visibleOrder[position];
                                control.SelectedItems.Add(control.Items[index]);
                            }
                        }
                        finally { applyingSelection = false; }
                        if (!_isApplyingCanonical() && AllowsAction(viewModel, "setSelection"))
                            _action(viewModel, "setSelection", CanonicalSelectionIndices(control.SelectedItems.Cast<object>().Select(control.Items.IndexOf)));
                        if (!_isApplyingCanonical() && next != viewModel.FocusedIndex &&
                            AllowsAction(viewModel, "setFocusedIndex"))
                            _action(viewModel, "setFocusedIndex", next);
                    }
                    else if (ctrl)
                    {
                        if (!_isApplyingCanonical() && next != viewModel.FocusedIndex &&
                            AllowsAction(viewModel, "setFocusedIndex"))
                            _action(viewModel, "setFocusedIndex", next);
                    }
                    else
                    {
                        selectionAnchor = next;
                        control.SelectedIndex = next;
                    }
                }
                args.Handled = true;
                return;
            }
            // F2 is the native in-place rename gesture; Space toggles the checkbox
            // column exactly as the native list does.
            var focusedIndex = FocusedItemIndex();
            if (args.Key == VirtualKey.F2 && viewModel.EditableLabels &&
                focusedIndex >= 0 && focusedIndex < rowContents.Count)
            {
                args.Handled = true;
                rowContents[focusedIndex].BeginEdit();
                return;
            }
            if (!viewModel.CheckBoxes || args.Key != VirtualKey.Space ||
                focusedIndex is < 0 || focusedIndex >= rowChecks.Count) return;
            rowChecks[focusedIndex].IsChecked = rowChecks[focusedIndex].IsChecked != true;
            args.Handled = true;
        };
        control.ContextRequested += (_, args) =>
        {
            var source = args.OriginalSource as DependencyObject;
            // An in-place label editor keeps its own text context menu.
            if (args.Handled || IsListItemEditor(source, control)) return;
            var container = AncestorOrSelf<ListViewItem>(source, control);
            var index = container is not null ? control.IndexFromContainer(container) : FocusedItemIndex();
            container ??= index >= 0 ? control.ContainerFromIndex(index) as ListViewItem : null;
            args.Handled = RequestContextMenu(viewModel, control, container, index, args, indexOffset: windowStart);
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
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Items) or nameof(viewModel.ColumnWidths)) RebuildParts();
        });
        return control;
    }

    private SemanticToolbarControl CreateToolbar(ControlNodeViewModel viewModel)
    {
        var control = new SemanticToolbarControl();
        long presentationGeneration = 0;
        void Rebuild()
        {
            var generation = ++presentationGeneration;
            var radioGroups = new Dictionary<int, List<ToolbarRadioButton>>();
            control.ClearItems();
            foreach (var item in viewModel.ToolbarItems)
            {
                FrameworkElement element;
                // Set for a text-only button, so the label can be fitted into the width
                // the native control committed to once that width is known.
                TextBlock? label = null;
                // The room an icon beside the label takes from it.
                var labelInset = 0.0;
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
                    var image = face is null ? null : new Image { Source = face, Stretch = Stretch.None };
                    FrameworkElement content = image is not null ? image : label!;
                    // A list-style toolbar draws the button's own label beside its icon.
                    if (image is not null && item.LabelBeside == true)
                    {
                        label = new TextBlock
                        {
                            Text = Win32Mnemonic.DisplayText(item.Text),
                            FontSize = NativeFontSize,
                            VerticalAlignment = VerticalAlignment.Center,
                            TextTrimming = TextTrimming.CharacterEllipsis,
                            TextWrapping = TextWrapping.NoWrap,
                        };
                        var beside = new StackPanel
                        {
                            Orientation = Orientation.Horizontal,
                            Spacing = 4,
                            VerticalAlignment = VerticalAlignment.Center,
                        };
                        beside.Children.Add(image);
                        beside.Children.Add(label);
                        content = beside;
                        labelInset = (item.ImageWidth ?? 0) * _scale + beside.Spacing + ToolbarLabelPadding;
                    }
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
                        if (generation == presentationGeneration && !_isApplyingCanonical() && AllowsAction(viewModel, "toolbarCommand"))
                            _action(viewModel, "toolbarCommand", commandId);
                    }
                    if (item.Kind == "radioButton")
                    {
                        var groupId = item.RadioGroup!.Value;
                        if (!radioGroups.TryGetValue(groupId, out var group))
                            radioGroups[groupId] = group = [];
                        ToolbarRadioButton? radio = null;
                        radio = new ToolbarRadioButton
                        {
                            Content = content, MinWidth = 0, MinHeight = 0, Padding = new Thickness(0),
                            HorizontalContentAlignment = HorizontalAlignment.Stretch,
                            VerticalContentAlignment = VerticalAlignment.Stretch,
                            IsEnabled = item.Enabled, IsTabStop = item.Enabled && !item.Hidden,
                            IsChecked = item.Checked == true,
                            SelectRequested = () =>
                            {
                                if (generation != presentationGeneration || _isApplyingCanonical()) return;
                                foreach (var member in group) member.IsChecked = ReferenceEquals(member, radio);
                                Emit();
                            },
                        };
                        group.Add(radio);
                        AutomationProperties.SetName(radio, Win32Mnemonic.DisplayText(item.Text));
                        element = radio;
                    }
                    else if (item.Kind == "toggleButton" || item.Checked == true)
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
                        if (item.DropDown == true)
                        {
                            // Two parts in one button, as comctl32 draws it: the body is the button's command
                            // and the arrow asks the owner for its menu (TBN_DROPDOWN). Only a button whose
                            // whole face is the arrow (BTNS_WHOLEDROPDOWN) never has a command of its own.
                            var arrowWidth = Math.Min(item.Rect.Width * _scale * 0.5, 15 * _scale);
                            var onArrow = false;
                            button.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler((_, args) =>
                            {
                                var x = args.GetCurrentPoint(button).Position.X;
                                onArrow = item.WholeDropDown == true || x >= button.ActualWidth - arrowWidth;
                            }), true);
                            button.Click += (_, _) =>
                            {
                                var arrow = onArrow || item.WholeDropDown == true;
                                onArrow = false;
                                if (!arrow) Emit();
                                else if (generation == presentationGeneration && !_isApplyingCanonical() &&
                                         AllowsAction(viewModel, "toolbarDropDown"))
                                    _action(viewModel, "toolbarDropDown", commandId);
                            };
                        }
                        else button.Click += (_, _) => Emit();
                        element = button;
                    }
                    if (face is not null)
                    {
                        BindToolbarImageState((ButtonBase)element, image!, item, face, viewModel.Style);
                        var tooltip = Win32Mnemonic.DisplayText(item.Text);
                        if (tooltip.Length != 0) ToolTipService.SetToolTip(element, tooltip);
                    }
                }
                element.Visibility = item.Hidden ? Visibility.Collapsed : Visibility.Visible;
                Canvas.SetLeft(element, item.Rect.X * _scale);
                Canvas.SetTop(element, item.Rect.Y * _scale);
                element.Width = item.Rect.Width * _scale;
                element.Height = item.Rect.Height * _scale;
                if (label is not null) FitLabelToWidth(label, element.Width - labelInset);
                control.AddItem(element);
            }
        }
        Rebuild();
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.ToolbarItems) or nameof(viewModel.Style)) Rebuild();
        });
        return control;
    }

    // The icon a toolbar item draws, or null when the control owns no image list and
    // the button is text-only.
    private static WriteableBitmap? BitmapForToolbarItem(ToolbarItemSnapshot item)
    {
        if (item.ImageWidth is null && item.ImageHeight is null &&
            item.ImageFormat is null && item.ImageData is null)
            return null;
        return BitmapFromBase64(item.ImageWidth, item.ImageHeight, item.ImageFormat, item.ImageData);
    }

    private static void BindToolbarImageState(ButtonBase button, Image image,
        ToolbarItemSnapshot item, WriteableBitmap normal, ulong nativeStyle)
    {
        const ulong hotTrackingStyles = 0x0800 | 0x1000; // TBSTYLE_FLAT | TBSTYLE_LIST.
        var hotTracking = (nativeStyle & hotTrackingStyles) != 0;
        static WriteableBitmap? Decode(ImageListEntry? entry) => entry is null ? null :
            BitmapFromBase64(entry.ImageWidth, entry.ImageHeight, entry.ImageFormat, entry.ImageData);
        var hot = Decode(item.HotImage);
        var disabled = Decode(item.DisabledImage);
        var pressed = Decode(item.PressedImage);
        void Apply()
        {
            image.Source = ToolbarImagePresentation.Select(item, button.IsEnabled,
                button.IsPointerOver, button.IsPressed,
                button is ToggleButton { IsChecked: true }, hotTracking) switch
            {
                ToolbarImageState.Hot => hot,
                ToolbarImageState.Disabled => disabled,
                ToolbarImageState.Pressed => pressed,
                _ => normal,
            };
        }
        // IsPressed also covers keyboard activation and capture cancellation;
        // IsEnabled reflects the toolbar's inherited effective enabled state.
        button.RegisterPropertyChangedCallback(ButtonBase.IsPressedProperty, (_, _) => Apply());
        button.RegisterPropertyChangedCallback(ButtonBase.IsPointerOverProperty, (_, _) => Apply());
        button.RegisterPropertyChangedCallback(Control.IsEnabledProperty, (_, _) => Apply());
        if (button is ToggleButton)
            button.RegisterPropertyChangedCallback(ToggleButton.IsCheckedProperty, (_, _) => Apply());
        Apply();
    }

    // Every owned pixel buffer arrives validated as canonical premultiplied BGRA. The
    // size is rechecked here because a bitmap built from a short buffer would silently
    // draw whatever memory followed it.
    internal static WriteableBitmap BitmapFromBase64(
        int? width, int? height, string? format, string? data)
    {
        if (width is not { } w || height is not { } h ||
            format != "bgra8-premultiplied" || data is null)
            throw new InvalidOperationException("Validated pixel metadata is unavailable.");
        return BitmapFromPixels(w, h, Convert.FromBase64String(data));
    }

    internal static WriteableBitmap BitmapFromPixels(int width, int height, byte[] pixels)
    {
        if (width <= 0 || height <= 0 || pixels.Length != checked(width * height * 4))
            throw new InvalidOperationException("Validated pixel count changed.");
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
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Text) or nameof(viewModel.Active) or
                nameof(viewModel.WindowState) or nameof(viewModel.ClientRect) or
                nameof(viewModel.Rect))
                Apply();
        });
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
        }, (vertical, position, lines) =>
        {
            if (_isApplyingCanonical() || !AllowsAction(viewModel, "setScroll")) return;
            _action(viewModel, "setScroll", new SetScrollActionValue
            {
                Vertical = vertical,
                Position = position,
                Lines = lines,
            });
        });
        void Apply()
        {
            control.ApplySplits(viewModel.Splits, _scale);
            control.ApplyChrome(viewModel.ChromeRegions, _scale);
            control.ApplyScrollBars(viewModel.ScrollBars, viewModel.ClientRect, _scale);
        }
        Apply();
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Splits) or nameof(viewModel.Rect) or
                nameof(viewModel.ChromeRegions) or nameof(viewModel.ScrollBars) or nameof(viewModel.ClientRect))
                Apply();
        });
        return control;
    }

    // An accessible island's elements own no HWND, so the projection renders a real
    // Fluent control for each one and drives it by asking the provider to perform that
    // element's own default action.  The action string is also the accessible
    // description, so what the projection promises is what the provider named.
    private FrameworkElement CreateDiskView(ControlNodeViewModel viewModel)
    {
        var control = new DiskViewControl(
            (action, id) => { if (!_isApplyingCanonical()) _action(viewModel, action, new DiskViewItemActionValue { ItemId = id }); },
            position => { if (!_isApplyingCanonical()) _action(viewModel, "diskScroll", new DiskViewScrollActionValue { Position = position }); });
        void Apply() { if (viewModel.DiskView is { } view) control.Apply(view, _scale); }
        Apply();
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.DiskView) or nameof(viewModel.Rect)) Apply();
        });
        return control;
    }

    private FrameworkElement CreateNativeDocument(ControlNodeViewModel viewModel)
    {
        var control = new NativeDocumentControl(
            index =>
            {
                if (_isApplyingCanonical() || viewModel.Document is not { } document ||
                    index < 0 || index >= document.Items.Count) return;
                var item = document.Items[index];
                _action(viewModel, "htmlInvoke", new NativeDocumentInvokeActionValue
                {
                    Index = index, DocumentId = document.DocumentId,
                    Key = item.Key, ActionIdentity = item.ActionIdentity,
                });
            },
            request => { if (!_isApplyingCanonical()) _action(viewModel, "htmlScroll", request); });
        void Apply() { if (viewModel.Document is { } document) control.Apply(document, _scale); }
        Apply();
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Document) or nameof(viewModel.Rect)) Apply();
        });
        return control;
    }
    private FrameworkElement CreateAccessibleIsland(ControlNodeViewModel viewModel)
    {
        if (viewModel.IslandItems.Count > 0 && viewModel.IslandItems[0].Kind == "pageTab")
        {
            var tabs = new SemanticTabControl(index =>
            {
                if (_isApplyingCanonical() || !AllowsAction(viewModel, "islandInvoke") ||
                    index < 0 || index >= viewModel.IslandItems.Count) return;
                var item = viewModel.IslandItems[index];
                if (item.Enabled && !item.Selected) _action(viewModel, "islandInvoke", index);
            });
            void ApplyTabs()
            {
                var items = viewModel.IslandItems;
                tabs.Rebuild(items.Select(item => item.Name).ToArray(),
                    items.Select(item => item.Rect).ToArray(),
                    items.Select((item, index) => (item, index)).Single(pair => pair.item.Selected).index,
                    _scale, labelsHaveMnemonics: false, allowEdgeOverlap: true);
                for (var index = 0; index < items.Count; ++index)
                    tabs.Headers[index].IsEnabled = items[index].Enabled;
            }
            ApplyTabs();
            _lifetime.Subscribe(viewModel, (_, args) =>
            {
                if (args.PropertyName is nameof(viewModel.IslandItems) or nameof(viewModel.Rect))
                    ApplyTabs();
            });
            return tabs;
        }
        var control = new SemanticAccessibleIsland(index =>
        {
            if (_isApplyingCanonical() || !AllowsAction(viewModel, "islandInvoke")) return;
            _action(viewModel, "islandInvoke", index);
        });
        void Apply() => control.ApplyItems(viewModel.IslandItems, _scale);
        Apply();
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.IslandItems) or nameof(viewModel.Rect))
                Apply();
        });
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
        // A native tab strip is a band of exact rectangles, and WinUI's TabView
        // template applies its own minimum item height and overflow behavior on top
        // of the sizes the projection asks for.  Recording both is the only way to
        // tell a capture that reported a wrong band from a strip WinUI reshaped.
        void LogTabLayout(object? sender, object args)
        {
            control.LayoutUpdated -= LogTabLayout;
            RendererDiagnostics.Log(
                $"tabControl layout requested={viewModel.Rect.Width}x{viewModel.Rect.Height} " +
                $"scale={_scale:F3} owner={control.ActualWidth:F1}x{control.ActualHeight:F1} " +
                $"headers=[{string.Join("; ", control.Headers.Select(header => header is null ? "null" : $"{header.ActualWidth:F1}x{header.ActualHeight:F1}"))}] " +
                $"rows=[{string.Join("; ", control.Rows.Select(row => $"{Canvas.GetLeft(row):F1},{Canvas.GetTop(row):F1} {row.ActualWidth:F1}x{row.ActualHeight:F1}"))}] " +
                $"itemRects=[{string.Join("; ", viewModel.ItemRects.Select(rect => $"{rect.X},{rect.Y} {rect.Width}x{rect.Height}"))}]");
        }
        control.LayoutUpdated += LogTabLayout;
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Items) or nameof(viewModel.ItemRects))
                RebuildHeaders();
            else if (args.PropertyName == nameof(viewModel.SelectedIndex))
                control.ApplySelection(viewModel.SelectedIndex);
        });
        return control;
    }

    // The native tree owns hierarchy, selection, and expansion, so the projection
    // routes both gestures back to it and rebuilds from whatever the next canonical
    // revision reports.  Expansion is a real route rather than a local view state:
    // a lazily populated tree only inserts its children when the control itself
    // raises TVN_ITEMEXPANDING.
    private SemanticTreeViewControl CreateTreeView(ControlNodeViewModel viewModel)
    {
        var endedNativeSession = -1;
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
                // The answer to the application's own session (see the list's).
                var native = index >= 0 && viewModel.EditingIndex == index;
                if (native) endedNativeSession = index;
                if (_isApplyingCanonical() || !viewModel.EditableLabels ||
                    index < 0 || index >= viewModel.Items.Count ||
                    text.Length == 0 || (text == viewModel.Items[index] && !native) ||
                    !AllowsAction(viewModel, "setItemText")) return;
                _action(viewModel, "setItemText", new ItemTextActionValue
                {
                    Index = index,
                    Text = text,
                });
            });

        ReportFocusEntry(control, viewModel);

        void Rebuild()
        {
            if (!HasRenderableTreeShape(viewModel)) return;
            control.EditableLabels = viewModel.EditableLabels;
            control.Rebuild(viewModel.Items, viewModel.ItemDepths,
                viewModel.ItemExpanded, viewModel.ItemHasChildren,
                ItemIcons(viewModel, viewModel.Items.Count, viewModel.SelectedIndex),
                StateIcons(viewModel, viewModel.Items.Count),
                viewModel.SelectedIndex);
            ApplyNativeEditSession();
        }

        void ApplyNativeEditSession()
        {
            var editing = viewModel.EditingIndex;
            if (editing != endedNativeSession) endedNativeSession = -1;
            control.ApplyNativeEditSession(editing == endedNativeSession ? -1 : editing);
        }
        control.Tree.ContextRequested += (_, args) =>
        {
            var source = args.OriginalSource as DependencyObject;
            if (args.Handled || source is TextBox) return;
            var index = control.ItemIndexFor(source, out var container);
            args.Handled = RequestContextMenu(viewModel, control, container, index, args);
        };
        // A double-click (or Enter) on a leaf of a snap-in's tree opens the item (Device Manager
        // opens the device's properties). An item with children keeps the expand/collapse a
        // double-click already means.
        bool ActivateLeaf(int index) =>
            index >= 0 && index < viewModel.ItemHasChildren.Count && !viewModel.ItemHasChildren[index] &&
            RequestOpenItem(viewModel, index);
        // The tree's own items handle these gestures (expansion, invocation), and the first press
        // of a double-click can rebuild the items under the pointer, which cancels a recognized
        // double-tap. So a second press on the same item within the system double-click time is
        // the gesture, and the handlers listen for events that are already marked handled too.
        control.Tree.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler((_, args) =>
        {
            if (args.OriginalSource is TextBox || !args.GetCurrentPoint(control.Tree).Properties.IsLeftButtonPressed) return;
            var index = control.ItemIndexFor(args.OriginalSource as DependencyObject, out var container);
            var doubled = _itemPresses.Note(viewModel.NodeId, container is null ? -1 : index,
                Environment.TickCount64, ProjectedDoubleClickTime.Milliseconds);
            if (viewModel.ItemDefaultActivation)
                RendererDiagnostics.Log($"tree press node={viewModel.NodeId} index={index} container={container is not null} doubled={doubled}");
            if (doubled && !ActivateLeaf(index))
                RendererDiagnostics.Log(
                    $"tree double-click was not opened node={viewModel.NodeId} index={index} " +
                    $"leaf={index >= 0 && index < viewModel.ItemHasChildren.Count && !viewModel.ItemHasChildren[index]} " +
                    $"defaultActivation={viewModel.ItemDefaultActivation} openItem={AllowsAction(viewModel, "openItem")}");
        }), true);
        control.Tree.AddHandler(UIElement.KeyDownEvent, new KeyEventHandler((_, args) =>
        {
            if (args.Key != VirtualKey.Enter || args.OriginalSource is TextBox) return;
            if (ActivateLeaf(control.SelectedIndex)) args.Handled = true;
        }), true);
        Rebuild();
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            // A node opening or closing, or an icon changing, does not change the hierarchy: the
            // nodes that stand for it keep their place (and the tree its scroll position).
            if (args.PropertyName == nameof(viewModel.ItemDepths) && HasRenderableTreeShape(viewModel) &&
                control.TryApplyExpansion(viewModel.Items, viewModel.ItemDepths, viewModel.ItemExpanded, viewModel.ItemHasChildren))
                return;
            if (args.PropertyName == nameof(viewModel.ItemImages) && HasRenderableTreeShape(viewModel) &&
                control.HasHierarchy(viewModel.Items, viewModel.ItemDepths, viewModel.ItemHasChildren))
            {
                control.ApplyIcons(ItemIcons(viewModel, viewModel.Items.Count, viewModel.SelectedIndex));
                control.ApplyStateIcons(StateIcons(viewModel, viewModel.Items.Count));
                return;
            }
            if (args.PropertyName is nameof(viewModel.Items) or nameof(viewModel.ItemDepths) or
                nameof(viewModel.ItemImages) or nameof(viewModel.EditableLabels))
            {
                if (viewModel.ItemDefaultActivation)
                    RendererDiagnostics.Log($"tree node={viewModel.NodeId} rebuilt for {args.PropertyName}");
                Rebuild();
            }
            else if (args.PropertyName == nameof(viewModel.SelectedIndex))
            {
                control.ApplySelection(viewModel.SelectedIndex);
                // A native tree draws the selected-state icon for the selected item,
                // so a selection move can change two rows' icons.
                if (viewModel.ItemSelectedImages.Count > 0)
                    control.ApplyIcons(ItemIcons(
                        viewModel, viewModel.Items.Count, viewModel.SelectedIndex));
            }
            else if (args.PropertyName == nameof(viewModel.EditingIndex))
                ApplyNativeEditSession();
        });
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

    // State images are a second list, drawn to the left of the item icon. -1,
    // or a missing entry, draws nothing. The index is not swapped on selection:
    // a state glyph is not the selected-state icon.
    private static IReadOnlyList<ImageSource?> StateIcons(ControlNodeViewModel viewModel, int itemCount)
    {
        var decoded = new WriteableBitmap?[viewModel.StateImageList.Count];
        var icons = new ImageSource?[itemCount];
        for (var index = 0; index < itemCount; ++index)
        {
            if (index >= viewModel.ItemStateImages.Count) continue;
            var image = viewModel.ItemStateImages[index];
            if (image < 0 || image >= decoded.Length) continue;
            decoded[image] ??= BitmapForImageListEntry(viewModel.StateImageList[image]);
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

    internal static WriteableBitmap BitmapForImageListEntry(ImageListEntry entry) =>
        BitmapFromBase64(entry.ImageWidth, entry.ImageHeight, entry.ImageFormat, entry.ImageData);

    // A container's own painted band, reproduced from the pixels the native window
    // drew.  The projection cannot re-render what it never parsed, so it publishes
    // exactly those pixels rather than approximating the band.
    // In dark mode a light band is redrawn on the theme's side of the scale (see
    // ChromeThemeAdaptation): a white header over a dark window read as a hole in it.
    internal static WriteableBitmap BitmapForChromeRegion(ChromeRegion region, bool dark = false)
    {
        if (region.ImageFormat != "bgra8-premultiplied")
            throw new InvalidOperationException("Validated pixel metadata is unavailable.");
        var pixels = Convert.FromBase64String(region.ImageData);
        if (dark) ChromeThemeAdaptation.AdaptToDark(pixels, region.ImageWidth);
        return BitmapFromPixels(region.ImageWidth, region.ImageHeight, pixels);
    }

    // A trackbar is projected as a stock Slider: integer steps, no invented ticks,
    // and no thumb tooltip, because the adapter captures a position and a range and
    // nothing else.  A vertical native trackbar counts downward from its top, which
    // is the reverse of a WinUI vertical Slider, so orientation and the native
    // TBS_REVERSED hint are combined into one direction.
    private Slider CreateSlider(ControlNodeViewModel viewModel)
    {
        // TBS_TOOLTIPS: the native control shows its position beside the thumb while it
        // is dragged, which is exactly what the Slider's thumb tooltip shows.
        const ulong tbsTooltips = 0x0100;
        var control = new Slider
        {
            Orientation = viewModel.Vertical ? Orientation.Vertical : Orientation.Horizontal,
            IsDirectionReversed = viewModel.Reversed ^ viewModel.Vertical,
            IsThumbToolTipEnabled = (viewModel.Style & tbsTooltips) != 0,
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
        _lifetime.Subscribe(viewModel, (_, args) =>
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
        });
        return control;
    }

    private const ulong UdsWrap = 0x0001;

    // Where one arrow step lands.  The up (or right) arrow moves toward the native
    // upper bound, which is the lower end when the range runs backwards; UDS_WRAP
    // carries a step past one end round to the other, and otherwise the end holds.
    internal static int UpDownStepTarget(
        int minimum, int maximum, int position, int step, bool reversed, bool wrap, bool up)
    {
        var delta = (long)step * ((up ^ reversed) ? 1 : -1);
        var target = position + delta;
        if (target > maximum) return wrap ? minimum : maximum;
        if (target < minimum) return wrap ? maximum : minimum;
        return (int)target;
    }

    private FrameworkElement CreateUpDown(ControlNodeViewModel viewModel)
    {
        var control = new SemanticUpDownControl(viewModel.Vertical);
        void Step(bool up)
        {
            if (_isApplyingCanonical() || !AllowsAction(viewModel, "setValue")) return;
            var target = UpDownStepTarget(viewModel.Minimum, viewModel.Maximum, viewModel.Position,
                Math.Max(1, viewModel.SmallChange), viewModel.Reversed,
                (viewModel.Style & UdsWrap) != 0, up);
            if (target != viewModel.Position) _action(viewModel, "setValue", target);
        }
        void RequestValue(int value)
        {
            if (_isApplyingCanonical() || !AllowsAction(viewModel, "setValue")) return;
            var target = Math.Clamp(value, viewModel.Minimum, viewModel.Maximum);
            if (target != viewModel.Position) _action(viewModel, "setValue", target);
        }
        control.Stepped += Step;
        control.ValueRequested += RequestValue;
        void ApplyCanonical() =>
            control.Apply(viewModel.Minimum, viewModel.Maximum, viewModel.Position,
                Math.Max(1, viewModel.SmallChange));
        ApplyCanonical();
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Minimum) or nameof(viewModel.Maximum) or
                nameof(viewModel.Position) or nameof(viewModel.SmallChange))
                ApplyCanonical();
        });
        return control;
    }

    // A native date or time picker (minor 37): the value is the position of a range -- a day
    // number or a second of the day -- and a chosen value is asked of the native control as
    // setValue, like a slider's.
    private SemanticDatePicker CreateDatePicker(ControlNodeViewModel viewModel)
    {
        var control = new SemanticDatePicker
        {
            FontSize = NativeFontSize,
            MinWidth = 0,
            MinHeight = 0,
            Padding = new Thickness(6, 0, 0, 0),
            PlaceholderText = string.Empty,
        };
        control.DayRequested += day =>
        {
            if (_isApplyingCanonical() || day == viewModel.Position || !AllowsAction(viewModel, "setValue")) return;
            _action(viewModel, "setValue", day);
        };
        void ApplyCanonical() => control.Apply(viewModel.Minimum, viewModel.Maximum, viewModel.Position);
        ApplyCanonical();
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Minimum) or nameof(viewModel.Maximum) or nameof(viewModel.Position))
                ApplyCanonical();
        });
        return control;
    }

    private SemanticTimePicker CreateTimePicker(ControlNodeViewModel viewModel)
    {
        var control = new SemanticTimePicker(NativeFontSize);
        control.SecondRequested += second =>
        {
            if (_isApplyingCanonical() || second == viewModel.Position || !AllowsAction(viewModel, "setValue")) return;
            _action(viewModel, "setValue", second);
        };
        control.Apply(viewModel.Position, viewModel.Text);
        _lifetime.Subscribe(viewModel, (_, args) =>
        {
            if (args.PropertyName is nameof(viewModel.Position) or nameof(viewModel.Text))
                control.Apply(viewModel.Position, viewModel.Text);
        });
        return control;
    }

    internal static SysLinkSegments SplitSysLinkText(string text, string label)
    {
        if (!TrySplitSysLinkText(text, label, out var segments))
            throw new ArgumentException("SysLink label must occur exactly once in its text.", nameof(label));
        return segments;
    }

    // The text as plain and link runs, in order.
    private static bool TrySplitSysLinkRuns(string text, IReadOnlyList<string> labels,
        out List<(string Text, bool IsLink)> runs)
    {
        runs = [];
        if (!ProtocolValidator.TryLocateSysLinkLabels(text, labels, out var starts)) return false;
        var cursor = 0;
        for (var index = 0; index < labels.Count; index++)
        {
            if (starts[index] > cursor) runs.Add((text[cursor..starts[index]], false));
            runs.Add((labels[index], true));
            cursor = starts[index] + labels[index].Length;
        }
        if (cursor < text.Length) runs.Add((text[cursor..], false));
        return true;
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
        (!viewModel.GroupViewEnabled || (viewModel.ItemRects.Count == viewModel.Items.Count &&
            viewModel.ItemGroupIds.Count == viewModel.Items.Count)) && (viewModel.ListViewMode == "report"
            ? viewModel.Columns.Count != 0 &&
              viewModel.ColumnWidths.Count == viewModel.Columns.Count &&
              viewModel.Rows.All(row => row.Count == viewModel.Columns.Count)
            : viewModel.Columns.Count == 0 && viewModel.Rows.Count == 0 &&
              viewModel.ItemRects.Count == viewModel.Items.Count);

    internal static bool ShouldRenderListViewHeader(ControlNodeViewModel viewModel) =>
        viewModel.ListViewMode == "report" && viewModel.ColumnHeadersVisible;

    // The user moving the keyboard focus into a projected list or tree is, natively, the click
    // (or Tab) that focuses that control.  The Bridge moves the thread's focus there as well,
    // because the application's menu commands act on the pane that holds it: regedit's Rename
    // and Delete otherwise reached the tree's key while the user worked in the list of values.
    // It is sent once per visit: the first focus the user gives the control (the window's own
    // first focus, or a restore after a rebuild, is programmatic and may already be inside it),
    // and again only after the focus has left it.
    private void ReportFocusEntry(UIElement control, ControlNodeViewModel viewModel)
    {
        var reported = false;
        control.GettingFocus += (_, args) =>
        {
            if (reported || !_lifetime.IsActive || _isApplyingCanonical() ||
                args.FocusState is not (FocusState.Pointer or FocusState.Keyboard) ||
                !AllowsAction(viewModel, "focus")) return;
            reported = true;
            _action(viewModel, "focus", null);
        };
        control.LosingFocus += (_, args) =>
        {
            if (!IsWithin(args.NewFocusedElement, control)) reported = false;
        };
    }

    private static bool IsWithin(DependencyObject? element, DependencyObject container)
    {
        for (var current = element; current is not null; current = VisualTreeHelper.GetParent(current))
            if (ReferenceEquals(current, container)) return true;
        return false;
    }

    private static bool IsListItemEditor(DependencyObject? source, DependencyObject boundary)
    {
        while (source is not null && source != boundary)
        {
            if (source is TextBox or CheckBox) return true;
            source = VisualTreeHelper.GetParent(source);
        }
        return false;
    }

    internal static ScrollViewer? FindScrollViewer(DependencyObject root)
    {
        for (var index = 0; index < VisualTreeHelper.GetChildrenCount(root); ++index)
        {
            var child = VisualTreeHelper.GetChild(root, index);
            if (child is ScrollViewer scroll) return scroll;
            if (FindScrollViewer(child) is { } descendant) return descendant;
        }
        return null;
    }

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
        viewModel.Items.Count <= ProtocolConstants.MaxItems &&
        viewModel.ItemDepths.Count == viewModel.Items.Count &&
        viewModel.ItemExpanded.Count == viewModel.Items.Count &&
        viewModel.ItemHasChildren.Count == viewModel.Items.Count &&
        viewModel.ItemStateImages.Count == viewModel.Items.Count &&
        (viewModel.Items.Count == 0 || viewModel.ItemDepths[0] == 0) &&
        viewModel.SelectedIndex >= -1 && viewModel.SelectedIndex < viewModel.Items.Count;

    internal static IReadOnlyList<TabHeaderRow> GroupTabHeaderRows(
        IReadOnlyList<PixelRect> rects, bool allowEdgeOverlap = false)
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
                var previous = items[index - 1].Rect;
                var current = items[index].Rect;
                var orderedEdges = previous.X < current.X &&
                    (long)previous.X + previous.Width < (long)current.X + current.Width;
                if ((long)previous.X + previous.Width > current.X &&
                    (!allowEdgeOverlap || !orderedEdges))
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
        "TextFillColorSecondaryBrush",
        "SolidBackgroundFillColorBaseBrush",
        "ControlStrongStrokeColorDefaultBrush",
        "ControlStrokeColorOnAccentSecondaryBrush",
        "LayerFillColorAltBrush",
        "DividerStrokeColorDefaultBrush",
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

    // A plain panel has no IsEnabled of its own, so its peer would always report
    // enabled.  A projected container answers with its native window's state instead,
    // which is what lets a container the application disabled (MMC disables its MDI
    // client while a snap-in holds the console) project truthfully.
    internal static bool NodeEnabled(FrameworkElement element) =>
        element.DataContext is not ControlNodeViewModel model || model.Enabled;

    internal static AutomationControlType AutomationControlTypeFor(string kind) => kind switch
    {
        "static" => AutomationControlType.Text,
        "staticIcon" or "staticBitmap" => AutomationControlType.Image,
        "upDown" => AutomationControlType.Spinner,
        "datePicker" or "timePicker" => AutomationControlType.Button,
        "sysLink" => AutomationControlType.Pane,
        "listView" => AutomationControlType.List,
        "treeView" => AutomationControlType.Tree,
        "tabControl" => AutomationControlType.Tab,
        "slider" => AutomationControlType.Slider,
        "dialogContainer" => AutomationControlType.Pane,
        "mdiClient" => AutomationControlType.Pane,
        "paneContainer" => AutomationControlType.Pane,
        "richText" => AutomationControlType.Pane,
        "performanceGraph" or "performanceValueBar" => AutomationControlType.Pane,
        "performanceLegend" => AutomationControlType.List,
        "accessibleIsland" or "nativeDocument" or "diskView" => AutomationControlType.Group,
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

    // A SysLink's default peer reports the centered text line, not the HWND slot.
    // MMC's About dialog measured that as 368x16 at y+11 against a 368x38 native
    // control, which misses the committed gate's 18px tolerance on height. The
    // visual is already in physical pixels and inset inside the slot, so the slot
    // origin is that visual shifted back by half the leftover on each axis.
    internal static global::Windows.Foundation.Rect CenteredLayoutSlotBounds(
        global::Windows.Foundation.Rect reported,
        double slotWidth,
        double slotHeight)
    {
        if (slotWidth <= 0 || slotHeight <= 0 ||
            reported.Width <= 0 || reported.Height <= 0)
            return reported;
        var x = reported.X;
        var y = reported.Y;
        var insetX = slotWidth - reported.Width;
        var insetY = slotHeight - reported.Height;
        if (insetX > 1) x -= insetX / 2;
        if (insetY > 1) y -= insetY / 2;
        return new global::Windows.Foundation.Rect(x, y, slotWidth, slotHeight);
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
        // The native control defines the extent. A default minimum larger than it (WinUI's
        // CheckBox is 120 px wide at least, a ComboBox or TextBox 64) would report bounds the
        // native control does not have.
        if (element.MinWidth > element.Width) element.MinWidth = 0;
        if (element.MinHeight > element.Height) element.MinHeight = 0;
        if (viewModel.Kind is "dialogContainer" or "tabControl" or "toolbar" or "treeView"
            or "mdiClient" or "mdiChild" or "paneContainer" or "accessibleIsland" or "nativeDocument"
            or "statusBar" or "staticDecoration" or "richText"
            or "performanceGraph" or "performanceLegend" or "performanceValueBar")
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

// A projected UpDown: the two arrows the native control draws, stacked or side by
// side as its orientation says.  It owns no value of its own -- each arrow or
// keyboard step asks the Bridge to run the native step, and the canonical position
// comes back through Apply.
internal sealed class SemanticUpDownControl : ContentControl
{
    private readonly bool _vertical;

    public SemanticUpDownControl(bool vertical)
    {
        _vertical = vertical;
        var grid = new Grid();
        var first = Arrow(vertical ? "" : "", up: vertical);
        var second = Arrow(vertical ? "" : "", up: !vertical);
        if (vertical)
        {
            grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
            grid.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
            Grid.SetRow(second, 1);
        }
        else
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            Grid.SetColumn(second, 1);
        }
        grid.Children.Add(first);
        grid.Children.Add(second);
        Content = grid;
        MinWidth = 0;
        MinHeight = 0;
        Padding = new Thickness(0);
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        VerticalContentAlignment = VerticalAlignment.Stretch;
    }

    public event Action<bool>? Stepped;
    public event Action<int>? ValueRequested;
    public int Minimum { get; private set; }
    public int Maximum { get; private set; }
    public int Value { get; private set; }
    public int SmallChange { get; private set; } = 1;

    public void Apply(int minimum, int maximum, int value, int smallChange)
    {
        Minimum = minimum;
        Maximum = maximum;
        Value = value;
        SmallChange = smallChange;
    }

    public void RequestValue(int value) => ValueRequested?.Invoke(value);

    private RepeatButton Arrow(string glyph, bool up)
    {
        var button = new RepeatButton
        {
            Content = new FontIcon { Glyph = glyph, FontSize = 8 },
            Padding = new Thickness(0),
            MinWidth = 0,
            MinHeight = 0,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
            IsTabStop = false,
        };
        // The arrows are parts of the one spinner the native control is, so only the
        // spinner itself appears to assistive technology.
        AutomationProperties.SetAccessibilityView(button, AccessibilityView.Raw);
        button.Click += (_, _) => Stepped?.Invoke(up);
        return button;
    }

    protected override void OnKeyDown(Microsoft.UI.Xaml.Input.KeyRoutedEventArgs args)
    {
        var up = args.Key switch
        {
            global::Windows.System.VirtualKey.Up => _vertical ? true : (bool?)null,
            global::Windows.System.VirtualKey.Down => _vertical ? false : (bool?)null,
            global::Windows.System.VirtualKey.Right => _vertical ? (bool?)null : true,
            global::Windows.System.VirtualKey.Left => _vertical ? (bool?)null : false,
            _ => null,
        };
        if (up is { } direction)
        {
            args.Handled = true;
            Stepped?.Invoke(direction);
            return;
        }
        base.OnKeyDown(args);
    }

    protected override AutomationPeer OnCreateAutomationPeer() =>
        new SemanticUpDownAutomationPeer(this);
}

internal sealed class SemanticUpDownAutomationPeer(SemanticUpDownControl owner)
    : FrameworkElementAutomationPeer(owner), IRangeValueProvider
{
    protected override AutomationControlType GetAutomationControlTypeCore() =>
        ControlFactory.AutomationControlTypeFor("upDown");
    protected override string GetClassNameCore() => "ProjectedUpDown";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
    protected override bool IsControlElementCore() => true;
    protected override bool IsContentElementCore() => true;
    protected override object GetPatternCore(PatternInterface patternInterface) =>
        patternInterface == PatternInterface.RangeValue
            ? this : base.GetPatternCore(patternInterface);

    public bool IsReadOnly => !owner.IsEnabled;
    public double Maximum => owner.Maximum;
    public double Minimum => owner.Minimum;
    public double Value => owner.Value;
    public double SmallChange => owner.SmallChange;
    public double LargeChange => owner.SmallChange;
    public void SetValue(double value) => owner.RequestValue((int)Math.Round(value));
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

internal sealed class SemanticGroupControl(Canvas children) : ContentControl, ISemanticContainer
{
    public UIElementCollection Children => children.Children;

    protected override AutomationPeer OnCreateAutomationPeer() => new SemanticGroupAutomationPeer(this);
}

internal sealed class SemanticTabControl : ContentControl, ISemanticContainer
{
    private readonly Action<int> _selectionRequested;
    private readonly List<TabViewItem> _headers = [];
    private readonly List<TabView> _rows = [];
    private readonly Dictionary<TabView, TabHeaderRow> _rowGeometry = [];
    private readonly Dictionary<TabView, global::Windows.Foundation.Rect> _rowBands = [];
    private readonly Dictionary<TabViewItem, double> _pillWidths = [];
    private readonly Dictionary<TabViewItem, int> _indices = [];
    private readonly Canvas _headerCanvas = new() { Background = null };
    // Windows Forms parents its TabPage windows (and the strip's own up-down) to the
    // tab control itself, so those projected nodes live here, above the strip.
    private readonly Canvas _pageCanvas = new() { Background = null };
    // Fills the control without taking input, so its peer measures the control's own
    // slot. The rendered content alone (headers, pages) neither starts at the
    // control's origin nor spans it once a strip overflows or pages are present.
    private readonly BoundsFrame _frame = new();
    private int _selectedIndex = -1;
    private bool _applyingSelection;

    public SemanticTabControl(Action<int> selectionRequested)
    {
        _selectionRequested = selectionRequested;
        var layers = new Grid { Background = null };
        layers.Children.Add(_frame);
        layers.Children.Add(_headerCanvas);
        layers.Children.Add(_pageCanvas);
        Content = layers;
        Background = null;
        BorderThickness = new Thickness(0);
        Padding = new Thickness(0);
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        VerticalContentAlignment = VerticalAlignment.Stretch;
        GotFocus += (_, args) =>
        {
            // Keyboard focus that lands on a header or on the control itself goes to the selected
            // header, as the native tab control takes the Tab key. Focus that a click gave a header,
            // or that sits in a control of the page, is where the user put it: this event bubbles
            // from the page, and pulling that focus back to the header left every edit on a
            // property page unable to take typing.
            if (args.OriginalSource is not TabViewItem && !ReferenceEquals(args.OriginalSource, this)) return;
            if (args.OriginalSource is UIElement { FocusState: not FocusState.Keyboard }) return;
            if (_selectedIndex >= 0 && _selectedIndex < _headers.Count &&
                !_headers[_selectedIndex].FocusState.Equals(FocusState.Keyboard))
                _headers[_selectedIndex].Focus(FocusState.Keyboard);
        };
    }

    public IReadOnlyList<TabViewItem> Headers => _headers;
    public IReadOnlyList<TabView> Rows => _rows;
    public UIElementCollection Children => _headerCanvas.Children;
    UIElementCollection ISemanticContainer.Children => _pageCanvas.Children;
    public UIElementCollection PageChildren => _pageCanvas.Children;
    internal FrameworkElement Frame => _frame;

    // A transparent, non-interactive canvas whose peer exists only to be measured.
    internal sealed class BoundsFrame : Canvas
    {
        public BoundsFrame()
        {
            Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
            IsHitTestVisible = false;
            AutomationProperties.SetAccessibilityView(this, AccessibilityView.Raw);
        }

        protected override AutomationPeer OnCreateAutomationPeer() => new FrameworkElementAutomationPeer(this);
    }
    public int SelectedIndex => _selectedIndex;
    public global::Windows.Foundation.Rect HeaderUnionBounds { get; private set; }

    public void Rebuild(
        IReadOnlyList<string> labels,
        IReadOnlyList<PixelRect> rects,
        int selectedIndex,
        double scale,
        bool labelsHaveMnemonics = true,
        bool allowEdgeOverlap = false)
    {
        Children.Clear();
        _headers.Clear();
        _rows.Clear();
        _rowGeometry.Clear();
        _rowBands.Clear();
        _pillWidths.Clear();
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
        foreach (var row in ControlFactory.GroupTabHeaderRows(rects, allowEdgeOverlap))
        {
            // The row is deliberately unsized.  Constraining a TabView to the native
            // band height makes its own template clip the tab strip to a few pixels,
            // so it is left to size to its content and calibrated into place after
            // layout instead.
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
                HorizontalAlignment = HorizontalAlignment.Left,
                VerticalAlignment = VerticalAlignment.Top,
            };
            var previousRight = row.Bounds.X;
            foreach (var placement in row.Items)
            {
                var text = labelsHaveMnemonics
                    ? Win32Mnemonic.DisplayText(labels[placement.Index]) : labels[placement.Index];
                var pillWidth = placement.Rect.Width * scale;
                // The native control sized each pill by measuring its label with GDI, and
                // WinUI's text metrics are wider at the same point size.  The label is
                // fitted to the room the pill already committed to, which is also what
                // keeps the pill at the native width: TabView clears any Width the
                // projection assigns and sizes SizeToContent pills from their content, so
                // the width is pinned by MinWidth from below and by the fitted content
                // from above.
                var label = new TextBlock
                {
                    Text = text,
                    FontSize = NativeTabFontSize,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center,
                };
                ControlFactory.FitLabelToWidth(label,
                    pillWidth - (HeaderChrome.Left + HeaderChrome.Right));
                AutomationProperties.SetAccessibilityView(label, AccessibilityView.Raw);
                var content = new Border
                {
                    Child = label,
                    Background = null,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center,
                    // The vertical half of the template's own header padding is given
                    // back, so an 18 DIP band still measures a full line box instead of
                    // clipping the label's descenders.
                    Margin = new Thickness(0, -HeaderChrome.Top, 0, -HeaderChrome.Bottom),
                };
                var header = new TabViewItem
                {
                    Header = content,
                    IsClosable = Policy.AreItemsClosable,
                    FontSize = NativeTabFontSize,
                    MinWidth = 0,
                    MinHeight = 0,
                    HorizontalContentAlignment = HorizontalAlignment.Center,
                    VerticalContentAlignment = VerticalAlignment.Center,
                    Height = placement.Rect.Height * scale,
                    Margin = new Thickness((placement.Rect.X - previousRight) * scale, 0, 0, 0),
                };
                AutomationProperties.SetName(header, text);
                _pillWidths[header] = pillWidth;
                tabView.TabItems.Add(header);
                _headers[placement.Index] = header;
                _indices.Add(header, placement.Index);
                previousRight = placement.Rect.X + placement.Rect.Width;
            }
            tabView.SelectionChanged += OnRowSelectionChanged;
            tabView.LayoutUpdated += (_, _) => CalibrateRow(tabView);
            Children.Add(tabView);
            _rows.Add(tabView);
            _rowGeometry.Add(tabView, row);
            _rowBands.Add(tabView, new global::Windows.Foundation.Rect(
                row.Bounds.X * scale,
                row.Bounds.Y * scale,
                row.Bounds.Width * scale,
                row.Bounds.Height * scale));
        }
        ApplySelection(selectedIndex);
    }

    // WinUI's TabView owns the strip's internal metrics: an inset above the tab row,
    // its own padding, and a width manager that clears whatever Width the projection
    // assigns.  None of those are contracts the projection may depend on, so the row
    // is placed by measurement rather than by arithmetic over theme resources: once
    // the template has realized the pills, the offset from the row's own origin to
    // the first pill is known, and the row is shifted so that pill lands exactly on
    // the native band.  The clip then bounds the row to the band, which keeps the tab
    // content area WinUI reserves below the strip from covering the projected page or
    // swallowing clicks over it.
    private void CalibrateRow(TabView row)
    {
        if (!_rowBands.TryGetValue(row, out var band) ||
            !_rowGeometry.TryGetValue(row, out var geometry)) return;
        var pills = geometry.Items
            .Select(placement => _headers[placement.Index])
            .Where(pill => pill is not null && pill.ActualWidth > 0 && pill.ActualHeight > 0)
            .ToList();
        if (pills.Count != geometry.Items.Count) return;
        var origin = pills[0].TransformToVisual(row)
            .TransformPoint(new global::Windows.Foundation.Point(0, 0));
        var left = band.X - origin.X;
        var top = band.Y - origin.Y;
        if (Canvas.GetLeft(row) != left) Canvas.SetLeft(row, left);
        if (Canvas.GetTop(row) != top) Canvas.SetTop(row, top);
        var clip = new global::Windows.Foundation.Rect(
            origin.X, origin.Y, band.Width, band.Height);
        if (row.Clip is not RectangleGeometry existing || existing.Rect != clip)
            row.Clip = new RectangleGeometry { Rect = clip };

        // TabView clears any Width assigned to a SizeToContent pill and sizes it from
        // its content plus template chrome that no public property exposes.  The chrome
        // is therefore measured -- pill width minus content width -- and the content is
        // sized to whatever is left of the native item rectangle.  One extra layout pass
        // converges, because the chrome does not depend on the content.
        foreach (var pill in pills)
        {
            if (pill.Header is not Border content || content.ActualWidth <= 0 ||
                !_pillWidths.TryGetValue(pill, out var pillWidth)) continue;
            var target = pillWidth - (pill.ActualWidth - content.ActualWidth);
            if (target <= 0 ||
                (!double.IsNaN(content.Width) && Math.Abs(content.Width - target) <= 0.5)) continue;
            content.Width = target;
            if (content.Child is TextBlock label) ControlFactory.FitLabelToWidth(label, target);
        }
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

    // The native control owns selection, so a click is a request: the canonical index
    // is re-asserted immediately and the pill only moves when the accepted action's
    // patch says the native page changed.
    private void OnRowSelectionChanged(object sender, SelectionChangedEventArgs args)
    {
        if (_applyingSelection || sender is not TabView row ||
            row.SelectedItem is not TabViewItem selected ||
            !_indices.TryGetValue(selected, out var index)) return;
        ApplySelection(_selectedIndex);
        RequestSelection(index);
    }

    public void RequestSelection(int index)
    {
        if (index >= 0 && index < _headers.Count) _selectionRequested(index);
    }

    protected override AutomationPeer OnCreateAutomationPeer() => new SemanticTabControlAutomationPeer(this);

    internal static TabViewPolicy Policy { get; } = new(
        IsAddTabButtonVisible: false,
        CanDragTabs: false,
        CanReorderTabs: false,
        AreItemsClosable: false,
        TabWidthMode: TabViewWidthMode.SizeToContent);

    private const double NativeTabFontSize = 12;
    // WinUI applies TabViewItemHeaderPadding inside the TabViewItem template, where a
    // local Padding never reaches it.  The value is read from the framework's own
    // dictionary so the projection compensates for whatever the current SDK uses,
    // rather than hardcoding today's 8,3,4,3.
    private static readonly Thickness HeaderChrome = ReadHeaderChrome();

    private static Thickness ReadHeaderChrome()
    {
        if (Application.Current?.Resources.TryGetValue(
                "TabViewItemHeaderPadding", out var value) == true &&
            value is Thickness padding)
        {
            return padding;
        }
        return new Thickness(0);
    }
}

internal sealed class SemanticTabControlAutomationPeer(SemanticTabControl owner) :
    FrameworkElementAutomationPeer(owner), ISelectionProvider
{
    protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Tab;
    protected override string GetClassNameCore() => "TabControl";
    protected override bool IsControlElementCore() => true;
    protected override object GetPatternCore(PatternInterface patternInterface) =>
        patternInterface == PatternInterface.Selection ? this : base.GetPatternCore(patternInterface);
    // The tab items, then the projected nodes a Windows Forms tab control parents
    // (its pages and the strip's up-down), which are its own children natively too.
    protected override IList<AutomationPeer> GetChildrenCore() => owner.Headers
        .Cast<UIElement>()
        .Concat(owner.PageChildren)
        .Select(CreatePeerForElement)
        .Where(peer => peer is not null)
        .Cast<AutomationPeer>()
        .ToList();
    protected override global::Windows.Foundation.Rect GetBoundingRectangleCore()
    {
        var reported = base.GetBoundingRectangleCore();
        // The frame fills the control exactly, so its screen bounds are the slot's.
        if (CreatePeerForElement(owner.Frame) is { } frame &&
            frame.GetBoundingRectangle() is { Width: > 0, Height: > 0 } slot)
            return slot;
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
    private readonly Image _state = new() { Stretch = Stretch.None, IsHitTestVisible = false };
    private readonly Image _icon = new() { Stretch = Stretch.None, IsHitTestVisible = false };
    private readonly TextBlock _label = new()
    {
        FontSize = LabelFontSize,
        VerticalAlignment = VerticalAlignment.Center,
        TextTrimming = TextTrimming.CharacterEllipsis,
    };
    private readonly Action<string> _commit;
    private readonly bool _largeIcon;
    private TextBox? _editor;

    public ProjectedItemContent(Action<string> commit, bool largeIcon = false)
    {
        _commit = commit;
        _largeIcon = largeIcon;
        if (largeIcon)
        {
            RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
            Grid.SetRow(_label, 1);
            _icon.HorizontalAlignment = HorizontalAlignment.Center;
            _label.TextAlignment = TextAlignment.Center;
            _label.TextWrapping = TextWrapping.Wrap;
            _label.VerticalAlignment = VerticalAlignment.Top;
        }
        else
        {
            ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            Grid.SetColumn(_icon, 1);
            Grid.SetColumn(_label, 2);
        }
        Children.Add(_state);
        Children.Add(_icon);
        Children.Add(_label);
        AutomationProperties.SetAccessibilityView(_state, AccessibilityView.Raw);
        AutomationProperties.SetAccessibilityView(_icon, AccessibilityView.Raw);
        AutomationProperties.SetAccessibilityView(_label, AccessibilityView.Raw);
        ApplyStateIcon(null);
    }

    public string Text { get; private set; } = string.Empty;
    public bool IsEditing => _editor is not null;

    // The open editor stands for a label session the application opened itself (its own
    // Rename command), so ending it always answers that session: a new label renames, and
    // the unchanged one ends the session without a rename.
    public bool NativeSession { get; private set; }

    // Whether the native control admits in-place renaming.  It gates both the F2
    // gesture and the UIA Value pattern, which is how assistive technology renames
    // an item natively.
    public bool Editable { get; set; }

    // TreeView binds nodes as data, so the item container derives its accessible
    // name from the data object.  This element *is* that object.
    public override string ToString() => Text;

    public void Apply(string text, ImageSource? icon, ImageSource? stateIcon = null)
    {
        Text = text;
        _label.Text = text;
        AutomationProperties.SetName(this, text);
        ApplyStateIcon(stateIcon);
        ApplyIcon(icon);
        if (_editor is not null) _editor.Text = text;
    }

    public void ApplyStateIcon(ImageSource? icon)
    {
        _state.Source = icon;
        _state.Visibility = icon is null ? Visibility.Collapsed : Visibility.Visible;
        _state.Margin = icon is null ? new Thickness(0) : new Thickness(0, 0, 4, 0);
    }

    public void ApplyIcon(ImageSource? icon)
    {
        _icon.Source = icon;
        _icon.Visibility = icon is null ? Visibility.Collapsed : Visibility.Visible;
        _icon.Margin = icon is null ? new Thickness(0)
            : _largeIcon ? new Thickness(0, 0, 0, 2) : new Thickness(0, 0, 6, 0);
    }

    // Opens the projected editor.  Nothing native happens yet: the native control's
    // own label session runs once, at commit, so a projected edit never depends on
    // the cloaked window keeping keyboard focus while the user types.
    public void BeginEdit(bool nativeSession = false)
    {
        if (_editor is not null)
        {
            NativeSession |= nativeSession;
            return;
        }
        NativeSession = nativeSession;
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
        if (_largeIcon) Grid.SetRow(_editor, 1);
        else Grid.SetColumn(_editor, 2);
        Children.Add(_editor);
        _editor.SelectAll();
        _editor.Focus(FocusState.Programmatic);
    }

    public void EndEdit(bool commit)
    {
        if (_editor is not { } editor) return;
        var requested = editor.Text;
        var native = NativeSession;
        CloseEditor(editor);
        if (ItemLabelEdit.Answer(requested, Text, commit, native) is { } label) _commit(label);
    }

    // The application ended its own session (the label committed or was cancelled natively):
    // the projected editor closes and sends nothing.
    public void AbandonEdit()
    {
        if (_editor is { } editor) CloseEditor(editor);
    }

    private void CloseEditor(TextBox editor)
    {
        _editor = null;
        NativeSession = false;
        Children.Remove(editor);
        _label.Visibility = Visibility.Visible;
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

internal static class ItemLabelEdit
{
    // The label a closing editor sends, or null when it sends nothing.  An empty label is how
    // native in-place editing signals a cancel, so it is never sent as a rename.  An editor that
    // answers the application's own session always sends: its unchanged label ends that session
    // without a rename.
    public static string? Answer(string requested, string current, bool commit, bool nativeSession)
    {
        var renamed = commit && requested.Length != 0 && requested != current;
        return renamed ? requested : nativeSession ? current : null;
    }
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
    // The hierarchy the nodes were built from: what a change of only the open state, or only
    // the icons, is compared against so those can be applied without rebuilding.
    private readonly List<string> _labels = [];
    private readonly List<int> _depths = [];
    private readonly List<bool> _hasChildren = [];
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
        GotFocus += (_, args) =>
        {
            // Focus that lands on the tree itself goes to its selected item. Focus that landed on
            // an item already is where the user (or the arrow keys) put it: moving it back to the
            // previous selection stole a clicked item's focus and scrolled the tree to the old
            // selection, so a double-click's second press met another row.
            if (args.OriginalSource is TreeViewItem) return;
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

    // The native index of the item that contains `source`, or the selected item
    // when the request did not come from an item (keyboard focus on the tree).
    public int ItemIndexFor(DependencyObject? source, out TreeViewItem? container)
    {
        while (source is not null && source != _tree && source is not TreeViewItem)
            source = VisualTreeHelper.GetParent(source);
        container = source as TreeViewItem;
        if (container is not null && _tree.NodeFromContainer(container) is { } node &&
            _indices.TryGetValue(node, out var index))
            return index;
        container = SelectedContainer as TreeViewItem;
        return _selectedIndex;
    }

    // Labels are native item text, not menu text: an ampersand in a tree item is
    // a literal character, so no mnemonic collapsing happens here.
    public void Rebuild(
        IReadOnlyList<string> labels,
        IReadOnlyList<int> depths,
        IReadOnlyList<bool> expanded,
        IReadOnlyList<bool> hasChildren,
        IReadOnlyList<ImageSource?> icons,
        IReadOnlyList<ImageSource?> stateIcons,
        int selectedIndex)
    {
        if (labels.Count != depths.Count || labels.Count != expanded.Count ||
            labels.Count != hasChildren.Count || labels.Count != icons.Count ||
            labels.Count != stateIcons.Count)
            throw new ArgumentException(
                "Tree labels, depths, expansion, child flags, and icons must have matching counts.",
                nameof(depths));
        // Replacing every row resets the scroll offset to the top, so the item under a pointer
        // would change because some property of the tree did (the second press of a double-click
        // landed on another device). The offset is put back once layout has restored the extent.
        var scroll = ControlFactory.FindScrollViewer(_tree);
        var keptOffset = scroll?.VerticalOffset ?? 0;
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
                content.Apply(labels[index], icons[index], stateIcons[index]);
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
            _labels.Clear();
            _labels.AddRange(labels);
            _depths.Clear();
            _depths.AddRange(depths);
            _hasChildren.Clear();
            _hasChildren.AddRange(hasChildren);
            ApplySelectionCore(selectedIndex);
        }
        finally
        {
            _applyingCanonical = false;
        }
        if (scroll is not null && keptOffset >= 1) RestoreOffset(scroll, keptOffset);
    }

    // True when the nodes already stand for exactly this hierarchy (same labels, depths and
    // child flags), so only the state that hangs on it has changed.
    public bool HasHierarchy(
        IReadOnlyList<string> labels, IReadOnlyList<int> depths, IReadOnlyList<bool> hasChildren) =>
        _nodes.Count > 0 && labels.Count == _labels.Count &&
        _labels.SequenceEqual(labels) && _depths.SequenceEqual(depths) &&
        _hasChildren.SequenceEqual(hasChildren);

    // Opening or closing a node changes nothing else about the tree, and a rebuild would throw
    // away the scroll position and every realized item. False when the hierarchy itself differs.
    public bool TryApplyExpansion(IReadOnlyList<string> labels, IReadOnlyList<int> depths,
        IReadOnlyList<bool> expanded, IReadOnlyList<bool> hasChildren)
    {
        if (expanded.Count != _nodes.Count || !HasHierarchy(labels, depths, hasChildren)) return false;
        _applyingCanonical = true;
        try
        {
            for (var index = 0; index < _nodes.Count; ++index)
                if (_nodes[index].IsExpanded != expanded[index]) _nodes[index].IsExpanded = expanded[index];
        }
        finally
        {
            _applyingCanonical = false;
        }
        return true;
    }

    // The application's own label session on an item (-1: none): its projected editor opens,
    // and closes without a rename once the application has ended the session itself.
    public void ApplyNativeEditSession(int editingIndex)
    {
        for (var index = 0; index < _contents.Count; ++index)
            if (index != editingIndex && _contents[index].NativeSession) _contents[index].AbandonEdit();
        if (!EditableLabels || editingIndex < 0 || editingIndex >= _contents.Count ||
            _contents[editingIndex].IsEditing) return;
        _contents[editingIndex].BeginEdit(nativeSession: true);
    }

    public void ApplyStateIcons(IReadOnlyList<ImageSource?> icons)
    {
        for (var index = 0; index < _contents.Count && index < icons.Count; ++index)
            _contents[index].ApplyStateIcon(icons[index]);
    }

    private void RestoreOffset(ScrollViewer scroll, double target)
    {
        var attempts = 0;
        void Restore(object? sender, object args)
        {
            // Layout reports the extent in steps while the items realize: keep asking until the
            // offset holds, and give up after a bounded number of layouts.
            if (Math.Abs(scroll.VerticalOffset - target) < 1 || ++attempts > 20)
            {
                _tree.LayoutUpdated -= Restore;
                RendererDiagnostics.Log(
                    $"tree scroll restore wanted {target:F0}, offset {scroll.VerticalOffset:F0} (extent {scroll.ScrollableHeight:F0}) after {attempts} layout(s)");
                return;
            }
            scroll.ChangeView(null, target, null, true);
        }
        _tree.LayoutUpdated += Restore;
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
    internal const int ScrollBarZIndex = ControlFactory.PaneSplitterZIndex + 1;
    private static readonly TimeSpan WheelFlushInterval = TimeSpan.FromMilliseconds(60);

    private readonly Action<int, int> _splitRequested;
    private readonly Action<bool, int?, int?> _scrollRequested;
    private readonly List<SemanticPaneSplitter> _splitters = [];
    private readonly List<Image> _chrome = [];
    // The projected child nodes live in their own canvas so a scrolling container
    // can clip them to its native viewport while its bars sit outside that clip.
    private readonly Canvas _content = new();
    private readonly List<(ScrollBarSnapshot Bar, ScrollBar Control)> _bars = [];
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _wheelTimer;
    private int _pendingWheelLines;
    private bool _pendingWheelVertical = true;

    public SemanticPaneContainer(Action<int, int> splitRequested, Action<bool, int?, int?> scrollRequested)
    {
        _splitRequested = splitRequested;
        _scrollRequested = scrollRequested;
        IsTabStop = false;
        // A Canvas with no Background renders no visual at all, which collapses its
        // UIA bounding rectangle onto the union of its children and makes the
        // Bridge's geometry gate compare the wrong rectangle.
        Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        Children.Add(_content);
        // The splitters span the container, and its own size arrives after the
        // factory has built them.
        SizeChanged += (_, _) => ApplyExtents();
        PointerWheelChanged += OnPointerWheelChanged;
        ActualThemeChanged += (_, _) =>
        {
            if (_chromeRegions is { } regions) ApplyChrome(regions, _chromeScale);
        };
    }

    UIElementCollection ISemanticContainer.Children => _content.Children;

    // The native scroll bars, drawn as WinUI bars where the window draws them. Only a
    // completed gesture asks the application to scroll: a thumb drag sends its final
    // position, an arrow or track click its line or page step.
    public void ApplyScrollBars(IReadOnlyList<ScrollBarSnapshot> bars, PixelRect viewport, double scale)
    {
        _content.Clip = bars.Count == 0 ? null : new RectangleGeometry
        {
            Rect = new Rect(viewport.X * scale, viewport.Y * scale, viewport.Width * scale, viewport.Height * scale),
        };
        while (_bars.Count > bars.Count)
        {
            Children.Remove(_bars[^1].Control);
            _bars.RemoveAt(_bars.Count - 1);
        }
        for (var index = 0; index < bars.Count; ++index)
        {
            var bar = bars[index];
            if (index == _bars.Count || _bars[index].Bar.Vertical != bar.Vertical)
            {
                if (index < _bars.Count) Children.Remove(_bars[index].Control);
                var control = new ScrollBar
                {
                    Orientation = bar.Vertical ? Orientation.Vertical : Orientation.Horizontal,
                    IndicatorMode = ScrollingIndicatorMode.MouseIndicator,
                    IsTabStop = false,
                };
                var slot = index;
                control.Scroll += (_, args) => OnBarScroll(slot, args);
                Canvas.SetZIndex(control, ScrollBarZIndex);
                Children.Add(control);
                if (index == _bars.Count) _bars.Add((bar, control));
                else _bars[index] = (bar, control);
            }
            var target = _bars[index].Control;
            _bars[index] = (bar, target);
            // SCROLLINFO's last reachable position is nMax - nPage + 1.
            var page = Math.Max(bar.Page, 1);
            target.Minimum = bar.Minimum;
            target.Maximum = Math.Max(bar.Minimum, bar.Maximum - page + 1);
            target.ViewportSize = bar.Page;
            target.LargeChange = page;
            target.SmallChange = 1;
            target.Value = Math.Clamp(bar.Position, target.Minimum, target.Maximum);
            target.IsEnabled = bar.Enabled;
            target.Width = Math.Max(0, bar.Rect.Width * scale);
            target.Height = Math.Max(0, bar.Rect.Height * scale);
            Canvas.SetLeft(target, bar.Rect.X * scale);
            Canvas.SetTop(target, bar.Rect.Y * scale);
        }
    }

    private void OnBarScroll(int slot, ScrollEventArgs args)
    {
        if (slot >= _bars.Count) return;
        var vertical = _bars[slot].Bar.Vertical;
        switch (args.ScrollEventType)
        {
            case ScrollEventType.SmallIncrement: _scrollRequested(vertical, null, 1); break;
            case ScrollEventType.SmallDecrement: _scrollRequested(vertical, null, -1); break;
            // Track clicks and thumb releases name where the bar now is; the Bridge
            // walks the application there with its own page and line steps.
            case ScrollEventType.LargeIncrement:
            case ScrollEventType.LargeDecrement:
            case ScrollEventType.ThumbPosition:
            case ScrollEventType.First:
            case ScrollEventType.Last:
                _scrollRequested(vertical, (int)Math.Round(args.NewValue), null);
                break;
        }
    }

    private void OnPointerWheelChanged(object sender, PointerRoutedEventArgs args)
    {
        if (_bars.Count == 0 || args.Handled) return;
        var point = args.GetCurrentPoint(this).Properties;
        var horizontal = point.IsHorizontalMouseWheel ||
            Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift)
                .HasFlag(global::Windows.UI.Core.CoreVirtualKeyStates.Down);
        var vertical = !horizontal;
        if (!_bars.Any(entry => entry.Bar.Vertical == vertical && entry.Bar.Enabled)) return;
        // Three lines per notch, like the system default; a horizontal wheel's
        // positive delta scrolls right, a vertical one's scrolls up.
        var lines = point.MouseWheelDelta / 40 * (horizontal && point.IsHorizontalMouseWheel ? 1 : -1);
        if (lines == 0) return;
        args.Handled = true;
        if (_pendingWheelLines != 0 && _pendingWheelVertical != vertical) FlushWheel();
        _pendingWheelVertical = vertical;
        _pendingWheelLines = Math.Clamp(_pendingWheelLines + lines,
            -ProtocolConstants.MaxScrollLines, ProtocolConstants.MaxScrollLines);
        if (_wheelTimer is null && DispatcherQueue is { } queue)
        {
            _wheelTimer = queue.CreateTimer();
            _wheelTimer.IsRepeating = false;
            _wheelTimer.Interval = WheelFlushInterval;
            _wheelTimer.Tick += (_, _) => FlushWheel();
        }
        if (_wheelTimer is { IsRunning: false }) _wheelTimer.Start();
    }

    private void FlushWheel()
    {
        _wheelTimer?.Stop();
        var lines = _pendingWheelLines;
        _pendingWheelLines = 0;
        if (lines != 0) _scrollRequested(_pendingWheelVertical, null, lines);
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
                Canvas.SetZIndex(created, ControlFactory.PaneSplitterZIndex);
                _splitters.Add(created);
                Children.Add(created);
            }
            _splitters[index].Apply(splits[index], scale);
        }
        ApplyExtents();
    }

    // The bands the container paints itself.  They sit behind the panes and behind the
    // splitters, which is where the native window drew them.
    private IReadOnlyList<ChromeRegion>? _chromeRegions;
    private double _chromeScale = 1;

    public void ApplyChrome(IReadOnlyList<ChromeRegion> regions, double scale)
    {
        _chromeRegions = regions;
        _chromeScale = scale;
        var dark = ActualTheme == ElementTheme.Dark;
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
                Canvas.SetZIndex(created, ControlFactory.PaneChromeZIndex);
                _chrome.Add(created);
                Children.Add(created);
            }
            var region = regions[index];
            var image = _chrome[index];
            image.Source = ControlFactory.BitmapForChromeRegion(region, dark);
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
    protected override bool IsEnabledCore() => ControlFactory.NodeEnabled(owner);
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
    // A DirectUI host whose elements overflow it (MMC's Actions pane in a small
    // window) scrolls them with its own hosted ScrollBar. The projection scrolls the
    // elements locally instead, as a report ListView does: every element is published
    // with its real rectangle, so nothing has to be scrolled natively to reach it.
    private readonly Canvas _items = new();
    private readonly ScrollViewer _viewport;
    private bool _presented;

    public SemanticAccessibleIsland(Action<int> invoked)
    {
        _invoked = invoked;
        IsTabStop = false;
        // A Canvas with no Background renders no visual at all, which collapses its UIA
        // bounding rectangle onto the union of its children.
        Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        // The elements keep the native positions from the top-left corner.  A content
        // shorter than the viewport would otherwise be centred in it (an element with an
        // explicit height in a stretching slot), which dropped a folded Actions pane's two
        // section headers to the middle of the pane.
        _items.HorizontalAlignment = HorizontalAlignment.Left;
        _items.VerticalAlignment = VerticalAlignment.Top;
        _viewport = new ScrollViewer
        {
            Content = _items,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            HorizontalScrollMode = ScrollMode.Disabled,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            ZoomMode = ZoomMode.Disabled,
            IsTabStop = false,
        };
        Children.Add(_viewport);
        SizeChanged += (_, args) =>
        {
            _viewport.Width = args.NewSize.Width;
            _viewport.Height = args.NewSize.Height;
        };
    }

    public void ApplyItems(IReadOnlyList<AccessibleIslandItem> items, double scale)
    {
        // Elements scrolled above the host have negative rectangles; the content
        // starts at the first one, and the first presentation opens at the native
        // scroll position. Later rebuilds keep whatever the user scrolled to.
        var top = items.Count == 0 ? 0 : Math.Min(0, items.Min(item => item.Rect.Y));
        _items.Width = items.Count == 0 ? 0 : items.Max(item => item.Rect.X + item.Rect.Width) * scale;
        _items.Height = items.Count == 0 ? 0 : (items.Max(item => item.Rect.Y + item.Rect.Height) - top) * scale;
        if (!_presented && top < 0)
        {
            var offset = -top * scale;
            _viewport.DispatcherQueue.TryEnqueue(() => _viewport.ChangeView(null, offset, null, true));
        }
        _presented = true;
        // The elements are rebuilt whenever the provider's description changes, which
        // is the same contract a projected Toolbar's buttons follow.
        var children = _items.Children;
        children.Clear();
        for (var index = 0; index < items.Count; ++index)
        {
            var item = items[index];
            FrameworkElement element = item.Kind switch
            {
                "text" or "heading" => new TextBlock
                {
                    Text = item.Name,
                    FontSize = ItemFontSize,
                    FontWeight = item.Kind == "heading"
                        ? Microsoft.UI.Text.FontWeights.SemiBold
                        : Microsoft.UI.Text.FontWeights.Normal,
                    VerticalAlignment = item.Kind == "heading"
                        ? VerticalAlignment.Top : VerticalAlignment.Center,
                    TextWrapping = TextWrapping.Wrap,
                },
                // The host painted this graphic itself; the projection draws exactly
                // those pixels and names it with the provider's own description.
                "image" => new Image
                {
                    Source = ControlFactory.BitmapFromBase64(
                        item.ImageWidth, item.ImageHeight, item.ImageFormat, item.ImageData),
                    Stretch = Stretch.Fill,
                    IsHitTestVisible = false,
                },
                _ => BuildActionable(item, index),
            };
            AutomationProperties.SetName(element, item.Name);
            // The provider's own action string is what the projection will perform, so it
            // is what an assistive client is told about.
            AutomationProperties.SetHelpText(element,
                item.Description.Length != 0 ? item.Description : item.ActionName);
            Canvas.SetLeft(element, item.Rect.X * scale);
            Canvas.SetTop(element, (item.Rect.Y - top) * scale);
            element.Width = Math.Max(0, item.Rect.Width * scale);
            // XAML lays CJK text out a little taller than GDI did, so inert text keeps its
            // native width and wraps at the same width but may grow downward rather than
            // lose its last line to an ellipsis.
            if (element is TextBlock)
                element.MinHeight = Math.Max(0, item.Rect.Height * scale);
            else
                element.Height = Math.Max(0, item.Rect.Height * scale);
            children.Add(element);
        }
    }

    // Where a dropdown element's popup opens, in this island's coordinates: below
    // the element as currently scrolled, not where its native rectangle would be.
    public global::Windows.Foundation.Point? ItemAnchor(int index, bool rightToLeft)
    {
        if (index < 0 || index >= _items.Children.Count || _items.Children[index] is not FrameworkElement element)
            return null;
        return element.TransformToVisual(this).TransformPoint(new global::Windows.Foundation.Point(
            rightToLeft ? element.ActualWidth : 0, element.ActualHeight));
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
        if (item.Expanded is not null) label.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
        if (item.DropDown || item.Expanded is not null)
        {
            // The element completes by opening a menu of its own, so the projection draws
            // the same affordance rather than promising an in-place result.  A section
            // header draws the fold it toggles instead: up while its rows show, down while
            // they are folded, as the native header does.
            var row = new Grid();
            row.ColumnDefinitions.Add(
                new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            Grid.SetColumn(label, 0);
            var chevron = new FontIcon
            {
                Glyph = item.Expanded switch
                {
                    true => "\uE70E",
                    false => "\uE70D",
                    null => "\uE76C",
                },
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
        if (item.Expanded is { } expanded)
            AutomationProperties.SetItemStatus(button, expanded ? "Expanded" : "Collapsed");
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
    protected override bool IsEnabledCore() => ControlFactory.NodeEnabled(owner);
    protected override global::Windows.Foundation.Rect GetBoundingRectangleCore() =>
        ControlFactory.PhysicalLayoutBounds(owner, base.GetBoundingRectangleCore());
}

// One splitter.  The native container leaves the gap empty, so the projection draws a
// Fluent divider inside exactly that gap and widens only the invisible grab target when
// the native strip is too thin to hit.  The position it reports is always in the
// container's own client pixels.
// Native splitters, MMC's among them, draw a tracking bar while the pointer moves and
// resize the panes once, on release. Resizing per pointer move would cost a source-
// thread command, a native re-layout and a full recapture for every frame of the drag,
// so the projection tracks locally and makes exactly one native request.
internal sealed class PaneSplitDrag
{
    public bool Active { get; private set; }
    public int Origin { get; private set; }
    public int Preview { get; private set; }

    public void Begin(int origin)
    {
        Active = true;
        Origin = origin;
        Preview = origin;
    }

    public bool Move(int position)
    {
        if (!Active || position == Preview) return false;
        Preview = position;
        return true;
    }

    // The single native request a completed drag makes, or null when it did not move.
    public int? Complete()
    {
        if (!Active) return null;
        Active = false;
        return Preview == Origin ? null : Preview;
    }

    public void Cancel() => Active = false;
}

internal sealed class SemanticPaneSplitter : ContentControl
{
    // How long a released tracking bar waits for the canonical split before it
    // returns to the native position (a refusal publishes nothing new).
    private static readonly TimeSpan SettleTimeout = TimeSpan.FromMilliseconds(1500);
    private const int KeyboardStep = 8;

    private readonly Action<int> _positionRequested;
    private readonly PaneSplitDrag _drag = new();
    private readonly TranslateTransform _offset = new();
    private int? _settling;
    private Microsoft.UI.Dispatching.DispatcherQueueTimer? _settleTimer;
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
    private double _grabOffset;

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
        RenderTransform = _offset;
        ApplyDividerBrush(false);
        PointerEntered += (_, _) => ApplyDividerBrush(true);
        PointerExited += (_, _) => { if (!_drag.Active) ApplyDividerBrush(false); };
        PointerPressed += OnPointerPressed;
        PointerMoved += OnPointerMoved;
        PointerReleased += OnPointerReleased;
        // Losing capture mid-drag (Alt+Tab, a modal) abandons the drag, as a native
        // tracking loop does; the panes never moved, so nothing needs undoing.
        PointerCaptureLost += (_, _) =>
        {
            if (!_drag.Active) return;
            _drag.Cancel();
            ShowTrackingBar(null);
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
        // A reconcile can land mid-drag; the bar keeps following the pointer. After
        // release it stays where it was dropped until the native split leaves the
        // drag's origin (the request landed) or the settle timeout passes.
        if (_drag.Active) ShowTrackingBar(_drag.Preview);
        else if (_settling is { } dropped && split.Position == _drag.Origin) ShowTrackingBar(dropped);
        else ShowTrackingBar(null);
    }

    private void ShowTrackingBar(int? position)
    {
        if (position is not { } at || at == _split.Position)
        {
            _settling = null;
            _settleTimer?.Stop();
            _offset.X = _offset.Y = 0;
            Opacity = 1;
            return;
        }
        var delta = (at - _split.Position) * _scale;
        _offset.X = _split.Vertical ? delta : 0;
        _offset.Y = _split.Vertical ? 0 : delta;
        // The tracking bar is a ghost of the divider; the panes stay where the
        // native window still has them until the single request lands.
        Opacity = 0.75;
    }

    private void Settle(int position)
    {
        _settling = position;
        ShowTrackingBar(position);
        if (_settleTimer is null && DispatcherQueue is { } queue)
        {
            _settleTimer = queue.CreateTimer();
            _settleTimer.IsRepeating = false;
            _settleTimer.Interval = SettleTimeout;
            _settleTimer.Tick += (_, _) => ShowTrackingBar(null);
        }
        _settleTimer?.Stop();
        _settleTimer?.Start();
    }

    // A splitter divides the whole container, so it spans it across its own axis.
    public void ApplyExtent(double width, double height)
    {
        if (_split.Vertical) Height = Math.Max(0, height);
        else Width = Math.Max(0, width);
    }

    // Native client pixels for the pointer, measured in the container so the
    // tracking bar's own render offset does not feed back into the position.
    private int PositionFor(PointerRoutedEventArgs args)
    {
        var point = args.GetCurrentPoint(Parent as UIElement ?? this).Position;
        var device = (_split.Vertical ? point.X : point.Y) - _grabOffset;
        var native = (int)Math.Round(device / _scale);
        return Math.Clamp(native, _split.Minimum, _split.Maximum);
    }

    private void OnPointerPressed(object sender, PointerRoutedEventArgs args)
    {
        var point = args.GetCurrentPoint(this).Position;
        // Grabbing anywhere on the strip keeps the pointer on the same spot of the
        // splitter for the whole drag instead of snapping its edge to the cursor.
        // The strip's own left/top edge sits `overhang` before the native position.
        var overhang = (_split.Vertical ? Width : Height) / 2 - Math.Max(1, _split.Thickness * _scale) / 2;
        _grabOffset = (_split.Vertical ? point.X : point.Y) - overhang;
        if (!CapturePointer(args.Pointer)) return;
        _drag.Begin(_split.Position);
        ShowTrackingBar(null);
        Focus(FocusState.Pointer);
        args.Handled = true;
    }

    private void OnPointerMoved(object sender, PointerRoutedEventArgs args)
    {
        if (!_drag.Active) return;
        if (_drag.Move(PositionFor(args))) ShowTrackingBar(_drag.Preview);
        args.Handled = true;
    }

    private void OnPointerReleased(object sender, PointerRoutedEventArgs args)
    {
        if (!_drag.Active) return;
        _drag.Move(PositionFor(args));
        var target = _drag.Complete();
        ReleasePointerCapture(args.Pointer);
        args.Handled = true;
        if (target is not { } position)
        {
            ShowTrackingBar(null);
            return;
        }
        Settle(position);
        _positionRequested(position);
    }

    private void OnKeyDown(object sender, KeyRoutedEventArgs args)
    {
        if (args.Key == VirtualKey.Escape && _drag.Active)
        {
            // Escape abandons a drag exactly like a native tracking loop.
            _drag.Cancel();
            ShowTrackingBar(null);
            args.Handled = true;
            return;
        }
        var step = args.Key switch
        {
            VirtualKey.Left or VirtualKey.Up => -KeyboardStep,
            VirtualKey.Right or VirtualKey.Down => KeyboardStep,
            _ => 0,
        };
        if (step == 0 || _drag.Active) return;
        args.Handled = true;
        RequestPosition(_split.Position + step);
    }

    public void RequestPosition(int position)
    {
        position = Math.Clamp(position, _split.Minimum, _split.Maximum);
        if (position != _split.Position) _positionRequested(position);
    }

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
    protected override bool IsEnabledCore() => ControlFactory.NodeEnabled(owner);
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
    protected override bool IsEnabledCore() => ControlFactory.NodeEnabled(owner);
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
    protected override global::Windows.Foundation.Rect GetBoundingRectangleCore()
    {
        var reported = base.GetBoundingRectangleCore();
        var scale = owner.XamlRoot?.RasterizationScale ?? 0;
        if (scale <= 0 || owner.ActualWidth <= 0 || owner.ActualHeight <= 0)
            return reported;
        return ControlFactory.CenteredLayoutSlotBounds(
            reported, owner.ActualWidth * scale, owner.ActualHeight * scale);
    }
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
