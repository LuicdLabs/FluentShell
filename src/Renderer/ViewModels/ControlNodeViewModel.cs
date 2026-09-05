using System.Collections.ObjectModel;
using System.Text.Json;
using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.ViewModels;

public sealed class ControlNodeViewModel : ObservableObject
{
    private string _text = string.Empty;
    private string _draftText = string.Empty;
    private string _automationName = string.Empty;
    private bool _visible;
    private bool _enabled;
    private int _checked;
    private int _selectedIndex = -1;
    private int _focusedIndex = -1;
    private bool _multiSelect;
    private bool _columnHeadersVisible;
    private bool _checkBoxes;
    private int _selectionStart;
    private int _selectionLength;
    private int _minimum;
    private int _maximum = 100;
    private int _position;
    private int _smallChange = 1;
    private int _largeChange = 10;
    private bool _vertical;
    private bool _reversed;
    private bool _indeterminate;
    private bool _editable;
    private bool _active;
    private string _windowState = "normal";
    private PixelRect _clientRect = new();
    private bool _editableLabels;
    private int _editingIndex = -1;
    private PixelRect _rect = new();
    private int _imageWidth;
    private int _imageHeight;
    private string _imageFormat = string.Empty;
    private string _imageData = string.Empty;
    private readonly Dictionary<string, string> _pendingEventIds = new(StringComparer.Ordinal);

