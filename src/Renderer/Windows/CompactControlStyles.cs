using System.Text.RegularExpressions;
using FluentShell.Renderer.Runtime;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;

namespace FluentShell.Renderer.Windows;

// WinUI's check box and radio button are laid out for a 32 px row: the glyph sits in a 32 px cell, so a
// native control 13 to 16 px tall (a dialog's usual) clips it -- a radio button showed as an arc. These
// templates keep each control's states and look but size the glyph to the native control: 16 px, centred
// on the same line as the label. A style that cannot be built, or that names a theme resource this
// WinUI does not have (which would fail later, inside layout), is not used: the control keeps WinUI's own.
internal static partial class CompactControlStyles
{
    // The gap between the glyph and the label. A native control is as wide as its label needs plus a few
    // pixels, so a wider gap clips the last characters of a tight label.
    internal static readonly Thickness LabelPadding = new(4, 0, 0, 0);

    private static Style? _radioButton;
    private static bool _radioButtonTried;
    private static Style? _checkBox;
    private static bool _checkBoxTried;

    // The animated templates move the way WinUI's own controls do: the check box draws its
    // tick with WinUI's AnimatedIcon (AnimatedAcceptVisualSource, the stock check box's own
    // visual) and the radio button's dot grows in.  The static templates are kept as the
    // fallback for a WinUI that cannot load the animated ones.
    internal static Style? RadioButton
    {
        get
        {
            if (!_radioButtonTried)
            {
                _radioButtonTried = true;
                _radioButton = Create(RadioButtonXaml, "RadioButton") ?? Create(StaticRadioButtonXaml, "RadioButton (static)");
            }
            return _radioButton;
        }
    }

    internal static Style? CheckBox
    {
        get
        {
            if (!_checkBoxTried)
            {
                _checkBoxTried = true;
                _checkBox = Create(CheckBoxXaml, "CheckBox") ?? Create(StaticCheckBoxXaml, "CheckBox (static)");
            }
            return _checkBox;
        }
    }

    // A native check box or radio button wraps its label only when it is BS_MULTILINE, and the capture does
    // not carry that bit; the rectangle says it: a control with room for two lines wraps, a one-line one
    // clips at its edge as the native control does (a label a few pixels wider than the native font's would
    // otherwise drop its last word onto a second line the control has no room to show).
    internal static void WrapWhenTall(Control control)
    {
        void Apply()
        {
            if (FindLabel(control) is not { } label) return;
            label.TextWrapping = control.ActualHeight >= control.FontSize * 2.2 ? TextWrapping.Wrap : TextWrapping.NoWrap;
        }
        control.Loaded += (_, _) => Apply();
        control.SizeChanged += (_, _) => Apply();
    }

    private static ContentPresenter? FindLabel(DependencyObject root)
    {
        for (var index = 0; index < VisualTreeHelper.GetChildrenCount(root); ++index)
        {
            var child = VisualTreeHelper.GetChild(root, index);
            if (child is ContentPresenter presenter) return presenter;
            if (FindLabel(child) is { } nested) return nested;
        }
        return null;
    }

    [GeneratedRegex(@"\{ThemeResource ([A-Za-z0-9_]+)\}")]
    private static partial Regex ThemeResourceKey();

    private static Style? Create(string xaml, string name)
    {
        try
        {
            if (Application.Current?.Resources is { } resources)
            {
                foreach (Match match in ThemeResourceKey().Matches(xaml))
                {
                    if (Has(resources, match.Groups[1].Value, 0)) continue;
                    RendererDiagnostics.Log($"compact {name} style not used: theme resource '{match.Groups[1].Value}' is missing");
                    return null;
                }
            }
            return (Style)XamlReader.Load(xaml);
        }
        catch (Exception exception)
        {
            RendererDiagnostics.Log($"compact {name} style not used: {exception.Message}");
            return null;
        }
    }

