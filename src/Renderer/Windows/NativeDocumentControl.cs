using System.Globalization;
using FluentShell.Renderer.Protocol;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace FluentShell.Renderer.Windows;

// The native DOM and embedded report remain authoritative. XAML owns the
// transient scroll gesture until it sends an absolute native scroll request.
internal sealed class NativeDocumentControl(Action<int> invoke,
    Action<NativeDocumentScrollActionValue> scroll) : Canvas
{
    private bool _applying;
    private NativeDocumentSnapshot? _document;
    private double _scale;
    private readonly Dictionary<int, FrameworkElement> _elements = [];
    private readonly Dictionary<int, ScrollViewer> _scrollers = [];
    private readonly Dictionary<int, List<(Grid Row, TextBlock Value)>> _samples = [];

    public void Apply(NativeDocumentSnapshot document, double scale)
    {
        var previous = _document;
        var reuse = _scale == scale && NativeDocumentPresentation.SameStructure(previous, document);
        _applying = true;
        _document = document;
        _scale = scale;
        if (reuse)
        {
            for (var index = 0; index < document.Items.Count; ++index) UpdateItem(index);
            foreach (var (index, view) in _scrollers)
            {
                ConfigureScroller(view, document.Regions[index]);
                if (previous!.Regions[index].OffsetX != document.Regions[index].OffsetX ||
                    previous.Regions[index].OffsetY != document.Regions[index].OffsetY ||
                    previous.Regions[index].MaximumX != document.Regions[index].MaximumX ||
                    previous.Regions[index].MaximumY != document.Regions[index].MaximumY) SynchronizeScroll(index);
            }
            _applying = false;
            return;
        }
        var focused = _elements.FirstOrDefault(entry => entry.Value is Control { FocusState: not FocusState.Unfocused });
        var focusedKey = previous is not null && focused.Value is not null ? previous.Items[focused.Key].Key : null;
        Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
        Children.Clear();
        _elements.Clear();
        _scrollers.Clear();
        _samples.Clear();
        var content = new Dictionary<int, Canvas>();
        for (var index = 0; index < document.Regions.Count; ++index)
        {
            var region = document.Regions[index];
            if (region.Kind != "html") continue;
            var canvas = new Canvas { Width = region.ContentWidth * scale, Height = region.ContentHeight * scale };
            content.Add(index, canvas);
            var view = CreateScroller(region, index, canvas);
            Place(view, region.Rect, scale);
            Children.Add(view);
        }
        for (var index = 0; index < document.Items.Count; ++index)
        {
            var item = document.Items[index];
            FrameworkElement element;
            if (item.Kind == "report")
            {
                var region = document.Regions[item.ReportRegionIndex];
                element = CreateScroller(region, item.ReportRegionIndex, BuildReport(item, region, scale, index));
            }
            else if (item.Kind == "link")
            {
                var capturedIndex = index;
                var link = new HyperlinkButton
                {
                    Content = item.Image is { } image ? Picture(image) : new TextBlock { Text = item.Name, TextWrapping = TextWrapping.Wrap },
                    Padding = new Thickness(0), MinWidth = 0, MinHeight = 0,
                    IsEnabled = item.Enabled,
                    HorizontalContentAlignment = HorizontalAlignment.Left,
                    VerticalContentAlignment = VerticalAlignment.Top,
                };
                link.Click += (_, _) =>
                {
                    if (!_applying && _elements.TryGetValue(capturedIndex, out var current) &&
                        ReferenceEquals(current, link)) invoke(capturedIndex);
                };
                element = link;
            }
            else if (item.Kind == "image")
                element = Picture(item.Image!);
            else
                element = new TextBlock
                {
                    Text = item.Name, FontSize = 12, TextWrapping = TextWrapping.Wrap,
                    FontWeight = item.Kind == "heading" ? Microsoft.UI.Text.FontWeights.SemiBold : Microsoft.UI.Text.FontWeights.Normal,
                };
            AutomationProperties.SetName(element, item.Name);
            Place(element, item.Rect, scale);
            (item.RegionIndex >= 0 ? content[item.RegionIndex].Children : Children).Add(element);
            _elements.Add(index, element);
        }
        _applying = false;
        if (focusedKey is not null)
            foreach (var (index, element) in _elements)
                if (document.Items[index].Key == focusedKey && element is Control control) control.Focus(FocusState.Programmatic);
    }

    private void UpdateItem(int index)
    {
        var item = _document!.Items[index];
        var element = _elements[index];
        AutomationProperties.SetName(element, item.Name);
        if (element is HyperlinkButton link)
        {
            link.IsEnabled = item.Enabled;
            if (item.Image is { } image) link.Content = Picture(image);
            else if (link.Content is TextBlock label) label.Text = item.Name;
        }
        else if (element is TextBlock text) text.Text = item.Name;
        else if (element is Image picture && item.Image is { } image)
            picture.Source = ControlFactory.BitmapFromBase64(image.ImageWidth, image.ImageHeight, image.ImageFormat, image.ImageData);
        if (_samples.TryGetValue(index, out var samples))
            for (var row = 0; row < samples.Count; ++row)
            {
                var source = item.ReportRows[row];
                samples[row].Value.Text = SampleText(source);
                AutomationProperties.SetName(samples[row].Row, $"{source.Path}: {samples[row].Value.Text}");
            }
    }

    private static Image Picture(ImageListEntry image) => new()
    {
        Source = ControlFactory.BitmapFromBase64(image.ImageWidth, image.ImageHeight, image.ImageFormat, image.ImageData),
        Stretch = Stretch.Fill,
    };

    private static void Place(FrameworkElement element, PixelRect rect, double scale)
    {
        SetLeft(element, rect.X * scale);
        SetTop(element, rect.Y * scale);
        element.Width = rect.Width * scale;
        element.Height = rect.Height * scale;
    }

    internal static int NativeOffset(double offset, double extent, int maximum) =>
        NativeDocumentPresentation.NativeOffset(offset, extent, maximum);

    private static void ConfigureScroller(ScrollViewer view, NativeDocumentRegion region)
    {
        // The semantic report can use more space in WinUI than in the native
        // report's column layout. Local overflow keeps every captured row
        // reachable even when that native axis has no scrollbar.
        var local = NativeDocumentPresentation.AllowsLocalOverflow(region);
        view.HorizontalScrollMode = region.MaximumX > 0 || local ? ScrollMode.Enabled : ScrollMode.Disabled;
        view.VerticalScrollMode = region.MaximumY > 0 || local ? ScrollMode.Enabled : ScrollMode.Disabled;
        view.HorizontalScrollBarVisibility = region.MaximumX > 0 || local ? ScrollBarVisibility.Auto : ScrollBarVisibility.Disabled;
        view.VerticalScrollBarVisibility = region.MaximumY > 0 || local ? ScrollBarVisibility.Auto : ScrollBarVisibility.Disabled;
    }

    private void SynchronizeScroll(int index)
    {
        var view = _scrollers[index];
        var region = _document!.Regions[index];
        view.ChangeView(
            region.MaximumX > 0 ? (double)region.OffsetX / region.MaximumX * view.ScrollableWidth : null,
            region.MaximumY > 0 ? (double)region.OffsetY / region.MaximumY * view.ScrollableHeight : null,
            null, disableAnimation: true);
    }

    private ScrollViewer CreateScroller(NativeDocumentRegion region, int index, UIElement content)
    {
        var view = new ScrollViewer
        {
            Content = content,
            ZoomMode = ZoomMode.Disabled,
        };
        ConfigureScroller(view, region);
        _scrollers.Add(index, view);
        view.Loaded += (_, _) => { if (_scrollers.TryGetValue(index, out var current) && ReferenceEquals(view, current)) SynchronizeScroll(index); };
        view.ViewChanged += (_, args) =>
        {
            if (_applying || args.IsIntermediate || _document is null ||
                !_scrollers.TryGetValue(index, out var current) || !ReferenceEquals(view, current)) return;
            var latest = _document.Regions[index];
            var x = NativeOffset(view.HorizontalOffset, view.ScrollableWidth, latest.MaximumX);
            var y = NativeOffset(view.VerticalOffset, view.ScrollableHeight, latest.MaximumY);
            if (x == latest.OffsetX && y == latest.OffsetY) return;
            scroll(new NativeDocumentScrollActionValue
            {
                Index = index, DocumentId = _document.DocumentId, Key = latest.Key, X = x, Y = y,
            });
        };
        return view;
    }

    private static string SampleText(NativeReportRow row) => row.Available ? row.Value.ToString("G6", CultureInfo.CurrentCulture) : "—";

    private FrameworkElement BuildReport(NativeDocumentItem item, NativeDocumentRegion region, double scale, int itemIndex)
    {
        // A native report that only scrolls horizontally arranges its values in
        // columns. Keep every row reachable along that same native scroll axis.
        var horizontal = region.MaximumX > 0 && region.MaximumY == 0;
        var rowsPerColumn = horizontal ? Math.Max(1, (int)((item.Rect.Height * scale - 24) / 24)) : int.MaxValue;
        var columns = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 16 };
        var samples = new List<(Grid Row, TextBlock Value)>();
        _samples.Add(itemIndex, samples);
        StackPanel? column = null;
        for (var index = 0; index < item.ReportRows.Count; ++index)
        {
            if (index % rowsPerColumn == 0)
            {
                column = new StackPanel { MinWidth = Math.Min(420, item.Rect.Width * scale), Spacing = 4 };
                columns.Children.Add(column);
            }
            var source = item.ReportRows[index];
            var row = new Grid { Height = 20, Width = Math.Max(160, Math.Min(420, item.Rect.Width * scale - 20)) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var path = new TextBlock { Text = source.Path, FontSize = 11, TextTrimming = TextTrimming.CharacterEllipsis };
            ToolTipService.SetToolTip(path, source.Path);
            var value = new TextBlock
            {
                Text = SampleText(source),
                FontSize = 11, Margin = new Thickness(12, 0, 0, 0),
            };
            Grid.SetColumn(value, 1);
            row.Children.Add(path);
            row.Children.Add(value);
            AutomationProperties.SetName(row, $"{source.Path}: {value.Text}");
            column!.Children.Add(row);
            samples.Add((row, value));
        }
        return columns;
    }

    protected override AutomationPeer OnCreateAutomationPeer() => new DocumentPeer(this);
    private sealed class DocumentPeer(NativeDocumentControl owner) : FrameworkElementAutomationPeer(owner)
    {
        protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Group;
        protected override string GetClassNameCore() => "ProjectedNativeDocument";
        protected override string GetNameCore() => AutomationProperties.GetName(owner);
        protected override bool IsControlElementCore() => true;
        protected override bool IsContentElementCore() => false;
        protected override global::Windows.Foundation.Rect GetBoundingRectangleCore() =>
            ControlFactory.PhysicalLayoutBounds(owner, base.GetBoundingRectangleCore());
    }
}
