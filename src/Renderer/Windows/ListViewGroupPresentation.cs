using FluentShell.Renderer.Protocol;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Automation.Provider;
using Microsoft.UI.Xaml.Controls;

namespace FluentShell.Renderer.Windows;

internal static class ListViewGroupPresentation
{
    internal static int[] VisibleOrder(IReadOnlyList<ListViewGroupSnapshot> groups, IReadOnlyList<string> membership) =>
        groups.Where(group => !group.Hidden && !group.Collapsed)
            .SelectMany(group => Enumerable.Range(0, membership.Count).Where(index => membership[index] == group.GroupId)).ToArray();

    internal static bool ItemVisible(IReadOnlyList<ListViewGroupSnapshot> groups, IReadOnlyList<string> membership, int index)
    {
        if (index < 0 || index >= membership.Count) return false;
        var group = groups.FirstOrDefault(entry => entry.GroupId == membership[index]);
        return group is { Hidden: false, Collapsed: false };
    }

    internal static TextAlignment Alignment(string value) => value switch
    {
        "center" => TextAlignment.Center, "right" => TextAlignment.Right, _ => TextAlignment.Left,
    };
}

// One real keyboard-operable group heading. State remains canonical until the
// native control acknowledges LVM_SETGROUPINFO and the next snapshot arrives.
internal sealed class ListViewGroupHeader : Button
{
    internal ListViewGroupSnapshot Group { get; }
    internal Action<bool> RequestExpanded { get; }

    internal ListViewGroupHeader(ListViewGroupSnapshot group, Action<bool> request)
    {
        DefaultStyleKey = typeof(Button);
        Group = group;
        RequestExpanded = request;
        MinWidth = MinHeight = 0;
        Padding = new Thickness(4, 0, 4, 0);
        HorizontalContentAlignment = HorizontalAlignment.Stretch;
        IsTabStop = group.Collapsible;
        Content = new TextBlock
        {
            Text = (group.Collapsible ? group.Collapsed ? "›  " : "⌄  " : string.Empty) + group.Header,
            TextAlignment = ListViewGroupPresentation.Alignment(group.HeaderAlignment),
            VerticalAlignment = VerticalAlignment.Center,
        };
        AutomationProperties.SetName(this, group.Header);
        AutomationProperties.SetAutomationId(this, "fls-group-" + group.GroupId);
        Click += (_, _) => { if (group.Collapsible) request(group.Collapsed); };
    }

    protected override AutomationPeer OnCreateAutomationPeer() => new ListViewGroupHeaderPeer(this);
}

internal sealed class ListViewGroupHeaderPeer(ListViewGroupHeader owner) : ButtonAutomationPeer(owner), IExpandCollapseProvider
{
    protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Group;
    protected override object GetPatternCore(PatternInterface patternInterface) =>
        patternInterface == PatternInterface.ExpandCollapse && owner.Group.Collapsible ? this :
        patternInterface == PatternInterface.Invoke && !owner.Group.Collapsible ? null! : base.GetPatternCore(patternInterface);
    public ExpandCollapseState ExpandCollapseState => !owner.Group.Collapsible ? ExpandCollapseState.LeafNode :
        owner.Group.Collapsed ? ExpandCollapseState.Collapsed : ExpandCollapseState.Expanded;
    public void Collapse() => Request(false);
    public void Expand() => Request(true);
    private void Request(bool expanded)
    {
        if (!owner.IsEnabled) throw new ElementNotEnabledException();
        if (owner.Group.Collapsible && owner.Group.Collapsed == expanded) owner.RequestExpanded(expanded);
    }
}
