using FluentShell.Renderer.Protocol;

namespace FluentShell.Renderer.Windows;

internal static class NativeDocumentPresentation
{
    // Samples, labels and link handlers are live state. They do not replace the
    // controls that own focus and an in-progress scroll gesture.
    internal static bool SameStructure(NativeDocumentSnapshot? before, NativeDocumentSnapshot after)
    {
        if (before is null || before.DocumentId != after.DocumentId ||
            before.Regions.Count != after.Regions.Count || before.Items.Count != after.Items.Count) return false;
        for (var index = 0; index < before.Regions.Count; ++index)
        {
            var left = before.Regions[index];
            var right = after.Regions[index];
            if (left.Key != right.Key || left.Kind != right.Kind || left.Rect != right.Rect ||
                left.ContentWidth != right.ContentWidth || left.ContentHeight != right.ContentHeight) return false;
        }
        for (var index = 0; index < before.Items.Count; ++index)
        {
            var left = before.Items[index];
            var right = after.Items[index];
            if (left.Key != right.Key || left.Kind != right.Kind || left.Rect != right.Rect ||
                left.RegionIndex != right.RegionIndex || left.ReportRegionIndex != right.ReportRegionIndex ||
                (left.Image is null) != (right.Image is null) ||
                !left.ReportRows.Select(row => row.Path).SequenceEqual(right.ReportRows.Select(row => row.Path))) return false;
        }
        return true;
    }

    internal static int NativeOffset(double offset, double extent, int maximum) =>
        extent <= 0 || maximum <= 0 ? 0 : Math.Clamp((int)Math.Round(offset / extent * maximum), 0, maximum);

    internal static bool AllowsLocalOverflow(NativeDocumentRegion region) => region.Kind == "report";
}
