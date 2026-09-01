using System.Collections.ObjectModel;
using System.Text.Json;
using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.ViewModels;

public sealed class WindowViewModel : ObservableObject
{
    private string _title = string.Empty;
    private PixelRect _bounds = new();
    private PixelRect _clientBounds = new();
    private bool _visible;
    private bool _enabled;
    private string _state = "normal";

    public Guid SurfaceId { get; private set; }
    public string SurfaceKind { get; private set; } = "window";
    public bool Modal { get; private set; }
    public bool CanCancel { get; private set; }
    public string Icon { get; private set; } = "none";
    public string Generation { get; private set; } = "0";
    public ulong Revision { get; private set; }
    public string NativeHwnd { get; private set; } = "0x0";
    public string? OwnerHwnd { get; private set; }
    public int Dpi { get; private set; } = 96;
    public ulong WindowStyle { get; private set; }
    public ulong WindowExStyle { get; private set; }
    public bool ShowInTaskbar { get; private set; }
    public bool Rtl { get; private set; }
    public string AdapterId { get; private set; } = string.Empty;
    public string PageId { get; private set; } = string.Empty;
    public bool IsResizable => (WindowStyle & 0x00040000) != 0;
    public bool IsMinimizable => (WindowStyle & 0x00020000) != 0;
    public bool IsMaximizable => (WindowStyle & 0x00010000) != 0;
    public bool IsAlwaysOnTop => (WindowExStyle & 0x00000008) != 0;

    public string Title { get => _title; private set => SetProperty(ref _title, value); }
    public PixelRect Bounds { get => _bounds; private set => SetProperty(ref _bounds, value); }
    public PixelRect ClientBounds { get => _clientBounds; private set => SetProperty(ref _clientBounds, value); }
    public bool Visible { get => _visible; private set => SetProperty(ref _visible, value); }
    public bool Enabled { get => _enabled; private set => SetProperty(ref _enabled, value); }
    public string State { get => _state; private set => SetProperty(ref _state, value); }
    public ObservableCollection<ControlNodeViewModel> Nodes { get; } = [];
    public ObservableCollection<MenuItemViewModel> Menu { get; } = [];

    public static WindowViewModel FromSnapshot(WindowSnapshot snapshot)
    {
        var viewModel = new WindowViewModel();
        viewModel.ApplySnapshot(snapshot);
        return viewModel;
    }

    public bool CanMergeSnapshot(WindowSnapshot snapshot)
    {
        if (snapshot.Dpi != Dpi || (snapshot.AdapterId ?? string.Empty) != AdapterId ||
            (snapshot.PageId ?? string.Empty) != PageId) return false;
        if (snapshot.Menu.Count != Menu.Count ||
            !Menu.Zip(snapshot.Menu).All(pair => pair.First.HasSameShape(pair.Second))) return false;
        if (snapshot.Nodes.Count != Nodes.Count) return false;
        var ordered = snapshot.Nodes.OrderBy(node => node.ZIndex).ToArray();
        for (var index = 0; index < ordered.Length; index++)
        {
            var incoming = ordered[index];
            var current = Nodes[index];
            if (incoming.NodeId != current.NodeId || incoming.Generation != current.Generation ||
                incoming.NativeHwnd != current.NativeHwnd || incoming.Kind != current.Kind ||
                incoming.ParentNodeId != current.ParentNodeId ||
                incoming.ZIndex != current.ZIndex ||
                (incoming.TabIndex ?? incoming.ZIndex) != current.TabIndex ||
                incoming.TabStop != current.TabStop ||
                (incoming.ReadOnly ?? false) != current.ReadOnly ||
                (incoming.Multiline ?? false) != current.Multiline ||
                incoming.Editable != current.Editable ||
                (incoming.IsDefault ?? false) != current.IsDefault ||
                (incoming.GroupStart ?? false) != current.GroupStart ||
                (incoming.AdapterId ?? string.Empty) != current.AdapterId ||
                (incoming.PageId ?? string.Empty) != current.PageId ||
                (incoming.SemanticKey ?? string.Empty) != current.SemanticKey ||
                (incoming.SourceKind ?? string.Empty) != current.SourceKind ||
                (incoming.PresentationVariant ?? string.Empty) != current.PresentationVariant ||
                !(incoming.SupportedActions ?? []).SequenceEqual(current.SupportedActions) ||
                (incoming.HelpText ?? string.Empty) != current.HelpText ||
                (incoming.AccessKey ?? string.Empty) != current.AccessKey)
            {
                return false;
            }
        }
        return true;
    }

