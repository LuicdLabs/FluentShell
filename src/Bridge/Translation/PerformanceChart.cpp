#include "PerformanceChart.h"
#include "AccessibleIsland.h"
#include "Sysmon.h"
#include "../../Common/FluentShell.h"

#include <commctrl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace FluentShell::Bridge::Translation {
namespace {
using Microsoft::WRL::ComPtr;

constexpr int kLineGraph = 1;
constexpr int kHistogram = 2;
constexpr int kCurrentActivity = 1;
constexpr int kMaximumSeries = 64;
constexpr int kMinimumSamples = 2;
// The control's own bound: it refuses a duration that makes more than this many data points.
constexpr int kMaximumSamples = 1000;
constexpr int kMinimumIntervalMs = 100;
constexpr int kMaximumIntervalMs = 3'600'000;

// sysmon.ocx string table ids of the value strip's labels (the control owns the localized text).
constexpr UINT kStringLast = 128;
constexpr UINT kStringAverage = 129;
constexpr UINT kStringMinimum = 130;
constexpr UINT kStringMaximum = 131;
constexpr UINT kStringDuration = 132;

bool Refuse(std::wstring& reason, const wchar_t* text) {
    reason = text;
    return false;
}

bool HasClass(HWND window, const wchar_t* className) noexcept {
    wchar_t name[64]{};
    return window && GetClassNameW(window, name, static_cast<int>(std::size(name))) > 0 && wcscmp(name, className) == 0;
}

uint32_t Rgb(COLORREF color) noexcept {
    return (static_cast<uint32_t>(GetRValue(color)) << 16) | (static_cast<uint32_t>(GetGValue(color)) << 8) |
        static_cast<uint32_t>(GetBValue(color));
}

const wchar_t* StyleName(int style) noexcept {
    switch (style) {
    case 1: return L"dash";
    case 2: return L"dot";
    case 3: return L"dashdot";
    case 4: return L"dashdotdot";
    default: return L"solid";
    }
}

// A chart part and the control around it, with the settings that say the projection can draw it.
struct ChartPart final {
    HWND control = nullptr;
    ComPtr<ISysmon2> sysmon;
    SysmonSettings settings;
};

bool OpenChartPart(HWND part, const wchar_t* className, ChartPart& chart, std::wstring& reason) {
    if (!HasClass(part, className)) return Refuse(reason, L"the window is not the expected System Monitor part");
    if (!WindowClassRegisteredBySystemModule(part, L"sysmon.ocx"))
        return Refuse(reason, L"the part's class is not registered by System32\\sysmon.ocx");
    chart.control = GetParent(part);
    if (!IsSystemMonitorWindow(chart.control))
        return Refuse(reason, L"the part does not sit in a System Monitor control");
    if (!OpenSystemMonitor(chart.control, chart.sysmon, reason)) return false;
    if (!ReadSystemMonitor(chart.sysmon.Get(), chart.settings, reason)) return false;
    const SysmonSettings& settings = chart.settings;
    if (settings.dataSource != kCurrentActivity)
        return Refuse(reason, L"the chart shows log or database data, not current activity");
    if (settings.displayType != kLineGraph && settings.displayType != kHistogram)
        return Refuse(reason, L"only the line and histogram chart types are supported");
    if (settings.dataPointCount < kMinimumSamples || settings.dataPointCount > kMaximumSamples)
        return Refuse(reason, L"the chart's data point count is outside its range");
    if (settings.counters.size() > static_cast<size_t>(kMaximumSeries))
        return Refuse(reason, L"the chart plots more counters than the projection carries");
    const double intervalMs = static_cast<double>(settings.updateInterval) * 1000.0;
    if (!std::isfinite(intervalMs) || intervalMs < kMinimumIntervalMs || intervalMs > kMaximumIntervalMs)
        return Refuse(reason, L"the chart's update interval is outside its range");
    if (settings.maximumScale <= settings.minimumScale)
        return Refuse(reason, L"the chart's scale is not a range");
    return true;
}

bool ProbePart(HWND window, const wchar_t* className, std::wstring& reason) noexcept {
    try {
        ChartPart chart;
        if (!OpenChartPart(window, className, chart, reason)) return false;
        // The history starts when the control is first seen, so by the time the view is projected a
        // chart that has been running for a moment is already recorded.
        TrackSystemMonitor(chart.control, chart.sysmon.Get());
        return true;
    } catch (...) {
        reason = L"reading the System Monitor control failed";
        return false;
    }
}

int IntervalMs(const SysmonSettings& settings) noexcept {
    return static_cast<int>(std::llround(static_cast<double>(settings.updateInterval) * 1000.0));
}

} // namespace

