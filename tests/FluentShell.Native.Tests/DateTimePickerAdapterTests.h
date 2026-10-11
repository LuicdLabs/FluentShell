#pragma once

// Include after main.cpp's Check helper, inside its test namespace.
//
// Real SysDateTimePick32 controls: a date picker, a time picker, and the formats the adapter
// refuses.  The parent records the DTN_DATETIMECHANGE notification a user's change produces,
// which is how a Windows Forms DateTimePicker learns of its new value.

struct DateTimeParentState final {
    int changes = 0;
    SYSTEMTIME last{};
    UINT_PTR lastFrom = 0;
};

LRESULT CALLBACK DateTimeParentSubclass(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto& state = *reinterpret_cast<DateTimeParentState*>(data);
    if (message == WM_NOTIFY) {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header && header->code == DTN_DATETIMECHANGE) {
            const auto* change = reinterpret_cast<NMDATETIMECHANGE*>(lParam);
            ++state.changes;
            state.last = change->st;
            state.lastFrom = header->idFrom;
            return 0;
        }
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

// The day number is DateOnly.DayNumber's: the renderer converts with that, so the two sides must
// agree on every day, and a round trip through the civil date must give the day back.
void TestDateTimePickerDayNumbers() {
    using namespace Translation;
    Check(DayNumberFromCivil(1, 1, 1) == 0 && DayNumberFromCivil(1970, 1, 1) == kDayNumberOfUnixEpoch &&
          kDayNumberOfUnixEpoch == 719162 && DayNumberFromCivil(2000, 3, 1) == 730179,
        "a day number was not counted from 0001-01-01");
    Check(DayNumberFromCivil(1900, 1, 1) == 693595 && kDatePickerMinDay == 693595 &&
          DayNumberFromCivil(2200, 12, 31) == kDatePickerMaxDay,
        "the picker's calendar range did not map to its day numbers");
    bool roundTripped = true;
    for (int day = kDatePickerMinDay; day <= kDatePickerMaxDay; day += 7) {
        int year = 0;
        int month = 0;
        int date = 0;
        CivilFromDayNumber(day, year, month, date);
        roundTripped = roundTripped && DayNumberFromCivil(year, month, date) == day &&
            month >= 1 && month <= 12 && date >= 1 && date <= 31;
    }
    Check(roundTripped, "a day number did not survive a round trip through its civil date");
    int year = 0;
    int month = 0;
    int day = 0;
    CivilFromDayNumber(DayNumberFromCivil(2024, 2, 29), year, month, day);
    Check(year == 2024 && month == 2 && day == 29, "a leap day did not round trip");
    // 1970-01-01 was a Thursday, 2026-10-09 a Friday.
    Check(DayOfWeekFromDayNumber(kDayNumberOfUnixEpoch) == 4 &&
          DayOfWeekFromDayNumber(DayNumberFromCivil(2026, 10, 9)) == 5,
        "a day number did not give its day of the week");
}

void TestDateTimePickerAdapter() {
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_DATE_CLASSES };
    InitCommonControlsEx(&controls);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(WS_EX_CONTROLPARENT, L"#32770", L"pickers",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 40, 40, 420, 240, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    const auto picker = [&](int id, DWORD style, int top) {
        return root ? CreateWindowExW(0, DATETIMEPICK_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
            12, top, 160, 24, root, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr)
                    : nullptr;
    };
    const HWND date = picker(41, DTS_SHORTDATEFORMAT, 12);
    const HWND time = picker(42, DTS_TIMEFORMAT, 44);
    const HWND none = picker(43, DTS_SHORTDATEFORMAT | DTS_SHOWNONE, 76);
    const HWND parsed = picker(44, DTS_SHORTDATEFORMAT | DTS_APPCANPARSE, 108);
    const HWND invalid = picker(45, 0x0008, 140);
    Check(root && date && time && none && parsed && invalid, "DateTimePicker fixture could not create its windows");
    if (!root || !date || !time || !none || !parsed || !invalid) return;
    DateTimeParentState state;
    SetWindowSubclass(root, DateTimeParentSubclass, 0xAA51, reinterpret_cast<DWORD_PTR>(&state));
    ShowWindow(root, SW_SHOWNOACTIVATE);

    SYSTEMTIME value{};
    value.wYear = 2026; value.wMonth = 10; value.wDay = 9; value.wDayOfWeek = 5;
    value.wHour = 14; value.wMinute = 30; value.wSecond = 45;
    SendMessageW(date, DTM_SETSYSTEMTIME, GDT_VALID, reinterpret_cast<LPARAM>(&value));
    SendMessageW(time, DTM_SETSYSTEMTIME, GDT_VALID, reinterpret_cast<LPARAM>(&value));

    using Translation::ControlKind;
    ControlKind kind{};
    std::wstring reason;
    Check(Translation::ClassifyControl(date, kind, reason) && kind == ControlKind::DatePicker,
        "a short-date DateTimePicker was not classified as datePicker");
    Check(Translation::ClassifyControl(time, kind, reason) && kind == ControlKind::TimePicker,
        "a time DateTimePicker was not classified as timePicker");
    reason.clear();
    Check(!Translation::ClassifyControl(none, kind, reason) && reason.find(L"DTS_SHOWNONE") != std::wstring::npos,
        "a DateTimePicker with a no-date check box was accepted");
    reason.clear();
    Check(!Translation::ClassifyControl(parsed, kind, reason) && reason.find(L"DTS_APPCANPARSE") != std::wstring::npos,
        "a DateTimePicker that parses its own text was accepted");
    reason.clear();
    Check(!Translation::ClassifyControl(invalid, kind, reason) && !reason.empty(),
        "a DateTimePicker with an impossible format style was accepted");
    // The picker with a no-date check box and the one that parses its own text keep the whole
    // dialog native: they are not hidden by the others.
    DestroyWindow(none);
    DestroyWindow(parsed);
    DestroyWindow(invalid);

    Translation::CaptureContext context;
    context.surfaceId = L"d1d1d1d1-2e2e-3f3f-4040-515151515151";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    const bool captured = Translation::CaptureWindow(root, context, snapshot, reason);
    if (!captured) std::wcerr << L"DateTimePicker capture: " << reason << L"\n";
    const auto find = [&snapshot](HWND window) {
        return std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [window](const auto& candidate) { return candidate.hwnd == window; });
    };
    const auto dateNode = find(date);
    const auto timeNode = find(time);
    Check(captured && dateNode != snapshot.nodes.end() && dateNode->kind == ControlKind::DatePicker &&
        dateNode->position == Translation::DayNumberFromCivil(2026, 10, 9) &&
        dateNode->minimum == Translation::kDatePickerMinDay &&
        dateNode->maximum == Translation::kDatePickerMaxDay,
        "a date picker's day number or range was not captured");
    Check(captured && timeNode != snapshot.nodes.end() && timeNode->kind == ControlKind::TimePicker &&
        timeNode->position == 14 * 3600 + 30 * 60 + 45 && timeNode->minimum == 0 &&
        timeNode->maximum == Translation::kSecondsPerDay - 1,
        "a time picker's second of the day or range was not captured");

    // A native range narrows the projected one.
    SYSTEMTIME limits[2]{};
    limits[0] = value; limits[0].wDay = 1;
    limits[1] = value; limits[1].wDay = 31;
    SendMessageW(date, DTM_SETRANGE, GDTR_MIN | GDTR_MAX, reinterpret_cast<LPARAM>(limits));
    context.revision = 2;
    Translation::WindowSnapshot ranged;
    const bool rangedCaptured = Translation::CaptureWindow(root, context, ranged, reason);
    const auto rangedNode = std::find_if(ranged.nodes.begin(), ranged.nodes.end(),
        [date](const auto& candidate) { return candidate.hwnd == date; });
    Check(rangedCaptured && rangedNode != ranged.nodes.end() &&
        rangedNode->minimum == Translation::DayNumberFromCivil(2026, 10, 1) &&
        rangedNode->maximum == Translation::DayNumberFromCivil(2026, 10, 31),
        "a date picker's native range was not carried");

    // A new date keeps the time of day, tells the parent, and is the verdict.
    const HWND hostRoot = root;
    const int newDay = Translation::DayNumberFromCivil(2026, 10, 20);
    Check(Translation::SetDateTimePickerValue(hostRoot, date, false, newDay) && state.changes == 1 &&
        state.last.wYear == 2026 && state.last.wMonth == 10 && state.last.wDay == 20 &&
        state.last.wHour == 14 && state.last.wMinute == 30 && state.lastFrom == 41,
        "a date was not applied with its notification and the time of day kept");
    SYSTEMTIME read{};
    SendMessageW(date, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&read));
    Check(read.wDay == 20 && read.wHour == 14 && read.wMinute == 30 && read.wSecond == 45,
        "the native date picker did not take the new date");
    // The same value is not a change.
    Check(Translation::SetDateTimePickerValue(hostRoot, date, false, newDay) && state.changes == 1,
        "an unchanged date raised a notification");
    // A date outside the native range is not applied (the control clamps it), so the answer is no.
    Check(!Translation::SetDateTimePickerValue(hostRoot, date, false,
              Translation::DayNumberFromCivil(2026, 12, 1)),
        "a date outside the native range was reported as applied");

    // A new time keeps the date.
    const int newSecond = 9 * 3600 + 5 * 60 + 7;
    const bool timeApplied = Translation::SetDateTimePickerValue(hostRoot, time, true, newSecond);
    Check(timeApplied && state.changes == 2 &&
        state.last.wHour == 9 && state.last.wMinute == 5 && state.last.wSecond == 7 &&
        state.last.wYear == 2026 && state.last.wDay == 9 && state.lastFrom == 42,
        "a time was not applied with its notification and the date kept");
    if (!(timeApplied && state.changes == 2)) {
        std::wcerr << L"time picker: applied=" << timeApplied << L" changes=" << state.changes
                   << L" last=" << state.last.wYear << L'-' << state.last.wMonth << L'-' << state.last.wDay << L' '
                   << state.last.wHour << L':' << state.last.wMinute << L':' << state.last.wSecond
                   << L" from=" << state.lastFrom << L"\n";
    }
    Check(!Translation::SetDateTimePickerValue(hostRoot, time, true, Translation::kSecondsPerDay) &&
          !Translation::SetDateTimePickerValue(hostRoot, time, true, -1),
        "a time outside the day was applied");
    RemoveWindowSubclass(root, DateTimeParentSubclass, 0xAA51);
}
