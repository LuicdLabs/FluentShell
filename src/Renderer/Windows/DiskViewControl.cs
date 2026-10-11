using FluentShell.Renderer.Protocol;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.System;

namespace FluentShell.Renderer.Windows;

// Each disk/partition is a real selectable XAML control. Geometry and text come
// from the native MSAA table; the bitmap legend remains a separate inert node.
internal sealed class DiskViewControl(Action<string, string> itemAction, Action<int> scroll) : Canvas
{
    private readonly Canvas _viewport = new();
    private readonly ScrollBar _scroll = new() { Orientation = Orientation.Vertical, SmallChange = 1 };
    private readonly Dictionary<string, DiskViewItemButton> _items = [];
    private DiskViewSnapshot? _view;
    private bool _applying;

    internal IReadOnlyCollection<DiskViewItemButton> Items => _items.Values;
    public void Apply(DiskViewSnapshot view, double scale)
    {
        _applying = true;
        try
        {
            _view = view;
            if (Children.Count == 0)
            {
                Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
                Children.Add(_viewport);
                Children.Add(_scroll);
                _scroll.Scroll += (_, _) => { if (!_applying) scroll((int)Math.Round(_scroll.Value)); };
                PointerWheelChanged += (_, args) =>
                {
                    if (_view is not { } current || current.ScrollMaximum == current.ScrollMinimum) return;
                    var delta = args.GetCurrentPoint(this).Properties.MouseWheelDelta;
                    if (delta == 0) return;
                    scroll(Math.Clamp(current.ScrollPosition - Math.Sign(delta) * 3,
                        current.ScrollMinimum, current.ScrollMaximum));
                    args.Handled = true;
                };
            }
            Place(_viewport, view.ClientRect, scale);
            _viewport.Clip = new RectangleGeometry { Rect = new(0, 0, view.ClientRect.Width * scale, view.ClientRect.Height * scale) };
            var keys = view.Items.Select(item => item.ItemId).ToHashSet(StringComparer.Ordinal);
            foreach (var key in _items.Keys.Where(key => !keys.Contains(key)).ToArray())
            {
                _viewport.Children.Remove(_items[key]);
                _items.Remove(key);
            }
            foreach (var item in view.Items)
            {
                if (!_items.TryGetValue(item.ItemId, out var button))
                {
                    button = new DiskViewItemButton(this,
                        action => { if (!_applying) itemAction(action, item.ItemId); });
                    _items.Add(item.ItemId, button);
                    _viewport.Children.Add(button);
                }
                button.Apply(item);
                Place(button, item.Rect, scale);
            }
            if (view.ScrollMaximum > _scroll.Maximum) _scroll.Maximum = view.ScrollMaximum;
            _scroll.Minimum = view.ScrollMinimum;
            _scroll.Maximum = view.ScrollMaximum;
            _scroll.Value = view.ScrollPosition;
            _scroll.LargeChange = Math.Max(1, view.ScrollPage);
            _scroll.ViewportSize = view.ScrollPage;
            _scroll.IsEnabled = view.ScrollMaximum > view.ScrollMinimum;
            _scroll.Visibility = view.ScrollRect.Width > 0 && view.ScrollRect.Height > 0 ? Visibility.Visible : Visibility.Collapsed;
            Place(_scroll, view.ScrollRect, scale);
        }
        finally { _applying = false; }
    }
    private static void Place(FrameworkElement element, PixelRect rect, double scale)
    {
        Canvas.SetLeft(element, rect.X * scale);
        Canvas.SetTop(element, rect.Y * scale);
        element.Width = rect.Width * scale;
        element.Height = rect.Height * scale;
    }
    protected override AutomationPeer OnCreateAutomationPeer() => new DiskViewPeer(this);
    private sealed class DiskViewPeer(DiskViewControl owner) : FrameworkElementAutomationPeer(owner), ISelectionProvider
    {
        protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Group;
        protected override string GetClassNameCore() => "NativeDiskView";
        protected override bool IsEnabledCore() => ControlFactory.NodeEnabled(owner);
        protected override object GetPatternCore(PatternInterface pattern) =>
            pattern == PatternInterface.Selection ? this : base.GetPatternCore(pattern);
        protected override global::Windows.Foundation.Rect GetBoundingRectangleCore() =>
            ControlFactory.PhysicalLayoutBounds(owner, base.GetBoundingRectangleCore());
        public bool CanSelectMultiple => false;
        public bool IsSelectionRequired => false;
        public IRawElementProviderSimple[] GetSelection() => owner.Items.Where(item => item.Item.Selected)
            .Select(item => ProviderFromPeer(CreatePeerForElement(item))).ToArray();
    }
}