    public string NodeId { get; private set; } = "0";
    public string Generation { get; private set; } = "0";
    public string? NativeHwnd { get; private set; } = "0x0";
    public string? ParentNodeId { get; private set; }
    public string Kind { get; private set; } = string.Empty;
    public int ControlId { get; private set; }
    public int ZIndex { get; private set; }
    public int TabIndex { get; private set; }
    public bool TabStop { get; private set; }
    public uint DialogCode { get; private set; }
    public bool ReadOnly { get; private set; }
    public bool Multiline { get; private set; }
    public bool Editable { get => _editable; private set => SetProperty(ref _editable, value); }
    public bool IsDefault { get; private set; }
    public bool GroupStart { get; private set; }
    // Native window style bits, already validated as canonical hex on admission.
    // The projection needs them where the native shape decides what a control
    // offers -- an MDI child's caption buttons are style bits, not content.
    public ulong Style { get; private set; }
    public bool Active { get => _active; private set => SetProperty(ref _active, value); }
    public string WindowState { get => _windowState; private set => SetProperty(ref _windowState, value); }
    public PixelRect ClientRect { get => _clientRect; private set => SetProperty(ref _clientRect, value); }
    public string Text { get => _text; private set => SetProperty(ref _text, value); }
    public string DraftText { get => _draftText; set => SetProperty(ref _draftText, value); }
    public string AutomationName { get => _automationName; private set => SetProperty(ref _automationName, value); }
    public bool Visible { get => _visible; private set => SetProperty(ref _visible, value); }
    public bool Enabled { get => _enabled; private set => SetProperty(ref _enabled, value); }
    public int Checked { get => _checked; private set => SetProperty(ref _checked, value); }
    public int SelectedIndex { get => _selectedIndex; private set => SetProperty(ref _selectedIndex, value); }
    public int FocusedIndex { get => _focusedIndex; private set => SetProperty(ref _focusedIndex, value); }
    public bool MultiSelect { get => _multiSelect; private set => SetProperty(ref _multiSelect, value); }
    public bool ColumnHeadersVisible { get => _columnHeadersVisible; private set => SetProperty(ref _columnHeadersVisible, value); }
    public bool CheckBoxes { get => _checkBoxes; private set => SetProperty(ref _checkBoxes, value); }
    public int SelectionStart { get => _selectionStart; private set => SetProperty(ref _selectionStart, value); }
    public int SelectionLength { get => _selectionLength; private set => SetProperty(ref _selectionLength, value); }
    public int Minimum { get => _minimum; private set => SetProperty(ref _minimum, value); }
    public int Maximum { get => _maximum; private set => SetProperty(ref _maximum, value); }
    public int Position { get => _position; private set => SetProperty(ref _position, value); }
    public int SmallChange { get => _smallChange; private set => SetProperty(ref _smallChange, value); }
    public int LargeChange { get => _largeChange; private set => SetProperty(ref _largeChange, value); }
    public bool Vertical { get => _vertical; private set => SetProperty(ref _vertical, value); }
    public bool Reversed { get => _reversed; private set => SetProperty(ref _reversed, value); }
    public bool Indeterminate { get => _indeterminate; private set => SetProperty(ref _indeterminate, value); }
    public PixelRect Rect { get => _rect; private set => SetProperty(ref _rect, value); }
    public int ImageWidth { get => _imageWidth; private set => SetProperty(ref _imageWidth, value); }
    public int ImageHeight { get => _imageHeight; private set => SetProperty(ref _imageHeight, value); }
    public string ImageFormat { get => _imageFormat; private set => SetProperty(ref _imageFormat, value); }
    public string ImageData { get => _imageData; private set => SetProperty(ref _imageData, value); }
    public ObservableCollection<string> Items { get; } = [];
    public ObservableCollection<PixelRect> ItemRects { get; } = [];
    public ObservableCollection<int> SelectedIndices { get; } = [];
    public ObservableCollection<int> CheckedIndices { get; } = [];
    public ObservableCollection<string> Columns { get; } = [];
    public ObservableCollection<int> ColumnWidths { get; } = [];
    public ObservableCollection<int> ColumnOrder { get; } = [];
    public ObservableCollection<int> ItemDepths { get; } = [];
    public ObservableCollection<bool> ItemExpanded { get; } = [];
    public ObservableCollection<bool> ItemHasChildren { get; } = [];
    public ObservableCollection<ImageListEntry> ImageList { get; } = [];
    public ObservableCollection<int> ItemImages { get; } = [];
    public ObservableCollection<int> ItemSelectedImages { get; } = [];
    public bool EditableLabels { get => _editableLabels; private set => SetProperty(ref _editableLabels, value); }
    public int EditingIndex { get => _editingIndex; private set => SetProperty(ref _editingIndex, value); }
    public ObservableCollection<IReadOnlyList<string>> Rows { get; } = [];
    public ObservableCollection<ToolbarItemSnapshot> ToolbarItems { get; } = [];
    public ObservableCollection<PaneSplit> Splits { get; } = [];
    public ObservableCollection<ChromeRegion> ChromeRegions { get; } = [];
    public ObservableCollection<AccessibleIslandItem> IslandItems { get; } = [];
    public string AdapterId { get; private set; } = string.Empty;
    public string PageId { get; private set; } = string.Empty;
    public string SemanticKey { get; private set; } = string.Empty;
    public string SourceKind { get; private set; } = string.Empty;
    public string PresentationVariant { get; private set; } = string.Empty;
    public IReadOnlyList<string> SupportedActions { get; private set; } = [];
    public string HelpText { get; private set; } = string.Empty;
    public string AccessKey { get; private set; } = string.Empty;

    public static ControlNodeViewModel FromSnapshot(ControlNode node)
    {
        var viewModel = new ControlNodeViewModel();
        viewModel.ApplySnapshot(node);
        return viewModel;
    }

