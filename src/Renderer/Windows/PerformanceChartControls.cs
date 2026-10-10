using System.Globalization;
using FluentShell.Renderer.Protocol;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace FluentShell.Renderer.Windows;

/// <summary>
/// Performance Monitor's graph (minor 36): the scale labels, the plot with its grid and time bar,
/// one polyline per counter and the time axis, drawn as real shapes. The native graph is pixels
/// of sysmon.ocx; what it shows is the control's scale, counters and samples, which arrive as
/// data, laid out in the slots the control draws them in.
/// </summary>
internal sealed class PerformanceGraphControl : Canvas
{
    // The native graph keeps these margins (physical pixels at 96 dpi) around its plot: scale
    // labels and their ticks to the left, time labels under it, and room at the right for the last
    // time label's half width.
    private const double GutterLeft = 45;
    private const double GutterRight = 45;
    private const double GutterTop = 11;
    private const double GutterBottom = 30;
    private const double Gap = 8;
    private const double TickLength = 5;
    private const double HatchHeight = 5;
    // The scale labels are at least this far apart.
    private const double MinimumLabelSpacing = 28;

    private readonly double _scale;
    private readonly double _fontSize;
    private PerformanceGraphSnapshot? _graph;

    public PerformanceGraphControl(double scale, double fontSize)
    {
        _scale = scale;
        _fontSize = fontSize;
        SizeChanged += (_, _) => Redraw();
        // The factory gives the control the native rectangle's extent right after creating it, and a
        // control built while its window is only being prepared can be laid out without ever raising
        // SizeChanged: a graph applied at size zero and never touched again (a console projected while
        // a dialog is open natively gets no live sample patches) would stay blank.  The extent it was
        // given is what it draws to.
        RegisterPropertyChangedCallback(WidthProperty, (_, _) => Redraw());
        RegisterPropertyChangedCallback(HeightProperty, (_, _) => Redraw());
    }

    public void Apply(PerformanceGraphSnapshot? graph)
    {
        _graph = graph;
        Redraw();
    }

    internal static global::Windows.UI.Color ColorOf(long rgb) =>
        ColorHelper.FromArgb(255, (byte)((rgb >> 16) & 0xFF), (byte)((rgb >> 8) & 0xFF), (byte)(rgb & 0xFF));

    private static SolidColorBrush BrushOf(long rgb) => new(ColorOf(rgb));

    // A dash pattern is in multiples of the stroke width, so each length is divided by the width to
    // draw the same absolute dash at any thickness.
    internal static DoubleCollection? DashFor(string style, double width)
    {
        double[] pattern = style switch
        {
            "dash" => new double[] { 4, 2 },
            "dot" => new double[] { 1, 2 },
            "dashdot" => new double[] { 4, 2, 1, 2 },
            "dashdotdot" => new double[] { 4, 2, 1, 2, 1, 2 },
            _ => Array.Empty<double>(),
        };
        if (pattern.Length == 0) return null;
        var dashes = new DoubleCollection();
        foreach (var length in pattern) dashes.Add(length / Math.Max(1, width));
        return dashes;
    }

    private TextBlock MakeText(string text, Brush brush) =>
        new() { Text = text, FontSize = _fontSize, Foreground = brush, IsHitTestVisible = false, TextWrapping = TextWrapping.NoWrap };