void LayOutPerformanceSamples(const SysmonSettings& settings, const SysmonHistory& history, PerformanceGraphSnapshot& graph) {
    const size_t slots = static_cast<size_t>(settings.dataPointCount);
    const size_t retained = history.times.size();
    const uint64_t collected = history.collected;
    const int intervalMs = graph.intervalMs;
    for (auto& series : graph.series) series.values.assign(slots, std::nullopt);
    graph.barSlot = -1;
    graph.wrapped = false;
    if (retained == 0 || collected == 0) {
        const int64_t now = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        graph.startTimeMs = now;
        graph.endTimeMs = now + static_cast<int64_t>(intervalMs) * (static_cast<int64_t>(slots) - 1);
        return;
    }
    // Global sample number of retained[0]: the retained samples are the newest `retained`.
    const uint64_t first = collected - retained;
    if (!settings.chartScroll) {
        for (size_t at = 0; at < retained; ++at) {
            const size_t slot = static_cast<size_t>((first + at) % slots);
            for (size_t counter = 0; counter < graph.series.size(); ++counter)
                graph.series[counter].values[slot] = history.values[counter][at];
        }
        graph.barSlot = static_cast<int>((collected - 1) % slots);
        graph.wrapped = collected > slots;
        // The pass in progress began with the sample that fell in slot 0.
        const uint64_t passStart = ((collected - 1) / slots) * slots;
        const uint64_t at = passStart >= first ? passStart - first : 0;
        graph.startTimeMs = history.times[static_cast<size_t>(std::min<uint64_t>(at, retained - 1))];
        graph.endTimeMs = graph.startTimeMs + static_cast<int64_t>(intervalMs) * (static_cast<int64_t>(slots) - 1);
        return;
    }
    // Scrolling: the newest sample sits in the last slot, older ones to its left.
    for (size_t at = 0; at < retained; ++at) {
        const int64_t slot = static_cast<int64_t>(slots) - static_cast<int64_t>(retained) + static_cast<int64_t>(at);
        if (slot < 0) continue;
        for (size_t counter = 0; counter < graph.series.size(); ++counter)
            graph.series[counter].values[static_cast<size_t>(slot)] = history.values[counter][at];
    }
    graph.endTimeMs = history.times[retained - 1];
    graph.startTimeMs = graph.endTimeMs - static_cast<int64_t>(intervalMs) * (static_cast<int64_t>(slots) - 1);
}

std::wstring TrimPerformanceScale(const std::wstring& text) {
    // The list holds the factor as "1.000"; the legend draws it with only the digits it needs.
    wchar_t* end = nullptr;
    const double value = wcstod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(value)) return text;
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.6f", value);
    std::wstring result = buffer;
    while (result.size() > 2 && result.back() == L'0' && result[result.size() - 2] != L'.') result.pop_back();
    return result;
}

// "m:ss", "h:mm:ss" or "d:hh:mm:ss": the duration the value strip shows.
std::wstring FormatPerformanceDuration(int64_t seconds) {
    if (seconds < 0) seconds = 0;
    const int64_t days = seconds / 86400;
    const int64_t hours = seconds % 86400 / 3600;
    const int64_t minutes = seconds % 3600 / 60;
    const int64_t rest = seconds % 60;
    wchar_t text[64]{};
    if (days > 0) swprintf_s(text, L"%lld:%02lld:%02lld:%02lld", days, hours, minutes, rest);
    else if (hours > 0) swprintf_s(text, L"%lld:%02lld:%02lld", hours, minutes, rest);
    else swprintf_s(text, L"%lld:%02lld", minutes, rest);
    return text;
}

