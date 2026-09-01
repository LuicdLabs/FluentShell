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

    [Fact]
    public void StaticIconSnapshotUpdatesOwnedPixelPayload()
    {
        var icon = TestData.Snapshot().Nodes[0] with
        {
            Kind = "staticIcon",
            TabStop = false,
            TabIndex = -1,
            ImageWidth = 1,
            ImageHeight = 2,
            ImageFormat = "bgra8-premultiplied",
            ImageData = Convert.ToBase64String([0, 0, 0, 0, 0, 0, 0, 0]),
        };
        var viewModel = ControlNodeViewModel.FromSnapshot(icon);
        var changed = new List<string?>();
        viewModel.PropertyChanged += (_, args) => changed.Add(args.PropertyName);

        var pixels = Convert.ToBase64String([1, 2, 3, 4, 1, 2, 3, 4]);
        viewModel.ApplySnapshot(icon with { ImageData = pixels });

        Assert.Equal(pixels, viewModel.ImageData);
        Assert.Contains(nameof(viewModel.ImageData), changed);

        changed.Clear();
        viewModel.ApplySnapshot(icon with
        {
            ImageWidth = 2,
            ImageHeight = 1,
            ImageData = pixels,
        });
        Assert.Equal(2, viewModel.ImageWidth);
        Assert.Contains(nameof(viewModel.ImageData), changed);
    }

    [Fact]
    public void PeriodicSnapshotReconcilesNativeProgressState()
    {
        var progress = TestData.Snapshot().Nodes[0] with
        {
            Kind = "progressBar",
            TabIndex = -1,
            TabStop = false,
            Minimum = 10,
            Maximum = 90,
            Position = 25,
            Indeterminate = false,
        };
        var snapshot = TestData.Snapshot() with { Nodes = [progress] };
        var viewModel = WindowViewModel.FromSnapshot(snapshot);
        var node = viewModel.GetNode("10");

        viewModel.ApplyPatch(new WindowPatchMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = snapshot.SurfaceId,
            BaseRevision = "7",
            Revision = "8",
            Operations = [],
            Snapshot = snapshot with
            {
                Revision = "8",
                Nodes = [progress with
                    { Minimum = 100, Maximum = 120, Position = 110, Indeterminate = true }],
            },
        });

        Assert.Same(node, viewModel.GetNode("10"));
        Assert.Equal(100, node.Minimum);
        Assert.Equal(120, node.Maximum);
        Assert.Equal(110, node.Position);
        Assert.True(node.Indeterminate);
    }

    [Fact]
    public void EditableComboPreservesCanonicalTextSelectionAndItems()
    {
        var combo = TestData.Snapshot().Nodes[0] with
        {
            Kind = "comboBox",
            Editable = true,
            Text = "custom",
            SelectedIndex = 1,
            Items = ["one", "two"],
        };

        var node = ControlNodeViewModel.FromSnapshot(combo);

        Assert.True(node.Editable);
        Assert.Equal("custom", node.Text);
        Assert.Equal("custom", node.DraftText);
        Assert.Equal(1, node.SelectedIndex);
        Assert.Equal(["one", "two"], node.Items);
    }

    [Fact]
    public void EditableComboSelectionEchoReconcilesCanonicalText()
    {
        var original = TestData.Snapshot() with
        {
            Nodes =
            [
                TestData.Snapshot().Nodes[0] with
                {
                    Kind = "comboBox",
                    Editable = true,
                    Text = "typed",
                    SelectedIndex = -1,
                    Items = ["one", "two"],
                },
            ],
        };
        var viewModel = WindowViewModel.FromSnapshot(original);
        var node = viewModel.GetNode("10");
        node.DraftText = "two";
        node.RegisterPending("selectedIndex", "55");

        viewModel.ApplyPatch(new WindowPatchMessage
        {
            SessionNonce = TestData.Nonce,
            SurfaceId = original.SurfaceId,
            BaseRevision = "7",
            Revision = "8",
            EventId = "55",
            Operations = [],
            Snapshot = original with
            {
                Revision = "8",
                Nodes = [original.Nodes[0] with { Text = "two", SelectedIndex = 1 }],
            },
        });

        Assert.Equal("two", node.Text);
        Assert.Equal("two", node.DraftText);
        Assert.Equal(1, node.SelectedIndex);
        Assert.False(node.IsPendingEcho("selectedIndex", "55"));
    }

    [Fact]
    public void MapsBoundedSysLinkAndListViewFields()
    {
        var source = TestData.Snapshot().Nodes[0];
        var link = ControlNodeViewModel.FromSnapshot(source with
        {
            Kind = "sysLink",
            Text = "Open advanced settings.",
            Items = ["advanced settings"],
        });
        var list = ControlNodeViewModel.FromSnapshot(source with
        {
            Kind = "listView",
            Text = string.Empty,
            Items = ["Alpha", "Beta"],
            Columns = ["Name", "State"],
            ColumnWidths = [120, 80],
            Rows = [["Alpha", "Ready"], ["Beta", "Busy"]],
            SelectedIndices = [1],
            FocusedIndex = 1,
            MultiSelect = true,
            ColumnHeadersVisible = false,
            CheckBoxes = true,
            CheckedIndices = [0],
        });

        Assert.Equal("Open advanced settings.", link.Text);
        Assert.Equal("advanced settings", link.Items.Single());
        Assert.Equal(["Name", "State"], list.Columns);
        Assert.Equal([120, 80], list.ColumnWidths);
        Assert.Equal(["Alpha", "Ready"], list.Rows[0]);
        Assert.Equal([1], list.SelectedIndices);
        Assert.Equal(1, list.FocusedIndex);
        Assert.True(list.MultiSelect);
        Assert.False(list.ColumnHeadersVisible);
        Assert.True(list.CheckBoxes);
        Assert.Equal([0], list.CheckedIndices);
    }

    [Fact]
    public void CanonicalListViewPatchReconcilesRowsColumnsAndSelection()
    {
        var node = ControlNodeViewModel.FromSnapshot(TestData.Snapshot().Nodes[0] with
        {
            Kind = "listView",
            Columns = ["Old"],
            ColumnWidths = [80],
            Rows = [["One"], ["Two"]],
            SelectedIndices = [0],
            FocusedIndex = 0,
            ColumnHeadersVisible = true,
            CheckBoxes = true,
            CheckedIndices = [0],
        });
        node.RegisterPending("selectedIndices", "42");
        node.RegisterPending("checkedIndices", "43");

        node.ApplyCanonical("columns", JsonSerializer.SerializeToElement(new[] { "Name", "State" }), null);
        node.ApplyCanonical("columnWidths", JsonSerializer.SerializeToElement(new[] { 120, 80 }), null);
        node.ApplyCanonical("rows", JsonSerializer.SerializeToElement(new[]
        {
            new[] { "One", "Ready" },
            new[] { "Two", "Busy" },
        }), null);
        node.ApplyCanonical("multiSelect", JsonSerializer.SerializeToElement(true), null);
        node.ApplyCanonical("focusedIndex", JsonSerializer.SerializeToElement(1), null);
        node.ApplyCanonical("columnHeadersVisible", JsonSerializer.SerializeToElement(false), null);
        node.ApplyCanonical("checkBoxes", JsonSerializer.SerializeToElement(true), null);
        node.ApplyCanonical("checkedIndices", JsonSerializer.SerializeToElement(new[] { 1 }), "43");
        node.ApplyCanonical("selectedIndices", JsonSerializer.SerializeToElement(new[] { 0, 1 }), "42");

        Assert.Equal(["Name", "State"], node.Columns);
        Assert.Equal([120, 80], node.ColumnWidths);
        Assert.Equal(["Two", "Busy"], node.Rows[1]);
        Assert.Equal([0, 1], node.SelectedIndices);
        Assert.Equal(1, node.FocusedIndex);
        Assert.True(node.MultiSelect);
        Assert.False(node.ColumnHeadersVisible);
        Assert.True(node.CheckBoxes);
        Assert.Equal([1], node.CheckedIndices);
        Assert.False(node.IsPendingEcho("selectedIndices", "42"));
        Assert.False(node.IsPendingEcho("checkedIndices", "43"));
    }

    [Fact]
    public void RejectedListViewCheckActionRaisesCanonicalRollback()
    {
        var node = ControlNodeViewModel.FromSnapshot(TestData.Snapshot().Nodes[0] with
        {
            Kind = "listView",
            CheckBoxes = true,
            CheckedIndices = [0],
        });
        var changed = new List<string?>();
        node.PropertyChanged += (_, args) => changed.Add(args.PropertyName);
        node.RegisterPending("checkedIndices", "99");

        node.RejectPending("checkedIndices", "99");

        Assert.Equal([0], node.CheckedIndices);
        Assert.Contains(nameof(node.CheckedIndices), changed);
        Assert.False(node.IsPendingEcho("checkedIndices", "99"));
    }

    [Fact]
    public void TabControlSnapshotAndPatchReconcileGeometryAndSelection()
    {
        var snapshot = TestData.Snapshot().Nodes[0] with
        {
            Kind = "tabControl",
            Items = ["General", "Advanced"],
            ItemRects =
            [
                new PixelRect { X = 0, Y = 0, Width = 80, Height = 24 },
                new PixelRect { X = 80, Y = 0, Width = 100, Height = 24 },
            ],
            SelectedIndex = 0,
        };
        var node = ControlNodeViewModel.FromSnapshot(snapshot);

        node.ApplyCanonical("selectedIndex", JsonSerializer.SerializeToElement(1), null);
        node.ApplyCanonical("itemRects", JsonSerializer.SerializeToElement(new[]
        {
            new PixelRect { X = 0, Y = 0, Width = 90, Height = 24 },
            new PixelRect { X = 0, Y = 24, Width = 100, Height = 24 },
        }), null);

        Assert.Equal(1, node.SelectedIndex);
        Assert.Equal(24, node.ItemRects[1].Y);
    }

    [Fact]
    public void RejectedTabSelectionRaisesCanonicalRollback()
    {
        var node = ControlNodeViewModel.FromSnapshot(TestData.Snapshot().Nodes[0] with
        {
            Kind = "tabControl",
            Items = ["General", "Advanced"],
            ItemRects =
            [
                new PixelRect { X = 0, Y = 0, Width = 80, Height = 24 },
                new PixelRect { X = 80, Y = 0, Width = 100, Height = 24 },
            ],
            SelectedIndex = 0,
        });
        var changed = new List<string?>();
        node.PropertyChanged += (_, args) => changed.Add(args.PropertyName);
        node.RegisterPending("selectedIndex", "100");

        node.RejectPending("selectedIndex", "100");

        Assert.Equal(0, node.SelectedIndex);
        Assert.Contains(nameof(node.SelectedIndex), changed);
        Assert.False(node.IsPendingEcho("selectedIndex", "100"));
    }

    [Fact]
    public void ToolbarSnapshotAndPatchReplaceTypedItemsAtomically()
    {
        var first = new ToolbarItemSnapshot
        {
            Kind = "pushButton", CommandId = 100, Text = "Open", Enabled = true,
            Rect = new PixelRect { X = 0, Y = 0, Width = 30, Height = 30 },
            ImageWidth = 1, ImageHeight = 1, ImageFormat = "bgra8-premultiplied",
            ImageData = Convert.ToBase64String([0, 0, 0, 0]),
        };
        var node = ControlNodeViewModel.FromSnapshot(TestData.Snapshot().Nodes[0] with
        {
            Kind = "toolbar",
            ToolbarItems = [first],
        });
        var replacement = first with { CommandId = 101, Text = "Save", Enabled = false };

        node.ApplyCanonical("toolbarItems", JsonSerializer.SerializeToElement(new[] { replacement }), null);

        Assert.Single(node.ToolbarItems);
        Assert.Equal("Save", node.ToolbarItems[0].Text);
        Assert.False(node.ToolbarItems[0].Enabled);
    }
}