    // Brushes live in theme dictionaries, usually inside merged dictionaries.
    private static bool Has(ResourceDictionary dictionary, string key, int depth)
    {
        if (dictionary.ContainsKey(key)) return true;
        if (depth > 6) return false;
        foreach (var theme in new[] { "Default", "Light", "Dark" })
            if (dictionary.ThemeDictionaries.TryGetValue(theme, out var themed) &&
                themed is ResourceDictionary themedDictionary && Has(themedDictionary, key, depth + 1)) return true;
        foreach (var merged in dictionary.MergedDictionaries)
            if (Has(merged, key, depth + 1)) return true;
        return false;
    }

    // The dot is scaled rather than resized, so the animation runs on the compositor.  The
    // states hold the end values with no duration and the transitions carry the motion: a
    // control built already checked (a rebuild, a page switch) shows its dot at once, and
    // only a change while the user watches animates.
    internal const string RadioButtonXaml = """
        <Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
               xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
               TargetType="RadioButton">
          <Setter Property="Foreground" Value="{ThemeResource TextFillColorPrimaryBrush}"/>
          <Setter Property="MinWidth" Value="0"/>
          <Setter Property="MinHeight" Value="0"/>
          <Setter Property="HorizontalContentAlignment" Value="Left"/>
          <Setter Property="VerticalContentAlignment" Value="Center"/>
          <Setter Property="UseSystemFocusVisuals" Value="True"/>
          <Setter Property="Template">
            <Setter.Value>
              <ControlTemplate TargetType="RadioButton">
                <Grid x:Name="RootGrid" Background="{TemplateBinding Background}">
                  <VisualStateManager.VisualStateGroups>
                    <VisualStateGroup x:Name="CommonStates">
                      <VisualState x:Name="Normal"/>
                      <VisualState x:Name="PointerOver">
                        <VisualState.Setters>
                          <Setter Target="Ring.Fill" Value="{ThemeResource ControlAltFillColorTertiaryBrush}"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="Pressed">
                        <VisualState.Setters>
                          <Setter Target="Ring.Fill" Value="{ThemeResource ControlAltFillColorQuarternaryBrush}"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="Disabled">
                        <VisualState.Setters>
                          <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TextFillColorDisabledBrush}"/>
                          <Setter Target="Ring.Stroke" Value="{ThemeResource ControlStrongStrokeColorDisabledBrush}"/>
                          <Setter Target="Ring.Fill" Value="{ThemeResource ControlAltFillColorDisabledBrush}"/>
                          <Setter Target="Dot.Fill" Value="{ThemeResource TextFillColorDisabledBrush}"/>
                        </VisualState.Setters>
                      </VisualState>
                    </VisualStateGroup>
                    <VisualStateGroup x:Name="CheckStates">
                      <VisualStateGroup.Transitions>
                        <VisualTransition From="Unchecked" To="Checked" GeneratedDuration="0:0:0.167">
                          <VisualTransition.GeneratedEasingFunction>
                            <CubicEase EasingMode="EaseOut"/>
                          </VisualTransition.GeneratedEasingFunction>
                        </VisualTransition>
                        <VisualTransition From="Checked" To="Unchecked" GeneratedDuration="0:0:0.083"/>
                      </VisualStateGroup.Transitions>
                      <VisualState x:Name="Unchecked">
                        <Storyboard>
                          <DoubleAnimation Storyboard.TargetName="DotScale" Storyboard.TargetProperty="ScaleX" To="0" Duration="0"/>
                          <DoubleAnimation Storyboard.TargetName="DotScale" Storyboard.TargetProperty="ScaleY" To="0" Duration="0"/>
                        </Storyboard>
                      </VisualState>
                      <VisualState x:Name="Checked">
                        <Storyboard>
                          <DoubleAnimation Storyboard.TargetName="DotScale" Storyboard.TargetProperty="ScaleX" To="1" Duration="0"/>
                          <DoubleAnimation Storyboard.TargetName="DotScale" Storyboard.TargetProperty="ScaleY" To="1" Duration="0"/>
                        </Storyboard>
                        <VisualState.Setters>
                          <Setter Target="Ring.Stroke" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
                          <Setter Target="Ring.Fill" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="Indeterminate"/>
                    </VisualStateGroup>
                  </VisualStateManager.VisualStateGroups>
                  <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="Auto"/>
                    <ColumnDefinition Width="*"/>
                  </Grid.ColumnDefinitions>
                  <Grid VerticalAlignment="Center" Width="16" Height="16">
                    <Ellipse x:Name="Ring" Width="16" Height="16" StrokeThickness="1"
                             Stroke="{ThemeResource ControlStrongStrokeColorDefaultBrush}"
                             Fill="{ThemeResource ControlAltFillColorSecondaryBrush}"/>
                    <Ellipse x:Name="Dot" Width="8" Height="8" RenderTransformOrigin="0.5,0.5"
                             Fill="{ThemeResource TextOnAccentFillColorPrimaryBrush}">
                      <Ellipse.RenderTransform>
                        <ScaleTransform x:Name="DotScale" ScaleX="0" ScaleY="0"/>
                      </Ellipse.RenderTransform>
                    </Ellipse>
                  </Grid>
                  <ContentPresenter x:Name="ContentPresenter" Grid.Column="1"
                                    Content="{TemplateBinding Content}"
                                    ContentTemplate="{TemplateBinding ContentTemplate}"
                                    Foreground="{TemplateBinding Foreground}"
                                    Padding="{TemplateBinding Padding}"
                                    HorizontalAlignment="{TemplateBinding HorizontalContentAlignment}"
                                    VerticalAlignment="{TemplateBinding VerticalContentAlignment}"
                                    TextWrapping="NoWrap"
                                    AutomationProperties.AccessibilityView="Raw"/>
                </Grid>
              </ControlTemplate>
            </Setter.Value>
          </Setter>
        </Style>
        """;