namespace {

std::wstring FormatValue(double value, bool grouping) {
    wchar_t plain[96]{};
    swprintf_s(plain, L"%.3f", value);
    wchar_t decimal[8]{};
    wchar_t thousand[8]{};
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, decimal, static_cast<int>(std::size(decimal))) ||
        !GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, thousand, static_cast<int>(std::size(thousand)))) {
        return plain;
    }
    NUMBERFMTW format{};
    format.NumDigits = 3;
    format.LeadingZero = 1;
    format.Grouping = grouping ? 3 : 0;
    format.lpDecimalSep = decimal;
    format.lpThousandSep = thousand;
    format.NegativeOrder = 1;
    wchar_t formatted[128]{};
    if (GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT, 0, plain, &format, formatted, static_cast<int>(std::size(formatted))) <= 0)
        return plain;
    return formatted;
}

HWND LegendList(HWND legend) noexcept {
    return FindWindowExW(legend, nullptr, WC_LISTVIEWW, nullptr);
}

std::wstring ListText(HWND list, int row, int column) {
    wchar_t text[256]{};
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = text;
    item.cchTextMax = static_cast<int>(std::size(text));
    const int length = static_cast<int>(SendMessageW(list, LVM_GETITEMTEXTW, static_cast<WPARAM>(row),
        reinterpret_cast<LPARAM>(&item)));
    return std::wstring(text, static_cast<size_t>((std::max)(0, (std::min)(length, static_cast<int>(std::size(text) - 1)))));
}

} // namespace

bool ProbePerformanceGraph(HWND window, std::wstring& reason) noexcept {
    return ProbePart(window, L"ChartGraphClass", reason);
}

bool ProbePerformanceLegend(HWND window, std::wstring& reason) noexcept {
    return ProbePart(window, L"PerfLegend", reason);
}

bool ProbePerformanceValueBar(HWND window, std::wstring& reason) noexcept {
    return ProbePart(window, L"PerfStatus", reason);
}

bool ReadPerformanceGraph(HWND window, PerformanceGraphSnapshot& graph, std::wstring& reason) noexcept {
    try {
        ChartPart chart;
        if (!OpenChartPart(window, L"ChartGraphClass", chart, reason)) return false;
        const SysmonSettings& settings = chart.settings;
        TrackSystemMonitor(chart.control, chart.sysmon.Get());
        SysmonHistory history;
        if (!ReadSystemMonitorHistory(chart.control, settings, static_cast<size_t>(settings.dataPointCount), history))
            return Refuse(reason, L"the chart's samples are not being recorded");
        graph = {};
        graph.displayType = settings.displayType == kHistogram ? L"histogram" : L"line";
        graph.minimum = settings.minimumScale;
        graph.maximum = settings.maximumScale;
        graph.sampleCount = settings.dataPointCount;
        graph.intervalMs = IntervalMs(settings);
        graph.scroll = settings.chartScroll;
        graph.showVerticalGrid = settings.showVerticalGrid;
        graph.showHorizontalGrid = settings.showHorizontalGrid;
        graph.showScaleLabels = settings.showScaleLabels;
        graph.showTimeLabels = settings.showTimeAxisLabels;
        graph.title = settings.title;
        graph.yAxisLabel = settings.yAxisLabel;
        graph.backColor = Rgb(settings.backColor);
        graph.gridColor = Rgb(settings.gridColor);
        graph.textColor = Rgb(settings.foreColor);
        graph.barColor = Rgb(settings.timeBarColor);
        graph.controlColor = Rgb(settings.controlColor);
        for (const SysmonCounter& counter : settings.counters) {
            PerformanceSeriesSnapshot series;
            series.name = counter.path;
            series.color = Rgb(counter.color);
            series.width = std::clamp(counter.width, 1, 8);
            series.style = StyleName(counter.lineStyle);
            // The control multiplies a value by ten to the scale factor before it plots it.
            series.scale = std::pow(10.0, std::clamp(counter.scaleFactor, -20, 20));
            series.visible = counter.visible;
            series.highlighted = settings.highlight && counter.selected;
            graph.series.push_back(std::move(series));
        }
        if (history.values.size() != graph.series.size())
            return Refuse(reason, L"the chart's recorded samples do not match its counters");
        LayOutPerformanceSamples(settings, history, graph);
        return true;
    } catch (...) {
        reason = L"reading the System Monitor graph failed";
        return false;
    }
}

