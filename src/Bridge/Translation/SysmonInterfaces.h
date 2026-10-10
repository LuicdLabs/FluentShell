#pragma once

#include <windows.h>
#include <oaidl.h>
#include <olectl.h>
#include <unknwn.h>

// The vtable interfaces of the System Monitor control (sysmon.ocx), declared by hand. The Windows
// SDK ships no header for them. Each interface lists its methods in vtable order exactly as the
// control's type library does (tools\Dump-TypeLib.ps1 C:\Windows\System32\sysmon.ocx), including
// the ones this project never calls: a missing placeholder would shift every later slot.
//
// Only calls by vtable are safe from outside the control's host. See Sysmon.h.

namespace FluentShell::Bridge::Translation {

inline constexpr GUID kIidSysmon2 =
    {0x08e3206a, 0x5fd2, 0x4fde, {0xa8, 0xa5, 0x8c, 0xb3, 0xb6, 0x3d, 0x26, 0x77}};
inline constexpr GUID kIidSysmonCounter2 =
    {0xeefcd4e1, 0xea1c, 0x4435, {0xb7, 0xf4, 0xe3, 0x41, 0xba, 0x03, 0xb4, 0xf9}};
// {C4D2D8E0-D1DD-11CE-940F-008029004347}: the control's class.
inline constexpr CLSID kClsidSystemMonitor =
    {0xc4d2d8e0, 0xd1dd, 0x11ce, {0x94, 0x0f, 0x00, 0x80, 0x29, 0x00, 0x43, 0x47}};
// The control's event sources: the dispinterface it fires and the vtable twin its type library lists.
inline constexpr GUID kDiidSysmonEvents =
    {0x84979930, 0x4ab3, 0x11cf, {0x94, 0x3a, 0x00, 0x80, 0x29, 0x00, 0x43, 0x47}};
inline constexpr GUID kIidSysmonEvents =
    {0xee660ea0, 0x4abd, 0x11cf, {0x94, 0x3a, 0x00, 0x80, 0x29, 0x00, 0x43, 0x47}};
// DISystemMonitorEvents dispatch ids.
inline constexpr DISPID kSysmonEventCounterSelected = 1;
inline constexpr DISPID kSysmonEventCounterAdded = 2;
inline constexpr DISPID kSysmonEventCounterDeleted = 3;
inline constexpr DISPID kSysmonEventSampleCollected = 4;
inline constexpr DISPID kSysmonEventDoubleClick = 5;

// ICounters: a dual interface, so its vtable is IDispatch's followed by these.
struct ISysmonCounters : IDispatch {
    virtual HRESULT STDMETHODCALLTYPE get_Count(long* count) = 0;              // 7
    virtual HRESULT STDMETHODCALLTYPE get__NewEnum(IUnknown** enumerator) = 0; // 8
    virtual HRESULT STDMETHODCALLTYPE get_Item(VARIANT index, IDispatch** item) = 0; // 9
    virtual HRESULT STDMETHODCALLTYPE Add(BSTR path, IDispatch** item) = 0;    // 10
    virtual HRESULT STDMETHODCALLTYPE Remove(VARIANT index) = 0;               // 11
};

// ICounterItem (slots 3-14) and ICounterItem2 (15-19), which derives from it.
struct ISysmonCounter2 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Value(double* value) = 0;            // 3
    virtual HRESULT STDMETHODCALLTYPE put_Color(OLE_COLOR color) = 0;          // 4
    virtual HRESULT STDMETHODCALLTYPE get_Color(OLE_COLOR* color) = 0;         // 5
    virtual HRESULT STDMETHODCALLTYPE put_Width(int width) = 0;                // 6
    virtual HRESULT STDMETHODCALLTYPE get_Width(int* width) = 0;               // 7
    virtual HRESULT STDMETHODCALLTYPE put_LineStyle(int style) = 0;            // 8
    virtual HRESULT STDMETHODCALLTYPE get_LineStyle(int* style) = 0;           // 9
    virtual HRESULT STDMETHODCALLTYPE put_ScaleFactor(int factor) = 0;         // 10
    virtual HRESULT STDMETHODCALLTYPE get_ScaleFactor(int* factor) = 0;        // 11
    virtual HRESULT STDMETHODCALLTYPE get_Path(BSTR* path) = 0;                // 12
    virtual HRESULT STDMETHODCALLTYPE GetValue(double* value, long* status) = 0; // 13
    virtual HRESULT STDMETHODCALLTYPE GetStatistics(double* maximum, double* minimum,
        double* average, long* status) = 0;                                    // 14
    virtual HRESULT STDMETHODCALLTYPE put_Selected(BOOL selected) = 0;         // 15
    virtual HRESULT STDMETHODCALLTYPE get_Selected(BOOL* selected) = 0;        // 16
    virtual HRESULT STDMETHODCALLTYPE put_Visible(BOOL visible) = 0;           // 17
    virtual HRESULT STDMETHODCALLTYPE get_Visible(BOOL* visible) = 0;          // 18
    // iWhich: 1 average, 2 minimum, 3 maximum, 4 time, 5 count. Only for log and SQL data sources: a
    // live chart's call fails with E_UNEXPECTED (the history is recorded from OnSampleCollected).
    virtual HRESULT STDMETHODCALLTYPE GetDataAt(int index, int which, VARIANT* value) = 0; // 19
};

