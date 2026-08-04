using System.Text.Json;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;

namespace FluentShell.Renderer.Tests;

public sealed class ViewModelTests
{
    [Fact]
    public void CanonicalPatchCommitsDraftAndSuppressesMatchingEcho()
    {
        var viewModel = WindowViewModel.FromSnapshot(TestData.Snapshot());
        var node = viewModel.GetNode("10");
        node.DraftText = "Local draft";
        node.RegisterPending("text", "55");

        viewModel.ApplyPatch(new WindowPatchMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = viewModel.SurfaceId,
            BaseRevision = "7",
            Revision = "8",
            Operations =
            [
                new PatchOperation
                {
                    Op = "replace",
                    NodeId = "10",
                    Property = "text",
                    Value = JsonSerializer.SerializeToElement("Native commit"),
                    EventId = "55",
                }
            ],
        });

        Assert.Equal(8UL, viewModel.Revision);
        Assert.Equal("Native commit", node.Text);
        Assert.Equal("Native commit", node.DraftText);
        Assert.False(node.IsPendingEcho("text", "55"));
    }

    [Fact]
    public void RejectsNonContiguousRevisionWithoutSnapshot()
    {
        var viewModel = WindowViewModel.FromSnapshot(TestData.Snapshot());
        var patch = new WindowPatchMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = viewModel.SurfaceId,
            BaseRevision = "6",
            Revision = "8",
            Operations = [],
        };
        Assert.Throws<ProtocolException>(() => viewModel.ApplyPatch(patch));
    }

    [Fact]
    public void FullSnapshotResynchronizesRegardlessOfBaseRevision()
    {
        var viewModel = WindowViewModel.FromSnapshot(TestData.Snapshot());
        var patch = new WindowPatchMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = viewModel.SurfaceId,
            BaseRevision = "1",
            Revision = "20",
            Operations = [],
            Snapshot = TestData.Snapshot("20", "Resynchronized"),
        };
        viewModel.ApplyPatch(patch);
        Assert.Equal(20UL, viewModel.Revision);
        Assert.Equal("Resynchronized", viewModel.GetNode("10").Text);
    }

    [Fact]
    public void MergeableFullSnapshotPreservesDraftAndNodeIdentity()
    {
        var viewModel = WindowViewModel.FromSnapshot(TestData.Snapshot());
        var node = viewModel.GetNode("10");
        node.DraftText = "Local draft";
        var patch = new WindowPatchMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = viewModel.SurfaceId,
            BaseRevision = "7",
            Revision = "8",
            Operations = [],
            Snapshot = TestData.Snapshot("8", "Native timer update"),
        };

        viewModel.ApplyPatch(patch);

        Assert.Same(node, viewModel.GetNode("10"));
        Assert.Equal("Native timer update", node.Text);
        Assert.Equal("Local draft", node.DraftText);
    }

    [Fact]
    public void MatchingFullSnapshotEchoCommitsCanonicalText()
    {
        var viewModel = WindowViewModel.FromSnapshot(TestData.Snapshot());
        var node = viewModel.GetNode("10");
        node.DraftText = "Local draft";
        node.RegisterPending("text", "55");

        viewModel.ApplyPatch(new WindowPatchMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = viewModel.SurfaceId,
            BaseRevision = "7",
            Revision = "8",
            EventId = "55",
            Operations = [],
            Snapshot = TestData.Snapshot("8", "Canonicalized text"),
        });

        Assert.Equal("Canonicalized text", node.Text);
        Assert.Equal("Canonicalized text", node.DraftText);
        Assert.False(node.IsPendingEcho("text", "55"));
    }

    [Fact]
    public void OlderFullSnapshotCannotRollBackCanonicalState()
    {
        var viewModel = WindowViewModel.FromSnapshot(TestData.Snapshot("8", "Current"));
        var patch = new WindowPatchMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = viewModel.SurfaceId,
            BaseRevision = "8",
            Revision = "7",
            Operations = [],
            Snapshot = TestData.Snapshot("7", "Older"),
        };

        Assert.Throws<ProtocolException>(() => viewModel.ApplyPatch(patch));
        Assert.Equal(8UL, viewModel.Revision);
        Assert.Equal("Current", viewModel.GetNode("10").Text);
    }

    [Fact]
    public void SameRevisionFullSnapshotCanResynchronizeCanonicalState()
    {
        var viewModel = WindowViewModel.FromSnapshot(TestData.Snapshot("8", "Current"));
        var patch = new WindowPatchMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = viewModel.SurfaceId,
            BaseRevision = "8",
            Revision = "8",
            Operations = [],
            Snapshot = TestData.Snapshot("8", "Resent"),
        };

        viewModel.ApplyPatch(patch);

        Assert.Equal("Resent", viewModel.GetNode("10").Text);
    }

    [Fact]
    public void OlderOperationEchoDoesNotOverwriteNewerDraft()
    {
        var viewModel = WindowViewModel.FromSnapshot(TestData.Snapshot());
        var node = viewModel.GetNode("10");
        node.DraftText = "Newest draft";
        node.RegisterPending("text", "56");

        viewModel.ApplyPatch(new WindowPatchMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = viewModel.SurfaceId,
            BaseRevision = "7",
            Revision = "8",
            Operations =
            [
                new PatchOperation
                {
                    Op = "replace",
                    NodeId = "10",
                    Property = "text",
                    Value = JsonSerializer.SerializeToElement("Earlier commit"),
                    EventId = "55",
                }
            ],
        });

        Assert.Equal("Earlier commit", node.Text);
        Assert.Equal("Newest draft", node.DraftText);
        Assert.True(node.IsPendingEcho("text", "56"));
    }

    [Fact]
    public void DpiChangeRequiresStructuralRebuild()
    {
        var viewModel = WindowViewModel.FromSnapshot(TestData.Snapshot());
        var changed = TestData.Snapshot() with { Dpi = 192 };

        Assert.False(viewModel.CanMergeSnapshot(changed));
    }

    [Fact]
    public void ParsesNativePresenterCapabilities()
    {
        var viewModel = WindowViewModel.FromSnapshot(TestData.Snapshot());

        Assert.True(viewModel.IsResizable);
        Assert.True(viewModel.IsMinimizable);
        Assert.True(viewModel.IsMaximizable);
        Assert.False(viewModel.IsAlwaysOnTop);
    }

    [Fact]
    public void NativeEditSelectionRaisesBindableProperties()
    {
        var node = ControlNodeViewModel.FromSnapshot(TestData.Snapshot().Nodes[0]);
        var changed = new List<string?>();
        node.PropertyChanged += (_, args) => changed.Add(args.PropertyName);

        node.ApplyCanonical("selectionStart", JsonSerializer.SerializeToElement(2), null);
        node.ApplyCanonical("selectionLength", JsonSerializer.SerializeToElement(3), null);

        Assert.Equal(2, node.SelectionStart);
        Assert.Equal(3, node.SelectionLength);
        Assert.Contains(nameof(node.SelectionStart), changed);
        Assert.Contains(nameof(node.SelectionLength), changed);
    }

    [Fact]
    public void UsesNativeTabIndexInsteadOfVisualZIndex()
    {
        var snapshot = TestData.Snapshot() with
        {
            Nodes =
            [
                TestData.Snapshot().Nodes[0] with { ZIndex = 7, TabIndex = 2 },
            ],
        };

        var node = WindowViewModel.FromSnapshot(snapshot).GetNode("10");

        Assert.Equal(7, node.ZIndex);
        Assert.Equal(2, node.TabIndex);
    }

    [Fact]
    public void MissingAdditiveTabIndexFallsBackToVisualOrder()
    {
        var snapshot = TestData.Snapshot() with
        {
            Nodes =
            [
                TestData.Snapshot().Nodes[0] with { ZIndex = 4, TabIndex = null },
            ],
        };

        Assert.Equal(4, WindowViewModel.FromSnapshot(snapshot).GetNode("10").TabIndex);
    }
}
