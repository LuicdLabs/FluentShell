using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;

namespace FluentShell.Renderer.Tests;

public sealed class ToolbarCanonicalStateTests
{
    private static ControlNode Toolbar() => TestData.Snapshot().Nodes[0] with
    {
        Kind = "toolbar",
        ToolbarItems =
        [
            new() { Kind = "radioButton", RadioGroup = 1, CommandId = 1, Checked = true, Text = "Icons" },
            new() { Kind = "radioButton", RadioGroup = 1, CommandId = 2, Checked = false, Text = "Details" },
        ],
    };

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void CompletedCommandRestoresCanonicalCheckedFlagsEvenWithoutStateChange(bool accepted)
    {
        var model = ControlNodeViewModel.FromSnapshot(Toolbar());
        bool[] projectedChecks = [false, true]; // Local click awaiting the native result.
        var refreshes = 0;
        model.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName != nameof(model.ToolbarItems)) return;
            ++refreshes;
            projectedChecks = model.ToolbarItems.Select(item => item.Checked == true).ToArray();
        };
        model.RegisterPending("toolbarCommand", "55");
        if (accepted) model.AcceptPending("toolbarCommand", "55");
        else model.RejectPending("toolbarCommand", "55");
        Assert.Equal([true, false], projectedChecks);
        Assert.Equal(1, refreshes);
        Assert.False(model.IsPendingEcho("toolbarCommand", "55"));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void MatchingSnapshotEchoRefreshesToolbarExactlyOnce(bool nativeChanged)
    {
        var source = Toolbar();
        var model = ControlNodeViewModel.FromSnapshot(source);
        var incoming = nativeChanged ? source with
        {
            ToolbarItems =
            [
                source.ToolbarItems![0] with { Checked = false },
                source.ToolbarItems![1] with { Checked = true },
            ],
        } : source;
        var refreshes = 0;
        model.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(model.ToolbarItems)) ++refreshes;
        };
        model.RegisterPending("toolbarCommand", "55");
        model.ApplySnapshot(incoming, preserveTransient: true, eventId: "55");
        model.AcceptPending("toolbarCommand", "55");
        Assert.Equal(1, refreshes);
        Assert.Equal(!nativeChanged, model.ToolbarItems[0].Checked);
        Assert.Equal(nativeChanged, model.ToolbarItems[1].Checked);
        Assert.False(model.IsPendingEcho("toolbarCommand", "55"));
    }

    [Fact]
    public void UnrelatedCompletionDoesNotResetAnotherPendingToolbarClick()
    {
        var source = Toolbar();
        var model = ControlNodeViewModel.FromSnapshot(source);
        var refreshes = 0;
        model.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(model.ToolbarItems)) ++refreshes;
        };
        model.RegisterPending("toolbarCommand", "56");
        model.ApplySnapshot(source, preserveTransient: true, eventId: "55");
        model.AcceptPending("toolbarCommand", "55");
        model.RejectPending("toolbarCommand", "55");
        Assert.Equal(0, refreshes);
        Assert.True(model.IsPendingEcho("toolbarCommand", "56"));
    }
}