bool ReadPerformanceLegend(HWND window, PerformanceLegendSnapshot& legend, std::wstring& reason) noexcept {
    try {
        ChartPart chart;
        if (!OpenChartPart(window, L"PerfLegend", chart, reason)) return false;
        const SysmonSettings& settings = chart.settings;
        const HWND list = LegendList(window);
        if (!list) return Refuse(reason, L"the legend has no list");
        const HWND header = ListView_GetHeader(list);
        const int columns = header ? Header_GetItemCount(header) : 0;
        if (columns < 3 || columns > 16) return Refuse(reason, L"the legend's columns are outside the supported range");
        if (ListView_GetItemCount(list) != static_cast<int>(settings.counters.size()))
            return Refuse(reason, L"the legend lists other counters than the chart plots");
        legend = {};
        legend.displayType = settings.displayType == kHistogram ? L"histogram" : L"line";
        for (int column = 0; column < columns; ++column) {
            wchar_t title[128]{};
            HDITEMW item{};
            item.mask = HDI_TEXT | HDI_WIDTH;
            item.pszText = title;
            item.cchTextMax = static_cast<int>(std::size(title));
            if (!Header_GetItem(header, column, &item)) return Refuse(reason, L"a legend column header could not be read");
            PerformanceLegendColumnSnapshot entry;
            entry.title = title;
            entry.width = std::clamp(item.cxy, 0, 16384);
            legend.columns.push_back(std::move(entry));
        }
        for (size_t row = 0; row < settings.counters.size(); ++row) {
            const SysmonCounter& counter = settings.counters[row];
            PerformanceLegendRowSnapshot entry;
            entry.visible = counter.visible;
            entry.color = Rgb(counter.color);
            entry.width = std::clamp(counter.width, 1, 8);
            entry.style = StyleName(counter.lineStyle);
            entry.selected = counter.selected;
            for (int column = 2; column < columns; ++column) {
                std::wstring text = ListText(list, static_cast<int>(row), column);
                if (column == 2) text = TrimPerformanceScale(text);
                entry.cells.push_back(std::move(text));
            }
            legend.rows.push_back(std::move(entry));
        }
        return true;
    } catch (...) {
        reason = L"reading the System Monitor legend failed";
        return false;
    }
}

bool ReadPerformanceValueBar(HWND window, PerformanceValueBarSnapshot& bar, std::wstring& reason) noexcept {
    try {
        ChartPart chart;
        if (!OpenChartPart(window, L"PerfStatus", chart, reason)) return false;
        const SysmonSettings& settings = chart.settings;
        const HMODULE module = GetModuleHandleW(L"sysmon.ocx");
        if (!module) return Refuse(reason, L"System32\\sysmon.ocx is not loaded");
        const auto label = [module](UINT id) {
            wchar_t text[64]{};
            const int length = LoadStringW(module, id, text, static_cast<int>(std::size(text)));
            return length > 0 ? std::wstring(text, static_cast<size_t>(length)) : std::wstring();
        };
        const std::wstring labels[5] = { label(kStringLast), label(kStringAverage), label(kStringMinimum),
            label(kStringMaximum), label(kStringDuration) };
        for (const auto& text : labels)
            if (text.empty()) return Refuse(reason, L"the control has no label text for its value strip");
        // The strip reports the counter the legend has selected.
        const SysmonCounter* selected = nullptr;
        for (const SysmonCounter& counter : settings.counters)
            if (counter.selected) {
                selected = &counter;
                break;
            }
        const auto value = [&](bool available, double number) {
            return available ? FormatValue(number, settings.digitGrouping) : std::wstring();
        };
        bar = {};
        const bool counted = selected != nullptr;
        bar.fields.push_back({ labels[0], value(counted && selected->hasValue, selected ? selected->value : 0) });
        bar.fields.push_back({ labels[1], value(counted && selected->hasStatistics, selected ? selected->average : 0) });
        bar.fields.push_back({ labels[2], value(counted && selected->hasStatistics, selected ? selected->minimum : 0) });
        bar.fields.push_back({ labels[3], value(counted && selected->hasStatistics, selected ? selected->maximum : 0) });
        const int64_t seconds = static_cast<int64_t>(std::llround(
            static_cast<double>(settings.dataPointCount) * static_cast<double>(settings.updateInterval)));
        bar.fields.push_back({ labels[4], FormatPerformanceDuration(seconds) });
        return true;
    } catch (...) {
        reason = L"reading the System Monitor value strip failed";
        return false;
    }
}

