using FluentShell.Renderer.Windows;
using Microsoft.UI.Windowing;

namespace FluentShell.Renderer.Tests;

public sealed class WindowStatePolicyTests
{
    [Theory]
    [InlineData(false, "normal", true)]
    [InlineData(false, "minimized", true)]
    [InlineData(true, "normal", true)]
    [InlineData(true, "minimized", false)]
    [InlineData(true, "maximized", false)]
    public void CanonicalBoundsAreNotAppliedToCommittedNonNormalWindows(
        bool committed,
        string state,
        bool expected) =>
        Assert.Equal(expected, TranslatedWindow.ShouldApplyCanonicalBounds(committed, state));

    [Theory]
    [InlineData("normal", OverlappedPresenterState.Restored, true)]
    [InlineData("normal", OverlappedPresenterState.Minimized, false)]
    [InlineData("normal", OverlappedPresenterState.Maximized, false)]
    [InlineData("minimized", OverlappedPresenterState.Restored, false)]
    [InlineData("maximized", OverlappedPresenterState.Restored, false)]
    public void GeometryActionsRequireRestoredPresenterAndCanonicalState(
        string state,
        OverlappedPresenterState presenterState,
        bool expected) =>
        Assert.Equal(expected, TranslatedWindow.ShouldEmitGeometry(state, presenterState));

    [Theory]
    [InlineData(false, false, false, false, true)]
    [InlineData(true, true, false, false, true)]
    [InlineData(true, false, true, false, true)]
    [InlineData(true, false, false, false, false)]
    public void EventlessBindingPatchesDoNotReapplyCanonicalPlacement(
        bool committed,
        bool placementChanged,
        bool placementActionPatch,
        bool localPlacementPending,
        bool expected) =>
        Assert.Equal(expected, TranslatedWindow.ShouldApplyCanonicalPlacement(
            committed, placementChanged, placementActionPatch, localPlacementPending));

    [Theory]
    [InlineData(true, false)]
    [InlineData(false, true)]
    public void CanonicalPlacementNeverFightsAPendingLocalGesture(
        bool placementChanged,
        bool placementActionPatch) =>
        Assert.False(TranslatedWindow.ShouldApplyCanonicalPlacement(
            committed: true, placementChanged, placementActionPatch,
            localPlacementPending: true));

    [Fact]
    public void UncommittedWindowsStillTakeCanonicalPlacementDuringAGesture() =>
        Assert.True(TranslatedWindow.ShouldApplyCanonicalPlacement(
            committed: false, placementChanged: false, placementActionPatch: false,
            localPlacementPending: true));
}
