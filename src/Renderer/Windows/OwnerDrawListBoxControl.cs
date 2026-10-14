using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;

namespace FluentShell.Renderer.Windows;

// Minor 41: an owner-draw ListBox (the Mouse applet's pointer list).  Selection, keyboard and
// scrolling stay a real ListBox; each row shows the face the native owner painted for it, and a
// selected row its selected face once the native list reports that selection.
internal sealed class OwnerDrawListBoxControl : ListBox
{
    private readonly List<OwnerDrawComboItemPresentation> _rows = [];
    private OwnerDrawListSnapshot? _painted;
    private double _scale = 1;

    internal OwnerDrawListBoxControl()
    {
        DefaultStyleKey = typeof(ListBox);
    }

    internal void ApplyNativeFaces(ControlNodeViewModel node, double scale)
    {
        _painted = node.OwnerDrawList;
        _scale = scale;
        if (_painted is null) return;
        if (Application.Current?.Resources.TryGetValue("OwnerDrawComboItemTemplate", out var template) == true)
            ItemTemplate = (DataTemplate)template;
        if (_rows.Count != node.Items.Count || !_rows.Select(row => row.Label).SequenceEqual(node.Items))
        {
            _rows.Clear();
            _rows.AddRange(node.Items.Select(label => new OwnerDrawComboItemPresentation(label)));
            ItemsSource = _rows.ToArray();
        }
        for (var index = 0; index < _rows.Count && index < _painted.Items.Count; ++index)
            _rows[index].Apply(FaceFor(_painted, index), _scale);
    }

    // The native face the row shows now: its selected face while the native list has it selected.
    internal static ImageListEntry FaceFor(OwnerDrawListSnapshot painted, int index) =>
        painted.Items[index].Selected ?? painted.Items[index].Normal;

    protected override void PrepareContainerForItemOverride(DependencyObject element, object item)
    {
        base.PrepareContainerForItemOverride(element, item);
        if (element is ListBoxItem container && item is OwnerDrawComboItemPresentation row)
        {
            AutomationProperties.SetName(container, row.Label);
            container.MinHeight = 0;
            container.Padding = new Thickness(0);
            container.HorizontalContentAlignment = HorizontalAlignment.Stretch;
        }
    }
}
