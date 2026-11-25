using System.Collections.Specialized;
using System.Text.Json;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

public sealed class CanonicalPresentationUpdateTests
{
    private static ControlNode ListNode() => TestData.Snapshot().Nodes[0] with
    {
        Kind = "listView", Items = ["Before"], Columns = ["Name"], ColumnWidths = [120],
        Rows = [["Before"]], ItemNativeIds = ["100"],
    };

    [Fact]
    public void FullSnapshotRebuildsOnceFromTheCompleteTuple()
    {
        var source = ListNode();
        var node = ControlNodeViewModel.FromSnapshot(source);
        using var lifetime = new PresentationLifetime();
        var builds = 0;
        using var update = new CanonicalPresentationUpdate(node, lifetime, () =>
        {
            ++builds;
            Assert.Equal(["After", "Second"], node.Items);
            Assert.Equal(["200", "201"], node.ItemNativeIds);
            Assert.Equal(2, node.Rows.Count);
            Assert.Equal(["New name", "Detail"], node.Columns);
            Assert.Equal(["After", "New detail"], node.Rows[0]);
        });
        lifetime.Subscribe(node, (_, args) =>
        {
            if (args.PropertyName is nameof(node.Items) or nameof(node.Columns) or
                nameof(node.ColumnWidths) or nameof(node.Rows) or nameof(node.ItemNativeIds))
                update.Request();
        });

        var next = source with
        {
            Items = ["After", "Second"], ItemNativeIds = ["200", "201"],
            Columns = ["New name", "Detail"], ColumnWidths = [200, 150],
            Rows = [["After", "New detail"], ["Second", "Other detail"]],
        };
        node.ApplySnapshot(next, preserveTransient: true);
        Assert.Equal(1, builds);
        node.ApplySnapshot(next, preserveTransient: true);
        Assert.Equal(1, builds);
    }

