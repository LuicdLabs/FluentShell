using System.Text.Json.Nodes;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.ViewModels;

namespace FluentShell.Renderer.Tests;

// Minor 35: a read-only rich edit control (a certificate's bold labels) is carried as
// paragraphs of runs, each run one character format.
public sealed class RichTextProtocolTests
{
    private static RichTextSnapshot Content() => new()
    {
        Paragraphs =
        [
            new RichParagraphSnapshot
            {
                SpaceAfter = 4,
                Runs =
                [
                    new RichRunSnapshot { Text = "Issued to: ", Bold = true, Size = 9 },
                    new RichRunSnapshot { Text = "Example", Size = 9, Color = 0x336699, Italic = true, Underline = true },
                ],
            },
            new RichParagraphSnapshot
            {
                Bullet = true, LeftIndent = 14, FirstLineIndent = -14, Alignment = "center",
                Runs = [new RichRunSnapshot { Text = "Proves your identity", Size = 9, Strike = true }],
            },
            new RichParagraphSnapshot(),
        ],
    };

    private static ControlNode Node(RichTextSnapshot? content, bool readOnly = true, string kind = "richText") =>
        TestData.Snapshot().Nodes[0] with { Kind = kind, ReadOnly = readOnly, Multiline = true, RichText = content };

    private static WindowSnapshot Snapshot(ControlNode node) => TestData.Snapshot() with { Nodes = [node] };

    private static WindowSnapshot RoundTrip(WindowSnapshot snapshot) =>
        Assert.IsType<WindowOpenMessage>(ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            ProtocolSerializer.Serialize(new WindowOpenMessage { SessionNonce = TestData.Nonce, Window = snapshot }))).Window;

    [Fact]
    public void FormattedRunsValidateAndSurviveTheWire()
    {
        ProtocolValidator.ValidateSnapshot(Snapshot(Node(Content())));
        var received = Assert.Single(RoundTrip(Snapshot(Node(Content()))).Nodes).RichText!;
        Assert.Equal(3, received.Paragraphs.Count);
        var first = received.Paragraphs[0].Runs;
        Assert.Equal("Issued to: ", first[0].Text);
        Assert.True(first[0].Bold);
        Assert.Equal(9, first[0].Size);
        Assert.Equal(-1, first[0].Color);
        Assert.Equal(0x336699, first[1].Color);
        Assert.True(first[1].Italic && first[1].Underline);
        Assert.Equal(4, received.Paragraphs[0].SpaceAfter);
        Assert.True(received.Paragraphs[1].Bullet);
        Assert.Equal("center", received.Paragraphs[1].Alignment);
        Assert.Equal(-14, received.Paragraphs[1].FirstLineIndent);
        Assert.True(received.Paragraphs[1].Runs[0].Strike);
        Assert.Empty(received.Paragraphs[2].Runs);
    }

    [Fact]
    public void ViewModelCarriesTheRunsAndAnnouncesAChange()
    {
        var model = ControlNodeViewModel.FromSnapshot(Node(Content()));
        Assert.Equal(3, model.RichText!.Paragraphs.Count);
        var changed = new List<string?>();
        model.PropertyChanged += (_, args) => changed.Add(args.PropertyName);
        var next = Content() with { Paragraphs = [Content().Paragraphs[0]] };
        model.ApplySnapshot(Node(next), preserveTransient: true);
        Assert.Single(model.RichText!.Paragraphs);
        Assert.Contains("RichText", changed);
    }

    [Fact]
    public void OnlyARichTextNodeCarriesIt()
    {
        Assert.Throws<ProtocolException>(() =>
            ProtocolValidator.ValidateSnapshot(Snapshot(Node(Content(), kind: "edit"))));
    }

    [Fact]
    public void ARichTextNodeMustBeReadOnlyAndHoldItsState()
    {
        Assert.Throws<ProtocolException>(() =>
            ProtocolValidator.ValidateSnapshot(Snapshot(Node(Content(), readOnly: false))));
        Assert.Throws<ProtocolException>(() =>
            ProtocolValidator.ValidateSnapshot(Snapshot(Node(null))));
    }

    public static TheoryData<string> Malformed() => new()
    {
        "empty text", "colour", "size", "alignment", "spacing", "runs", "paragraphs", "characters", "nul",
    };

    [Theory]
    [MemberData(nameof(Malformed))]
    public void OutOfBoundsContentIsRejected(string flaw)
    {
        var content = Content();
        var run = new RichRunSnapshot { Text = "x", Size = 9 };
        content = flaw switch
        {
            "empty text" => content with { Paragraphs = [new() { Runs = [run with { Text = "" }] }] },
            "colour" => content with { Paragraphs = [new() { Runs = [run with { Color = 0x1000000 }] }] },
            "size" => content with { Paragraphs = [new() { Runs = [run with { Size = 0 }] }] },
            "alignment" => content with { Paragraphs = [new() { Alignment = "diagonal", Runs = [run] }] },
            "spacing" => content with { Paragraphs = [new() { SpaceBefore = -1, Runs = [run] }] },
            "runs" => content with
            {
                Paragraphs = [new() { Runs = [.. Enumerable.Repeat(run, RichTextProtocol.MaxRunsPerParagraph + 1)] }],
            },
            "paragraphs" => content with
            {
                Paragraphs = [.. Enumerable.Repeat(new RichParagraphSnapshot(), RichTextProtocol.MaxParagraphs + 1)],
            },
            "characters" => content with
            {
                Paragraphs = [new() { Runs = [run with { Text = new string('x', RichTextProtocol.MaxCharacters) }, run] }],
            },
            _ => content with { Paragraphs = [new() { Runs = [run with { Text = "a\0b" }] }] },
        };
        Assert.Throws<ProtocolException>(() => ProtocolValidator.ValidateSnapshot(Snapshot(Node(content))));
    }

    [Theory]
    [InlineData("alignment")]
    [InlineData("runs")]
    [InlineData("color")]
    public void ARawFieldTheSerializerWouldDefaultIsStillRequired(string field)
    {
        var json = JsonNode.Parse(ProtocolSerializer.Serialize(new WindowOpenMessage
        {
            SessionNonce = TestData.Nonce, Window = Snapshot(Node(Content())),
        }))!;
        var paragraph = json["window"]!["nodes"]![0]!["richText"]!["paragraphs"]![0]!.AsObject();
        if (field == "color") paragraph["runs"]![0]!.AsObject().Remove(field);
        else paragraph.Remove(field);
        Assert.Throws<ProtocolException>(() => ProtocolSerializer.Deserialize(FrameMessageType.WindowOpen,
            System.Text.Encoding.UTF8.GetBytes(json.ToJsonString())));
    }
}