    public void ApplySnapshot(
        ControlNode node,
        bool preserveTransient = false,
        string? eventId = null)
    {
        var previousText = Text;
        var matchingTextEcho = preserveTransient && IsPendingEcho("text", eventId);
        var hasDraft = preserveTransient && !matchingTextEcho && DraftText != previousText;
        NodeId = node.NodeId;
        Generation = node.Generation;
        NativeHwnd = node.NativeHwnd;
        ParentNodeId = node.ParentNodeId;
        AdapterId = node.AdapterId ?? string.Empty;
        PageId = node.PageId ?? string.Empty;
        SemanticKey = node.SemanticKey ?? string.Empty;
        SourceKind = node.SourceKind ?? string.Empty;
        PresentationVariant = node.PresentationVariant ?? string.Empty;
        SupportedActions = node.SupportedActions ?? [];
        HelpText = node.HelpText ?? string.Empty;
        AccessKey = node.AccessKey ?? string.Empty;
        Kind = node.Kind;
        ControlId = node.ControlId;
        ZIndex = node.ZIndex;
        TabIndex = node.TabIndex ?? node.ZIndex;
        Rect = node.Rect;
        Visible = node.Visible;
        Enabled = node.Enabled;
        TabStop = node.TabStop;
        DialogCode = node.DialogCode;
        Text = node.Text;
        if (!hasDraft) DraftText = node.Text;
        AutomationName = node.AutomationName ?? node.Text;
        Checked = node.Checked ?? 0;
        SelectedIndex = node.SelectedIndex ?? -1;
        FocusedIndex = node.FocusedIndex ?? -1;
        MultiSelect = node.MultiSelect;
        ColumnHeadersVisible = node.ColumnHeadersVisible ?? false;
        CheckBoxes = node.CheckBoxes ?? false;
        SelectionStart = node.SelectionStart ?? 0;
        SelectionLength = node.SelectionLength ?? 0;
        ReadOnly = node.ReadOnly ?? false;
        Multiline = node.Multiline ?? false;
        Editable = node.Editable;
        IsDefault = node.IsDefault ?? false;
        GroupStart = node.GroupStart ?? false;
        Style = ParseStyle(node.Style);
        Active = node.Active ?? false;
        WindowState = node.WindowState ?? "normal";
        ClientRect = node.ClientRect ?? new PixelRect();
        ApplyProgressState(node.Minimum ?? 0, node.Maximum ?? 100, node.Position ?? 0);
        SmallChange = node.SmallChange ?? 1;
        LargeChange = node.LargeChange ?? 10;
        Vertical = node.Vertical;
        Reversed = node.Reversed;
        Indeterminate = node.Indeterminate ?? false;
        ReplaceItems(node.Items);
        ReplaceTreeState(node.ItemDepths ?? [], node.ItemExpanded ?? [], node.ItemHasChildren ?? []);
        ReplaceItemImagery(
            node.ImageList ?? [], node.ItemImages ?? [], node.ItemSelectedImages ?? []);
        EditableLabels = node.EditableLabels ?? false;
        EditingIndex = node.EditingIndex ?? -1;
        ReplaceItemRects(node.ItemRects ?? []);
        ReplaceSelectedIndices(node.SelectedIndices);
        ReplaceCheckedIndices(node.CheckedIndices ?? []);
        ReplaceColumns(node.Columns);
        ReplaceColumnWidths(node.ColumnWidths);
        ReplaceColumnOrder(node.ColumnOrder ?? []);
        ReplaceRows(node.Rows);
        ReplaceToolbarItems(node.ToolbarItems ?? []);
        ReplaceSplits(node.Splits ?? []);
        ReplaceChromeRegions(node.ChromeRegions ?? []);
        ReplaceIslandItems(node.IslandItems ?? []);
        ApplyImageState(
            node.ImageWidth ?? 0,
            node.ImageHeight ?? 0,
            node.ImageFormat ?? string.Empty,
            node.ImageData ?? string.Empty);
        if (!preserveTransient) _pendingEventIds.Clear();
        else if (eventId is not null)
        {
            foreach (var property in _pendingEventIds
                .Where(entry => entry.Value == eventId)
                .Select(entry => entry.Key)
                .ToArray())
            {
                _pendingEventIds.Remove(property);
            }
        }
    }

    public void RegisterPending(string property, string eventId) => _pendingEventIds[property] = eventId;

    public bool IsPendingEcho(string property, string? eventId) =>
        eventId is not null && _pendingEventIds.TryGetValue(property, out var pending) && pending == eventId;