    private double MeasureText(string text)
    {
        var block = MakeText(text, new SolidColorBrush(Colors.Black));
        block.Measure(new global::Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
        return block.DesiredSize.Width;
    }

    // Places text by its measured size: the anchor says which point of the text sits at (x, y).
    private void AddText(string text, Brush brush, double x, double y, double anchorX, double anchorY)
    {
        var block = MakeText(text, brush);
        block.Measure(new global::Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
        SetLeft(block, x - block.DesiredSize.Width * anchorX);
        SetTop(block, y - block.DesiredSize.Height * anchorY);
        Children.Add(block);
    }

    private void AddLine(double x1, double y1, double x2, double y2, Brush brush, double thickness = 1)
    {
        Children.Add(new Line { X1 = x1, Y1 = y1, X2 = x2, Y2 = y2, Stroke = brush, StrokeThickness = thickness, IsHitTestVisible = false });
    }

    // A "nice" label step (1, 2 or 5 times a power of ten) that keeps the labels apart.
    private static double LabelStep(double range, double height, double spacing)
    {
        var maximumLabels = Math.Max(1, (int)Math.Floor(height / spacing));
        var raw = range / maximumLabels;
        var magnitude = Math.Pow(10, Math.Floor(Math.Log10(raw)));
        foreach (var factor in new[] { 1d, 2d, 5d, 10d })
        {
            var step = factor * magnitude;
            if (step >= raw - raw * 1e-9) return step;
        }
        return range;
    }

    // The spacings (seconds) the time axis steps by between its end labels.
    private static readonly double[] NiceStepsSeconds =
        [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 21600, 43200, 86400];

    private static string TimeText(double unixMilliseconds) =>
        DateTimeOffset.FromUnixTimeMilliseconds((long)unixMilliseconds).ToLocalTime()
            .ToString("T", CultureInfo.CurrentCulture);

    private void Redraw()
    {
        Children.Clear();
        var graph = _graph;
        var width = double.IsNaN(Width) || Width <= 0 ? ActualWidth : Width;
        var height = double.IsNaN(Height) || Height <= 0 ? ActualHeight : Height;
        if (graph is null || width < 40 || height < 40) return;
        Background = BrushOf(graph.ControlColor);
        var text = BrushOf(graph.TextColor);

        var left = (graph.ShowScaleLabels ? GutterLeft : Gap) * _scale;
        var right = (graph.ShowTimeLabels ? GutterRight : Gap) * _scale;
        var top = (graph.ShowScaleLabels ? GutterTop : Gap) * _scale;
        var bottom = (graph.ShowTimeLabels ? GutterBottom : Gap) * _scale;
        if (graph.YAxisLabel.Length > 0) left += _fontSize + 6;
        if (graph.Title.Length > 0) top += _fontSize + 8;
        var plotWidth = width - left - right;
        var plotHeight = height - top - bottom;
        if (plotWidth < 16 || plotHeight < 16) return;
        var plotBottom = top + plotHeight;
        var tick = TickLength * _scale;

        if (graph.Title.Length > 0)
            AddText(graph.Title, text, left + plotWidth / 2, 4, 0.5, 0);
        if (graph.YAxisLabel.Length > 0)
        {
            var caption = MakeText(graph.YAxisLabel, text);
            caption.Measure(new global::Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
            caption.RenderTransform = new RotateTransform { Angle = -90 };
            SetLeft(caption, 2);
            SetTop(caption, top + plotHeight / 2 + caption.DesiredSize.Width / 2);
            Children.Add(caption);
        }

        var plot = new Rectangle { Width = plotWidth, Height = plotHeight, Fill = BrushOf(graph.BackColor), IsHitTestVisible = false };
        SetLeft(plot, left);
        SetTop(plot, top);
        Children.Add(plot);

        var range = graph.Maximum - graph.Minimum;
        var grid = BrushOf(graph.GridColor);
        double YOf(double value) => plotBottom - Math.Clamp((value - graph.Minimum) / range, 0, 1) * plotHeight;

        // Scale labels, their ticks, and the horizontal grid.
        var step = LabelStep(range, plotHeight, MinimumLabelSpacing * _scale);
        var format = step < 1 ? "0.##" : "0";
        for (var index = 0; graph.Minimum + index * step <= graph.Maximum + step * 1e-9; ++index)
        {
            var value = graph.Minimum + index * step;
            var y = YOf(value);
            if (graph.ShowHorizontalGrid && value > graph.Minimum && value < graph.Maximum)
                AddLine(left, y, left + plotWidth, y, grid);
            if (!graph.ShowScaleLabels) continue;
            AddLine(left - tick, y, left, y, text);
            AddText(value.ToString(format, CultureInfo.CurrentCulture), text, left - tick - 3 * _scale, y, 1, 0.5);
        }

        var slots = graph.SampleCount;
        // The sample of slot n is drawn at the right end of its share of the plot's width.
        double SlotX(int slot) => left + plotWidth * (slot + 1) / slots;

        // The time axis labels the time of the sample each slot holds: the first slot at the plot's left
        // edge, the last at its right edge, and one a nice number of seconds apart in between. After a
        // wrap the slots right of the time bar still hold the previous pass, one pass older.
        double SlotTime(int slot) => graph.StartTimeMs + (double)slot * graph.IntervalMs -
            (graph.Wrapped && graph.BarSlot >= 0 && slot > graph.BarSlot ? (double)slots * graph.IntervalMs : 0);
        // A histogram has no time axis: it shows the counters' latest values side by side.
        var timeAxis = graph.DisplayType != "histogram";
        var ticks = new List<(double X, int Slot)> { (left, 0), (left + plotWidth, slots - 1) };
        if (graph.ShowTimeLabels && timeAxis)
        {
            // Labels are kept at least twice their own width apart.
            var labelWidth = MeasureText(TimeText(graph.StartTimeMs));
            var slotWidth = plotWidth / slots;
            var timeStep = NiceStepsSeconds.FirstOrDefault(seconds =>
                seconds * 1000.0 / graph.IntervalMs * slotWidth >= 2.2 * labelWidth);
            var stepSlots = timeStep <= 0 ? 0 : (int)Math.Round(timeStep * 1000.0 / graph.IntervalMs);
            for (var slot = stepSlots + 1; stepSlots > 0 && slot < slots; slot += stepSlots)
            {
                var x = SlotX(slot);
                // Clear of the end labels.
                if (x - left > plotWidth * 0.14 && left + plotWidth - x > plotWidth * 0.14) ticks.Add((x, slot));
            }
        }
        foreach (var (x, slot) in ticks)
        {
            if (graph.ShowVerticalGrid && x > left && x < left + plotWidth) AddLine(x, top, x, plotBottom, grid);
            if (!graph.ShowTimeLabels || !timeAxis) continue;
            AddLine(x, plotBottom, x, plotBottom + tick, text);
            AddText(TimeText(SlotTime(slot)), text, x, plotBottom + tick + 4 * _scale, 0.5, 0);
        }

        // The strip along the top of the plot that the control hatches, with a rule under it.
        var hatchHeight = HatchHeight * _scale;
        var hatch = new Canvas { Width = plotWidth, Height = hatchHeight, Clip = new RectangleGeometry { Rect = new global::Windows.Foundation.Rect(0, 0, plotWidth, hatchHeight) }, IsHitTestVisible = false };
        var spacing = Math.Max(3, 5 * _scale);
        for (var x = -hatchHeight; x < plotWidth; x += spacing)
            hatch.Children.Add(new Line { X1 = x, Y1 = hatchHeight, X2 = x + hatchHeight, Y2 = 0, Stroke = text, StrokeThickness = 1, IsHitTestVisible = false });
        SetLeft(hatch, left);
        SetTop(hatch, top);
        Children.Add(hatch);
        AddLine(left, top + hatchHeight, left + plotWidth, top + hatchHeight, text);

        var drawn = graph.Series.Where(candidate => candidate.Visible).ToList();
        if (graph.DisplayType == "histogram")
        {
            // A histogram shows each counter's latest sample as one bar, centred in its share of the
            // plot's width (a lone counter's bar is the middle two fifths of it).
            var bars = drawn.Where(series => series.Values.Any(value => value is not null)).ToList();
            var share = plotWidth / Math.Max(1, bars.Count);
            for (var index = 0; index < bars.Count; ++index)
            {
                var series = bars[index];
                var latest = series.Values.LastOrDefault(value => value is not null);
                if (latest is not { } value) continue;
                var barTop = YOf(value * series.Scale);
                var barWidth = Math.Max(2, share * 0.4);
                var bar = new Rectangle
                {
                    Width = barWidth, Height = Math.Max(0, plotBottom - barTop),
                    Fill = BrushOf(series.Color), IsHitTestVisible = false,
                };
                SetLeft(bar, left + share * (index + 0.5) - barWidth / 2);
                SetTop(bar, barTop);
                Children.Add(bar);
            }
        }
        else
        {
            foreach (var series in drawn)
            {
                var stroke = BrushOf(series.Color);
                var thickness = series.Width + (series.Highlighted ? 2 : 0);
                var run = new List<global::Windows.Foundation.Point>();
                void Flush()
                {
                    if (run.Count >= 2)
                    {
                        var line = new Polyline
                        {
                            Stroke = stroke, StrokeThickness = thickness, StrokeLineJoin = PenLineJoin.Round,
                            StrokeDashArray = DashFor(series.Style, thickness), IsHitTestVisible = false,
                        };
                        foreach (var point in run) line.Points.Add(point);
                        Children.Add(line);
                    }
                    else if (run.Count == 1)
                    {
                        // A lone sample is a dot until the next one joins it.
                        var dot = new Rectangle { Width = thickness, Height = thickness, Fill = stroke, IsHitTestVisible = false };
                        SetLeft(dot, run[0].X - thickness / 2);
                        SetTop(dot, run[0].Y - thickness / 2);
                        Children.Add(dot);
                    }
                    run.Clear();
                }
                // A wrapping chart's line ends at the time bar, and the slot after it is blank (the control
                // clears the sample the next one will overwrite): what lies beyond is the previous pass.
                var blank = !graph.Scroll && graph.BarSlot >= 0 ? (graph.BarSlot + 1) % slots : -1;
                for (var slot = 0; slot < series.Values.Count && slot < slots; ++slot)
                {
                    if (slot == blank || series.Values[slot] is not { } value)
                    {
                        Flush();
                        continue;
                    }
                    run.Add(new global::Windows.Foundation.Point(SlotX(slot), YOf(value * series.Scale)));
                    if (!graph.Scroll && slot == graph.BarSlot) Flush();
                }
                Flush();
            }
            if (!graph.Scroll && graph.BarSlot >= 0 && graph.BarSlot < slots)
                AddLine(SlotX(graph.BarSlot), top, SlotX(graph.BarSlot), plotBottom, BrushOf(graph.BarColor));
        }

        var frame = new Rectangle { Width = plotWidth, Height = plotHeight, Stroke = text, StrokeThickness = 1, IsHitTestVisible = false };
        SetLeft(frame, left);
        SetTop(frame, top);
        Children.Add(frame);
    }

    protected override AutomationPeer OnCreateAutomationPeer() => new PerformanceGraphAutomationPeer(this);
}

internal sealed class PerformanceGraphAutomationPeer(PerformanceGraphControl owner) : FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Pane;
    protected override string GetClassNameCore() => "PerformanceGraph";
    protected override bool IsControlElementCore() => true;
    // A Canvas is not a Control, so its peer reports it enabled whatever the native control is: a console
    // whose modal dialog is open natively (the window is disabled) would fail the committed gate.
    protected override bool IsEnabledCore() => ControlFactory.NodeEnabled(owner);
}

/// <summary>
/// The legend under Performance Monitor's graph (minor 36): one row per counter with its Show
/// check box, line swatch and descriptive cells, in the columns the native legend has.
/// </summary>
internal sealed class PerformanceLegendControl : Grid
{
    // A native legend row is this tall (physical pixels), whatever the dialog font.
    private const double RowHeightPixels = 17;
    private readonly double _scale;
    private readonly double _fontSize;
    private readonly Action<int> _selectRequested;
    private readonly Action<int, bool> _showRequested;
    private readonly ListView _list = new() { SelectionMode = ListViewSelectionMode.Single, Padding = new Thickness(0) };
    private PerformanceLegendSnapshot? _legend;
    private bool _applying;

    public PerformanceLegendControl(double scale, double fontSize, Action<int> selectRequested, Action<int, bool> showRequested)
    {
        _scale = scale;
        _fontSize = fontSize;
        _selectRequested = selectRequested;
        _showRequested = showRequested;
        Background = new SolidColorBrush(Colors.White);
        BorderBrush = new SolidColorBrush(global::Windows.UI.Color.FromArgb(255, 160, 160, 160));
        BorderThickness = new Thickness(1);
        RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        // A list item is 40 pixels tall by default; a legend row is as tall as the native one.
        var itemStyle = new Style(typeof(ListViewItem));
        itemStyle.Setters.Add(new Setter(Control.MinHeightProperty, 0d));
        itemStyle.Setters.Add(new Setter(Control.PaddingProperty, new Thickness(0)));
        itemStyle.Setters.Add(new Setter(Control.HorizontalContentAlignmentProperty, HorizontalAlignment.Stretch));
        _list.ItemContainerStyle = itemStyle;
        _list.SelectionChanged += (_, _) =>
        {
            if (_applying) return;
            if (_list.SelectedIndex >= 0) _selectRequested(_list.SelectedIndex);
        };
        SetRow(_list, 1);
        Children.Add(_list);
    }

    public void Apply(PerformanceLegendSnapshot? legend)
    {
        _legend = legend;
        _applying = true;
        try
        {
            while (Children.Count > 1) Children.RemoveAt(0);
            _list.Items.Clear();
            if (legend is null) return;
            var header = Row(legend, headerRow: true, row: null, index: -1);
            SetRow(header, 0);
            Children.Insert(0, header);
            for (var index = 0; index < legend.Rows.Count; ++index)
                _list.Items.Add(Row(legend, headerRow: false, legend.Rows[index], index));
            _list.SelectedIndex = legend.Rows.FindIndex(row => row.Selected);
        }
        finally
        {
            _applying = false;
        }
    }

    private Grid Row(PerformanceLegendSnapshot legend, bool headerRow, PerformanceLegendRowSnapshot? row, int index)
    {
        var grid = new Grid { MinHeight = 0, Height = RowHeightPixels * _scale };
        foreach (var column in legend.Columns)
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(Math.Max(0, column.Width * _scale)) });
        for (var column = 0; column < legend.Columns.Count; ++column)
        {
            FrameworkElement cell;
            if (headerRow)
                cell = new TextBlock { Text = legend.Columns[column].Title, FontSize = _fontSize, Margin = new Thickness(4, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center };
            else if (column == 0)
            {
                var show = new CheckBox { IsChecked = row!.Visible, MinWidth = 0, MinHeight = 0, Padding = new Thickness(0), Margin = new Thickness(4, 0, 0, 0) };
                var rowIndex = index;
                show.Click += (_, _) =>
                {
                    if (!_applying) _showRequested(rowIndex, show.IsChecked == true);
                };
                // The counter's name, then its instance: "% Processor Time _Total".
                AutomationPropertiesName(show, string.Join(' ', row.Cells.Skip(1).Take(2).Where(cell => cell.Length > 0)));
                cell = show;
            }
            else if (column == 1 && legend.DisplayType == "histogram")
            {
                // A histogram's counters are bars, so their swatch is a solid bar of the counter's colour.
                var colour = new SolidColorBrush(PerformanceGraphControl.ColorOf(row!.Color));
                cell = new Rectangle
                {
                    Fill = colour, Height = 7 * _scale, VerticalAlignment = VerticalAlignment.Center,
                    Margin = new Thickness(4 * _scale, 0, 8 * _scale, 0),
                };
            }
            else if (column == 1)
            {
                var swatch = new Line { X1 = 4, X2 = Math.Max(8, legend.Columns[1].Width * _scale - 4), Y1 = 10, Y2 = 10, Stroke = new SolidColorBrush(PerformanceGraphControl.ColorOf(row!.Color)), StrokeThickness = row.Width, StrokeDashArray = PerformanceGraphControl.DashFor(row.Style, row.Width) };
                cell = swatch;
            }
            else
                cell = new TextBlock { Text = row!.Cells[column - 2], FontSize = _fontSize, Margin = new Thickness(4, 0, 0, 0), VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
            SetColumn(cell, column);
            grid.Children.Add(cell);
        }
        return grid;
    }

    private static void AutomationPropertiesName(DependencyObject target, string name) =>
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(target, name);

    protected override AutomationPeer OnCreateAutomationPeer() => new PerformanceLegendAutomationPeer(this);
}

internal sealed class PerformanceLegendAutomationPeer(PerformanceLegendControl owner) : FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.List;
    protected override string GetClassNameCore() => "PerformanceLegend";
    protected override bool IsControlElementCore() => true;
    protected override bool IsEnabledCore() => ControlFactory.NodeEnabled(owner);
}

/// <summary>The value strip under the graph (minor 36): Last, Average, Minimum, Maximum, Duration.</summary>
internal sealed class PerformanceValueBarControl : Grid
{
    private readonly double _fontSize;

