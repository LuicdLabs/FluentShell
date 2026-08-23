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
    private int _selectionStart;
    private int _selectionLength;
    private int _minimum;
    private int _maximum = 100;
    private int _position;
    private bool _editable;
    private PixelRect _rect = new();
    private readonly Dictionary<string, string> _pendingEventIds = new(StringComparer.Ordinal);

    public string NodeId { get; private set; } = "0";
    public string Generation { get; private set; } = "0";
    public string NativeHwnd { get; private set; } = "0x0";
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
    public string Text { get => _text; private set => SetProperty(ref _text, value); }
    public string DraftText { get => _draftText; set => SetProperty(ref _draftText, value); }
    public string AutomationName { get => _automationName; private set => SetProperty(ref _automationName, value); }
    public bool Visible { get => _visible; private set => SetProperty(ref _visible, value); }
    public bool Enabled { get => _enabled; private set => SetProperty(ref _enabled, value); }
    public int Checked { get => _checked; private set => SetProperty(ref _checked, value); }
    public int SelectedIndex { get => _selectedIndex; private set => SetProperty(ref _selectedIndex, value); }
    public int FocusedIndex { get => _focusedIndex; private set => SetProperty(ref _focusedIndex, value); }
    public bool MultiSelect { get => _multiSelect; private set => SetProperty(ref _multiSelect, value); }
    public int SelectionStart { get => _selectionStart; private set => SetProperty(ref _selectionStart, value); }
    public int SelectionLength { get => _selectionLength; private set => SetProperty(ref _selectionLength, value); }
    public int Minimum { get => _minimum; private set => SetProperty(ref _minimum, value); }
    public int Maximum { get => _maximum; private set => SetProperty(ref _maximum, value); }
    public int Position { get => _position; private set => SetProperty(ref _position, value); }
    public PixelRect Rect { get => _rect; private set => SetProperty(ref _rect, value); }
    public ObservableCollection<string> Items { get; } = [];
    public ObservableCollection<int> SelectedIndices { get; } = [];
    public ObservableCollection<string> Columns { get; } = [];
    public ObservableCollection<int> ColumnWidths { get; } = [];
    public ObservableCollection<IReadOnlyList<string>> Rows { get; } = [];

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
        SelectionStart = node.SelectionStart ?? 0;
        SelectionLength = node.SelectionLength ?? 0;
        ReadOnly = node.ReadOnly ?? false;
        Multiline = node.Multiline ?? false;
        Editable = node.Editable;
        IsDefault = node.IsDefault ?? false;
        GroupStart = node.GroupStart ?? false;
        ApplyProgressState(node.Minimum ?? 0, node.Maximum ?? 100, node.Position ?? 0);
        ReplaceItems(node.Items);
        ReplaceSelectedIndices(node.SelectedIndices);
        ReplaceColumns(node.Columns);
        ReplaceColumnWidths(node.ColumnWidths);
        ReplaceRows(node.Rows);
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
            case "selectionStart": SelectionStart = value.GetInt32(); break;
            case "selectionLength": SelectionLength = value.GetInt32(); break;
            case "editable": Editable = value.GetBoolean(); break;
            case "minimum": Minimum = value.GetInt32(); break;
            case "maximum": Maximum = value.GetInt32(); break;
            case "position": Position = value.GetInt32(); break;
            case "rect": Rect = value.Deserialize<PixelRect>() ?? throw new ProtocolException("Invalid node rect patch."); break;
            case "items": ReplaceItems(value.Deserialize<List<string>>() ?? []); break;
            case "columns": ReplaceColumns(value.Deserialize<List<string>>() ?? []); break;
            case "columnWidths": ReplaceColumnWidths(value.Deserialize<List<int>>() ?? []); break;
            case "rows": ReplaceRows(value.Deserialize<List<List<string>>>() ?? []); break;
            default: throw new ProtocolException($"Unsupported node patch property '{property}'.");
        }
        if (matchingEcho) _pendingEventIds.Remove(property);
    }

    private void ReplaceItems(IEnumerable<string> items)
    {
        if (Items.SequenceEqual(items, StringComparer.Ordinal)) return;
        Items.Clear();
        foreach (var item in items) Items.Add(item);
        RaisePropertyChanged(nameof(Items));
    }

    private void ReplaceSelectedIndices(IEnumerable<int> selectedIndices)
    {
        if (SelectedIndices.SequenceEqual(selectedIndices)) return;
        SelectedIndices.Clear();
        foreach (var index in selectedIndices) SelectedIndices.Add(index);
        RaisePropertyChanged(nameof(SelectedIndices));
    }

    private void ReplaceColumns(IEnumerable<string> columns)
    {
        if (Columns.SequenceEqual(columns, StringComparer.Ordinal)) return;
        Columns.Clear();
        foreach (var column in columns) Columns.Add(column);
        RaisePropertyChanged(nameof(Columns));
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
}