    public void RejectPending(string property, string eventId)
    {
        if (!IsPendingEcho(property, eventId)) return;
        _pendingEventIds.Remove(property);
        if (property == "text") DraftText = Text;
        else if (property == "selectedIndex") RaisePropertyChanged(nameof(SelectedIndex));
        else if (property == "checkedIndices") RaisePropertyChanged(nameof(CheckedIndices));
        else if (property == "position") RaisePropertyChanged(nameof(Position));
        else if (property == "itemExpanded") RaisePropertyChanged(nameof(ItemDepths));
        // A refused split leaves the canonical geometry in place, and the projected
        // splitter follows it back.
        else if (property == "splits") RaisePropertyChanged(nameof(Splits));
    }

    public void AcceptPending(string property, string eventId)
    {
        if (IsPendingEcho(property, eventId)) _pendingEventIds.Remove(property);
    }

    public void ApplyCanonical(string property, System.Text.Json.JsonElement value, string? eventId)
    {
        var matchingEcho = IsPendingEcho(property, eventId);
        switch (property)
        {
            case "text":
                var previousText = Text;
                Text = value.GetString() ?? string.Empty;
                if (matchingEcho || DraftText == previousText) DraftText = Text;
                break;
            case "automationName": AutomationName = value.GetString() ?? string.Empty; break;
            case "visible": Visible = value.GetBoolean(); break;
            case "enabled": Enabled = value.GetBoolean(); break;
            case "checked": Checked = value.GetInt32(); break;
            case "selectedIndex": SelectedIndex = value.GetInt32(); break;
            case "selectedIndices": ReplaceSelectedIndices(value.Deserialize<List<int>>() ?? []); break;
            case "focusedIndex": FocusedIndex = value.GetInt32(); break;
            case "multiSelect": MultiSelect = value.GetBoolean(); break;
            case "columnHeadersVisible": ColumnHeadersVisible = value.GetBoolean(); break;
            case "checkBoxes": CheckBoxes = value.GetBoolean(); break;
            case "checkedIndices": ReplaceCheckedIndices(value.Deserialize<List<int>>() ?? []); break;
            case "selectionStart": SelectionStart = value.GetInt32(); break;
            case "selectionLength": SelectionLength = value.GetInt32(); break;
            case "editable": Editable = value.GetBoolean(); break;
            case "minimum": Minimum = value.GetInt32(); break;
            case "maximum": Maximum = value.GetInt32(); break;
            case "position": Position = value.GetInt32(); break;
            case "smallChange": SmallChange = value.GetInt32(); break;
            case "largeChange": LargeChange = value.GetInt32(); break;
            case "vertical": Vertical = value.GetBoolean(); break;
            case "reversed": Reversed = value.GetBoolean(); break;
            case "editableLabels": EditableLabels = value.GetBoolean(); break;
            case "editingIndex": EditingIndex = value.GetInt32(); break;
            case "itemDepths":
            case "itemExpanded":
            case "itemHasChildren":
                // The three tree arrays are one hierarchy, so a projected tree is
                // never rebuilt from a partially updated set of them.
                throw new ProtocolException(
                    "TreeView hierarchy state is republished as a full snapshot, not as a field patch.");
            case "indeterminate": Indeterminate = value.GetBoolean(); break;
            case "active": Active = value.GetBoolean(); break;
            case "windowState": WindowState = value.GetString() ?? "normal"; break;
            case "clientRect": ClientRect = value.Deserialize<PixelRect>() ?? throw new ProtocolException("Invalid MDI child client rect patch."); break;
            case "rect": Rect = value.Deserialize<PixelRect>() ?? throw new ProtocolException("Invalid node rect patch."); break;
            case "items": ReplaceItems(value.Deserialize<List<string>>() ?? []); break;
            case "itemRects": ReplaceItemRects(value.Deserialize<List<PixelRect>>() ?? []); break;
            case "columns": ReplaceColumns(value.Deserialize<List<string>>() ?? []); break;
            case "columnWidths": ReplaceColumnWidths(value.Deserialize<List<int>>() ?? []); break;
            case "columnOrder": ReplaceColumnOrder(value.Deserialize<List<int>>() ?? []); break;
            case "rows": ReplaceRows(value.Deserialize<List<List<string>>>() ?? []); break;
            case "imageWidth": ImageWidth = value.GetInt32(); break;
            case "imageHeight": ImageHeight = value.GetInt32(); break;
            case "imageFormat": ImageFormat = value.GetString() ?? string.Empty; break;
            case "imageData": ImageData = value.GetString() ?? string.Empty; break;
            case "toolbarItems": ReplaceToolbarItems(value.Deserialize<List<ToolbarItemSnapshot>>() ?? []); break;
            default: throw new ProtocolException($"Unsupported node patch property '{property}'.");
        }
        if (matchingEcho) _pendingEventIds.Remove(property);
    }

