#include "UiAutomationValidator.h"

#include <combaseapi.h>
#include <UIAutomation.h>
#include <dwmapi.h>
#include <oleauto.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "uiautomationcore.lib")

namespace FluentShell::Bridge::Translation {
namespace {

using Microsoft::WRL::ComPtr;

struct ComScope final {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() {
        if (SUCCEEDED(result)) CoUninitialize();
    }
    bool Usable() const noexcept {
        return SUCCEEDED(result);
    }
};

struct PhysicalCoordinateScope final {
    DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ~PhysicalCoordinateScope() {
        if (previous) SetThreadDpiAwarenessContext(previous);
    }
};

struct ElementInfo final {
    ComPtr<IUIAutomationElement> element;
    int processId = 0;
    CONTROLTYPEID controlType = 0;
    std::wstring name;
    std::wstring framework;
    std::wstring automationId;
    RECT bounds{};
    BOOL isControl = FALSE;
    BOOL isEnabled = FALSE;
    BOOL isKeyboardFocusable = FALSE;
};

enum class RequiredPattern {
    None,
    Invoke,
    Toggle,
    SelectionItem,
    Value,
    Selection,
    ExpandCollapse,
};

std::wstring HResultText(HRESULT result) {
    return L"HRESULT=" + std::to_wstring(static_cast<unsigned long>(result));
}

bool Fail(std::wstring& error, std::wstring_view reason) noexcept {
    error.assign(reason);
    return false;
}

bool FailHr(std::wstring& error, std::wstring_view reason, HRESULT result) noexcept {
    error.assign(reason);
    error.append(L" (");
    error.append(HResultText(result));
    error.push_back(L')');
    return false;
}

bool ReadElementInfo(IUIAutomationElement* element, ElementInfo& info, std::wstring& error) noexcept {
    info.element = element;
    if (FAILED(element->get_CurrentProcessId(&info.processId)) ||
        FAILED(element->get_CurrentControlType(&info.controlType)) ||
        FAILED(element->get_CurrentBoundingRectangle(&info.bounds)) ||
        FAILED(element->get_CurrentIsControlElement(&info.isControl)) ||
        FAILED(element->get_CurrentIsEnabled(&info.isEnabled)) ||
        FAILED(element->get_CurrentIsKeyboardFocusable(&info.isKeyboardFocusable))) {
        return Fail(error, L"UIA property read failed");
    }
    BSTR name = nullptr;
    BSTR framework = nullptr;
    BSTR automationId = nullptr;
    const HRESULT nameResult = element->get_CurrentName(&name);
    const HRESULT frameworkResult = element->get_CurrentFrameworkId(&framework);
    const HRESULT idResult = element->get_CurrentAutomationId(&automationId);
    if (FAILED(nameResult) || FAILED(frameworkResult) || FAILED(idResult)) {
        if (name) SysFreeString(name);
        if (framework) SysFreeString(framework);
        if (automationId) SysFreeString(automationId);
        return Fail(error, L"UIA string property read failed");
    }
    info.name.assign(name ? name : L"");
    info.framework.assign(framework ? framework : L"");
    info.automationId.assign(automationId ? automationId : L"");
    if (name) SysFreeString(name);
    if (framework) SysFreeString(framework);
    if (automationId) SysFreeString(automationId);
    return true;
}

std::wstring DisplayText(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] != L'&') {
            result.push_back(value[index]);
            continue;
        }
        if (index + 1 < value.size() && value[index + 1] == L'&') {
            result.push_back(L'&');
            ++index;
        } else if (index + 1 >= value.size()) {
            result.push_back(L'&');
        }
    }
    return result;
}

bool RectClose(const RECT& left, const RECT& right, LONG tolerance = 18) noexcept {
    return std::abs(left.left - right.left) <= tolerance &&
        std::abs(left.top - right.top) <= tolerance &&
        std::abs((left.right - left.left) - (right.right - right.left)) <= tolerance &&
        std::abs((left.bottom - left.top) - (right.bottom - right.top)) <= tolerance;
}

