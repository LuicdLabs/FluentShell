using FluentShell.Renderer.Protocol;
using Microsoft.UI;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Media;

namespace FluentShell.Renderer.Windows;

/// <summary>
/// Draws a read-only rich edit control's paragraphs of formatted runs (minor 35) into a
/// RichTextBlock. Sizes, indents and spacing arrive in points; XAML measures in DIPs, which are
/// 1/96 inch, so a point is 96/72 of one.
/// </summary>
internal static class RichTextPresenter
{
    private const double DipsPerPoint = 96.0 / 72.0;
    // The width a bullet and its gap take when the native paragraph gives no first-line indent.
    private const string BulletPrefix = "• ";

    internal static void Apply(RichTextBlock block, RichTextSnapshot? snapshot, double defaultFontSize)
    {
        block.Blocks.Clear();
        if (snapshot is null) return;
        foreach (var source in snapshot.Paragraphs)
        {
            var paragraph = new Paragraph
            {
                TextAlignment = source.Alignment switch
                {
                    "center" => TextAlignment.Center,
                    "right" => TextAlignment.Right,
                    "justify" => TextAlignment.Justify,
                    _ => TextAlignment.Left,
                },
                Margin = new Thickness(Math.Max(0, source.LeftIndent * DipsPerPoint), source.SpaceBefore * DipsPerPoint,
                    0, source.SpaceAfter * DipsPerPoint),
                TextIndent = source.FirstLineIndent * DipsPerPoint,
            };
            if (source.Bullet && source.Runs.Count > 0)
                paragraph.Inlines.Add(Format(new Run { Text = BulletPrefix }, source.Runs[0] with { Bold = false, Underline = false, Strike = false }, defaultFontSize));
            foreach (var run in source.Runs)
            {
                // A vertical tab in the native text is a line break inside the paragraph.
                var lines = run.Text.Split('\n');
                for (var index = 0; index < lines.Length; ++index)
                {
                    if (index > 0) paragraph.Inlines.Add(new LineBreak());
                    if (lines[index].Length > 0)
                        paragraph.Inlines.Add(Format(new Run { Text = lines[index] }, run, defaultFontSize));
                }
            }
            // An empty paragraph is a blank line: it takes its height from a non-breaking space.
            if (paragraph.Inlines.Count == 0)
                paragraph.Inlines.Add(new Run { Text = " ", FontSize = defaultFontSize });
            block.Blocks.Add(paragraph);
        }
    }

    private static Run Format(Run target, RichRunSnapshot run, double defaultFontSize)
    {
        target.FontSize = run.Size > 0 ? run.Size * DipsPerPoint : defaultFontSize;
        if (run.Bold) target.FontWeight = FontWeights.Bold;
        if (run.Italic) target.FontStyle = global::Windows.UI.Text.FontStyle.Italic;
        var decorations = global::Windows.UI.Text.TextDecorations.None;
        if (run.Underline) decorations |= global::Windows.UI.Text.TextDecorations.Underline;
        if (run.Strike) decorations |= global::Windows.UI.Text.TextDecorations.Strikethrough;
        if (decorations != global::Windows.UI.Text.TextDecorations.None) target.TextDecorations = decorations;
        if (run.Color >= 0)
            target.Foreground = new SolidColorBrush(ColorHelper.FromArgb(0xFF,
                (byte)((run.Color >> 16) & 0xFF), (byte)((run.Color >> 8) & 0xFF), (byte)(run.Color & 0xFF)));
        return target;
    }
}