    // WinUI's check box goes to one combined state per (check, pointer) pair -- its stock
    // template has no other group -- and drives the tick through AnimatedIcon.State: the
    // icon plays the segment between the state it was in and the one it is given, which is
    // the draw-in and wipe-out of the tick.  The brushes are the ones WinUI's own check box
    // resources resolve to.  Indeterminate's down-level glyph is swapped by a storyboard
    // because the fallback icon source is not an element a setter can target.
    internal const string CheckBoxXaml = """
        <Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
               xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
               xmlns:animatedvisuals="using:Microsoft.UI.Xaml.Controls.AnimatedVisuals"
               TargetType="CheckBox">
          <Setter Property="Foreground" Value="{ThemeResource TextFillColorPrimaryBrush}"/>
          <Setter Property="MinWidth" Value="0"/>
          <Setter Property="MinHeight" Value="0"/>
          <Setter Property="HorizontalContentAlignment" Value="Left"/>
          <Setter Property="VerticalContentAlignment" Value="Center"/>
          <Setter Property="UseSystemFocusVisuals" Value="True"/>
          <Setter Property="Template">
            <Setter.Value>
              <ControlTemplate TargetType="CheckBox">
                <Grid x:Name="RootGrid" Background="{TemplateBinding Background}">
                  <VisualStateManager.VisualStateGroups>
                    <VisualStateGroup x:Name="CombinedStates">
                      <VisualState x:Name="UncheckedNormal">
                        <VisualState.Setters>
                          <Setter Target="CheckGlyph.(AnimatedIcon.State)" Value="NormalOff"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="UncheckedPointerOver">
                        <VisualState.Setters>
                          <Setter Target="Box.Background" Value="{ThemeResource ControlAltFillColorTertiaryBrush}"/>
                          <Setter Target="CheckGlyph.(AnimatedIcon.State)" Value="PointerOverOff"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="UncheckedPressed">
                        <VisualState.Setters>
                          <Setter Target="Box.Background" Value="{ThemeResource ControlAltFillColorQuarternaryBrush}"/>
                          <Setter Target="Box.BorderBrush" Value="{ThemeResource ControlStrongStrokeColorDisabledBrush}"/>
                          <Setter Target="CheckGlyph.(AnimatedIcon.State)" Value="PressedOff"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="UncheckedDisabled">
                        <VisualState.Setters>
                          <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TextFillColorDisabledBrush}"/>
                          <Setter Target="Box.Background" Value="{ThemeResource ControlAltFillColorDisabledBrush}"/>
                          <Setter Target="Box.BorderBrush" Value="{ThemeResource ControlStrongStrokeColorDisabledBrush}"/>
                          <Setter Target="CheckGlyph.(AnimatedIcon.State)" Value="NormalOff"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="CheckedNormal">
                        <VisualState.Setters>
                          <Setter Target="Box.Background" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
                          <Setter Target="Box.BorderBrush" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
                          <Setter Target="CheckGlyph.(AnimatedIcon.State)" Value="NormalOn"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="CheckedPointerOver">
                        <VisualState.Setters>
                          <Setter Target="Box.Background" Value="{ThemeResource AccentFillColorSecondaryBrush}"/>
                          <Setter Target="Box.BorderBrush" Value="{ThemeResource AccentFillColorSecondaryBrush}"/>
                          <Setter Target="CheckGlyph.(AnimatedIcon.State)" Value="PointerOverOn"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="CheckedPressed">
                        <VisualState.Setters>
                          <Setter Target="Box.Background" Value="{ThemeResource AccentFillColorTertiaryBrush}"/>
                          <Setter Target="Box.BorderBrush" Value="{ThemeResource AccentFillColorTertiaryBrush}"/>
                          <Setter Target="CheckGlyph.Foreground" Value="{ThemeResource TextOnAccentFillColorSecondaryBrush}"/>
                          <Setter Target="CheckGlyph.(AnimatedIcon.State)" Value="PressedOn"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="CheckedDisabled">
                        <VisualState.Setters>
                          <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TextFillColorDisabledBrush}"/>
                          <Setter Target="Box.Background" Value="{ThemeResource AccentFillColorDisabledBrush}"/>
                          <Setter Target="Box.BorderBrush" Value="{ThemeResource AccentFillColorDisabledBrush}"/>
                          <Setter Target="CheckGlyph.Foreground" Value="{ThemeResource TextOnAccentFillColorDisabledBrush}"/>
                          <Setter Target="CheckGlyph.(AnimatedIcon.State)" Value="NormalOn"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="IndeterminateNormal">
                        <Storyboard>
                          <ObjectAnimationUsingKeyFrames Storyboard.TargetName="DownLevelCheckGlyph" Storyboard.TargetProperty="Glyph">
                            <DiscreteObjectKeyFrame KeyTime="0" Value="&#xE9AE;"/>
                          </ObjectAnimationUsingKeyFrames>
                        </Storyboard>
                        <VisualState.Setters>
                          <Setter Target="Box.Background" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
                          <Setter Target="Box.BorderBrush" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
                          <Setter Target="CheckGlyph.(AnimatedIcon.State)" Value="NormalIndeterminate"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="IndeterminatePointerOver">
                        <Storyboard>
                          <ObjectAnimationUsingKeyFrames Storyboard.TargetName="DownLevelCheckGlyph" Storyboard.TargetProperty="Glyph">
                            <DiscreteObjectKeyFrame KeyTime="0" Value="&#xE9AE;"/>
                          </ObjectAnimationUsingKeyFrames>
                        </Storyboard>
                        <VisualState.Setters>
                          <Setter Target="Box.Background" Value="{ThemeResource AccentFillColorSecondaryBrush}"/>
                          <Setter Target="Box.BorderBrush" Value="{ThemeResource AccentFillColorSecondaryBrush}"/>
                          <Setter Target="CheckGlyph.(AnimatedIcon.State)" Value="PointerOverIndeterminate"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="IndeterminatePressed">
                        <Storyboard>
                          <ObjectAnimationUsingKeyFrames Storyboard.TargetName="DownLevelCheckGlyph" Storyboard.TargetProperty="Glyph">
                            <DiscreteObjectKeyFrame KeyTime="0" Value="&#xE9AE;"/>
                          </ObjectAnimationUsingKeyFrames>
                        </Storyboard>
                        <VisualState.Setters>
                          <Setter Target="Box.Background" Value="{ThemeResource AccentFillColorTertiaryBrush}"/>
                          <Setter Target="Box.BorderBrush" Value="{ThemeResource AccentFillColorTertiaryBrush}"/>
                          <Setter Target="CheckGlyph.Foreground" Value="{ThemeResource TextOnAccentFillColorSecondaryBrush}"/>
                          <Setter Target="CheckGlyph.(AnimatedIcon.State)" Value="PressedIndeterminate"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="IndeterminateDisabled">
                        <Storyboard>
                          <ObjectAnimationUsingKeyFrames Storyboard.TargetName="DownLevelCheckGlyph" Storyboard.TargetProperty="Glyph">
                            <DiscreteObjectKeyFrame KeyTime="0" Value="&#xE9AE;"/>
                          </ObjectAnimationUsingKeyFrames>
                        </Storyboard>
                        <VisualState.Setters>
                          <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TextFillColorDisabledBrush}"/>
                          <Setter Target="Box.Background" Value="{ThemeResource AccentFillColorDisabledBrush}"/>
                          <Setter Target="Box.BorderBrush" Value="{ThemeResource AccentFillColorDisabledBrush}"/>
                          <Setter Target="CheckGlyph.Foreground" Value="{ThemeResource TextOnAccentFillColorDisabledBrush}"/>
                          <Setter Target="CheckGlyph.(AnimatedIcon.State)" Value="NormalIndeterminate"/>
                        </VisualState.Setters>
                      </VisualState>
                    </VisualStateGroup>
                  </VisualStateManager.VisualStateGroups>
                  <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="Auto"/>
                    <ColumnDefinition Width="*"/>
                  </Grid.ColumnDefinitions>
                  <Grid VerticalAlignment="Center" Width="16" Height="16">
                    <Border x:Name="Box" Width="16" Height="16" CornerRadius="3" BorderThickness="1"
                            BorderBrush="{ThemeResource ControlStrongStrokeColorDefaultBrush}"
                            Background="{ThemeResource ControlAltFillColorSecondaryBrush}"/>
                    <AnimatedIcon x:Name="CheckGlyph" Width="16" Height="16" AnimatedIcon.State="NormalOff"
                                  Foreground="{ThemeResource TextOnAccentFillColorPrimaryBrush}">
                      <animatedvisuals:AnimatedAcceptVisualSource/>
                      <AnimatedIcon.FallbackIconSource>
                        <FontIconSource x:Name="DownLevelCheckGlyph" Glyph="&#xE73E;" FontSize="10"
                                        FontFamily="{ThemeResource SymbolThemeFontFamily}"/>
                      </AnimatedIcon.FallbackIconSource>
                    </AnimatedIcon>
                  </Grid>
                  <ContentPresenter x:Name="ContentPresenter" Grid.Column="1"
                                    Content="{TemplateBinding Content}"
                                    ContentTemplate="{TemplateBinding ContentTemplate}"
                                    Foreground="{TemplateBinding Foreground}"
                                    Padding="{TemplateBinding Padding}"
                                    HorizontalAlignment="{TemplateBinding HorizontalContentAlignment}"
                                    VerticalAlignment="{TemplateBinding VerticalContentAlignment}"
                                    TextWrapping="NoWrap"
                                    AutomationProperties.AccessibilityView="Raw"/>
                </Grid>
              </ControlTemplate>
            </Setter.Value>
          </Setter>
        </Style>
        """;