int ExpectedControlType(ControlKind kind) noexcept {
    switch (kind) {
    case ControlKind::StaticText: return UIA_TextControlTypeId;
    case ControlKind::Separator: return UIA_SeparatorControlTypeId;
    case ControlKind::Button: return UIA_ButtonControlTypeId;
    case ControlKind::CheckBox:
    case ControlKind::ThreeState: return UIA_CheckBoxControlTypeId;
    case ControlKind::RadioButton: return UIA_RadioButtonControlTypeId;
    case ControlKind::Edit:
    case ControlKind::Password: return UIA_EditControlTypeId;
    case ControlKind::ComboBox: return UIA_ComboBoxControlTypeId;
    case ControlKind::ListBox: return UIA_ListControlTypeId;
    default: return 0;
    }
}

RequiredPattern PatternFor(ControlKind kind) noexcept {
    switch (kind) {
    case ControlKind::Button: return RequiredPattern::Invoke;
    case ControlKind::CheckBox:
    case ControlKind::ThreeState: return RequiredPattern::Toggle;
    case ControlKind::RadioButton: return RequiredPattern::SelectionItem;
    case ControlKind::Edit: return RequiredPattern::Value;
    case ControlKind::Password: return RequiredPattern::None;
    case ControlKind::ComboBox: return RequiredPattern::Selection;
    case ControlKind::ListBox: return RequiredPattern::Selection;
    default: return RequiredPattern::None;
    }
}

bool HasPattern(
    IUIAutomationElement* element,
    RequiredPattern pattern,
    std::wstring& error) noexcept {
    if (pattern == RequiredPattern::None) return true;
    PATTERNID patternId = 0;
    const IID* iid = &IID_IUnknown;
    PROPERTYID availableProperty = 0;
    switch (pattern) {
    case RequiredPattern::Invoke:
        patternId = UIA_InvokePatternId; iid = &IID_IUIAutomationInvokePattern;
        availableProperty = UIA_IsInvokePatternAvailablePropertyId; break;
    case RequiredPattern::Toggle:
        patternId = UIA_TogglePatternId; iid = &IID_IUIAutomationTogglePattern;
        availableProperty = UIA_IsTogglePatternAvailablePropertyId; break;
    case RequiredPattern::SelectionItem:
        patternId = UIA_SelectionItemPatternId; iid = &IID_IUIAutomationSelectionItemPattern;
        availableProperty = UIA_IsSelectionItemPatternAvailablePropertyId; break;
    case RequiredPattern::Value:
        patternId = UIA_ValuePatternId; iid = &IID_IUIAutomationValuePattern;
        availableProperty = UIA_IsValuePatternAvailablePropertyId; break;
    case RequiredPattern::Selection:
        patternId = UIA_SelectionPatternId; iid = &IID_IUIAutomationSelectionPattern;
        availableProperty = UIA_IsSelectionPatternAvailablePropertyId; break;
    case RequiredPattern::ExpandCollapse:
        patternId = UIA_ExpandCollapsePatternId; iid = &IID_IUIAutomationExpandCollapsePattern;
        availableProperty = UIA_IsExpandCollapsePatternAvailablePropertyId; break;
    default: break;
    }
    VARIANT value{};
    VariantInit(&value);
    const HRESULT propertyResult = element->GetCurrentPropertyValue(availableProperty, &value);
    const bool propertyAvailable = SUCCEEDED(propertyResult) &&
        value.vt == VT_BOOL && value.boolVal == VARIANT_TRUE;
    VariantClear(&value);
    if (!propertyAvailable) {
        return FailHr(error, L"required UIA pattern property is unavailable",
            FAILED(propertyResult) ? propertyResult : E_NOINTERFACE);
    }
    void* raw = nullptr;
    const HRESULT patternResult = element->GetCurrentPatternAs(patternId, *iid, &raw);
    if (FAILED(patternResult) || !raw)
        return FailHr(error, L"required UIA pattern is unavailable", patternResult);
    static_cast<IUnknown*>(raw)->Release();
    return true;
}

bool IsAbove(HWND first, HWND second) noexcept {
    if (!first || !second || first == second) return first == second;
    for (HWND current = GetTopWindow(nullptr); current; current = GetWindow(current, GW_HWNDNEXT)) {
        if (current == first) return true;
        if (current == second) return false;
    }
    return false;
}

bool VerifyProcessIdentity(DWORD processId, uint64_t created) noexcept {
    if (!processId || !created) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;
    const uint64_t actual = Ipc::ProcessCreationTime(process);
    CloseHandle(process);
    return actual != 0 && actual == created;
}

