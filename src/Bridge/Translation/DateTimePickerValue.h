#pragma once

#include <windows.h>

namespace FluentShell::Bridge::Translation {

// A date picker's value travels as the one integer a node carries for a position: the day
// number, counted from 0001-01-01 the way .NET's DateOnly.DayNumber counts, so the renderer
// converts with no calendar arithmetic of its own.  A time picker's is the second of the day.

// DayNumber of 1970-01-01.
constexpr int kDayNumberOfUnixEpoch = 719162;

// Proleptic Gregorian civil date -> day number (days_from_civil, shifted to 0001-01-01).
constexpr int DayNumberFromCivil(int year, int month, int day) noexcept {
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear =
        (153u * static_cast<unsigned>(month > 2 ? month - 3 : month + 9) + 2u) / 5u +
        static_cast<unsigned>(day) - 1u;
    const unsigned dayOfEra = yearOfEra * 365u + yearOfEra / 4u - yearOfEra / 100u + dayOfYear;
    return era * 146097 + static_cast<int>(dayOfEra) - 719468 + kDayNumberOfUnixEpoch;
}

// Day number -> civil date.
constexpr void CivilFromDayNumber(int dayNumber, int& year, int& month, int& day) noexcept {
    const int shifted = dayNumber - kDayNumberOfUnixEpoch + 719468;
    const int era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(shifted - era * 146097);
    const unsigned yearOfEra =
        (dayOfEra - dayOfEra / 1460u + dayOfEra / 36524u - dayOfEra / 146096u) / 365u;
    const unsigned dayOfYear = dayOfEra - (365u * yearOfEra + yearOfEra / 4u - yearOfEra / 100u);
    const unsigned monthIndex = (5u * dayOfYear + 2u) / 153u;
    day = static_cast<int>(dayOfYear - (153u * monthIndex + 2u) / 5u + 1u);
    month = static_cast<int>(monthIndex < 10u ? monthIndex + 3u : monthIndex - 9u);
    year = static_cast<int>(yearOfEra) + era * 400 + (month <= 2 ? 1 : 0);
}

// The years a projected date picker can show: WinUI's calendar picker is built for them, and
// a native range that reaches past either end is clamped to it.
constexpr int kDatePickerMinDay = DayNumberFromCivil(1900, 1, 1);
constexpr int kDatePickerMaxDay = DayNumberFromCivil(2200, 12, 31);

constexpr int kSecondsPerDay = 24 * 60 * 60;

// 0001-01-01 was a Monday; SYSTEMTIME counts from Sunday.
constexpr WORD DayOfWeekFromDayNumber(int dayNumber) noexcept {
    return static_cast<WORD>((dayNumber + 1) % 7);
}

} // namespace FluentShell::Bridge::Translation
