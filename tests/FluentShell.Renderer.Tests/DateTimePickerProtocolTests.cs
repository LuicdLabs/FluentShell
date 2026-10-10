using System.Globalization;
using FluentShell.Renderer.Protocol;
using FluentShell.Renderer.Windows;
using Microsoft.UI.Xaml.Automation.Peers;

namespace FluentShell.Renderer.Tests;

// A native date or time picker (minor 37) carries its value as the position of a range: a date
// picker's day number, a time picker's second of the day.  The projection converts with
// DateOnly's own numbering, so the two sides agree on every day.
public sealed class DateTimePickerProtocolTests
{
    private static ControlNode Date() => new()
    {
        NodeId = "31", Generation = "1", NativeHwnd = "0x4331", Kind = "datePicker",
        Style = "0x50010000", ExStyle = "0x0", Rect = new PixelRect { Width = 100, Height = 23 },
        Visible = true, Enabled = true, TabIndex = 0, TabStop = true,
        Minimum = ProtocolConstants.MinPickerDay, Maximum = ProtocolConstants.MaxPickerDay,
        Position = new DateOnly(2026, 10, 9).DayNumber, SmallChange = 1, LargeChange = 30,
    };

    private static ControlNode Time() => new()
    {
        NodeId = "32", Generation = "1", NativeHwnd = "0x4332", Kind = "timePicker",
        Style = "0x50010009", ExStyle = "0x0", Rect = new PixelRect { Width = 100, Height = 23 },
        Visible = true, Enabled = true, TabIndex = 1, TabStop = true,
        Minimum = 0, Maximum = ProtocolConstants.SecondsPerDay - 1,
        Position = 14 * 3600 + 30 * 60 + 45, SmallChange = 60, LargeChange = 3600,
    };

    private static void Validate(ControlNode node) =>
        ProtocolValidator.ValidateSnapshot(TestData.Snapshot() with { Nodes = [node] });

    [Fact]
    public void ADateAndATimeAreAdmittedAndProjectAsButtons()
    {
        Validate(Date());
        Validate(Date() with { Minimum = new DateOnly(2026, 10, 1).DayNumber, Maximum = new DateOnly(2026, 10, 31).DayNumber });
        Validate(Time());
        Validate(Time() with { Position = 0 });
        Validate(Time() with { Position = ProtocolConstants.SecondsPerDay - 1 });
        Assert.Equal(AutomationControlType.Button, ControlFactory.AutomationControlTypeFor("datePicker"));
        Assert.Equal(AutomationControlType.Button, ControlFactory.AutomationControlTypeFor("timePicker"));
    }

    [Fact]
    public void ADateOutsideItsRangeOrTheCalendarIsRefused()
    {
        foreach (var node in new[]
        {
            Date() with { Minimum = null },
            Date() with { Position = null },
            Date() with { Minimum = ProtocolConstants.MinPickerDay - 1 },
            Date() with { Maximum = ProtocolConstants.MaxPickerDay + 1 },
            Date() with { Minimum = 700_000, Maximum = 699_999 },
            Date() with { Position = ProtocolConstants.MaxPickerDay + 1 },
            Date() with { Position = ProtocolConstants.MinPickerDay - 1 },
            Date() with { Minimum = new DateOnly(2026, 10, 10).DayNumber },
        })
            Assert.Throws<ProtocolException>(() => Validate(node));
    }

    [Fact]
    public void ATimeOutsideTheDayIsRefused()
    {
        foreach (var node in new[]
        {
            Time() with { Maximum = null },
            Time() with { Minimum = 1 },
            Time() with { Maximum = ProtocolConstants.SecondsPerDay },
            Time() with { Position = ProtocolConstants.SecondsPerDay },
            Time() with { Position = -1 },
        })
            Assert.Throws<ProtocolException>(() => Validate(node));
    }

    [Fact]
    public void ADayNumberIsTheDayItNamesInTheLocalCalendar()
    {
        var failures = new List<int>();
        for (var day = ProtocolConstants.MinPickerDay; day <= ProtocolConstants.MaxPickerDay; day += 13)
        {
            var offset = DateTimePickerValues.DateOf(day);
            // Midday in the local zone: the calendar reads the same date whatever the offset.
            if (offset.Hour != 12 || DateTimePickerValues.DayNumberOf(offset) != day ||
                DateOnly.FromDayNumber(day) != DateOnly.FromDateTime(offset.DateTime))
                failures.Add(day);
        }
        Assert.Empty(failures);
        // The numbering the Bridge uses: 1970-01-01 is 719162, 0001-01-01 is 0.
        Assert.Equal(719162, new DateOnly(1970, 1, 1).DayNumber);
        Assert.Equal(0, new DateOnly(1, 1, 1).DayNumber);
        Assert.Equal(693595, ProtocolConstants.MinPickerDay);
    }

    [Fact]
    public void ATimeKeepsItsSecondsWhenTheFlyoutChoosesHoursAndMinutes()
    {
        Assert.Equal(9 * 3600 + 5 * 60 + 7, DateTimePickerValues.SecondOfDay(new TimeSpan(9, 5, 0), 14 * 3600 + 30 * 60 + 7));
        Assert.Equal(0, DateTimePickerValues.SecondOfDay(TimeSpan.Zero, 0));
        Assert.Equal(ProtocolConstants.SecondsPerDay - 1, DateTimePickerValues.SecondOfDay(new TimeSpan(23, 59, 0), 59));
        Assert.Equal(new DateTime(2000, 1, 1, 14, 30, 45).ToString("T", CultureInfo.CurrentCulture),
            DateTimePickerValues.TimeText(14 * 3600 + 30 * 60 + 45));
        // A value outside the day is clamped rather than wrapped.
        Assert.Equal(DateTimePickerValues.TimeText(ProtocolConstants.SecondsPerDay - 1),
            DateTimePickerValues.TimeText(ProtocolConstants.SecondsPerDay + 5));
    }
}