bool IsDescendant(
    IUIAutomation* automation,
    IUIAutomationElement* root,
    IUIAutomationElement* candidate) noexcept {
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(automation->get_ControlViewWalker(&walker)) || !walker) return false;
    ComPtr<IUIAutomationElement> current;
    candidate->AddRef();
    current.Attach(candidate);
    for (unsigned depth = 0; depth < 64 && current; ++depth) {
        BOOL same = FALSE;
        if (SUCCEEDED(automation->CompareElements(root, current.Get(), &same)) && same) return true;
        ComPtr<IUIAutomationElement> parent;
        if (FAILED(walker->GetParentElement(current.Get(), &parent))) break;
        current = std::move(parent);
    }
    return false;
}

bool ValidateNativeIsolation(
    IUIAutomation* automation,
    const UiAutomationValidationOptions& options,
    std::wstring& error) noexcept {
    if (!options.nativeRoot) return true;
    DWORD cloak = 0;
    if (FAILED(DwmGetWindowAttribute(options.nativeRoot, DWMWA_CLOAKED, &cloak, sizeof(cloak))) ||
        (cloak & DWM_CLOAKED_APP) == 0) {
        return Fail(error, L"native root is not application-cloaked");
    }
    ComPtr<IUIAutomationElement> nativeElement;
    const HRESULT result = automation->ElementFromHandle(options.nativeRoot, &nativeElement);
    if (FAILED(result) || !nativeElement)
        return FailHr(error, L"cloaked native root has no UIA provider", result);
    int nativeProcessId = 0;
    if (FAILED(nativeElement->get_CurrentProcessId(&nativeProcessId)) ||
        nativeProcessId != static_cast<int>(GetCurrentProcessId()))
        return Fail(error, L"cloaked native root UIA provider has the wrong process identity");
    BOOL offscreen = FALSE;
    // DWM application cloaking does not consistently propagate to the UIA
    // IsOffscreen property on Windows 10/11.  Treat a false value as
    // advisory, but a property-read failure is still a hard gate failure.
    if (FAILED(nativeElement->get_CurrentIsOffscreen(&offscreen)))
        return Fail(error, L"cloaked native root UIA isolation property unavailable");

    // IsOffscreen is not a reliable cloak signal on every Windows build.  The
    // stronger contract is that the exact native HWND must not be discoverable
    // from the desktop UIA tree.  Search by both process and native handle so
    // a stale provider or a provider exposed by an accessibility bridge fails
    // the whole-window cutover instead of leaving duplicate UIA surfaces.
    ComPtr<IUIAutomationElement> desktop;
    if (FAILED(automation->GetRootElement(&desktop)) || !desktop)
        return Fail(error, L"desktop UIA root is unavailable for native isolation");
    VARIANT processValue{};
    VariantInit(&processValue);
    processValue.vt = VT_I4;
    processValue.lVal = static_cast<LONG>(GetCurrentProcessId());
    ComPtr<IUIAutomationCondition> processCondition;
    HRESULT conditionResult = automation->CreatePropertyCondition(
        UIA_ProcessIdPropertyId, processValue, &processCondition);
    VariantClear(&processValue);
    if (FAILED(conditionResult) || !processCondition)
        return FailHr(error, L"native isolation process condition creation failed", conditionResult);

    VARIANT hwndValue{};
    VariantInit(&hwndValue);
    hwndValue.vt = VT_I4;
    hwndValue.lVal = static_cast<LONG>(reinterpret_cast<ULONG_PTR>(options.nativeRoot));
    ComPtr<IUIAutomationCondition> hwndCondition;
    conditionResult = automation->CreatePropertyCondition(
        UIA_NativeWindowHandlePropertyId, hwndValue, &hwndCondition);
    VariantClear(&hwndValue);
    if (FAILED(conditionResult) || !hwndCondition)
        return FailHr(error, L"native isolation HWND condition creation failed", conditionResult);

    ComPtr<IUIAutomationCondition> exactCondition;
    conditionResult = automation->CreateAndCondition(
        processCondition.Get(), hwndCondition.Get(), &exactCondition);
    if (FAILED(conditionResult) || !exactCondition)
        return FailHr(error, L"native isolation condition composition failed", conditionResult);
    ComPtr<IUIAutomationElement> exposed;
    const HRESULT findResult = desktop->FindFirst(
        TreeScope_Descendants, exactCondition.Get(), &exposed);
    if (FAILED(findResult))
        return FailHr(error, L"desktop UIA native isolation query failed", findResult);
    if (exposed)
        return Fail(error, L"native root remains discoverable in the desktop UIA tree");
    return true;
}