bool SelectPerformanceCounter(HWND legendWindow, int index) noexcept {
    try {
        ChartPart chart;
        std::wstring reason;
        if (!OpenChartPart(legendWindow, L"PerfLegend", chart, reason)) return false;
        const HWND list = LegendList(legendWindow);
        if (!list || index < 0 || index >= ListView_GetItemCount(list) ||
            static_cast<size_t>(index) >= chart.settings.counters.size()) return false;
        // What a click on the row does: the row is selected and focused, and the control's own
        // notification handler follows.
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED);
        ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SysmonSettings after;
        if (ReadSystemMonitor(chart.sysmon.Get(), after, reason) && static_cast<size_t>(index) < after.counters.size() &&
            after.counters[static_cast<size_t>(index)].selected) return true;
        // The list moved but the control did not follow: ask the control directly.
        ComPtr<IUnknown> item;
        ComPtr<ISysmonCounter2> counter;
        if (FAILED(chart.sysmon->Counter(index + chart.settings.counterBase, item.GetAddressOf())) || !item ||
            FAILED(item->QueryInterface(kIidSysmonCounter2, reinterpret_cast<void**>(counter.GetAddressOf()))) ||
            FAILED(counter->put_Selected(TRUE))) return false;
        return ReadSystemMonitor(chart.sysmon.Get(), after, reason) && static_cast<size_t>(index) < after.counters.size() &&
            after.counters[static_cast<size_t>(index)].selected;
    } catch (...) {
        return false;
    }
}

bool SetPerformanceCounterVisible(HWND legendWindow, int index, bool visible) noexcept {
    try {
        ChartPart chart;
        std::wstring reason;
        if (!OpenChartPart(legendWindow, L"PerfLegend", chart, reason)) return false;
        const HWND list = LegendList(legendWindow);
        if (!list || index < 0 || index >= ListView_GetItemCount(list) ||
            static_cast<size_t>(index) >= chart.settings.counters.size()) return false;
        // What a click on the Show box does: the row's state image flips, and the control's own
        // notification handler follows.
        ListView_SetItemState(list, index, INDEXTOSTATEIMAGEMASK(visible ? 2 : 1), LVIS_STATEIMAGEMASK);
        SysmonSettings after;
        if (ReadSystemMonitor(chart.sysmon.Get(), after, reason) && static_cast<size_t>(index) < after.counters.size() &&
            after.counters[static_cast<size_t>(index)].visible == visible) return true;
        ComPtr<IUnknown> item;
        ComPtr<ISysmonCounter2> counter;
        if (FAILED(chart.sysmon->Counter(index + chart.settings.counterBase, item.GetAddressOf())) || !item ||
            FAILED(item->QueryInterface(kIidSysmonCounter2, reinterpret_cast<void**>(counter.GetAddressOf()))) ||
            FAILED(counter->put_Visible(visible ? TRUE : FALSE))) return false;
        return ReadSystemMonitor(chart.sysmon.Get(), after, reason) && static_cast<size_t>(index) < after.counters.size() &&
            after.counters[static_cast<size_t>(index)].visible == visible;
    } catch (...) {
        return false;
    }
}

bool IsPerformanceChartImplementationChild(HWND window) noexcept {
    try {
        const HWND parent = window ? GetParent(window) : nullptr;
        if (!parent) return false;
        // The graph's surface is an ATL window of the Windows visualization components (wvc.dll),
        // which the control hosts inside its own graph window.
        if (HasClass(parent, L"ChartGraphClass"))
            return WindowClassRegisteredBySystemModule(parent, L"sysmon.ocx") &&
                WindowClassRegisteredBySystemModule(window, L"wvc.dll");
        if (HasClass(parent, L"PerfLegend"))
            return HasClass(window, WC_LISTVIEWW) && WindowClassRegisteredBySystemModule(parent, L"sysmon.ocx");
        return false;
    } catch (...) {
        return false;
    }
}

} // namespace FluentShell::Bridge::Translation