    private static ulong ParseStyle(string? value) =>
        value is not null && value.StartsWith("0x", StringComparison.Ordinal) &&
        ulong.TryParse(value.AsSpan(2), System.Globalization.NumberStyles.AllowHexSpecifier,
            System.Globalization.CultureInfo.InvariantCulture, out var parsed)
            ? parsed
            : 0;

    private void ReplaceItems(IEnumerable<string> items)
    {
        if (Items.SequenceEqual(items, StringComparer.Ordinal)) return;
        Items.Clear();
        foreach (var item in items) Items.Add(item);
        RaisePropertyChanged(nameof(Items));
    }

    // Depths, expansion, and child flags describe one tree, so they are replaced
    // together and announced once: the factory rebuilds from that notification
    // rather than from an intermediate combination of the three.
    private void ReplaceTreeState(
        IReadOnlyList<int> depths,
        IReadOnlyList<bool> expanded,
        IReadOnlyList<bool> hasChildren)
    {
        if (ItemDepths.SequenceEqual(depths) && ItemExpanded.SequenceEqual(expanded) &&
            ItemHasChildren.SequenceEqual(hasChildren)) return;
        ItemDepths.Clear();
        foreach (var depth in depths) ItemDepths.Add(depth);
        ItemExpanded.Clear();
        foreach (var value in expanded) ItemExpanded.Add(value);
        ItemHasChildren.Clear();
        foreach (var value in hasChildren) ItemHasChildren.Add(value);
        RaisePropertyChanged(nameof(ItemDepths));
    }

    private void ReplaceSelectedIndices(IEnumerable<int> selectedIndices)
    {
        if (SelectedIndices.SequenceEqual(selectedIndices)) return;
        SelectedIndices.Clear();
        foreach (var index in selectedIndices) SelectedIndices.Add(index);
        RaisePropertyChanged(nameof(SelectedIndices));
    }

    private void ReplaceItemRects(IEnumerable<PixelRect> rects)
    {
        if (ItemRects.SequenceEqual(rects)) return;
        ItemRects.Clear();
        foreach (var rect in rects) ItemRects.Add(rect);
        RaisePropertyChanged(nameof(ItemRects));
    }

    private void ReplaceCheckedIndices(IEnumerable<int> checkedIndices)
    {
        if (CheckedIndices.SequenceEqual(checkedIndices)) return;
        CheckedIndices.Clear();
        foreach (var index in checkedIndices) CheckedIndices.Add(index);
        RaisePropertyChanged(nameof(CheckedIndices));
    }

    private void ReplaceColumns(IEnumerable<string> columns)
    {
        if (Columns.SequenceEqual(columns, StringComparer.Ordinal)) return;
        Columns.Clear();
        foreach (var column in columns) Columns.Add(column);
        RaisePropertyChanged(nameof(Columns));
    }

