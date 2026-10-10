using System.Globalization;
using FluentShell.Renderer.Protocol;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Automation.Peers;
using Microsoft.UI.Xaml.Controls;

namespace FluentShell.Renderer.Windows;

// A native date or time picker (SysDateTimePick32), minor 37.  Its value is the position of a
// range: a date's day number (days since 0001-01-01, DateOnly.DayNumber) or a time's second of
// the day.  A date picker is WinUI's calendar picker; a time picker is the field a native one
// is, opening WinUI's time flyout.  Seconds have no column in that flyout, so a time chosen
// there keeps the seconds the native control already had.
internal static class DateTimePickerValues
{
    // CalendarDatePicker reads a DateTimeOffset in the calendar's own (local) time zone, so a
    // date is built from local midday: no offset can move it across a day.
    internal static DateTimeOffset DateOf(int dayNumber) =>
        new(DateTime.SpecifyKind(
            DateOnly.FromDayNumber(dayNumber).ToDateTime(new TimeOnly(12, 0)), DateTimeKind.Local));

    internal static int DayNumberOf(DateTimeOffset value) => DateOnly.FromDateTime(value.DateTime).DayNumber;

    internal static string TimeText(int secondOfDay) =>
        DateTime.Today.AddSeconds(Math.Clamp(secondOfDay, 0, ProtocolConstants.SecondsPerDay - 1))
            .ToString("T", CultureInfo.CurrentCulture);

    // The flyout's hour and minute, with the native seconds kept.
    internal static int SecondOfDay(TimeSpan picked, int current) =>
        Math.Clamp(picked.Hours * 3600 + picked.Minutes * 60 + current % 60, 0, ProtocolConstants.SecondsPerDay - 1);
}

internal sealed class SemanticDatePicker : CalendarDatePicker
{
    private bool _applying;

    internal event Action<int>? DayRequested;

    public SemanticDatePicker()
    {
        DateChanged += (_, args) =>
        {
            if (_applying || args.NewDate is not { } picked) return;
            DayRequested?.Invoke(DateTimePickerValues.DayNumberOf(picked));
        };
    }

    // The range moves outward first so the control never clamps the shown date against a
    // stale bound.
    internal void Apply(int minimum, int maximum, int position)
    {
        _applying = true;
        try
        {
            var low = DateTimePickerValues.DateOf(minimum);
            var high = DateTimePickerValues.DateOf(maximum);
            if (low < MinDate) MinDate = low;
            if (high > MaxDate) MaxDate = high;
            MinDate = low;
            MaxDate = high;
            Date = DateTimePickerValues.DateOf(position);
        }
        finally
        {
            _applying = false;
        }
    }

    protected override AutomationPeer OnCreateAutomationPeer() => new SemanticDatePickerAutomationPeer(this);
}

// The peer says what the native control is named, not what the calendar picker would make
// of its displayed date (the Bridge's gate compares the two).
internal sealed class SemanticDatePickerAutomationPeer(SemanticDatePicker owner) : CalendarDatePickerAutomationPeer(owner)
{
    protected override AutomationControlType GetAutomationControlTypeCore() => AutomationControlType.Button;
    protected override string GetClassNameCore() => "ProjectedDatePicker";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
}

internal sealed class SemanticTimePicker : Button
{
    private readonly TimePickerFlyout _flyout = new() { MinuteIncrement = 1 };
    private int _secondOfDay;

    internal event Action<int>? SecondRequested;

    public SemanticTimePicker(double fontSize)
    {
        FontSize = fontSize;
        MinWidth = 0;
        MinHeight = 0;
        Padding = new Thickness(6, 0, 6, 0);
        HorizontalContentAlignment = HorizontalAlignment.Left;
        VerticalContentAlignment = VerticalAlignment.Center;
        _flyout.TimePicked += (_, args) =>
            SecondRequested?.Invoke(DateTimePickerValues.SecondOfDay(args.NewTime, _secondOfDay));
        Click += (_, _) =>
        {
            _flyout.Time = TimeSpan.FromSeconds(_secondOfDay - _secondOfDay % 60);
            _flyout.ShowAt(this);
        };
    }

    // The field shows the text the native picker shows (its format is the user's regional one,
    // whatever it is); a snapshot without that text gets the current culture's long time.
    internal void Apply(int secondOfDay, string nativeText)
    {
        _secondOfDay = secondOfDay;
        Content = nativeText.Length != 0 ? nativeText : DateTimePickerValues.TimeText(secondOfDay);
    }

    protected override AutomationPeer OnCreateAutomationPeer() => new SemanticTimePickerAutomationPeer(this);
}

internal sealed class SemanticTimePickerAutomationPeer(SemanticTimePicker owner) : ButtonAutomationPeer(owner)
{
    protected override string GetClassNameCore() => "ProjectedTimePicker";
    protected override string GetNameCore() => AutomationProperties.GetName(owner);
}