    public void ApplySnapshot(
        WindowSnapshot snapshot,
        bool preserveTransient = false,
        string? eventId = null)
    {
        var merge = preserveTransient && CanMergeSnapshot(snapshot);
        SurfaceId = snapshot.SurfaceId;
        SurfaceKind = snapshot.SurfaceKind;
        Modal = snapshot.Modal;
        CanCancel = snapshot.CanCancel;
        Icon = snapshot.Icon;
        Generation = snapshot.Generation;
        Revision = ProtocolSerializer.ParseCanonicalUInt64(snapshot.Revision, "revision");
        NativeHwnd = snapshot.NativeHwnd;
        OwnerHwnd = snapshot.OwnerHwnd;
        Title = snapshot.Title;
        Dpi = snapshot.Dpi;
        WindowStyle = ParseHex64(snapshot.WindowStyle, "windowStyle");
        WindowExStyle = ParseHex64(snapshot.WindowExStyle, "windowExStyle");
        Bounds = snapshot.Bounds;
        ClientBounds = snapshot.ClientBounds;
        Visible = snapshot.Visible;
        Enabled = snapshot.Enabled;
        State = snapshot.State;
        ShowInTaskbar = snapshot.ShowInTaskbar;
        Rtl = snapshot.Rtl;
        AdapterId = snapshot.AdapterId ?? string.Empty;
        PageId = snapshot.PageId ?? string.Empty;
        var ordered = snapshot.Nodes.OrderBy(node => node.ZIndex).ToArray();
        if (merge)
        {
            for (var index = 0; index < Menu.Count; index++)
                Menu[index].ApplySnapshot(snapshot.Menu[index], true);
        }
        else
        {
            Menu.Clear();
            foreach (var item in snapshot.Menu) Menu.Add(MenuItemViewModel.FromSnapshot(item));
        }
        if (merge)
        {
            for (var index = 0; index < ordered.Length; index++)
                Nodes[index].ApplySnapshot(
                    ordered[index], preserveTransient: true, eventId: eventId);
        }
        else
        {
            Nodes.Clear();
            foreach (var node in ordered) Nodes.Add(ControlNodeViewModel.FromSnapshot(node));
        }
    }

    public void ApplyPatch(WindowPatchMessage patch)
    {
        var revision = ProtocolSerializer.ParseCanonicalUInt64(patch.Revision, "revision");
        if (patch.Snapshot is not null)
        {
            var fullResyncBaseRevision = ProtocolSerializer.ParseCanonicalUInt64(patch.BaseRevision, "baseRevision");
            if (patch.Snapshot.SurfaceId != SurfaceId ||
                ProtocolSerializer.ParseCanonicalUInt64(patch.Snapshot.Revision, "snapshot.revision") != revision ||
                fullResyncBaseRevision > revision || revision < Revision)
            {
                throw new ProtocolException("Full-resync snapshot identity or revision is stale or inconsistent.");
            }
            ApplySnapshot(patch.Snapshot, preserveTransient: true, patch.EventId);
            return;
        }

        var baseRevision = ProtocolSerializer.ParseCanonicalUInt64(patch.BaseRevision, "baseRevision");
        if (baseRevision != Revision || revision <= Revision)
        {
            throw new ProtocolException($"Patch revision {baseRevision}->{revision} does not follow canonical revision {Revision}.");
        }
        foreach (var operation in patch.Operations) ApplyOperation(operation);
        Revision = revision;
        RaisePropertyChanged(nameof(Revision));
    }

    public ControlNodeViewModel GetNode(string nodeId) =>
        Nodes.FirstOrDefault(node => node.NodeId == nodeId) ?? throw new ProtocolException($"Unknown node '{nodeId}'.");

    private static ulong ParseHex64(string value, string field)
    {
        if (value.Length is < 3 or > 18 || !value.StartsWith("0x", StringComparison.Ordinal) ||
            !ulong.TryParse(value.AsSpan(2), System.Globalization.NumberStyles.AllowHexSpecifier,
                System.Globalization.CultureInfo.InvariantCulture, out var parsed))
        {
            throw new ProtocolException($"{field} is not a canonical hexadecimal value.");
        }
        return parsed;
    }

    private void ApplyOperation(PatchOperation operation)
    {
        if (operation.NodeId is not null)
        {
            if (operation.Op == "remove" && operation.Property == "node")
            {
                Nodes.Remove(GetNode(operation.NodeId));
                return;
            }
            GetNode(operation.NodeId).ApplyCanonical(operation.Property, operation.Value, operation.EventId);
            return;
        }

        if (operation.Op == "add" && operation.Property == "node")
        {
            var node = operation.Value.Deserialize<ControlNode>() ?? throw new ProtocolException("Invalid added node.");
            Nodes.Add(ControlNodeViewModel.FromSnapshot(node));
            return;
        }
        if (operation.Op != "replace") throw new ProtocolException($"Unsupported window patch operation '{operation.Op}'.");
        switch (operation.Property)
        {
            case "title": Title = operation.Value.GetString() ?? string.Empty; break;
            case "bounds": Bounds = operation.Value.Deserialize<PixelRect>() ?? throw new ProtocolException("Invalid window bounds patch."); break;
            case "clientBounds": ClientBounds = operation.Value.Deserialize<PixelRect>() ?? throw new ProtocolException("Invalid client bounds patch."); break;
            case "visible": Visible = operation.Value.GetBoolean(); break;
            case "enabled": Enabled = operation.Value.GetBoolean(); break;
            case "state": State = operation.Value.GetString() ?? "normal"; break;
            case "icon": Icon = operation.Value.GetString() ?? "none"; RaisePropertyChanged(nameof(Icon)); break;
            default: throw new ProtocolException($"Unsupported window patch property '{operation.Property}'.");
        }
    }
}
