using System.Collections.ObjectModel;
using System.Collections.Specialized;
using System.ComponentModel;

namespace FluentShell.Renderer.ViewModels;

// ItemsSource listeners receive the complete canonical collection once. A reset
// followed by one Add per item repeatedly realizes and lays out the same list.
internal sealed class SnapshotCollection<T> : ObservableCollection<T>
{
    internal void ReplaceWith(IEnumerable<T> items)
    {
        var replacement = items.ToArray();
        CheckReentrancy();
        Items.Clear();
        foreach (var item in replacement) Items.Add(item);
        OnPropertyChanged(new PropertyChangedEventArgs(nameof(Count)));
        OnPropertyChanged(new PropertyChangedEventArgs("Item[]"));
        OnCollectionChanged(new NotifyCollectionChangedEventArgs(NotifyCollectionChangedAction.Reset));
    }
}
