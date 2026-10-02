using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.UI.Xaml.Controls.Primitives;

namespace FluentShell.Renderer.Windows;

// Native toolbar radio items are latched icon buttons. Keep the stock toggle
// template while exposing exclusive SelectionItem semantics to accessibility.
internal sealed class ToolbarRadioButton : ToggleButton
{
    internal Action? SelectRequested { get; init; }
    public ToolbarRadioButton()
    {
        DefaultStyleKey = typeof(ToggleButton);
        Click += (_, _) => SelectItem();
    }
    internal void SelectItem()
    {
        if (IsEnabled) SelectRequested?.Invoke();
    }
    protected override AutomationPeer OnCreateAutomationPeer() => new ToolbarRadioButtonPeer(this);
}

internal sealed class ToolbarRadioButtonPeer(ToolbarRadioButton owner) :
    ToggleButtonAutomationPeer(owner), ISelectionItemProvider
{
    protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.RadioButton;
    protected override object GetPatternCore(PatternInterface patternInterface) => patternInterface switch
    {
        PatternInterface.SelectionItem => this,
        PatternInterface.Toggle => null!,
        _ => base.GetPatternCore(patternInterface),
    };
    public bool IsSelected => owner.IsChecked == true;
    public IRawElementProviderSimple SelectionContainer => null!;
    public void Select()
    {
        if (!owner.IsEnabled) throw new ElementNotEnabledException();
        owner.SelectItem();
    }
    public void AddToSelection() => Select();
    public void RemoveFromSelection() => throw new InvalidOperationException("A toolbar radio item is replaced by selecting another item.");
}
