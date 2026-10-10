#include "Sysmon.h"
#include "AccessibleIsland.h"
#include "../../Common/FluentShell.h"

#include <commctrl.h>
#include <objbase.h>
#include <ocidl.h>
#include <oleidl.h>
#include <olectl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace FluentShell::Bridge::Translation {
namespace {
using Microsoft::WRL::ComPtr;

constexpr wchar_t kDropTargetProperty[] = L"OleDropTargetInterface";

// A raw interface pointer read from a window property is checked before it is dereferenced: the
// memory has to be committed and readable, and its first word (the vtable) has to point into
// readable memory too.
bool Readable(const void* address, size_t size) noexcept {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
    const auto begin = reinterpret_cast<uintptr_t>(address);
    const auto end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return begin + size <= end;
}

// The module (a loaded image) that contains `address`, as its full path, or empty.
std::wstring ModulePathOf(const void* address) noexcept {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module) || !module) return {};
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    return length == 0 || length >= MAX_PATH ? std::wstring() : std::wstring(path, length);
}

bool IsSysmonModule(const std::wstring& path) noexcept {
    wchar_t system[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (path.empty() || length == 0 || length >= MAX_PATH) return false;
    return FluentShell::EqualsIgnoreCase(path, std::wstring(system, length) + L"\\sysmon.ocx");
}

// A call into code whose owner is not trusted yet cannot be allowed to take the process down: the
// guard turns a fault into a failure. (No C++ object with a destructor may live in a function that
// uses structured exception handling, so the guarded work is a plain function of one pointer.)
bool RunGuarded(bool (*work)(void*), void* context) noexcept {
    __try {
        return work(context);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HRESULT QueryInterfaceGuarded(IUnknown* unknown, REFIID iid, void** result) noexcept {
    __try {
        return unknown->QueryInterface(iid, result);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

HRESULT ClassIdGuarded(IPersist* persist, CLSID* clsid) noexcept {
    __try {
        return persist->GetClassID(clsid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

HRESULT WindowGuarded(IOleWindow* window, HWND* result) noexcept {
    __try {
        return window->GetWindow(result);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

bool Open(HWND sysmonWindow, ComPtr<ISysmon2>& control, std::wstring& reason) noexcept {
    try {
        control.Reset();
        if (!IsSystemMonitorWindow(sysmonWindow)) {
            reason = L"the window is not a System Monitor control registered by System32\\sysmon.ocx";
            return false;
        }
        if (GetWindowThreadProcessId(sysmonWindow, nullptr) != GetCurrentThreadId()) {
            reason = L"the control is not on this GUI thread";
            return false;
        }
        auto* registered = reinterpret_cast<IUnknown*>(GetPropW(sysmonWindow, kDropTargetProperty));
        if (!registered) {
            reason = L"the control registered no drop target on its window";
            return false;
        }
        // The interface pointer is a pointer to an object whose first word is its vtable.
        if (!Readable(registered, sizeof(void*))) {
            reason = L"the drop target pointer is not readable memory";
            return false;
        }
        const void* const* vtable = *reinterpret_cast<const void* const* const*>(registered);
        if (!Readable(vtable, 3 * sizeof(void*))) {
            reason = L"the drop target's vtable is not readable memory";
            return false;
        }
        const std::wstring owner = ModulePathOf(vtable[0]);
        if (!IsSysmonModule(owner)) {
            reason = L"the drop target's code is in '" + owner + L"', not System32\\sysmon.ocx";
            return false;
        }
        ComPtr<ISysmon2> candidate;
        const HRESULT queried = QueryInterfaceGuarded(registered, kIidSysmon2,
            reinterpret_cast<void**>(candidate.GetAddressOf()));
        if (FAILED(queried) || !candidate) {
            wchar_t text[96]{};
            swprintf_s(text, L"the control does not answer ISystemMonitor2 (0x%08lX)", static_cast<unsigned long>(queried));
            reason = text;
            return false;
        }
        // The slots this project calls have to be code in the control, as the type library says they are.
        const void* const* methods = *reinterpret_cast<const void* const* const*>(candidate.Get());
        if (!Readable(methods, 99 * sizeof(void*)) || !IsSysmonModule(ModulePathOf(methods[31])) ||
            !IsSysmonModule(ModulePathOf(methods[88])) || !IsSysmonModule(ModulePathOf(methods[42]))) {
            reason = L"the control's ISystemMonitor2 methods are not code in System32\\sysmon.ocx";
            return false;
        }
        ComPtr<IPersist> persist;
        CLSID clsid{};
        if (FAILED(QueryInterfaceGuarded(candidate.Get(), IID_IPersist, reinterpret_cast<void**>(persist.GetAddressOf()))) ||
            !persist || FAILED(ClassIdGuarded(persist.Get(), &clsid)) || clsid != kClsidSystemMonitor) {
            reason = L"the object is not the System Monitor control (no matching IPersist class id)";
            return false;
        }
        ComPtr<IOleWindow> oleWindow;
        HWND owned = nullptr;
        if (FAILED(QueryInterfaceGuarded(candidate.Get(), IID_IOleWindow, reinterpret_cast<void**>(oleWindow.GetAddressOf()))) ||
            !oleWindow || FAILED(WindowGuarded(oleWindow.Get(), &owned)) || !owned) {
            reason = L"the object names no window through IOleWindow";
            return false;
        }
        // The object names the in-place site window (Hatchwin) around the control's own window.
        if (owned != sysmonWindow && owned != GetParent(sysmonWindow) && GetParent(owned) != sysmonWindow) {
            wchar_t text[96]{};
            swprintf_s(text, L"the object belongs to window 0x%p, not this control", static_cast<void*>(owned));
            reason = text;
            return false;
        }
        control = std::move(candidate);
        return true;
    } catch (...) {
        reason = L"opening the System Monitor object failed";
        return false;
    }
}

std::wstring TakeString(BSTR text) {
    std::wstring value = text ? std::wstring(text, SysStringLen(text)) : std::wstring();
    SysFreeString(text);
    return value;
}

COLORREF ColorOf(OLE_COLOR color, COLORREF fallback) noexcept {
    COLORREF resolved = 0;
    return SUCCEEDED(OleTranslateColor(color, nullptr, &resolved)) ? resolved : fallback;
}

// One counter, read through its own interface.
bool ReadCounter(IUnknown* item, SysmonCounter& counter, std::wstring& reason) {
    ComPtr<ISysmonCounter2> full;
    if (FAILED(item->QueryInterface(kIidSysmonCounter2, reinterpret_cast<void**>(full.GetAddressOf()))) || !full) {
        reason = L"a counter item does not answer ICounterItem2";
        return false;
    }
    BSTR path = nullptr;
    OLE_COLOR color = 0;
    int width = 1, style = 0, scale = 0;
    BOOL visible = TRUE, selected = FALSE;
    // COM identity is the pointer IUnknown answers with, whichever interface the item was reached by.
    ComPtr<IUnknown> identity;
    if (FAILED(item->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(identity.GetAddressOf()))) || !identity) {
        reason = L"a counter item has no identity";
        return false;
    }
    counter.identity = reinterpret_cast<uintptr_t>(identity.Get());
    if (FAILED(full->get_Path(&path))) { reason = L"a counter's Path could not be read"; return false; }
    counter.path = TakeString(path);
    if (FAILED(full->get_Color(&color)) || FAILED(full->get_Width(&width)) || FAILED(full->get_LineStyle(&style)) ||
        FAILED(full->get_ScaleFactor(&scale)) || FAILED(full->get_Visible(&visible)) ||
        FAILED(full->get_Selected(&selected))) {
        reason = L"a counter's line, scale or state could not be read";
        return false;
    }
    counter.color = ColorOf(color, RGB(255, 0, 0));
    counter.width = width;
    counter.lineStyle = style;
    counter.scaleFactor = scale;
    counter.visible = visible != FALSE;
    counter.selected = selected != FALSE;
    // PDH status: 0 valid data, 1 new data. Anything else (the control has not sampled yet) is no value.
    double value = 0;
    long status = -1;
    counter.hasValue = SUCCEEDED(full->GetValue(&value, &status)) && (status == 0 || status == 1) &&
        std::isfinite(value);
    counter.value = counter.hasValue ? value : 0;
    double maximum = 0, minimum = 0, average = 0;
    long statisticsStatus = -1;
    counter.hasStatistics = SUCCEEDED(full->GetStatistics(&maximum, &minimum, &average, &statisticsStatus)) &&
        (statisticsStatus == 0 || statisticsStatus == 1) && std::isfinite(maximum) && std::isfinite(minimum) &&
        std::isfinite(average);
    if (counter.hasStatistics) {
        counter.maximum = maximum;
        counter.minimum = minimum;
        counter.average = average;
    }
    return true;
}

struct ReadRequest final {
    ISysmon2* control = nullptr;
    SysmonSettings* settings = nullptr;
    std::wstring* reason = nullptr;
};

bool ReadImpl(void* raw) {
    auto& request = *static_cast<ReadRequest*>(raw);
    ISysmon2* control = request.control;
    SysmonSettings& settings = *request.settings;
    std::wstring& reason = *request.reason;
    OLE_COLOR back = 0, backControl = 0, fore = 0, grid = 0, bar = 0;
    int display = 0, minimum = 0, maximum = 0, points = 0, source = 0;
    float interval = 0;
    BOOL scroll = FALSE, vertical = FALSE, horizontal = FALSE, legend = TRUE, labels = TRUE, valueBar = TRUE,
        toolbar = TRUE, timeLabels = TRUE, manual = FALSE, readOnly = FALSE, highlight = FALSE, grouping = TRUE;
    BSTR title = nullptr, axis = nullptr;
    if (FAILED(control->get_DisplayType(&display)) || FAILED(control->get_MinimumScale(&minimum)) ||
        FAILED(control->get_MaximumScale(&maximum)) || FAILED(control->get_UpdateInterval(&interval)) ||
        FAILED(control->get_DataPointCount(&points)) || FAILED(control->get_ChartScroll(&scroll))) {
        reason = L"the control's display type, scale, interval or data point count could not be read";
        return false;
    }
    if (FAILED(control->get_ShowVerticalGrid(&vertical)) || FAILED(control->get_ShowHorizontalGrid(&horizontal)) ||
        FAILED(control->get_ShowLegend(&legend)) || FAILED(control->get_ShowScaleLabels(&labels)) ||
        FAILED(control->get_ShowValueBar(&valueBar)) || FAILED(control->get_ShowToolbar(&toolbar)) ||
        FAILED(control->get_ShowTimeAxisLabels(&timeLabels)) || FAILED(control->get_ManualUpdate(&manual)) ||
        FAILED(control->get_ReadOnly(&readOnly)) || FAILED(control->get_Highlight(&highlight)) ||
        FAILED(control->get_DataSourceType(&source)) || FAILED(control->get_EnableDigitGrouping(&grouping))) {
        reason = L"the control's display flags could not be read";
        return false;
    }
    if (FAILED(control->get_GraphTitle(&title)) || FAILED(control->get_YAxisLabel(&axis))) {
        SysFreeString(title);
        SysFreeString(axis);
        reason = L"the control's title could not be read";
        return false;
    }
    settings.title = TakeString(title);
    settings.yAxisLabel = TakeString(axis);
    if (FAILED(control->get_BackColor(&back)) || FAILED(control->get_BackColorCtl(&backControl)) ||
        FAILED(control->get_ForeColor(&fore)) || FAILED(control->get_GridColor(&grid)) ||
        FAILED(control->get_TimeBarColor(&bar))) {
        reason = L"the control's colours could not be read";
        return false;
    }
    settings.displayType = display;
    settings.minimumScale = minimum;
    settings.maximumScale = maximum;
    settings.updateInterval = interval;
    settings.dataPointCount = points;
    settings.chartScroll = scroll != FALSE;
    settings.showVerticalGrid = vertical != FALSE;
    settings.showHorizontalGrid = horizontal != FALSE;
    settings.showLegend = legend != FALSE;
    settings.showScaleLabels = labels != FALSE;
    settings.showValueBar = valueBar != FALSE;
    settings.showToolbar = toolbar != FALSE;
    settings.showTimeAxisLabels = timeLabels != FALSE;
    settings.manualUpdate = manual != FALSE;
    settings.readOnly = readOnly != FALSE;
    settings.highlight = highlight != FALSE;
    settings.digitGrouping = grouping != FALSE;
    settings.dataSource = source;
    settings.backColor = ColorOf(back, RGB(255, 255, 255));
    settings.controlColor = ColorOf(backControl, RGB(240, 240, 240));
    settings.foreColor = ColorOf(fore, 0);
    settings.gridColor = ColorOf(grid, RGB(128, 128, 128));
    settings.timeBarColor = ColorOf(bar, RGB(255, 0, 0));
    settings.counters.clear();
    ComPtr<ISysmonCounters> counters;
    long count = 0;
    if (FAILED(control->get_Counters(counters.GetAddressOf())) || !counters || FAILED(counters->get_Count(&count)) ||
        count < 0 || count > 256) {
        reason = L"the control's counter collection could not be read";
        return false;
    }
    // Counter(index) is documented with no base: try zero first, then one.
    bool oneBased = false;
    settings.counterBase = 0;
    for (long index = 0; index < count; ++index) {
        ComPtr<IUnknown> item;
        HRESULT hr = control->Counter(static_cast<int>(index + (oneBased ? 1 : 0)), item.GetAddressOf());
        if ((FAILED(hr) || !item) && index == 0 && !oneBased) {
            oneBased = true;
            settings.counterBase = 1;
            hr = control->Counter(1, item.GetAddressOf());
        }
        if (FAILED(hr) || !item) {
            reason = L"counter " + std::to_wstring(index) + L" could not be fetched";
            return false;
        }
        SysmonCounter counter;
        if (!ReadCounter(item.Get(), counter, reason)) return false;
        settings.counters.push_back(std::move(counter));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Recording a live chart's samples.

constexpr size_t kMaximumRecordedSamples = 4096;

int64_t UnixMillisecondsNow() noexcept {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;
    constexpr uint64_t kUnixEpoch = 116444736000000000ull; // 1601 -> 1970, in 100 ns units
    return static_cast<int64_t>((ticks.QuadPart - kUnixEpoch) / 10000ull);
}

struct RecordedSample final {
    int64_t unixMilliseconds = 0;
    // A counter that had no value at that moment is simply absent.
    std::vector<std::pair<uintptr_t, double>> values;
};

class Recorder final {
public:
    Recorder(HWND window, uintptr_t control) noexcept : window_(window), control_(control) {}

    HWND Window() const noexcept { return window_; }
    uintptr_t Control() const noexcept { return control_; }

    // Appends the sample the control just collected: every counter's value, as it reports it now. A
    // sample that carries no value at all is a real one when the control announced it (a rate
    // counter has nothing to show for its first sample), but not when it is only a first look at a
    // control that has not collected anything yet.
    void Record(ISysmon2* control, bool announced) {
        SysmonSettings settings;
        std::wstring reason;
        if (!ReadSystemMonitor(control, settings, reason)) return;
        RecordedSample sample;
        sample.unixMilliseconds = UnixMillisecondsNow();
        for (const SysmonCounter& counter : settings.counters)
            if (counter.hasValue) sample.values.emplace_back(counter.identity, counter.value);
        if (!announced && sample.values.empty()) return;
        std::scoped_lock lock(mutex_);
        // The toolbar's Clear Display (or a change of the chart's duration) empties the control's
        // history without an event: the statistics are the witness.
        if (announced && HistoryWasCleared(settings)) {
            samples_.clear();
            collected_ = 0;
        }
        samples_.push_back(std::move(sample));
        if (samples_.size() > kMaximumRecordedSamples) samples_.pop_front();
        ++collected_;
    }

    bool Read(const SysmonSettings& settings, size_t capacity, SysmonHistory& history) {
        std::scoped_lock lock(mutex_);
        const size_t count = (std::min)(capacity, samples_.size());
        const size_t first = samples_.size() - count;
        history.times.assign(count, 0);
        history.values.assign(settings.counters.size(), std::vector<std::optional<double>>(count));
        for (size_t offset = 0; offset < count; ++offset) {
            const RecordedSample& sample = samples_[first + offset];
            history.times[offset] = sample.unixMilliseconds;
            for (const auto& [identity, value] : sample.values)
                for (size_t counter = 0; counter < settings.counters.size(); ++counter)
                    if (settings.counters[counter].identity == identity) {
                        history.values[counter][offset] = value;
                        break;
                    }
        }
        history.collected = collected_;
        return true;
    }

private:
    // The control's statistics (minimum to maximum) cover the samples its chart holds. When they have
    // collapsed to one value while the last `dataPointCount` samples recorded here still spread, the
    // control has started over. A chart that is flat for its whole window agrees with its recording.
    bool HistoryWasCleared(const SysmonSettings& settings) const {
        const size_t window = static_cast<size_t>((std::max)(settings.dataPointCount, 2));
        for (const SysmonCounter& counter : settings.counters) {
            if (!counter.hasStatistics) continue;
            const double tolerance = 1e-9 * (std::max)(1.0, std::fabs(counter.maximum));
            if (counter.maximum - counter.minimum > tolerance) continue;
            double low = 0;
            double high = 0;
            size_t seen = 0;
            const size_t from = samples_.size() > window ? samples_.size() - window : 0;
            for (size_t at = from; at < samples_.size(); ++at)
                for (const auto& [identity, value] : samples_[at].values) {
                    if (identity != counter.identity) continue;
                    low = seen == 0 ? value : (std::min)(low, value);
                    high = seen == 0 ? value : (std::max)(high, value);
                    ++seen;
                }
            if (seen >= 3 && high - low > tolerance) return true;
        }
        return false;
    }

    HWND window_ = nullptr;
    uintptr_t control_ = 0;
    std::mutex mutex_;
    std::deque<RecordedSample> samples_;
    uint64_t collected_ = 0;
};

// The control fires DISystemMonitorEvents through IDispatch::Invoke of whatever sink is advised. The
// sink answers no type information (the control's own dispatch is what is broken, not ours).
class EventSink final : public IDispatch {
public:
    EventSink(std::weak_ptr<Recorder> recorder, ISysmon2* control) noexcept
        : recorder_(std::move(recorder)), control_(control) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDispatch || iid == kDiidSysmonEvents) {
            *result = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        *result = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }
    STDMETHODIMP GetTypeInfoCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = 0;
        return S_OK;
    }
    STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo** info) override {
        if (info) *info = nullptr;
        return E_NOTIMPL;
    }
    STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return DISP_E_UNKNOWNNAME; }
    STDMETHODIMP Invoke(DISPID dispatchId, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override {
        if (dispatchId == kSysmonEventSampleCollected) OnSampleCollected();
        return S_OK;
    }

private:
    void OnSampleCollected() noexcept {
        try {
            const auto recorder = recorder_.lock();
            // The recorder is dropped with its window, and the control is then gone or going.
            if (!recorder || !IsWindow(recorder->Window())) return;
            recorder->Record(control_, true);
        } catch (...) {
        }
    }

    std::atomic<ULONG> references_{1};
    std::weak_ptr<Recorder> recorder_;
    // Not owned: the control outlives every event it fires.
    ISysmon2* control_ = nullptr;
};

struct SubscribeRequest final {
    ISysmon2* control = nullptr;
    IDispatch* sink = nullptr;
    std::wstring* detail = nullptr;
};

std::wstring GuidText(const GUID& guid) {
    wchar_t text[64]{};
    return StringFromGUID2(guid, text, static_cast<int>(std::size(text))) > 0 ? std::wstring(text) : std::wstring();
}

bool SubscribeImpl(void* raw) {
    auto& request = *static_cast<SubscribeRequest*>(raw);
    std::wstring& detail = *request.detail;
    ComPtr<IConnectionPointContainer> container;
    if (FAILED(request.control->QueryInterface(IID_IConnectionPointContainer,
            reinterpret_cast<void**>(container.GetAddressOf()))) || !container) {
        detail = L"the control has no connection points";
        return false;
    }
    // Which sources the control offers, for a refusal's log line.
    const auto offered = [&] {
        std::wstring names;
        ComPtr<IEnumConnectionPoints> enumerator;
        if (SUCCEEDED(container->EnumConnectionPoints(enumerator.GetAddressOf())) && enumerator) {
            ComPtr<IConnectionPoint> point;
            ULONG fetched = 0;
            while (enumerator->Next(1, point.ReleaseAndGetAddressOf(), &fetched) == S_OK && fetched == 1) {
                IID iid{};
                if (SUCCEEDED(point->GetConnectionInterface(&iid))) names += L" " + GuidText(iid);
            }
        }
        return names;
    };
    ComPtr<IConnectionPoint> point;
    HRESULT hr = container->FindConnectionPoint(kDiidSysmonEvents, point.GetAddressOf());
    if (FAILED(hr) || !point) {
        wchar_t text[64]{};
        swprintf_s(text, L"no DISystemMonitorEvents source (0x%08lX); offered:", static_cast<unsigned long>(hr));
        detail = text + offered();
        return false;
    }
    DWORD cookie = 0;
    hr = point->Advise(request.sink, &cookie);
    if (FAILED(hr)) {
        wchar_t text[64]{};
        swprintf_s(text, L"Advise failed (0x%08lX); offered:", static_cast<unsigned long>(hr));
        detail = text + offered();
        return false;
    }
    return true;
}

std::mutex g_recordersMutex;
std::unordered_map<HWND, std::shared_ptr<Recorder>> g_recorders;

} // namespace

bool IsSystemMonitorWindow(HWND window) noexcept {
    wchar_t name[32]{};
    return window && GetClassNameW(window, name, static_cast<int>(std::size(name))) &&
        wcscmp(name, L"SysmonCtrl") == 0 && WindowClassRegisteredBySystemModule(window, L"sysmon.ocx");
}

bool OpenSystemMonitor(HWND sysmonWindow, ComPtr<ISysmon2>& control, std::wstring& reason) noexcept {
    return Open(sysmonWindow, control, reason);
}

bool ReadSystemMonitor(ISysmon2* control, SysmonSettings& settings, std::wstring& reason) noexcept {
    try {
        if (!control) {
            reason = L"no System Monitor control";
            return false;
        }
        ReadRequest request{control, &settings, &reason};
        if (!RunGuarded(&ReadImpl, &request)) {
            if (reason.empty()) reason = L"reading the System Monitor control faulted";
            return false;
        }
        return true;
    } catch (...) {
        reason = L"reading the System Monitor control failed";
        return false;
    }
}

void TrackSystemMonitor(HWND sysmonWindow, ISysmon2* control) noexcept {
    try {
        if (!control || !sysmonWindow) return;
        std::shared_ptr<Recorder> recorder;
        {
            std::scoped_lock lock(g_recordersMutex);
            // A destroyed window's recorder goes with it; its control dropped our sink when it died.
            for (auto entry = g_recorders.begin(); entry != g_recorders.end();)
                entry = IsWindow(entry->first) ? std::next(entry) : g_recorders.erase(entry);
            const auto found = g_recorders.find(sysmonWindow);
            const auto identity = reinterpret_cast<uintptr_t>(control);
            if (found != g_recorders.end() && found->second->Control() == identity) return;
            recorder = std::make_shared<Recorder>(sysmonWindow, identity);
            g_recorders[sysmonWindow] = recorder;
        }
        auto* sink = new EventSink(recorder, control);
        std::wstring detail;
        SubscribeRequest request{control, sink, &detail};
        const bool subscribed = RunGuarded(&SubscribeImpl, &request);
        sink->Release();
        FluentShell::Log(subscribed ? std::wstring(L"System Monitor chart samples are being recorded")
                                    : L"System Monitor chart samples cannot be recorded: " + detail);
        // The first sample is whatever the counters report now: the chart has been running a moment.
        recorder->Record(control, false);
    } catch (...) {
    }
}

bool ReadSystemMonitorHistory(HWND sysmonWindow, const SysmonSettings& settings, size_t capacity,
    SysmonHistory& history) noexcept {
    try {
        std::shared_ptr<Recorder> recorder;
        {
            std::scoped_lock lock(g_recordersMutex);
            const auto found = g_recorders.find(sysmonWindow);
            if (found == g_recorders.end()) return false;
            recorder = found->second;
        }
        return recorder->Read(settings, capacity, history);
    } catch (...) {
        return false;
    }
}

} // namespace FluentShell::Bridge::Translation