    [Fact]
    public void FieldPatchRebuildsOnceAfterRevisionAndAllRowsCommit()
    {
        var model = WindowViewModel.FromSnapshot(TestData.Snapshot() with { Nodes = [ListNode()] });
        var node = model.GetNode("10");
        using var lifetime = new PresentationLifetime();
        var builds = 0;
        using var update = new CanonicalPresentationUpdate(node, lifetime, () =>
        {
            ++builds;
            Assert.Equal(8UL, model.Revision);
            Assert.Equal("After", node.Items.Single());
            Assert.Equal("After", node.Rows.Single().Single());
        });
        lifetime.Subscribe(node, (_, args) =>
        {
            if (args.PropertyName is nameof(node.Items) or nameof(node.Rows)) update.Request();
        });
        model.ApplyPatch(new WindowPatchMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = model.SurfaceId, BaseRevision = "7", Revision = "8",
            Operations =
            [
                new PatchOperation { Op = "replace", NodeId = "10", Property = "items",
                    Value = JsonSerializer.SerializeToElement(new[] { "After" }) },
                new PatchOperation { Op = "replace", NodeId = "10", Property = "rows",
                    Value = JsonSerializer.SerializeToElement(new[] { new[] { "After" } }) },
            ],
        });
        Assert.Equal(1, builds);
    }

    [Fact]
    public void CompletionFailureStillClosesEveryAffectedNodeTransaction()
    {
        var snapshot = TestData.Snapshot();
        var model = WindowViewModel.FromSnapshot(snapshot with
        {
            Nodes = [snapshot.Nodes[0], snapshot.Nodes[0] with { NodeId = "11", ZIndex = 1 }],
        });
        var first = model.GetNode("10");
        var second = model.GetNode("11");
        var expected = new InvalidOperationException("The first presenter failed.");
        first.CanonicalUpdateCompleted += () => throw expected;
        var secondCompleted = false;
        second.CanonicalUpdateCompleted += () => secondCompleted = true;
        var actual = Assert.Throws<InvalidOperationException>(() => model.ApplyPatch(TwoNodePatch(model, "text")));
        Assert.Same(expected, actual);
        Assert.True(secondCompleted);
        Assert.False(first.IsApplyingCanonicalUpdate);
        Assert.False(second.IsApplyingCanonicalUpdate);
        Assert.Equal("Updated", first.Text);
        Assert.Equal("Updated", second.Text);
        Assert.Equal(8UL, model.Revision);
    }

    [Fact]
    public void PatchFailureIsPreservedWhenCompletionAlsoFails()
    {
        var snapshot = TestData.Snapshot();
        var model = WindowViewModel.FromSnapshot(snapshot with
        {
            Nodes = [snapshot.Nodes[0], snapshot.Nodes[0] with { NodeId = "11", ZIndex = 1 }],
        });
        foreach (var node in model.Nodes)
            node.CanonicalUpdateCompleted += () => throw new InvalidOperationException("Completion failed.");
        var actual = Assert.Throws<ProtocolException>(() => model.ApplyPatch(TwoNodePatch(model, "unsupported")));
        Assert.Contains("Unsupported node patch property", actual.Message);
        Assert.All(model.Nodes, node => Assert.False(node.IsApplyingCanonicalUpdate));
        Assert.Equal(7UL, model.Revision);
    }

    private static WindowPatchMessage TwoNodePatch(WindowViewModel model, string firstProperty) => new()
    {
        SessionNonce = TestData.Nonce, SurfaceId = model.SurfaceId, BaseRevision = "7", Revision = "8",
        Operations =
        [
            new PatchOperation { Op = "replace", NodeId = "10", Property = firstProperty,
                Value = JsonSerializer.SerializeToElement("Updated") },
            new PatchOperation { Op = "replace", NodeId = "11", Property = "text",
                Value = JsonSerializer.SerializeToElement("Updated") },
        ],
    };

    [Theory]
    [InlineData(false, "After")]
    [InlineData(true, "Local draft")]
    public void SnapshotRestoresIntendedTextAfterItemsSourceWritesBack(bool preserveTransient, string expected)
    {
        var source = ListNode() with { Kind = "comboBox", Text = "Before", Editable = true };
        var node = ControlNodeViewModel.FromSnapshot(source);
        node.DraftText = "Local draft";
        node.Items.CollectionChanged += (_, _) => node.DraftText = string.Empty;
        node.ApplySnapshot(source with { Items = ["After"], Text = "After" }, preserveTransient);
        Assert.Equal(expected, node.DraftText);
        Assert.Equal("After", node.Text);
    }

    [Fact]
    public void ItemsFieldPatchKeepsTheDraftWhenItemsSourceWritesBack()
    {
        var node = ControlNodeViewModel.FromSnapshot(ListNode() with { Kind = "comboBox", Editable = true });
        node.DraftText = "Local draft";
        node.Items.CollectionChanged += (_, _) => node.DraftText = string.Empty;
        node.ApplyCanonical("items", JsonSerializer.SerializeToElement(new[] { "After" }), null);
        Assert.Equal("Local draft", node.DraftText);
    }

    [Fact]
    public void StructuralPatchCanAddThenUpdateItsNewNode()
    {
        var model = WindowViewModel.FromSnapshot(TestData.Snapshot());
        model.ApplyPatch(new WindowPatchMessage
        {
            SessionNonce = TestData.Nonce, SurfaceId = model.SurfaceId, BaseRevision = "7", Revision = "8",
            Operations =
            [
                new PatchOperation { Op = "add", Property = "node",
                    Value = JsonSerializer.SerializeToElement(TestData.Snapshot().Nodes[0] with { NodeId = "11", ZIndex = 1 }) },
                new PatchOperation { Op = "replace", NodeId = "11", Property = "text",
                    Value = JsonSerializer.SerializeToElement("Added and updated") },
            ],
        });
        Assert.Equal("Added and updated", model.GetNode("11").Text);
        Assert.Equal(8UL, model.Revision);
        Assert.False(model.GetNode("11").IsApplyingCanonicalUpdate);
    }

    [Fact]
    public void CollectionObserversSeeOneCompleteResetForThousandsOfItems()
    {
        var source = ListNode();
        var node = ControlNodeViewModel.FromSnapshot(source);
        var expected = Enumerable.Range(0, 4000).Select(index => "Item " + index).ToList();
        var resets = 0;
        node.Items.CollectionChanged += (_, args) =>
        {
            ++resets;
            Assert.Equal(NotifyCollectionChangedAction.Reset, args.Action);
            Assert.Equal(expected, node.Items);
        };
        node.ApplySnapshot(source with { Items = expected });
        Assert.Equal(1, resets);
        node.ApplySnapshot(source with { Items = expected });
        Assert.Equal(1, resets);
    }

    [Fact]
    public void NestedTransactionsAndRetirementDoNotPublishStaleWork()
    {
        var node = ControlNodeViewModel.FromSnapshot(ListNode());
        using var lifetime = new PresentationLifetime();
        var builds = 0;
        using var update = new CanonicalPresentationUpdate(node, lifetime, () => ++builds);
        using (node.BeginCanonicalUpdate())
        {
            using (node.BeginCanonicalUpdate()) update.Request();
            Assert.Equal(0, builds);
            update.Request();
        }
        Assert.Equal(1, builds);
        using (node.BeginCanonicalUpdate())
        {
            update.Request();
            lifetime.Dispose();
        }
        update.Request();
        Assert.Equal(1, builds);
        Assert.False(node.IsApplyingCanonicalUpdate);
    }

    [Fact]
    public void UnchangedRowsKeepTheirOwnedStorage()
    {
        var source = ListNode() with { ItemDetails = [["Detail"]] };
        var node = ControlNodeViewModel.FromSnapshot(source);
        var row = node.Rows.Single();
        var detail = node.ItemDetails.Single();
        node.ApplySnapshot(source);
        Assert.Same(row, node.Rows.Single());
        Assert.Same(detail, node.ItemDetails.Single());
    }
}