    private void ReplaceColumnOrder(IEnumerable<int> order)
    {
        if (ColumnOrder.SequenceEqual(order)) return;
        ColumnOrder.Clear();
        foreach (var logical in order) ColumnOrder.Add(logical);
        RaisePropertyChanged(nameof(ColumnOrder));
    }

    private void ReplaceColumnWidths(IEnumerable<int> widths)
    {
        if (ColumnWidths.SequenceEqual(widths)) return;
        ColumnWidths.Clear();
        foreach (var width in widths) ColumnWidths.Add(width);
        RaisePropertyChanged(nameof(ColumnWidths));
    }

    private void ReplaceRows(IEnumerable<IEnumerable<string>> rows)
    {
        var copies = rows.Select(row => (IReadOnlyList<string>)row.ToArray()).ToArray();
        if (Rows.Count == copies.Length && Rows.Zip(copies).All(pair =>
                pair.First.SequenceEqual(pair.Second, StringComparer.Ordinal))) return;
        Rows.Clear();
        foreach (var row in copies) Rows.Add(row);
        RaisePropertyChanged(nameof(Rows));
    }

    private void ReplaceToolbarItems(IEnumerable<ToolbarItemSnapshot> items)
    {
        if (ToolbarItems.SequenceEqual(items)) return;
        ToolbarItems.Clear();
        foreach (var item in items) ToolbarItems.Add(item);
        RaisePropertyChanged(nameof(ToolbarItems));
    }

    private void ReplaceSplits(IEnumerable<PaneSplit> splits)
    {
        if (Splits.SequenceEqual(splits)) return;
        Splits.Clear();
        foreach (var split in splits) Splits.Add(split);
        RaisePropertyChanged(nameof(Splits));
    }

    private void ReplaceChromeRegions(IEnumerable<ChromeRegion> regions)
    {
        if (ChromeRegions.SequenceEqual(regions)) return;
        ChromeRegions.Clear();
        foreach (var region in regions) ChromeRegions.Add(region);
        RaisePropertyChanged(nameof(ChromeRegions));
    }

    private void ReplaceIslandItems(IEnumerable<AccessibleIslandItem> items)
    {
        if (IslandItems.SequenceEqual(items)) return;
        IslandItems.Clear();
        foreach (var item in items) IslandItems.Add(item);
        RaisePropertyChanged(nameof(IslandItems));
    }

    // Icons and their per-item indexes are one description of the same items, so
    // they are replaced together and announced once.
    private void ReplaceItemImagery(
        IReadOnlyList<ImageListEntry> imageList,
        IReadOnlyList<int> itemImages,
        IReadOnlyList<int> selectedImages)
    {
        if (ImageList.SequenceEqual(imageList) && ItemImages.SequenceEqual(itemImages) &&
            ItemSelectedImages.SequenceEqual(selectedImages)) return;
        ImageList.Clear();
        foreach (var entry in imageList) ImageList.Add(entry);
        ItemImages.Clear();
        foreach (var index in itemImages) ItemImages.Add(index);
        ItemSelectedImages.Clear();
        foreach (var index in selectedImages) ItemSelectedImages.Add(index);
        RaisePropertyChanged(nameof(ItemImages));
    }

    private void ApplyProgressState(int minimum, int maximum, int position)
    {
        if (minimum > Maximum)
        {
            Maximum = maximum;
            Minimum = minimum;
        }
        else
        {
            Minimum = minimum;
            Maximum = maximum;
        }
        Position = position;
    }

    private void ApplyImageState(int width, int height, string format, string data)
    {
        if (_imageWidth == width && _imageHeight == height &&
            _imageFormat == format && _imageData == data) return;
        _imageWidth = width;
        _imageHeight = height;
        _imageFormat = format;
        _imageData = data;
        // The factory rebuilds from this notification after the complete image
        // tuple has changed, never from an intermediate width/data combination.
        RaisePropertyChanged(nameof(ImageData));
    }
}