bool ValidateOnMta(
    const UiAutomationValidationOptions& options,
    const WindowSnapshot& snapshot,
    std::wstring& error) noexcept {
    PhysicalCoordinateScope dpi;
    ComScope com;
    if (!com.Usable()) return FailHr(error, L"UIA COM initialization failed", com.result);

    ComPtr<IUIAutomation> automation;
    HRESULT result = CoCreateInstance(
        CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    if (FAILED(result)) {
        result = CoCreateInstance(
            CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&automation));
    }
    if (FAILED(result) || !automation) return FailHr(error, L"UIA client creation failed", result);
    ComPtr<IUIAutomation6> automation6;
    if (FAILED(automation.As(&automation6)) || !automation6 ||
        FAILED(automation6->put_ConnectionTimeout(1000)) ||
        FAILED(automation6->put_TransactionTimeout(1000)))
        return Fail(error, L"bounded UIA client timeouts are unavailable");
    if (!VerifyProcessIdentity(options.rendererProcessId, options.rendererCreated))
        return Fail(error, L"renderer process creation identity mismatch");

    ComPtr<IUIAutomationElement> root;
    result = automation->ElementFromHandle(options.proxy, &root);
    if (FAILED(result) || !root) return FailHr(error, L"proxy UIA root unavailable", result);
    ElementInfo rootInfo;
    if (!ReadElementInfo(root.Get(), rootInfo, error)) return false;
    if (rootInfo.processId != static_cast<int>(options.rendererProcessId))
        return Fail(error, L"proxy UIA root process identity mismatch");
    if (rootInfo.controlType != UIA_WindowControlTypeId)
        return Fail(error, L"proxy UIA root is not a Window control");
    if (rootInfo.framework != L"XAML" && rootInfo.framework != L"Win32")
        return Fail(error, L"proxy UIA root provider is neither XAML nor Win32 host");
    if (rootInfo.name != snapshot.title)
        return Fail(error, L"proxy UIA root name does not match native title");
    if (rootInfo.isEnabled != (snapshot.enabled ? TRUE : FALSE))
        return Fail(error, L"proxy UIA enabled state does not match native state");
    UIA_HWND nativeHandle = nullptr;
    if (SUCCEEDED(root->get_CurrentNativeWindowHandle(&nativeHandle)) && nativeHandle &&
        reinterpret_cast<HWND>(nativeHandle) != options.proxy)
        return Fail(error, L"proxy UIA native window handle mismatch");
    if (!options.committed || options.requireVisible) {
        RECT expectedBounds = snapshot.bounds;
        if (options.committed && snapshot.state == L"maximized") {
            // Capture stores rcNormalPosition for a maximized native window;
            // compare the committed proxy with its actual maximized rectangle.
            if (!GetWindowRect(options.proxy, &expectedBounds))
                return Fail(error, L"maximized proxy bounds unavailable");
        }
        if (rootInfo.bounds.right <= rootInfo.bounds.left ||
            rootInfo.bounds.bottom <= rootInfo.bounds.top)
            return Fail(error, L"proxy UIA root has empty bounds");
        if (!RectClose(rootInfo.bounds, expectedBounds, 24))
            return Fail(error, L"proxy UIA root bounds do not match native bounds");
    }

    // DWM-cloaked WinUI HWNDs expose only their host provider; the XAML
    // descendant tree is materialized after the provisional commit/uncloak.
    if (!options.committed) {
        if (options.expectedOwner) {
            if (GetWindow(options.proxy, GW_OWNER) != options.expectedOwner)
                return Fail(error, L"prepared proxy owner does not match projected owner");
            if (IsWindowEnabled(options.expectedOwner))
                return Fail(error, L"prepared projected modal owner remains enabled");
        } else if (GetWindow(options.proxy, GW_OWNER) != nullptr) {
            return Fail(error, L"prepared non-modal proxy unexpectedly has an owner");
        }
        return VerifyProcessIdentity(options.rendererProcessId, options.rendererCreated) ||
            Fail(error, L"renderer identity changed during prepared UIA validation");
    }

    void* windowPatternRaw = nullptr;
    result = root->GetCurrentPatternAs(
        UIA_WindowPatternId, IID_IUIAutomationWindowPattern, &windowPatternRaw);
    if (FAILED(result) || !windowPatternRaw)
        return FailHr(error, L"proxy UIA Window pattern is unavailable", result);
    ComPtr<IUIAutomationWindowPattern> windowPattern;
    windowPattern.Attach(static_cast<IUIAutomationWindowPattern*>(windowPatternRaw));
    BOOL modal = FALSE;
    if (FAILED(windowPattern->get_CurrentIsModal(&modal)) ||
        modal != (snapshot.modal ? TRUE : FALSE))
        return Fail(error, L"proxy UIA modal state does not match native semantics");
    if (options.requireVisible) {
        BOOL offscreen = TRUE;
        if (FAILED(root->get_CurrentIsOffscreen(&offscreen)) || offscreen || !IsWindowVisible(options.proxy))
            return Fail(error, L"visible proxy is UIA-offscreen");
        DWORD cloak = DWM_CLOAKED_APP;
        if (FAILED(DwmGetWindowAttribute(options.proxy, DWMWA_CLOAKED, &cloak, sizeof(cloak))) ||
            (cloak & DWM_CLOAKED_APP) != 0)
            return Fail(error, L"visible proxy remains application-cloaked");
    }

    ComPtr<IUIAutomationCondition> trueCondition;
    if (FAILED(automation->CreateTrueCondition(&trueCondition)))
        return Fail(error, L"UIA true condition creation failed");
    ComPtr<IUIAutomationElementArray> array;
    result = root->FindAll(TreeScope_Descendants, trueCondition.Get(), &array);
    if (FAILED(result) || !array) return FailHr(error, L"proxy UIA descendant enumeration failed", result);
    int length = 0;
    if (FAILED(array->get_Length(&length)) || length < 0 || length > 8192)
        return Fail(error, L"proxy UIA descendant count is invalid");
    std::vector<ElementInfo> elements;
    elements.reserve(static_cast<size_t>(length));
    for (int index = 0; index < length; ++index) {
        ComPtr<IUIAutomationElement> element;
        const HRESULT elementResult = array->GetElement(index, &element);
        if (FAILED(elementResult) || !element)
            return FailHr(error, L"proxy UIA descendant element read failed", elementResult);
        ElementInfo info;
        if (!ReadElementInfo(element.Get(), info, error)) return false;
        if (info.processId != 0 && info.processId != static_cast<int>(options.rendererProcessId))
            return Fail(error, L"proxy UIA subtree contains a foreign process");
        elements.push_back(std::move(info));
    }

    ComPtr<IUIAutomationElement> projectionRoot = root;
    if (rootInfo.framework != L"XAML") {
        auto candidate = std::find_if(elements.begin(), elements.end(), [&](const ElementInfo& info) {
            return info.framework == L"XAML" && info.name == snapshot.title &&
                (info.controlType == UIA_WindowControlTypeId ||
                 info.controlType == UIA_PaneControlTypeId ||
                 info.controlType == UIA_GroupControlTypeId);
        });
        if (candidate == elements.end()) {
            candidate = std::find_if(elements.begin(), elements.end(), [](const ElementInfo& info) {
                return info.framework == L"XAML" && info.automationId.empty() &&
                    (info.controlType == UIA_PaneControlTypeId ||
                     info.controlType == UIA_GroupControlTypeId);
            });
        }
        if (candidate == elements.end())
            return Fail(error, L"Win32 host has no XAML UIA projection descendant");
        projectionRoot = candidate->element;
        ComPtr<IUIAutomationElementArray> projectionArray;
        result = projectionRoot->FindAll(TreeScope_Descendants, trueCondition.Get(), &projectionArray);
        if (FAILED(result) || !projectionArray)
            return FailHr(error, L"XAML projection descendant enumeration failed", result);
        int projectionLength = 0;
        if (FAILED(projectionArray->get_Length(&projectionLength)) || projectionLength < 0 ||
            projectionLength > 8192)
            return Fail(error, L"XAML projection descendant count is invalid");
        elements.clear();
        elements.reserve(static_cast<size_t>(projectionLength));
        for (int index = 0; index < projectionLength; ++index) {
            ComPtr<IUIAutomationElement> element;
            const HRESULT elementResult = projectionArray->GetElement(index, &element);
            if (FAILED(elementResult) || !element)
                return FailHr(error, L"XAML projection element read failed", elementResult);
            ElementInfo info;
            if (!ReadElementInfo(element.Get(), info, error)) return false;
            if (info.processId != 0 && info.processId != static_cast<int>(options.rendererProcessId))
                return Fail(error, L"XAML projection contains a foreign process");
            elements.push_back(std::move(info));
        }
    }

    POINT clientOrigin{ 0, 0 };
    if (!ClientToScreen(options.proxy, &clientOrigin))
        return Fail(error, L"proxy client origin unavailable");
    std::vector<std::pair<int, size_t>> tabOrder;
    std::vector<bool> used(elements.size(), false);
    std::unordered_set<std::wstring> expectedAutomationIds;
    for (const auto& node : snapshot.nodes) {
        const auto expectedId = L"FluentShell.Node." + std::to_wstring(node.nodeId) +
            L"." + std::to_wstring(node.generation);
        expectedAutomationIds.insert(expectedId);
        if (!node.visible || node.kind == ControlKind::Separator) continue;
        const int expectedType = ExpectedControlType(node.kind);
        if (!expectedType) return Fail(error, L"snapshot contains an unknown UIA control kind");
        const auto expectedName = DisplayText(node.automationName);
        RECT expectedBounds{
            clientOrigin.x + node.rect.left,
            clientOrigin.y + node.rect.top,
            clientOrigin.x + node.rect.right,
            clientOrigin.y + node.rect.bottom,
        };
        size_t match = elements.size();
        for (size_t index = 0; index < elements.size(); ++index) {
            const auto& candidate = elements[index];
            if (used[index] || candidate.processId != static_cast<int>(options.rendererProcessId) ||
            candidate.controlType != expectedType || candidate.framework != L"XAML" ||
                candidate.automationId != expectedId || candidate.name != expectedName ||
                (options.requireVisible && !RectClose(candidate.bounds, expectedBounds))) continue;
            match = index;
            break;
        }
        if (match == elements.size())
            return Fail(error, L"expected native control is absent from the proxy UIA tree");
        const size_t duplicateCount = static_cast<size_t>(std::count_if(
            elements.begin(), elements.end(), [&](const ElementInfo& candidate) {
                return candidate.automationId == expectedId;
            }));
        if (duplicateCount != 1)
            return Fail(error, L"proxy UIA node AutomationId is missing or duplicated");
        used[match] = true;
        const auto& matched = elements[match];
        if (!matched.isControl)
            return Fail(error, L"proxy UIA node is not a control element");
        if (matched.isEnabled != (node.enabled ? TRUE : FALSE))
            return Fail(error, L"proxy UIA control enabled state does not match native state");
        if (node.tabStop && node.enabled && !matched.isKeyboardFocusable)
            return Fail(error, L"tab-stop native control is not keyboard focusable in XAML");
        if (!HasPattern(matched.element.Get(), PatternFor(node.kind), error)) return false;
        if (node.kind == ControlKind::ComboBox &&
            !HasPattern(matched.element.Get(), RequiredPattern::ExpandCollapse, error)) return false;
        if (node.kind == ControlKind::Password) {
            BOOL password = FALSE;
            if (FAILED(matched.element->get_CurrentIsPassword(&password)) || !password)
                return Fail(error, L"password control is not exposed as a password UIA element");
        }
        if (node.tabStop && node.enabled) {
            if (node.tabIndex < 0)
                return Fail(error, L"focusable native control has no dialog-manager tab index");
            tabOrder.emplace_back(node.tabIndex, match);
        }
    }
    for (const auto& element : elements) {
        if (element.automationId.starts_with(L"FluentShell.Node.") &&
            !expectedAutomationIds.contains(element.automationId))
            return Fail(error, L"proxy UIA tree contains an unexpected FluentShell node");
    }
    std::sort(tabOrder.begin(), tabOrder.end());
    for (size_t index = 1; index < tabOrder.size(); ++index) {
        if (tabOrder[index - 1].second >= tabOrder[index].second)
            return Fail(error, L"proxy UIA focus order does not match native tab order");
    }

    if (options.expectedOwner) {
        if (GetWindow(options.proxy, GW_OWNER) != options.expectedOwner)
            return Fail(error, L"proxy owner does not match projected owner");
        if (IsWindowEnabled(options.expectedOwner))
            return Fail(error, L"projected modal owner remains enabled");
        if (options.requireVisible && !IsAbove(options.proxy, options.expectedOwner))
            return Fail(error, L"proxy modal is below its owner in z-order");
    } else if (GetWindow(options.proxy, GW_OWNER) != nullptr) {
        return Fail(error, L"non-modal proxy unexpectedly has an owner");
    }
    if (options.nativeRoot && !IsAbove(options.proxy, options.nativeRoot))
        return Fail(error, L"proxy is below the cloaked native root in z-order");

    if (options.requireFocus) {
        if (GetForegroundWindow() != options.proxy)
            return Fail(error, L"proxy did not take the native foreground slot");
        ComPtr<IUIAutomationElement> focused;
        if (FAILED(automation->GetFocusedElement(&focused)) || !focused)
            return Fail(error, L"renderer focus element is unavailable");
        int focusedPid = 0;
        if (FAILED(focused->get_CurrentProcessId(&focusedPid)) ||
            focusedPid != static_cast<int>(options.rendererProcessId) ||
            !IsDescendant(automation.Get(), root.Get(), focused.Get()))
            return Fail(error, L"keyboard focus escaped the proxy tree");
        GUITHREADINFO threadInfo{ sizeof(threadInfo) };
        const DWORD threadId = GetWindowThreadProcessId(options.proxy, nullptr);
        if (threadId && GetGUIThreadInfo(threadId, &threadInfo) && threadInfo.hwndFocus &&
            GetAncestor(threadInfo.hwndFocus, GA_ROOT) != options.proxy)
            return Fail(error, L"Win32 focus root is outside the proxy");
    }
    if (options.requireVisible) {
        const LONG width = snapshot.bounds.right - snapshot.bounds.left;
        const LONG height = snapshot.bounds.bottom - snapshot.bounds.top;
        const POINT samples[] = {
            { snapshot.bounds.left + width / 2, snapshot.bounds.top + height / 2 },
            { snapshot.bounds.left + width / 4, snapshot.bounds.top + height / 3 },
            { snapshot.bounds.left + width * 3 / 4, snapshot.bounds.top + height / 3 },
            { snapshot.bounds.left + width / 4, snapshot.bounds.top + height * 2 / 3 },
            { snapshot.bounds.left + width * 3 / 4, snapshot.bounds.top + height * 2 / 3 },
        };
        for (const auto& point : samples) {
            ComPtr<IUIAutomationElement> hit;
            if (FAILED(automation->ElementFromPoint(point, &hit)) || !hit)
                return Fail(error, L"screen hit-test did not return a UIA element");
            int hitPid = 0;
            if (FAILED(hit->get_CurrentProcessId(&hitPid)) || hitPid != static_cast<int>(options.rendererProcessId) ||
                !IsDescendant(automation.Get(), root.Get(), hit.Get()))
                return Fail(error, L"screen hit-test does not resolve to the proxy tree");
        }
    }
    if (!ValidateNativeIsolation(automation.Get(), options, error)) return false;
    return VerifyProcessIdentity(options.rendererProcessId, options.rendererCreated) ||
        Fail(error, L"renderer identity changed during committed UIA validation");
}

} // namespace