internal sealed class DiskViewItemButton : Button
{
    internal DiskViewControl View { get; }
    internal DiskViewItemSnapshot Item { get; private set; } = new();
    private readonly Action<string> _request;
    private readonly TextBlock _name = new() { FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly TextBlock _value = new() { TextWrapping = TextWrapping.Wrap, TextTrimming = TextTrimming.CharacterEllipsis, MaxLines = 4, FontSize = 11 };

    internal DiskViewItemButton(DiskViewControl view, Action<string> request)
    {
        DefaultStyleKey = typeof(Button);
        View = view;
        _request = request;
        MinWidth = MinHeight = 0;
        Padding = new Thickness(6, 3, 6, 3);
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        VerticalContentAlignment = VerticalAlignment.Top;
        Content = new StackPanel { Children = { _name, _value } };
        Click += (_, _) => Select();
        RightTapped += (_, args) => { if (IsEnabled) _request("diskContext"); args.Handled = true; };
        KeyDown += (_, args) =>
        {
            if (args.Key == VirtualKey.Application || args.Key == VirtualKey.F10 &&
                Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Shift).HasFlag(global::Windows.UI.Core.CoreVirtualKeyStates.Down))
            { if (IsEnabled) _request("diskContext"); args.Handled = true; }
        };
    }
    internal void Apply(DiskViewItemSnapshot item)
    {
        Item = item;
        IsEnabled = item.Enabled;
        _name.Text = item.Name;
        _value.Text = item.Value;
        Background = item.Selected
            ? new SolidColorBrush(global::Windows.UI.Color.FromArgb(255, 0, 80, 150))
            : new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        if (item.Selected) Foreground = new SolidColorBrush(Microsoft.UI.Colors.White);
        else ClearValue(ForegroundProperty);
        BorderThickness = new Thickness(item.Selected ? 2 : 1);
        AutomationProperties.SetName(this, item.Name);
        AutomationProperties.SetHelpText(this, item.Value);
        AutomationProperties.SetAutomationId(this, "fls-disk-item-" + item.ItemId);
        ToolTipService.SetToolTip(this, item.Name + "\n" + item.Value);
    }
    internal void Select() { if (IsEnabled && !Item.Selected) _request("diskSelect"); }
    protected override AutomationPeer OnCreateAutomationPeer() => new DiskItemPeer(this);
    private sealed class DiskItemPeer(DiskViewItemButton owner) : ButtonAutomationPeer(owner), ISelectionItemProvider
    {
        protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.ListItem;
        protected override object GetPatternCore(PatternInterface pattern) =>
            pattern == PatternInterface.SelectionItem ? this : base.GetPatternCore(pattern);
        public bool IsSelected => owner.Item.Selected;
        public IRawElementProviderSimple SelectionContainer => ProviderFromPeer(CreatePeerForElement(owner.View));
        public void Select()
        {
            if (!owner.IsEnabled) throw new ElementNotEnabledException();
            owner.Select();
        }
        public void AddToSelection()
        {
            if (owner.View.Items.Any(item => item != owner && item.Item.Selected))
                throw new InvalidOperationException("The native disk table allows one selected item.");
            Select();
        }
        public void RemoveFromSelection() => throw new InvalidOperationException("Native disk selection is replaced by selecting another item.");
    }
}