    private const string StaticRadioButtonXaml = """
        <Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
               xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
               TargetType="RadioButton">
          <Setter Property="Foreground" Value="{ThemeResource TextFillColorPrimaryBrush}"/>
          <Setter Property="MinWidth" Value="0"/>
          <Setter Property="MinHeight" Value="0"/>
          <Setter Property="HorizontalContentAlignment" Value="Left"/>
          <Setter Property="VerticalContentAlignment" Value="Center"/>
          <Setter Property="UseSystemFocusVisuals" Value="True"/>
          <Setter Property="Template">
            <Setter.Value>
              <ControlTemplate TargetType="RadioButton">
                <Grid x:Name="RootGrid" Background="{TemplateBinding Background}">
                  <VisualStateManager.VisualStateGroups>
                    <VisualStateGroup x:Name="CommonStates">
                      <VisualState x:Name="Normal"/>
                      <VisualState x:Name="PointerOver">
                        <VisualState.Setters>
                          <Setter Target="Ring.Fill" Value="{ThemeResource ControlAltFillColorTertiaryBrush}"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="Pressed">
                        <VisualState.Setters>
                          <Setter Target="Ring.Fill" Value="{ThemeResource ControlAltFillColorQuarternaryBrush}"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="Disabled">
                        <VisualState.Setters>
                          <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TextFillColorDisabledBrush}"/>
                          <Setter Target="Ring.Stroke" Value="{ThemeResource ControlStrongStrokeColorDisabledBrush}"/>
                          <Setter Target="Ring.Fill" Value="{ThemeResource ControlAltFillColorDisabledBrush}"/>
                          <Setter Target="Dot.Fill" Value="{ThemeResource TextFillColorDisabledBrush}"/>
                        </VisualState.Setters>
                      </VisualState>
                    </VisualStateGroup>
                    <VisualStateGroup x:Name="CheckStates">
                      <VisualState x:Name="Unchecked"/>
                      <VisualState x:Name="Checked">
                        <VisualState.Setters>
                          <Setter Target="Ring.Stroke" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
                          <Setter Target="Ring.Fill" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
                          <Setter Target="Dot.Opacity" Value="1"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="Indeterminate"/>
                    </VisualStateGroup>
                  </VisualStateManager.VisualStateGroups>
                  <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="Auto"/>
                    <ColumnDefinition Width="*"/>
                  </Grid.ColumnDefinitions>
                  <Grid VerticalAlignment="Center" Width="16" Height="16">
                    <Ellipse x:Name="Ring" Width="16" Height="16" StrokeThickness="1"
                             Stroke="{ThemeResource ControlStrongStrokeColorDefaultBrush}"
                             Fill="{ThemeResource ControlAltFillColorSecondaryBrush}"/>
                    <Ellipse x:Name="Dot" Width="8" Height="8" Opacity="0"
                             Fill="{ThemeResource TextOnAccentFillColorPrimaryBrush}"/>
                  </Grid>
                  <ContentPresenter x:Name="ContentPresenter" Grid.Column="1"
                                    Content="{TemplateBinding Content}"
                                    ContentTemplate="{TemplateBinding ContentTemplate}"
                                    Foreground="{TemplateBinding Foreground}"
                                    Padding="{TemplateBinding Padding}"
                                    HorizontalAlignment="{TemplateBinding HorizontalContentAlignment}"
                                    VerticalAlignment="{TemplateBinding VerticalContentAlignment}"
                                    TextWrapping="NoWrap"
                                    AutomationProperties.AccessibilityView="Raw"/>
                </Grid>
              </ControlTemplate>
            </Setter.Value>
          </Setter>
        </Style>
        """;

