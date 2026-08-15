using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.ViewModels;

public sealed class MenuItemViewModel : ObservableObject
{
    private string _text = string.Empty;
    private bool _enabled;
    private bool _checked;
    private bool _radio;
    private bool _isDefault;

    public string ItemId { get; private set; } = string.Empty;
    public string Kind { get; private set; } = string.Empty;
    public int CommandId { get; private set; }
    public string Text { get => _text; private set => SetProperty(ref _text, value); }
    public bool Enabled { get => _enabled; private set => SetProperty(ref _enabled, value); }
    public bool Checked { get => _checked; private set => SetProperty(ref _checked, value); }
    public bool Radio { get => _radio; private set => SetProperty(ref _radio, value); }
    public bool IsDefault { get => _isDefault; private set => SetProperty(ref _isDefault, value); }
    public List<MenuItemViewModel> Items { get; } = [];

    public static MenuItemViewModel FromSnapshot(MenuItemSnapshot snapshot)
    {
        var result = new MenuItemViewModel();
        result.ApplySnapshot(snapshot, false);
        return result;
    }

    public bool HasSameShape(MenuItemSnapshot snapshot) =>
        ItemId == snapshot.ItemId && Kind == snapshot.Kind && CommandId == snapshot.CommandId &&
        Radio == snapshot.Radio && (Checked || Radio) == (snapshot.Checked || snapshot.Radio) &&
        Items.Count == snapshot.Items.Count &&
        Items.Zip(snapshot.Items).All(pair => pair.First.HasSameShape(pair.Second));

    public void ApplySnapshot(MenuItemSnapshot snapshot, bool merge)
    {
        ItemId = snapshot.ItemId;
        Kind = snapshot.Kind;
        CommandId = snapshot.CommandId;
        Text = snapshot.Text;
        Enabled = snapshot.Enabled;
        Checked = snapshot.Checked;
        Radio = snapshot.Radio;
        IsDefault = snapshot.IsDefault;
        if (merge && HasSameShape(snapshot))
        {
            for (var index = 0; index < Items.Count; index++)
                Items[index].ApplySnapshot(snapshot.Items[index], true);
            return;
        }
        Items.Clear();
        Items.AddRange(snapshot.Items.Select(FromSnapshot));
    }
}
