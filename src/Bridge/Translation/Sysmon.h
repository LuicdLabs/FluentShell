#pragma once

#include "SysmonInterfaces.h"

#include <windows.h>
#include <oaidl.h>
#include <wrl/client.h>

#include <optional>
#include <string>
#include <vector>

namespace FluentShell::Bridge::Translation {

// The System Monitor control (sysmon.ocx) hosts Performance Monitor's chart: a toolbar, a graph,
// a value bar and a legend, all child windows of one `SysmonCtrl` window. Pixels of the graph say
// nothing about the counters behind them, so the contract the projection trusts is the control's
// own automation interface, not a picture.
//
// The control's IDispatch is no use to a reader outside its host. It resolves names through a
// process-wide ITypeLib pointer that sysmon.ocx clears when *any* instance is destroyed, and MMC
// destroys the previous view's instance (the Performance page's embedded counter report) a moment
// after it creates the chart's: from then on GetIDsOfNames answers E_UNEXPECTED and Invoke E_FAIL
// for every instance in the process, while the control itself keeps working. The vtable interfaces
// (ISystemMonitor, ISystemMonitor2, ICounterItem, ICounterItem2) never touch that pointer, so they
// are called directly; their layout is the one in the control's own type library
// (tools\Dump-TypeLib.ps1 prints it).

// A `SysmonCtrl` window whose class sysmon.ocx in System32 registered.
bool IsSystemMonitorWindow(HWND window) noexcept;

// The control behind `sysmonWindow`, found in this process: the control registers itself as the
// window's OLE drop target, and that interface pointer belongs to the control. Trust rests on
// identity checks, not on the pointer: the object's vtable lies in System32\sysmon.ocx, it answers
// IPersist::GetClassID with the System Monitor CLSID, and IOleWindow names this window or the
// in-place site window directly around it.
// GUI thread of the control only (the object lives in that thread's apartment).
bool OpenSystemMonitor(HWND sysmonWindow, Microsoft::WRL::ComPtr<ISysmon2>& control,
    std::wstring& reason) noexcept;

// What a counter shows: its line (colour, width, style), its scale, whether the legend shows it
// and has it selected, its latest value and the statistics the value bar reports for it.
struct SysmonCounter final {
    // The counter item object's COM identity: stable while the counter exists, so a recorded history
    // finds its counter again when others are added or deleted around it.
    uintptr_t identity = 0;
    std::wstring path;
    COLORREF color = 0;
    int width = 1;
    // GDI pen style: 0 solid, 1 dash, 2 dot, 3 dash-dot, 4 dash-dot-dot.
    int lineStyle = 0;
    // The control's scale factor: a power of ten.
    int scaleFactor = 0;
    bool visible = true;
    bool selected = false;
    bool hasValue = false;
    double value = 0;
    bool hasStatistics = false;
    double maximum = 0;
    double minimum = 0;
    double average = 0;
};

struct SysmonSettings final {
    // 1 line, 2 histogram, 3 report, 4 area, 5 stacked area.
    int displayType = 1;
    int minimumScale = 0;
    int maximumScale = 100;
    float updateInterval = 1;
    int dataPointCount = 100;
    // True when the chart scrolls, false when it wraps with a time bar sweeping across it.
    bool chartScroll = false;
    bool showVerticalGrid = false;
    bool showHorizontalGrid = false;
    bool showLegend = true;
    bool showScaleLabels = true;
    bool showValueBar = true;
    bool showToolbar = true;
    bool showTimeAxisLabels = true;
    bool manualUpdate = false;
    bool readOnly = false;
    bool highlight = false;
    bool digitGrouping = true;
    // 1 current activity, 2 log files, 3 SQL log.
    int dataSource = 1;
    // What the control's Counter(index) takes as its first index: the type library does not say.
    int counterBase = 0;
    std::wstring title;
    std::wstring yAxisLabel;
    // The graph's own background, and the control's behind the labels and the legend.
    COLORREF backColor = RGB(255, 255, 255);
    COLORREF controlColor = RGB(240, 240, 240);
    COLORREF foreColor = 0;
    COLORREF gridColor = RGB(128, 128, 128);
    COLORREF timeBarColor = RGB(255, 0, 0);
    std::vector<SysmonCounter> counters;
};

// Reads the control's settings and counters. A refusal names the call that failed. GUI thread only.
bool ReadSystemMonitor(ISysmon2* control, SysmonSettings& settings, std::wstring& reason) noexcept;

// The control keeps the history of a live chart to itself: GetDataAt answers only for log and SQL
// sources. What it does publish is an event for every sample it collects, so the Bridge subscribes
// to it and keeps the values it is told about -- the values the control's own counters report at
// that moment, which are the ones it plots.
struct SysmonHistory final {
    // Sample times (milliseconds since the Unix epoch), oldest first, and for every counter of the
    // settings the history is read for, the value each of those samples recorded.
    std::vector<int64_t> times;
    std::vector<std::vector<std::optional<double>>> values;
    // Samples collected since recording began, including those no longer in `times`.
    uint64_t collected = 0;
};

// Starts recording the control's samples. Idempotent per window; the recorder lives until the window
// is destroyed, and the control owns the subscription (it drops it when it is destroyed). GUI thread only.
void TrackSystemMonitor(HWND sysmonWindow, ISysmon2* control) noexcept;

// The recorded samples of the counters in `settings` (last `capacity` at most), matched by counter
// identity. False when this window is not being recorded.
bool ReadSystemMonitorHistory(HWND sysmonWindow, const SysmonSettings& settings, size_t capacity,
    SysmonHistory& history) noexcept;

} // namespace FluentShell::Bridge::Translation