    private const string StaticCheckBoxXaml = """
        <Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
               xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
               TargetType="CheckBox">
          <Setter Property="Foreground" Value="{ThemeResource TextFillColorPrimaryBrush}"/>
          <Setter Property="MinWidth" Value="0"/>
          <Setter Property="MinHeight" Value="0"/>
          <Setter Property="HorizontalContentAlignment" Value="Left"/>
          <Setter Property="VerticalContentAlignment" Value="Center"/>
          <Setter Property="UseSystemFocusVisuals" Value="True"/>
          <Setter Property="Template">
            <Setter.Value>
              <ControlTemplate TargetType="CheckBox">
                <Grid x:Name="RootGrid" Background="{TemplateBinding Background}">
                  <VisualStateManager.VisualStateGroups>
                    <VisualStateGroup x:Name="CommonStates">
                      <VisualState x:Name="Normal"/>
                      <VisualState x:Name="PointerOver">
                        <VisualState.Setters>
                          <Setter Target="Box.Background" Value="{ThemeResource ControlAltFillColorTertiaryBrush}"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="Pressed">
                        <VisualState.Setters>
                          <Setter Target="Box.Background" Value="{ThemeResource ControlAltFillColorQuarternaryBrush}"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="Disabled">
                        <VisualState.Setters>
                          <Setter Target="ContentPresenter.Foreground" Value="{ThemeResource TextFillColorDisabledBrush}"/>
                          <Setter Target="Box.BorderBrush" Value="{ThemeResource ControlStrongStrokeColorDisabledBrush}"/>
                          <Setter Target="Box.Background" Value="{ThemeResource ControlAltFillColorDisabledBrush}"/>
                          <Setter Target="Glyph.Foreground" Value="{ThemeResource TextFillColorDisabledBrush}"/>
                        </VisualState.Setters>
                      </VisualState>
                    </VisualStateGroup>
                    <VisualStateGroup x:Name="CheckStates">
                      <VisualState x:Name="Unchecked"/>
                      <VisualState x:Name="Checked">
                        <VisualState.Setters>
                          <Setter Target="Box.Background" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
                          <Setter Target="Box.BorderBrush" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
                          <Setter Target="Glyph.Opacity" Value="1"/>
                        </VisualState.Setters>
                      </VisualState>
                      <VisualState x:Name="Indeterminate">
                        <VisualState.Setters>
                          <Setter Target="Box.Background" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
                          <Setter Target="Box.BorderBrush" Value="{ThemeResource AccentFillColorDefaultBrush}"/>
                          <Setter Target="Glyph.Glyph" Value="&#xE738;"/>
                          <Setter Target="Glyph.Opacity" Value="1"/>
                        </VisualState.Setters>
                      </VisualState>
                    </VisualStateGroup>
                  </VisualStateManager.VisualStateGroups>
                  <Grid.ColumnDefinitions>
                    <ColumnDefinition Width="Auto"/>
                    <ColumnDefinition Width="*"/>
                  </Grid.ColumnDefinitions>
                  <Grid VerticalAlignment="Center" Width="16" Height="16">
                    <Border x:Name="Box" Width="16" Height="16" CornerRadius="3" BorderThickness="1"
                            BorderBrush="{ThemeResource ControlStrongStrokeColorDefaultBrush}"
                            Background="{ThemeResource ControlAltFillColorSecondaryBrush}"/>
                    <FontIcon x:Name="Glyph" Glyph="&#xE73E;" FontSize="12" Opacity="0"
                              FontFamily="{ThemeResource SymbolThemeFontFamily}"
                              Foreground="{ThemeResource TextOnAccentFillColorPrimaryBrush}"/>
                  </Grid>
                  <ContentPresenter x:Name="ContentPresenter" Grid.Column="1"
                                    Content="{TemplateBinding Content}"
                                    ContentTemplate="{TemplateBinding ContentTemplate}"
                                    Foreground="{TemplateBinding Foreground}"
                                    Padding="{TemplateBinding Padding}"
                                    HorizontalAlignment="{TemplateBinding HorizontalContentAlignment}"
                                    VerticalAlignment="{TemplateBinding VerticalContentAlignment}"
                                    TextWrapping="NoWrap"
                                    AutomationProperties.AccessibilityView="Raw"/>
                </Grid>
              </ControlTemplate>
            </Setter.Value>
          </Setter>
        </Style>
        """;
}
