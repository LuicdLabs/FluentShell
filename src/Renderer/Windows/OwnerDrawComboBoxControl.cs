using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Data;
using Microsoft.UI.Xaml.Media;

namespace FluentShell.Renderer.Windows;

// The selection, keyboard search, popup, and editor remain a real ComboBox. Only
// its item template is supplied by the native owner's bounded WM_DRAWITEM faces.
internal sealed class OwnerDrawComboBoxControl : ComboBox
{
    private readonly List<OwnerDrawComboItemPresentation> _faces = [];
    private OwnerDrawComboSnapshot? _painted;
    private int _canonicalSelection = -1;
    private double _scale = 1;
    private bool _emptyFaceAssigned;

    internal OwnerDrawComboBoxControl()
    {
        DefaultStyleKey = typeof(ComboBox);
        SelectionChanged += (_, _) => RefreshFaces();
        DropDownOpened += (_, _) => RefreshFaces();
        DropDownClosed += (_, _) => RefreshFaces();
    }

    internal void ApplyNativeFaces(ControlNodeViewModel node, double scale)
    {
        _painted = node.OwnerDrawCombo;
        _canonicalSelection = node.SelectedIndex;
        _scale = scale;
        if (_painted is null)
        {
            ItemTemplate = null;
            ItemsSource = node.Items;
            return;
        }
        if (Application.Current?.Resources.TryGetValue("OwnerDrawComboItemTemplate", out var template) == true)
            ItemTemplate = (DataTemplate)template;
        if (_faces.Count != node.Items.Count ||
            !_faces.Select(face => face.Label).SequenceEqual(node.Items))
        {
            _faces.Clear();
            _faces.AddRange(node.Items.Select(label => new OwnerDrawComboItemPresentation(label)));
            ItemsSource = _faces.ToArray();
        }
        SelectedIndex = node.SelectedIndex;
        RefreshFaces();
    }

    internal void ApplyCanonicalSelection(int selectedIndex)
    {
        _canonicalSelection = selectedIndex;
        SelectedIndex = selectedIndex;
        RefreshFaces();
    }

    internal static ImageListEntry FaceFor(OwnerDrawComboSnapshot painted, int itemIndex,
        int selectedIndex, int canonicalSelection, bool dropDownOpen)
    {
        if (!dropDownOpen && itemIndex == selectedIndex && selectedIndex == canonicalSelection &&
            painted.SelectionFace is { } closed) return closed;
        var item = painted.Items[itemIndex];
        return itemIndex == selectedIndex ? item.Selected : item.Normal;
    }

    protected override void PrepareContainerForItemOverride(DependencyObject element, object item)
    {
        base.PrepareContainerForItemOverride(element, item);
        if (element is ComboBoxItem container && item is OwnerDrawComboItemPresentation face)
        {
            AutomationProperties.SetName(container, face.Label);
            container.MinHeight = 0;
            container.Padding = new Thickness(0);
            container.HorizontalContentAlignment = HorizontalAlignment.Stretch;
        }
    }

    protected override void OnApplyTemplate()
    {
        base.OnApplyTemplate();
        RefreshFaces();
    }

    private void RefreshFaces()
    {
        if (_emptyFaceAssigned && (SelectedIndex != -1 || _painted is null) &&
            GetTemplateChild("ContentPresenter") is ContentPresenter previous)
        {
            previous.ClearValue(ContentPresenter.ContentProperty);
            previous.ClearValue(ContentPresenter.ContentTemplateProperty);
            _emptyFaceAssigned = false;
        }
        if (_painted is null || _faces.Count != _painted.Items.Count) return;
        for (var index = 0; index < _faces.Count; ++index)
            _faces[index].Apply(FaceFor(_painted, index, SelectedIndex,
                _canonicalSelection, IsDropDownOpen), _scale);
        // With no selected item the standard template has no item to present.
        // WM_DRAWITEM still receives UINT(-1) for this field; retain that face.
        if (SelectedIndex == -1 && _painted.SelectionFace is { } empty &&
            GetTemplateChild("ContentPresenter") is ContentPresenter presenter)
        {
            presenter.ContentTemplate = null;
            presenter.Content = new Image
            {
                Source = ControlFactory.BitmapForImageListEntry(empty),
                Width = empty.ImageWidth * _scale,
                Height = empty.ImageHeight * _scale,
                Stretch = Stretch.Fill,
                IsHitTestVisible = false,
            };
            _emptyFaceAssigned = true;
        }
    }
}

[Bindable]
public sealed class OwnerDrawComboItemPresentation(string label) : ObservableObject
{
    private ImageListEntry? _image;
    private ImageSource? _source;
    private double _width;
    private double _height;
    public string Label { get; } = label;
    public ImageSource? Source { get => _source; private set => SetProperty(ref _source, value); }
    public double Width { get => _width; private set => SetProperty(ref _width, value); }
    public double Height { get => _height; private set => SetProperty(ref _height, value); }
    // ComboBox text search and its editable selection text use this native label,
    // even when two different native items have identical displayed strings.
    public override string ToString() => Label;
    internal void Apply(ImageListEntry image, double scale)
    {
        if (_image != image)
        {
            _image = image;
            Source = ControlFactory.BitmapForImageListEntry(image);
        }
        Width = image.ImageWidth * scale;
        Height = image.ImageHeight * scale;
    }
}
