#pragma once

#include "Sysmon.h"
#include "WindowSnapshot.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace FluentShell::Bridge::Translation {

// Performance Monitor's chart view is the System Monitor control (sysmon.ocx): one `SysmonCtrl`
// window whose children are a toolbar, the graph (`ChartGraphClass`), a value strip (`PerfStatus`)
// and a legend (`PerfLegend`). The graph and the strip are pixels the control draws itself, and the
// legend is an owner-draw list, so none of them is a control the projection can describe by its
// window. What they show is the control's own state, read through its automation interface
// (Sysmon.h), and that is what the three kinds below carry.
//
// Every function is for the GUI thread of the window it is given.

// What a chart part's probe checks: the window is the class sysmon.ocx registered, sits directly in
// a System Monitor control that answers through its interface, and shows a chart the projection can
// draw (current-activity data, line or histogram). A refusal names why. A probe also starts
// recording the control's samples, so the history is there by the time the part is projected.
bool ProbePerformanceGraph(HWND window, std::wstring& reason) noexcept;
bool ProbePerformanceLegend(HWND window, std::wstring& reason) noexcept;
bool ProbePerformanceValueBar(HWND window, std::wstring& reason) noexcept;

bool ReadPerformanceGraph(HWND window, PerformanceGraphSnapshot& graph, std::wstring& reason) noexcept;
bool ReadPerformanceLegend(HWND window, PerformanceLegendSnapshot& legend, std::wstring& reason) noexcept;
bool ReadPerformanceValueBar(HWND window, PerformanceValueBarSnapshot& bar, std::wstring& reason) noexcept;

// The legend's own gestures: a row selects its counter, and the Show box decides whether the graph
// plots it. Both go through the native list, so the control reacts exactly as it does to a click,
// and both report whether the control's state ended up as asked.
bool SelectPerformanceCounter(HWND legendWindow, int index) noexcept;
bool SetPerformanceCounterVisible(HWND legendWindow, int index, bool visible) noexcept;

// A child the chart parts own: the graph's drawing surface, the legend's list and its header.
bool IsPerformanceChartImplementationChild(HWND window) noexcept;

// The graph's slots, filled from the recorded samples the way the control fills them: a wrapping
// chart writes sample n into slot n mod N and leaves the previous pass right of its time bar until
// it is overwritten, a scrolling one keeps the newest sample in the last slot. Also the times the
// axis shows. `graph` has one series per counter of `settings` already (the values are replaced).
void LayOutPerformanceSamples(const SysmonSettings& settings, const SysmonHistory& history,
    PerformanceGraphSnapshot& graph);

// The legend draws the scale factor "1.000" the list holds with only the digits it needs ("1.0").
std::wstring TrimPerformanceScale(const std::wstring& text);

// "m:ss", "h:mm:ss" or "d:hh:mm:ss": the duration the value strip shows.
std::wstring FormatPerformanceDuration(int64_t seconds);

} // namespace FluentShell::Bridge::Translation