    public PerformanceValueBarControl(double fontSize)
    {
        _fontSize = fontSize;
    }

    public void Apply(PerformanceValueBarSnapshot? bar)
    {
        Children.Clear();
        ColumnDefinitions.Clear();
        if (bar is null) return;
        // Each field is its label (as wide as its text) and a value box; the boxes share the
        // strip's width equally.
        for (var index = 0; index < bar.Fields.Count; ++index)
        {
            var field = bar.Fields[index];
            ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            var label = new TextBlock
            {
                Text = field.Label, FontSize = _fontSize, VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(index == 0 ? 2 : 8, 0, 4, 0),
            };
            var value = new Border
            {
                BorderThickness = new Thickness(1),
                BorderBrush = new SolidColorBrush(global::Windows.UI.Color.FromArgb(255, 160, 160, 160)),
                Padding = new Thickness(4, 0, 4, 0),
                Margin = new Thickness(0, 1, 0, 1),
                Child = new TextBlock { Text = field.Value, FontSize = _fontSize, TextAlignment = TextAlignment.Right, VerticalAlignment = VerticalAlignment.Center },
            };
            SetColumn(label, index * 2);
            SetColumn(value, index * 2 + 1);
            Children.Add(label);
            Children.Add(value);
        }
    }

    protected override AutomationPeer OnCreateAutomationPeer() => new PerformanceValueBarAutomationPeer(this);
}

internal sealed class PerformanceValueBarAutomationPeer(PerformanceValueBarControl owner) : FrameworkElementAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Pane;
    protected override string GetClassNameCore() => "PerformanceValueBar";
    protected override bool IsControlElementCore() => true;
    protected override bool IsEnabledCore() => ControlFactory.NodeEnabled(owner);
}
