using System.Xml.Linq;
using FluentShell.Renderer.Windows;

namespace FluentShell.Renderer.Tests;

/// <summary>
/// The compact check box and radio button templates keep WinUI's motion: the check box drives
/// WinUI's own animated tick through every combined state the control goes to, and the radio
/// button's dot animates only between states, never on load.
/// </summary>
public sealed class CompactControlStyleTests
{
    private static readonly XNamespace Presentation = "http://schemas.microsoft.com/winfx/2006/xaml/presentation";
    private static readonly XNamespace Xaml = "http://schemas.microsoft.com/winfx/2006/xaml";

    private static XElement Parse(string xaml) => XElement.Parse(xaml);

    private static IEnumerable<XElement> States(XElement style, string group) =>
        style.Descendants(Presentation + "VisualStateGroup")
            .Single(element => (string?)element.Attribute(Xaml + "Name") == group)
            .Elements(Presentation + "VisualState");

    private static string? SetterValue(XElement state, string target) =>
        state.Descendants(Presentation + "Setter")
            .FirstOrDefault(setter => (string?)setter.Attribute("Target") == target)
            ?.Attribute("Value")?.Value;

    [Fact]
    public void TheCheckBoxDrivesTheAnimatedTickThroughEveryCombinedState()
    {
        var style = Parse(CompactControlStyles.CheckBoxXaml);
        // WinUI's check box goes to exactly these states; its stock template has no other group.
        var expected = new Dictionary<string, string>
        {
            ["UncheckedNormal"] = "NormalOff",
            ["UncheckedPointerOver"] = "PointerOverOff",
            ["UncheckedPressed"] = "PressedOff",
            ["UncheckedDisabled"] = "NormalOff",
            ["CheckedNormal"] = "NormalOn",
            ["CheckedPointerOver"] = "PointerOverOn",
            ["CheckedPressed"] = "PressedOn",
            ["CheckedDisabled"] = "NormalOn",
            ["IndeterminateNormal"] = "NormalIndeterminate",
            ["IndeterminatePointerOver"] = "PointerOverIndeterminate",
            ["IndeterminatePressed"] = "PressedIndeterminate",
            ["IndeterminateDisabled"] = "NormalIndeterminate",
        };
        var states = States(style, "CombinedStates").ToDictionary(
            state => (string)state.Attribute(Xaml + "Name")!,
            state => SetterValue(state, "CheckGlyph.(AnimatedIcon.State)"));
        Assert.Equal(expected.Keys.OrderBy(key => key), states.Keys.OrderBy(key => key));
        foreach (var (name, iconState) in expected) Assert.Equal(iconState, states[name]);

        var icon = style.Descendants(Presentation + "AnimatedIcon").Single();
        Assert.Equal("CheckGlyph", (string?)icon.Attribute(Xaml + "Name"));
        Assert.Single(icon.Elements(), element => element.Name.LocalName == "AnimatedAcceptVisualSource");
        // A native control is 13 to 16 px tall: the box and the tick stay at 16 px.
        Assert.Equal("16", (string?)icon.Attribute("Width"));
        Assert.Equal("16", (string?)style.Descendants(Presentation + "Border")
            .Single(border => (string?)border.Attribute(Xaml + "Name") == "Box").Attribute("Height"));
    }

    [Fact]
    public void TheRadioButtonDotAnimatesOnlyBetweenStates()
    {
        var style = Parse(CompactControlStyles.RadioButtonXaml);
        var group = style.Descendants(Presentation + "VisualStateGroup")
            .Single(element => (string?)element.Attribute(Xaml + "Name") == "CheckStates");
        var transitions = group.Descendants(Presentation + "VisualTransition").ToList();
        Assert.Contains(transitions, transition =>
            (string?)transition.Attribute("From") == "Unchecked" && (string?)transition.Attribute("To") == "Checked" &&
            transition.Attribute("GeneratedDuration") is not null);
        // The states themselves jump: a control created already checked shows its dot at once.
        foreach (var animation in group.Elements(Presentation + "VisualState").Descendants(Presentation + "DoubleAnimation"))
        {
            Assert.Equal("DotScale", (string?)animation.Attribute("Storyboard.TargetName"));
            Assert.Equal("0", (string?)animation.Attribute("Duration"));
        }
        var checkedScale = States(style, "CheckStates")
            .Single(state => (string?)state.Attribute(Xaml + "Name") == "Checked")
            .Descendants(Presentation + "DoubleAnimation")
            .Select(animation => (string?)animation.Attribute("To"));
        Assert.All(checkedScale, value => Assert.Equal("1", value));
    }
}