bool ValidateProjectedSurface(
    const UiAutomationValidationOptions& options,
    const WindowSnapshot& snapshot,
    std::wstring& error) noexcept {
    error.clear();
    if (!options.proxy || !IsWindow(options.proxy) || options.rendererProcessId == 0 ||
        options.rendererCreated == 0)
        return Fail(error, L"invalid UIA validation identity");

    struct Result final {
        bool ok = false;
        std::wstring error;
    };
    try {
        auto result = std::make_shared<std::promise<Result>>();
        auto future = result->get_future();
        std::thread worker([result, options, snapshot] {
            Result value;
            try {
                value.ok = ValidateOnMta(options, snapshot, value.error);
            } catch (...) {
                value.ok = false;
                value.error = L"UIA validator exception";
            }
            try { result->set_value(std::move(value)); } catch (...) {}
        });
        if (future.wait_for(std::chrono::milliseconds(1800)) != std::future_status::ready) {
            // UIA6 connection/transaction timeouts bound the COM calls in the
            // worker. Join before returning so no detached validator can outlive
            // the target surface or accumulate across reconciliation scans.
            worker.join();
            return Fail(error, L"UIA validation deadline expired");
        }
        worker.join();
        auto value = future.get();
        error = std::move(value.error);
        return value.ok;
    } catch (...) {
        return Fail(error, L"UIA validation worker setup failed");
    }
}

} // namespace FluentShell::Bridge::Translation