// ISystemMonitor (slots 3-78) and ISystemMonitor2 (79-98), which derives from it.
struct ISysmon2 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_Appearance(int*) = 0;                // 3
    virtual HRESULT STDMETHODCALLTYPE put_Appearance(int) = 0;                 // 4
    virtual HRESULT STDMETHODCALLTYPE get_BackColor(OLE_COLOR* color) = 0;     // 5
    virtual HRESULT STDMETHODCALLTYPE put_BackColor(OLE_COLOR) = 0;            // 6
    virtual HRESULT STDMETHODCALLTYPE get_BorderStyle(int*) = 0;               // 7
    virtual HRESULT STDMETHODCALLTYPE put_BorderStyle(int) = 0;                // 8
    virtual HRESULT STDMETHODCALLTYPE get_ForeColor(OLE_COLOR* color) = 0;     // 9
    virtual HRESULT STDMETHODCALLTYPE put_ForeColor(OLE_COLOR) = 0;            // 10
    virtual HRESULT STDMETHODCALLTYPE get_Font(IDispatch**) = 0;               // 11
    virtual HRESULT STDMETHODCALLTYPE putref_Font(IDispatch*) = 0;             // 12
    virtual HRESULT STDMETHODCALLTYPE get_Counters(ISysmonCounters** counters) = 0; // 13
    virtual HRESULT STDMETHODCALLTYPE put_ShowVerticalGrid(BOOL) = 0;          // 14
    virtual HRESULT STDMETHODCALLTYPE get_ShowVerticalGrid(BOOL*) = 0;         // 15
    virtual HRESULT STDMETHODCALLTYPE put_ShowHorizontalGrid(BOOL) = 0;        // 16
    virtual HRESULT STDMETHODCALLTYPE get_ShowHorizontalGrid(BOOL*) = 0;       // 17
    virtual HRESULT STDMETHODCALLTYPE put_ShowLegend(BOOL) = 0;                // 18
    virtual HRESULT STDMETHODCALLTYPE get_ShowLegend(BOOL*) = 0;               // 19
    virtual HRESULT STDMETHODCALLTYPE put_ShowScaleLabels(BOOL) = 0;           // 20
    virtual HRESULT STDMETHODCALLTYPE get_ShowScaleLabels(BOOL*) = 0;          // 21
    virtual HRESULT STDMETHODCALLTYPE put_ShowValueBar(BOOL) = 0;              // 22
    virtual HRESULT STDMETHODCALLTYPE get_ShowValueBar(BOOL*) = 0;             // 23
    virtual HRESULT STDMETHODCALLTYPE put_MaximumScale(int) = 0;               // 24
    virtual HRESULT STDMETHODCALLTYPE get_MaximumScale(int*) = 0;              // 25
    virtual HRESULT STDMETHODCALLTYPE put_MinimumScale(int) = 0;               // 26
    virtual HRESULT STDMETHODCALLTYPE get_MinimumScale(int*) = 0;              // 27
    virtual HRESULT STDMETHODCALLTYPE put_UpdateInterval(float) = 0;           // 28
    virtual HRESULT STDMETHODCALLTYPE get_UpdateInterval(float*) = 0;          // 29
    virtual HRESULT STDMETHODCALLTYPE put_DisplayType(int) = 0;                // 30
    virtual HRESULT STDMETHODCALLTYPE get_DisplayType(int*) = 0;               // 31
    virtual HRESULT STDMETHODCALLTYPE put_ManualUpdate(BOOL) = 0;              // 32
    virtual HRESULT STDMETHODCALLTYPE get_ManualUpdate(BOOL*) = 0;             // 33
    virtual HRESULT STDMETHODCALLTYPE put_GraphTitle(BSTR) = 0;                // 34
    virtual HRESULT STDMETHODCALLTYPE get_GraphTitle(BSTR*) = 0;               // 35
    virtual HRESULT STDMETHODCALLTYPE put_YAxisLabel(BSTR) = 0;                // 36
    virtual HRESULT STDMETHODCALLTYPE get_YAxisLabel(BSTR*) = 0;               // 37
    virtual HRESULT STDMETHODCALLTYPE CollectSample() = 0;                     // 38
    virtual HRESULT STDMETHODCALLTYPE UpdateGraph() = 0;                       // 39
    virtual HRESULT STDMETHODCALLTYPE BrowseCounters() = 0;                    // 40
    virtual HRESULT STDMETHODCALLTYPE DisplayProperties() = 0;                 // 41
    // The counter items are ICounterItem pointers: asked for ISysmonCounter2 by QueryInterface, not cast.
    virtual HRESULT STDMETHODCALLTYPE Counter(int index, IUnknown** counter) = 0;        // 42
    virtual HRESULT STDMETHODCALLTYPE AddCounter(BSTR path, IUnknown** counter) = 0;     // 43
    virtual HRESULT STDMETHODCALLTYPE DeleteCounter(IUnknown*) = 0;            // 44
    virtual HRESULT STDMETHODCALLTYPE get_BackColorCtl(OLE_COLOR*) = 0;        // 45
    virtual HRESULT STDMETHODCALLTYPE put_BackColorCtl(OLE_COLOR) = 0;         // 46
    virtual HRESULT STDMETHODCALLTYPE put_LogFileName(BSTR) = 0;               // 47
    virtual HRESULT STDMETHODCALLTYPE get_LogFileName(BSTR*) = 0;              // 48
    virtual HRESULT STDMETHODCALLTYPE put_LogViewStart(DATE) = 0;              // 49
    virtual HRESULT STDMETHODCALLTYPE get_LogViewStart(DATE*) = 0;             // 50
    virtual HRESULT STDMETHODCALLTYPE put_LogViewStop(DATE) = 0;               // 51
    virtual HRESULT STDMETHODCALLTYPE get_LogViewStop(DATE*) = 0;              // 52
    virtual HRESULT STDMETHODCALLTYPE get_GridColor(OLE_COLOR*) = 0;           // 53
    virtual HRESULT STDMETHODCALLTYPE put_GridColor(OLE_COLOR) = 0;            // 54
    virtual HRESULT STDMETHODCALLTYPE get_TimeBarColor(OLE_COLOR*) = 0;        // 55
    virtual HRESULT STDMETHODCALLTYPE put_TimeBarColor(OLE_COLOR) = 0;         // 56
    virtual HRESULT STDMETHODCALLTYPE get_Highlight(BOOL*) = 0;                // 57
    virtual HRESULT STDMETHODCALLTYPE put_Highlight(BOOL) = 0;                 // 58
    virtual HRESULT STDMETHODCALLTYPE get_ShowToolbar(BOOL*) = 0;              // 59
    virtual HRESULT STDMETHODCALLTYPE put_ShowToolbar(BOOL) = 0;               // 60
    virtual HRESULT STDMETHODCALLTYPE Paste() = 0;                             // 61
    virtual HRESULT STDMETHODCALLTYPE Copy() = 0;                              // 62
    virtual HRESULT STDMETHODCALLTYPE Reset() = 0;                             // 63
    virtual HRESULT STDMETHODCALLTYPE put_ReadOnly(BOOL) = 0;                  // 64
    virtual HRESULT STDMETHODCALLTYPE get_ReadOnly(BOOL*) = 0;                 // 65
    virtual HRESULT STDMETHODCALLTYPE put_ReportValueType(int) = 0;            // 66
    virtual HRESULT STDMETHODCALLTYPE get_ReportValueType(int*) = 0;           // 67
    virtual HRESULT STDMETHODCALLTYPE put_MonitorDuplicateInstances(BOOL) = 0; // 68
    virtual HRESULT STDMETHODCALLTYPE get_MonitorDuplicateInstances(BOOL*) = 0; // 69
    virtual HRESULT STDMETHODCALLTYPE put_DisplayFilter(int) = 0;              // 70
    virtual HRESULT STDMETHODCALLTYPE get_DisplayFilter(int*) = 0;             // 71
    virtual HRESULT STDMETHODCALLTYPE get_LogFiles(IDispatch**) = 0;           // 72
    virtual HRESULT STDMETHODCALLTYPE put_DataSourceType(int) = 0;             // 73
    virtual HRESULT STDMETHODCALLTYPE get_DataSourceType(int*) = 0;            // 74
    virtual HRESULT STDMETHODCALLTYPE put_SqlDsnName(BSTR) = 0;                // 75
    virtual HRESULT STDMETHODCALLTYPE get_SqlDsnName(BSTR*) = 0;               // 76
    virtual HRESULT STDMETHODCALLTYPE put_SqlLogSetName(BSTR) = 0;             // 77
    virtual HRESULT STDMETHODCALLTYPE get_SqlLogSetName(BSTR*) = 0;            // 78
    // ISystemMonitor2
    virtual HRESULT STDMETHODCALLTYPE put_EnableDigitGrouping(BOOL) = 0;       // 79
    virtual HRESULT STDMETHODCALLTYPE get_EnableDigitGrouping(BOOL*) = 0;      // 80
    virtual HRESULT STDMETHODCALLTYPE put_EnableToolTips(BOOL) = 0;            // 81
    virtual HRESULT STDMETHODCALLTYPE get_EnableToolTips(BOOL*) = 0;           // 82
    virtual HRESULT STDMETHODCALLTYPE put_ShowTimeAxisLabels(BOOL) = 0;        // 83
    virtual HRESULT STDMETHODCALLTYPE get_ShowTimeAxisLabels(BOOL*) = 0;       // 84
    virtual HRESULT STDMETHODCALLTYPE put_ChartScroll(BOOL) = 0;               // 85
    virtual HRESULT STDMETHODCALLTYPE get_ChartScroll(BOOL*) = 0;              // 86
    virtual HRESULT STDMETHODCALLTYPE put_DataPointCount(int) = 0;             // 87
    virtual HRESULT STDMETHODCALLTYPE get_DataPointCount(int*) = 0;            // 88
    virtual HRESULT STDMETHODCALLTYPE ScaleToFit(BOOL selectedOnly) = 0;       // 89
    virtual HRESULT STDMETHODCALLTYPE SaveAs(BSTR, int) = 0;                   // 90
    virtual HRESULT STDMETHODCALLTYPE Relog(BSTR, int, int) = 0;               // 91
    virtual HRESULT STDMETHODCALLTYPE ClearData() = 0;                         // 92
    virtual HRESULT STDMETHODCALLTYPE get_LogSourceStartTime(DATE*) = 0;       // 93
    virtual HRESULT STDMETHODCALLTYPE get_LogSourceStopTime(DATE*) = 0;        // 94
    virtual HRESULT STDMETHODCALLTYPE SetLogViewRange(DATE, DATE) = 0;         // 95
    virtual HRESULT STDMETHODCALLTYPE GetLogViewRange(DATE*, DATE*) = 0;       // 96
    virtual HRESULT STDMETHODCALLTYPE BatchingLock(BOOL, int) = 0;             // 97
    virtual HRESULT STDMETHODCALLTYPE LoadSettings(BSTR) = 0;                  // 98
};

} // namespace FluentShell::Bridge::Translation
