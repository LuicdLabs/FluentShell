#include "UiAutomationValidator.h"
#include "UiAutomationGeometry.h"

#include "../../Common/FluentShell.h"

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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "uiautomationcore.lib")

namespace FluentShell::Bridge::Translation {
namespace {

using Microsoft::WRL::ComPtr;

// A provider can violate UIA6's transaction timeout. Never block rollback on
// that provider and never accumulate detached COM workers in one Bridge session.
std::atomic<bool> g_projectedUiaWorkerPoisoned{ false };

constexpr std::wstring_view kContentViewportAutomationId =
    L"FluentShell.ContentViewport";

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
    std::wstring helpText;
    std::wstring accessKey;
    RECT bounds{};
    BOOL isControl = FALSE;
    BOOL isEnabled = FALSE;
    BOOL isKeyboardFocusable = FALSE;
    bool actionable = false;
};

enum class RequiredPattern {
    None,
    Invoke,
    Toggle,
    SelectionItem,
    Value,
    Selection,
    ExpandCollapse,
    RangeValue,
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

// Every property ElementInfo carries, so one enumeration can fetch them all in a
// single cross-process round trip.  Reading them per element costs sixteen calls,
// and a projected tree or list multiplies the element count while the whole
// validation pass shares one deadline.
constexpr PROPERTYID kElementProperties[] = {
    UIA_ProcessIdPropertyId,
    UIA_ControlTypePropertyId,
    UIA_BoundingRectanglePropertyId,
    UIA_IsControlElementPropertyId,
    UIA_IsEnabledPropertyId,
    UIA_IsKeyboardFocusablePropertyId,
    UIA_NamePropertyId,
    UIA_FrameworkIdPropertyId,
    UIA_AutomationIdPropertyId,
    UIA_HelpTextPropertyId,
    UIA_AccessKeyPropertyId,
    UIA_IsInvokePatternAvailablePropertyId,
    UIA_IsTogglePatternAvailablePropertyId,
    UIA_IsExpandCollapsePatternAvailablePropertyId,
    UIA_IsSelectionItemPatternAvailablePropertyId,
    UIA_IsValuePatternAvailablePropertyId,
};

constexpr PROPERTYID kActionProperties[] = {
    UIA_IsInvokePatternAvailablePropertyId,
    UIA_IsTogglePatternAvailablePropertyId,
    UIA_IsExpandCollapsePatternAvailablePropertyId,
    UIA_IsSelectionItemPatternAvailablePropertyId,
    UIA_IsValuePatternAvailablePropertyId,
};

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
    BSTR helpText = nullptr;
    BSTR accessKey = nullptr;
    const HRESULT nameResult = element->get_CurrentName(&name);
    const HRESULT frameworkResult = element->get_CurrentFrameworkId(&framework);
    const HRESULT idResult = element->get_CurrentAutomationId(&automationId);
    const HRESULT helpResult = element->get_CurrentHelpText(&helpText);
    const HRESULT accessResult = element->get_CurrentAccessKey(&accessKey);
    if (FAILED(nameResult) || FAILED(frameworkResult) || FAILED(idResult) ||
        FAILED(helpResult) || FAILED(accessResult)) {
        if (name) SysFreeString(name);
        if (framework) SysFreeString(framework);
        if (automationId) SysFreeString(automationId);
        if (helpText) SysFreeString(helpText);
        if (accessKey) SysFreeString(accessKey);
        return Fail(error, L"UIA string property read failed");
    }
    info.name.assign(name ? name : L"");
    info.framework.assign(framework ? framework : L"");
    info.automationId.assign(automationId ? automationId : L"");
    info.helpText.assign(helpText ? helpText : L"");
    info.accessKey.assign(accessKey ? accessKey : L"");
    if (name) SysFreeString(name);
    if (framework) SysFreeString(framework);
    if (automationId) SysFreeString(automationId);
    if (helpText) SysFreeString(helpText);
    if (accessKey) SysFreeString(accessKey);
    for (const PROPERTYID property : kActionProperties) {
        VARIANT value{};
        VariantInit(&value);
        if (FAILED(element->GetCurrentPropertyValue(property, &value)) || value.vt != VT_BOOL) {
            VariantClear(&value);
            return Fail(error, L"UIA action property read failed");
        }
        info.actionable = info.actionable || value.boolVal == VARIANT_TRUE;
        VariantClear(&value);
    }
    return true;
}

// The cached mirror of ReadElementInfo.  AutomationElementMode_Full keeps the
// live element alongside the cache, so pattern objects can still be fetched from
// a matched node afterwards.
bool ReadCachedElementInfo(
    IUIAutomationElement* element, ElementInfo& info, std::wstring& error) noexcept {
    info.element = element;
    if (FAILED(element->get_CachedProcessId(&info.processId)) ||
        FAILED(element->get_CachedControlType(&info.controlType)) ||
        FAILED(element->get_CachedBoundingRectangle(&info.bounds)) ||
        FAILED(element->get_CachedIsControlElement(&info.isControl)) ||
        FAILED(element->get_CachedIsEnabled(&info.isEnabled)) ||
        FAILED(element->get_CachedIsKeyboardFocusable(&info.isKeyboardFocusable))) {
        return Fail(error, L"cached UIA property read failed");
    }
    struct CachedString final {
        HRESULT (STDMETHODCALLTYPE IUIAutomationElement::*read)(BSTR*);
        std::wstring* target;
    };
    const CachedString strings[] = {
        { &IUIAutomationElement::get_CachedName, &info.name },
        { &IUIAutomationElement::get_CachedFrameworkId, &info.framework },
        { &IUIAutomationElement::get_CachedAutomationId, &info.automationId },
        { &IUIAutomationElement::get_CachedHelpText, &info.helpText },
        { &IUIAutomationElement::get_CachedAccessKey, &info.accessKey },
    };
    for (const auto& entry : strings) {
        BSTR value = nullptr;
        const HRESULT result = (element->*entry.read)(&value);
        if (FAILED(result)) {
            if (value) SysFreeString(value);
            return Fail(error, L"cached UIA string property read failed");
        }
        entry.target->assign(value ? value : L"");
        if (value) SysFreeString(value);
    }
    for (const PROPERTYID property : kActionProperties) {
        VARIANT value{};
        VariantInit(&value);
        if (FAILED(element->GetCachedPropertyValue(property, &value)) || value.vt != VT_BOOL) {
            VariantClear(&value);
            return Fail(error, L"cached UIA action property read failed");
        }
        info.actionable = info.actionable || value.boolVal == VARIANT_TRUE;
        VariantClear(&value);
    }
    return true;
}

bool BuildElementCacheRequest(
    IUIAutomation* automation,
    ComPtr<IUIAutomationCacheRequest>& request,
    std::wstring& error) noexcept {
    HRESULT result = automation->CreateCacheRequest(&request);
    if (FAILED(result) || !request)
        return FailHr(error, L"UIA cache request creation failed", result);
    for (const PROPERTYID property : kElementProperties) {
        result = request->AddProperty(property);
        if (FAILED(result))
            return FailHr(error, L"UIA cache property registration failed", result);
    }
    result = request->put_AutomationElementMode(AutomationElementMode_Full);
    if (FAILED(result))
        return FailHr(error, L"UIA cache element mode is unavailable", result);
    return true;
}

// One cached enumeration of a subtree.  The caller supplies the diagnostics so a
// failure still names which enumeration refused, exactly as the two separate
// loops did before they shared this walk.
bool CollectCachedElements(
    IUIAutomationElement* scopeRoot,
    IUIAutomationCondition* condition,
    IUIAutomationCacheRequest* cache,
    DWORD rendererProcessId,
    const wchar_t* enumerationFailure,
    const wchar_t* countFailure,
    const wchar_t* foreignFailure,
    std::vector<ElementInfo>& elements,
    std::wstring& error) noexcept {
    ComPtr<IUIAutomationElementArray> array;
    const auto startedAt = GetTickCount64();
    const HRESULT result = scopeRoot->FindAllBuildCache(
        TreeScope_Descendants, condition, cache, &array);
    if (FAILED(result) || !array) return FailHr(error, enumerationFailure, result);
    int length = 0;
    if (FAILED(array->get_Length(&length)) || length < 0 || length > 8192)
        return Fail(error, countFailure);
    // The whole validation pass shares one bounded deadline, so an enumeration
    // that costs a large part of it is the first thing worth knowing about.
    if (const auto elapsed = GetTickCount64() - startedAt; elapsed > 250) {
        FluentShell::Log(L"slow proxy UIA enumeration: " + std::to_wstring(length) +
            L" elements in " + std::to_wstring(elapsed) + L" ms");
    }
    elements.clear();
    elements.reserve(static_cast<size_t>(length));
    for (int index = 0; index < length; ++index) {
        ComPtr<IUIAutomationElement> element;
        const HRESULT elementResult = array->GetElement(index, &element);
        if (FAILED(elementResult) || !element)
            return FailHr(error, enumerationFailure, elementResult);
        ElementInfo info;
        if (!ReadCachedElementInfo(element.Get(), info, error)) return false;
        if (info.processId != 0 && info.processId != static_cast<int>(rendererProcessId))
            return Fail(error, foreignFailure);
        elements.push_back(std::move(info));
    }
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

std::wstring DiagnosticText(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (character == L'\r') result.append(L"\\r");
        else if (character == L'\n') result.append(L"\\n");
        else if (character == L'\t') result.append(L"\\t");
        else if (character < L' ') result.push_back(L'?');
        else result.push_back(character);
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
    case ControlKind::StaticIcon: return UIA_ImageControlTypeId;
    case ControlKind::Separator: return UIA_SeparatorControlTypeId;
    case ControlKind::Button: return UIA_ButtonControlTypeId;
    case ControlKind::CheckBox:
    case ControlKind::ThreeState: return UIA_CheckBoxControlTypeId;
    case ControlKind::RadioButton: return UIA_RadioButtonControlTypeId;
    case ControlKind::Edit:
    case ControlKind::Password: return UIA_EditControlTypeId;
    case ControlKind::ComboBox: return UIA_ComboBoxControlTypeId;
    case ControlKind::ListBox: return UIA_ListControlTypeId;
    case ControlKind::GroupBox: return UIA_GroupControlTypeId;
    case ControlKind::ProgressBar: return UIA_ProgressBarControlTypeId;
    case ControlKind::SysLink: return UIA_PaneControlTypeId;
    case ControlKind::ListView: return UIA_ListControlTypeId;
    case ControlKind::TreeView: return UIA_TreeControlTypeId;
    case ControlKind::Slider: return UIA_SliderControlTypeId;
    case ControlKind::DialogContainer: return UIA_PaneControlTypeId;
    case ControlKind::MdiClient: return UIA_PaneControlTypeId;
    // A private container is a frame around other windows, which is exactly what a
    // UIA pane is.
    case ControlKind::PaneContainer: return UIA_PaneControlTypeId;
    // An accessible island frames a set of elements the projection renders itself, so
    // it reports the role a set of related controls has.
    case ControlKind::AccessibleIsland: return UIA_GroupControlTypeId;
    // A native MDI child is a window inside a window and UIA reports it as one,
    // so the projection keeps that role rather than inventing a pane.
    case ControlKind::MdiChild: return UIA_WindowControlTypeId;
    case ControlKind::TabControl: return UIA_TabControlTypeId;
    case ControlKind::StatusBar: return UIA_StatusBarControlTypeId;
    case ControlKind::Toolbar: return UIA_ToolBarControlTypeId;
    default: return 0;
    }
}

RequiredPattern PatternFor(const ControlNode& node) noexcept {
    switch (node.kind) {
    case ControlKind::Button: return RequiredPattern::Invoke;
    case ControlKind::CheckBox:
    case ControlKind::ThreeState: return RequiredPattern::Toggle;
    case ControlKind::RadioButton: return RequiredPattern::SelectionItem;
    case ControlKind::Edit: return RequiredPattern::Value;
    case ControlKind::Password: return RequiredPattern::None;
    case ControlKind::ComboBox: return RequiredPattern::Selection;
    case ControlKind::ListBox: return RequiredPattern::Selection;
    case ControlKind::ListView: return RequiredPattern::Selection;
    case ControlKind::TreeView: return RequiredPattern::Selection;
    case ControlKind::Slider: return RequiredPattern::RangeValue;
    case ControlKind::TabControl: return RequiredPattern::Selection;
    case ControlKind::ProgressBar:
        return node.indeterminate ? RequiredPattern::None : RequiredPattern::RangeValue;
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
    case RequiredPattern::RangeValue:
        patternId = UIA_RangeValuePatternId; iid = &IID_IUIAutomationRangeValuePattern;
        availableProperty = UIA_IsRangeValuePatternAvailablePropertyId; break;
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

bool LacksRangeValuePattern(
    IUIAutomationElement* element,
    std::wstring& error) noexcept {
    VARIANT value{};
    VariantInit(&value);
    const HRESULT result = element->GetCurrentPropertyValue(
        UIA_IsRangeValuePatternAvailablePropertyId, &value);
    const bool unavailable = SUCCEEDED(result) && value.vt == VT_BOOL &&
        value.boolVal == VARIANT_FALSE;
    VariantClear(&value);
    return unavailable || FailHr(error,
        L"indeterminate ProgressBar unexpectedly exposes RangeValue",
        FAILED(result) ? result : E_UNEXPECTED);
}

bool ValidateSysLinkDescendant(
    IUIAutomation* automation,
    IUIAutomationElement* root,
    const ControlNode& node,
    DWORD rendererProcessId,
    std::wstring& error) noexcept {
    if (!automation || !root || node.items.size() != 1 || node.items.front().empty())
        return Fail(error, L"SysLink snapshot has no unique link label");
    VARIANT value{};
    VariantInit(&value);
    value.vt = VT_I4;
    value.lVal = UIA_HyperlinkControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    const HRESULT conditionResult = automation->CreatePropertyCondition(
        UIA_ControlTypePropertyId, value, &condition);
    VariantClear(&value);
    if (FAILED(conditionResult) || !condition)
        return FailHr(error, L"SysLink hyperlink condition failed", conditionResult);
    ComPtr<IUIAutomationElementArray> matches;
    const HRESULT findResult = root->FindAll(
        TreeScope_Descendants, condition.Get(), &matches);
    int length = 0;
    if (FAILED(findResult) || !matches || FAILED(matches->get_Length(&length)))
        return FailHr(error, L"SysLink hyperlink enumeration failed", findResult);
    if (length != 1)
        return Fail(error, L"SysLink projection does not contain exactly one hyperlink");
    ComPtr<IUIAutomationElement> link;
    if (FAILED(matches->GetElement(0, &link)) || !link)
        return Fail(error, L"SysLink hyperlink is unavailable");
    ElementInfo info;
    if (!ReadElementInfo(link.Get(), info, error)) return false;
    if (info.processId != static_cast<int>(rendererProcessId) ||
        info.framework != L"XAML" || info.controlType != UIA_HyperlinkControlTypeId ||
        info.name != DisplayText(node.items.front()) || !info.isControl ||
        info.isEnabled != (node.enabled ? TRUE : FALSE) ||
        info.isKeyboardFocusable != (node.tabStop && node.enabled ? TRUE : FALSE)) {
        return Fail(error, L"SysLink hyperlink role, name, process, enabled, or focus state is incorrect");
    }
    return HasPattern(link.Get(), RequiredPattern::Invoke, error);
}

bool ValidateSliderRangeValue(
    IUIAutomationElement* element,
    const ControlNode& node,
    std::wstring& error) noexcept {
    if (!element) return Fail(error, L"Trackbar UIA element is unavailable");
    ComPtr<IUIAutomationRangeValuePattern> range;
    const HRESULT patternResult = element->GetCurrentPatternAs(
        UIA_RangeValuePatternId, IID_IUIAutomationRangeValuePattern,
        reinterpret_cast<void**>(range.GetAddressOf()));
    if (FAILED(patternResult) || !range)
        return FailHr(error, L"Trackbar RangeValue pattern read failed", patternResult);
    double minimum = 0;
    double maximum = 0;
    double value = 0;
    BOOL readOnly = TRUE;
    if (FAILED(range->get_CurrentMinimum(&minimum)) ||
        FAILED(range->get_CurrentMaximum(&maximum)) ||
        FAILED(range->get_CurrentValue(&value)) ||
        FAILED(range->get_CurrentIsReadOnly(&readOnly)))
        return Fail(error, L"Trackbar RangeValue state read failed");
    if (static_cast<int>(minimum) != node.minimum ||
        static_cast<int>(maximum) != node.maximum ||
        static_cast<int>(value) != node.position)
        return Fail(error, L"projected Trackbar range or value does not match native state");
    // A native trackbar the user can move must not project as a read-only range;
    // a disabled one is already excluded by the enabled-state comparison above.
    if (node.enabled && readOnly)
        return Fail(error, L"enabled Trackbar projects a read-only RangeValue");
    return true;
}

bool ValidateListViewSelectionCapability(
    IUIAutomationElement* element,
    const ControlNode& node,
    std::wstring& error) noexcept {
    if (!element) return Fail(error, L"ListView UIA element is unavailable");
    ComPtr<IUIAutomationSelectionPattern> selection;
    const HRESULT patternResult = element->GetCurrentPatternAs(
        UIA_SelectionPatternId, IID_IUIAutomationSelectionPattern,
        reinterpret_cast<void**>(selection.GetAddressOf()));
    if (FAILED(patternResult) || !selection)
        return FailHr(error, L"ListView Selection pattern read failed", patternResult);
    BOOL canSelectMultiple = FALSE;
    const HRESULT propertyResult = selection->get_CurrentCanSelectMultiple(&canSelectMultiple);
    if (FAILED(propertyResult))
        return FailHr(error, L"ListView Selection capability read failed", propertyResult);
    if (canSelectMultiple != (node.multiSelect ? TRUE : FALSE))
        return Fail(error, L"ListView multi-select UIA capability does not match native state");
    return true;
}

bool ValidateListViewCheckboxes(
    IUIAutomation* automation,
    IUIAutomationElement* element,
    const ControlNode& node,
    std::wstring& error) noexcept {
    if (!automation || !element) return Fail(error, L"ListView UIA element is unavailable");
    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = UIA_CheckBoxControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    const HRESULT conditionResult = automation->CreatePropertyCondition(
        UIA_ControlTypePropertyId, value, &condition);
    if (FAILED(conditionResult) || !condition)
        return FailHr(error, L"ListView checkbox condition failed", conditionResult);
    ComPtr<IUIAutomationElementArray> matches;
    const HRESULT findResult = element->FindAll(
        TreeScope_Descendants, condition.Get(), &matches);
    int length = 0;
    if (FAILED(findResult) || !matches || FAILED(matches->get_Length(&length)))
        return FailHr(error, L"ListView checkbox enumeration failed", findResult);
    const int rowCount = static_cast<int>(node.rows.size());
    if ((!node.checkBoxes && length != 0) ||
        (node.checkBoxes && rowCount != 0 && (length <= 0 || length > rowCount))) {
        return Fail(error, L"ListView checkbox descendants do not match native capability");
    }
    for (int index = 0; index < length; ++index) {
        ComPtr<IUIAutomationElement> checkBox;
        if (FAILED(matches->GetElement(index, &checkBox)) || !checkBox)
            return Fail(error, L"ListView checkbox descendant is unavailable");
        ComPtr<IUIAutomationTogglePattern> toggle;
        const HRESULT patternResult = checkBox->GetCurrentPatternAs(
            UIA_TogglePatternId, IID_IUIAutomationTogglePattern,
            reinterpret_cast<void**>(toggle.GetAddressOf()));
        if (FAILED(patternResult) || !toggle)
            return FailHr(error, L"ListView row checkbox lacks Toggle pattern", patternResult);
        ToggleState state = ToggleState_Off;
        if (FAILED(toggle->get_CurrentToggleState(&state)))
            return Fail(error, L"ListView row checkbox state is unavailable");
        const bool checked = std::binary_search(
            node.checkedIndices.begin(), node.checkedIndices.end(), index);
        if (state != (checked ? ToggleState_On : ToggleState_Off))
            return Fail(error, L"ListView row checkbox UIA state does not match native state");
    }
    return true;
}

bool ValidateTabControlItems(
    IUIAutomation* automation,
    IUIAutomationElement* element,
    const ControlNode& node,
    std::wstring& error) noexcept {
    if (!automation || !element)
        return Fail(error, L"TabControl UIA element is unavailable");
    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = UIA_TabItemControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    HRESULT result = automation->CreatePropertyCondition(
        UIA_ControlTypePropertyId, value, &condition);
    if (FAILED(result) || !condition)
        return FailHr(error, L"TabControl TabItem condition failed", result);
    ComPtr<IUIAutomationElementArray> items;
    result = element->FindAll(TreeScope_Children, condition.Get(), &items);
    int count = 0;
    if (FAILED(result) || !items || FAILED(items->get_Length(&count)) ||
        count != static_cast<int>(node.items.size()))
        return Fail(error, L"TabControl UIA TabItem count does not match native items");
    int selectedCount = 0;
    for (int index = 0; index < count; ++index) {
        ComPtr<IUIAutomationElement> item;
        if (FAILED(items->GetElement(index, &item)) || !item)
            return Fail(error, L"TabControl UIA TabItem is unavailable");
        BSTR rawName = nullptr;
        const HRESULT nameResult = item->get_CurrentName(&rawName);
        const std::wstring name = rawName ? rawName : L"";
        if (rawName) SysFreeString(rawName);
        if (FAILED(nameResult) || name != DisplayText(node.items[static_cast<size_t>(index)]))
            return Fail(error, L"TabControl UIA TabItem name does not match its native label");
        BOOL focusable = FALSE;
        if (FAILED(item->get_CurrentIsKeyboardFocusable(&focusable)) || !focusable)
            return Fail(error, L"TabControl UIA TabItem is not keyboard focusable");
        ComPtr<IUIAutomationSelectionItemPattern> selection;
        result = item->GetCurrentPatternAs(UIA_SelectionItemPatternId,
            IID_IUIAutomationSelectionItemPattern,
            reinterpret_cast<void**>(selection.GetAddressOf()));
        BOOL selected = FALSE;
        if (FAILED(result) || !selection || FAILED(selection->get_CurrentIsSelected(&selected)))
            return Fail(error, L"TabControl UIA TabItem lacks SelectionItem state");
        if (selected) ++selectedCount;
        if ((selected != FALSE) != (index == node.selectedIndex))
            return Fail(error, L"TabControl UIA selection does not match native selection");
    }
    return selectedCount == 1 ||
        Fail(error, L"TabControl UIA must expose exactly one selected TabItem");
}

bool ValidateToolbarItems(
    IUIAutomation* automation,
    IUIAutomationElement* element,
    const ControlNode& node,
    std::wstring& error) noexcept {
    if (!automation || !element) return Fail(error, L"Toolbar UIA element is unavailable");
    ComPtr<IUIAutomationCondition> condition;
    HRESULT result = automation->CreateTrueCondition(&condition);
    if (FAILED(result) || !condition)
        return FailHr(error, L"Toolbar child condition failed", result);
    ComPtr<IUIAutomationElementArray> children;
    result = element->FindAll(TreeScope_Children, condition.Get(), &children);
    int count = 0;
    const auto visibleCount = static_cast<int>(std::count_if(
        node.toolbarItems.begin(), node.toolbarItems.end(),
        [](const ToolbarItemSnapshot& item) { return !item.hidden; }));
    if (FAILED(result) || !children || FAILED(children->get_Length(&count)) || count != visibleCount) {
        // Name what the two sides actually hold: a count on its own cannot say which
        // item the projection dropped.
        error = L"Toolbar UIA child count does not match visible native items: expected " +
            std::to_wstring(visibleCount) + L" actual " + std::to_wstring(count) + L" native=";
        for (const auto& item : node.toolbarItems) {
            if (item.hidden) continue;
            error += L"[" + std::wstring(item.kind == ToolbarItemKind::Separator
                    ? L"sep" : item.kind == ToolbarItemKind::ToggleButton ? L"toggle" : L"push") +
                L" '" + DiagnosticText(DisplayText(item.text)) + L"']";
        }
        error += L" proxy=";
        for (int index = 0; children && index < count; ++index) {
            ComPtr<IUIAutomationElement> child;
            if (FAILED(children->GetElement(index, &child)) || !child) continue;
            CONTROLTYPEID type = 0;
            child->get_CurrentControlType(&type);
            BSTR rawName = nullptr;
            child->get_CurrentName(&rawName);
            error += L"[" + std::to_wstring(type) + L" '" +
                DiagnosticText(rawName ? rawName : L"") + L"']";
            if (rawName) SysFreeString(rawName);
        }
        return false;
    }
    int childIndex = 0;
    for (const auto& expected : node.toolbarItems) {
        if (expected.hidden) continue;
        ComPtr<IUIAutomationElement> child;
        if (FAILED(children->GetElement(childIndex++, &child)) || !child)
            return Fail(error, L"Toolbar UIA child is unavailable");
        CONTROLTYPEID type = 0;
        // A latched button is a XAML ToggleButton, which publishes the Button control
        // type and the Toggle pattern rather than Invoke.
        const CONTROLTYPEID expectedType =
            expected.kind == ToolbarItemKind::Separator
                ? UIA_SeparatorControlTypeId : UIA_ButtonControlTypeId;
        if (FAILED(child->get_CurrentControlType(&type)) || type != expectedType)
            return Fail(error, L"Toolbar UIA child type does not match native item kind");
        if (expected.kind == ToolbarItemKind::Separator) continue;
        BSTR rawName = nullptr;
        const HRESULT nameResult = child->get_CurrentName(&rawName);
        const std::wstring name = rawName ? rawName : L"";
        if (rawName) SysFreeString(rawName);
        BOOL enabled = FALSE;
        if (FAILED(nameResult) || name != DisplayText(expected.text) ||
            FAILED(child->get_CurrentIsEnabled(&enabled)) || enabled != (expected.enabled ? TRUE : FALSE))
            return Fail(error, L"Toolbar UIA button name or enabled state does not match native state");
        // A latched button owns Toggle; a command button owns Invoke.  The projection
        // draws whichever the control's own style declares.
        const bool latched = expected.kind == ToolbarItemKind::ToggleButton || expected.checked;
        if (!HasPattern(child.Get(),
                latched ? RequiredPattern::Toggle : RequiredPattern::Invoke, error))
            return false;
    }
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

// The AutomationIds of every control-view descendant of one element, fetched in
// a single cached enumeration.  Verifying nesting by walking each child's
// ancestors costs a cross-process call per level per child, which is the
// difference between fitting the validation deadline and blowing it on a nested
// surface like an MDI frame.
bool CollectDescendantAutomationIds(
    IUIAutomation* automation,
    IUIAutomationElement* element,
    IUIAutomationCondition* condition,
    std::unordered_set<std::wstring>& identifiers,
    std::wstring& error) noexcept {
    identifiers.clear();
    ComPtr<IUIAutomationCacheRequest> request;
    HRESULT result = automation->CreateCacheRequest(&request);
    if (FAILED(result) || !request)
        return FailHr(error, L"UIA nesting cache request creation failed", result);
    result = request->AddProperty(UIA_AutomationIdPropertyId);
    if (FAILED(result))
        return FailHr(error, L"UIA nesting cache property registration failed", result);
    result = request->put_AutomationElementMode(AutomationElementMode_None);
    if (FAILED(result))
        return FailHr(error, L"UIA nesting cache element mode is unavailable", result);
    ComPtr<IUIAutomationElementArray> array;
    result = element->FindAllBuildCache(
        TreeScope_Descendants, condition, request.Get(), &array);
    if (FAILED(result) || !array)
        return FailHr(error, L"UIA nesting enumeration failed", result);
    int length = 0;
    if (FAILED(array->get_Length(&length)) || length < 0 || length > 8192)
        return Fail(error, L"UIA nesting descendant count is invalid");
    for (int index = 0; index < length; ++index) {
        ComPtr<IUIAutomationElement> descendant;
        if (FAILED(array->GetElement(index, &descendant)) || !descendant) continue;
        BSTR identifier = nullptr;
        if (FAILED(descendant->get_CachedAutomationId(&identifier))) continue;
        if (identifier) {
            identifiers.emplace(identifier);
            SysFreeString(identifier);
        }
    }
    return true;
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
    if (!snapshot.adapterId.empty()) {
        // Adapter snapshots are closed worlds: every node must carry the
        // adapter identity and a semantic key, virtual slots must have no
        // backing HWND, and backing slots must have one. The admitted key
        // set itself is enforced by the renderer-side profile table.
        for (const ControlNode& node : snapshot.nodes) {
            if (node.adapterId != snapshot.adapterId || node.semanticKey.empty() ||
                (node.sourceKind == L"uiaVirtual" && node.hwnd != nullptr) ||
                (node.sourceKind == L"nativeBacking" && node.hwnd == nullptr))
                return Fail(error, L"unknown application-adapter snapshot shape");
        }
    }

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

    ComPtr<IUIAutomationCondition> controlViewCondition;
    if (FAILED(automation->get_ControlViewCondition(&controlViewCondition)))
        return Fail(error, L"UIA control-view condition creation failed");
    ComPtr<IUIAutomationCacheRequest> elementCache;
    if (!BuildElementCacheRequest(automation.Get(), elementCache, error)) return false;
    std::vector<ElementInfo> elements;
    if (!CollectCachedElements(root.Get(), controlViewCondition.Get(), elementCache.Get(),
            options.rendererProcessId,
            L"proxy UIA descendant enumeration failed",
            L"proxy UIA descendant count is invalid",
            L"proxy UIA subtree contains a foreign process",
            elements, error)) {
        return false;
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
        if (!CollectCachedElements(projectionRoot.Get(), controlViewCondition.Get(),
                elementCache.Get(), options.rendererProcessId,
                L"XAML projection descendant enumeration failed",
                L"XAML projection descendant count is invalid",
                L"XAML projection contains a foreign process",
                elements, error)) {
            return false;
        }
    }

    const auto viewportCount = static_cast<size_t>(std::count_if(
        elements.begin(), elements.end(), [](const ElementInfo& info) {
            return info.automationId == kContentViewportAutomationId;
        }));
    if (viewportCount != 1)
        return Fail(error, L"proxy XAML content viewport is missing or duplicated");
    const auto viewport = std::find_if(
        elements.begin(), elements.end(), [](const ElementInfo& info) {
            return info.automationId == kContentViewportAutomationId;
        });
    if (viewport->processId != static_cast<int>(options.rendererProcessId) ||
        viewport->framework != L"XAML" || viewport->controlType != UIA_PaneControlTypeId ||
        !viewport->isControl ||
        !ContentViewportFitsCanonicalSize(viewport->bounds, snapshot.clientBounds))
        return Fail(error, L"proxy XAML content viewport identity or bounds are invalid");
    const RECT rootViewport = viewport->bounds;
    const POINT contentOrigin{ rootViewport.left, rootViewport.top };
    std::vector<bool> used(elements.size(), false);
    std::unordered_set<std::wstring> expectedAutomationIds;
    std::unordered_map<uint64_t, ComPtr<IUIAutomationElement>> matchedNodes;
    std::unordered_map<uint64_t, std::unordered_set<std::wstring>> containerDescendants;
    std::unordered_map<uint64_t, RECT> expectedNodeBounds;
    for (const auto& node : snapshot.nodes) {
        const auto expectedId = L"FluentShell.Node." + std::to_wstring(node.nodeId) +
            L"." + std::to_wstring(node.generation);
        expectedAutomationIds.insert(expectedId);
        if (!node.visible || node.kind == ControlKind::Separator) continue;
        const int expectedType = ExpectedControlType(node.kind);
        if (!expectedType) return Fail(error, L"snapshot contains an unknown UIA control kind");
        const auto expectedName = DisplayText(node.automationName);
        const RECT* parentVisibleBounds = nullptr;
        if (node.parentNodeId) {
            const auto parentBounds = expectedNodeBounds.find(*node.parentNodeId);
            if (parentBounds == expectedNodeBounds.end())
                return Fail(error, L"snapshot UIA parent does not precede its child");
            parentVisibleBounds = &parentBounds->second;
        }
        RECT expectedBounds{};
        if (!ComputeVisibleUiaBounds(
                node.rect, contentOrigin, rootViewport, parentVisibleBounds, expectedBounds))
            return Fail(error, L"visible native control is fully clipped in the proxy viewport");
        expectedNodeBounds.emplace(node.nodeId, expectedBounds);
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
        if (match == elements.size()) {
            const auto sameId = std::find_if(elements.begin(), elements.end(),
                [&](const ElementInfo& candidate) {
                    return candidate.automationId == expectedId;
                });
            error = L"expected native control mismatch: id=" + expectedId +
                L" kind=" + ControlKindName(node.kind) +
                L" expectedType=" + std::to_wstring(expectedType) +
                L" expectedName='" + DiagnosticText(expectedName) + L"' expectedBounds=" +
                std::to_wstring(expectedBounds.left) + L"," +
                std::to_wstring(expectedBounds.top) + L"," +
                std::to_wstring(expectedBounds.right - expectedBounds.left) + L"x" +
                std::to_wstring(expectedBounds.bottom - expectedBounds.top);
            if (sameId == elements.end()) {
                error += L" actual=missing candidates=" + std::to_wstring(elements.size());
            } else {
                error += L" actualType=" + std::to_wstring(sameId->controlType) +
                    L" actualFramework='" + sameId->framework +
                    L"' actualName='" + DiagnosticText(sameId->name) + L"' actualBounds=" +
                    std::to_wstring(sameId->bounds.left) + L"," +
                    std::to_wstring(sameId->bounds.top) + L"," +
                    std::to_wstring(sameId->bounds.right - sameId->bounds.left) + L"x" +
                    std::to_wstring(sameId->bounds.bottom - sameId->bounds.top);
            }
            return false;
        }
        const size_t duplicateCount = static_cast<size_t>(std::count_if(
            elements.begin(), elements.end(), [&](const ElementInfo& candidate) {
                return candidate.automationId == expectedId;
            }));
        if (duplicateCount != 1)
            return Fail(error, L"proxy UIA node AutomationId is missing or duplicated");
        used[match] = true;
        const auto& matched = elements[match];
        if (node.parentNodeId) {
            const auto parent = matchedNodes.find(*node.parentNodeId);
            if (parent == matchedNodes.end())
                return Fail(error, L"proxy UIA node has no matched native parent");
            // One cached enumeration per container answers nesting for every child
            // it owns, instead of walking each child's ancestors one call at a time.
            auto nested = containerDescendants.find(*node.parentNodeId);
            if (nested == containerDescendants.end()) {
                std::unordered_set<std::wstring> identifiers;
                if (!CollectDescendantAutomationIds(automation.Get(), parent->second.Get(),
                        controlViewCondition.Get(), identifiers, error)) return false;
                nested = containerDescendants.emplace(
                    *node.parentNodeId, std::move(identifiers)).first;
            }
            if (!nested->second.contains(expectedId))
                return Fail(error, L"proxy UIA node is not nested under its native parent");
        }
        matchedNodes.emplace(node.nodeId, matched.element);
        if (!matched.isControl)
            return Fail(error, L"proxy UIA node is not a control element");
        if (matched.isEnabled != (node.enabled ? TRUE : FALSE))
            return Fail(error, L"proxy UIA control enabled state does not match native state");
        if (!snapshot.adapterId.empty() &&
            (matched.helpText != node.helpText || matched.accessKey != node.accessKey))
            return Fail(error, L"application-adapter HelpText or AccessKey mismatch");
        if (snapshot.adapterId == L"microsoft.mdsched.directui" &&
            node.kind == ControlKind::StaticIcon &&
            ((matched.bounds.right - matched.bounds.left) != 32 ||
             (matched.bounds.bottom - matched.bounds.top) != 32))
            return Fail(error, L"application-adapter Image is not exactly 32x32 physical pixels");
        if (node.tabStop && node.enabled && node.kind != ControlKind::SysLink &&
            !matched.isKeyboardFocusable)
            return Fail(error, L"tab-stop native control is not keyboard focusable in XAML");
        if (node.kind == ControlKind::StaticIcon && matched.isKeyboardFocusable)
            return Fail(error, L"Static icon projection is unexpectedly keyboard focusable");
        if (!HasPattern(matched.element.Get(), PatternFor(node), error)) return false;
        if (node.kind == ControlKind::ProgressBar && node.indeterminate &&
            !LacksRangeValuePattern(matched.element.Get(), error)) return false;
        if (node.kind == ControlKind::SysLink &&
            !ValidateSysLinkDescendant(automation.Get(), matched.element.Get(), node,
                options.rendererProcessId, error)) return false;
        if (node.kind == ControlKind::ListView &&
            !ValidateListViewSelectionCapability(matched.element.Get(), node, error)) return false;
        if (node.kind == ControlKind::Slider &&
            !ValidateSliderRangeValue(matched.element.Get(), node, error)) return false;
        if (node.kind == ControlKind::ListView &&
            !ValidateListViewCheckboxes(
                automation.Get(), matched.element.Get(), node, error)) return false;
        if (node.kind == ControlKind::TabControl &&
            !ValidateTabControlItems(
                automation.Get(), matched.element.Get(), node, error)) return false;
        if (node.kind == ControlKind::Toolbar &&
            !ValidateToolbarItems(
                automation.Get(), matched.element.Get(), node, error)) return false;
        if (node.kind == ControlKind::ComboBox &&
            !HasPattern(matched.element.Get(), RequiredPattern::ExpandCollapse, error)) return false;
        if (node.kind == ControlKind::ComboBox && node.editable &&
            !HasPattern(matched.element.Get(), RequiredPattern::Value, error)) return false;
        if (node.kind == ControlKind::Password) {
            BOOL password = FALSE;
            if (FAILED(matched.element->get_CurrentIsPassword(&password)) || !password)
                return Fail(error, L"password control is not exposed as a password UIA element");
        }
        if (node.tabStop && node.enabled) {
            if (node.tabIndex < 0)
                return Fail(error, L"focusable native control has no dialog-manager tab index");
        }
    }
    for (const auto& element : elements) {
        if (element.automationId.starts_with(L"FluentShell.Node.") &&
            !expectedAutomationIds.contains(element.automationId))
            return Fail(error, L"proxy UIA tree contains an unexpected FluentShell node");
        if (!snapshot.adapterId.empty() &&
            (element.actionable || element.isKeyboardFocusable) &&
            !expectedAutomationIds.contains(element.automationId))
            return Fail(error, L"application-adapter proxy contains an unexpected actionable element");
    }

    const auto findMenuElement = [&](std::wstring_view automationId,
                                     ComPtr<IUIAutomationElement>& found) -> bool {
        VARIANT value{};
        VariantInit(&value);
        value.vt = VT_BSTR;
        value.bstrVal = SysAllocStringLen(automationId.data(),
            static_cast<UINT>(automationId.size()));
        if (!value.bstrVal) return Fail(error, L"menu UIA identity allocation failed");
        ComPtr<IUIAutomationCondition> condition;
        const HRESULT conditionResult = automation->CreatePropertyCondition(
            UIA_AutomationIdPropertyId, value, &condition);
        VariantClear(&value);
        if (FAILED(conditionResult) || !condition)
            return FailHr(error, L"menu UIA identity condition failed", conditionResult);
        ComPtr<IUIAutomationElementArray> matches;
        const HRESULT findResult = projectionRoot->FindAll(TreeScope_Descendants,
            condition.Get(), &matches);
        int matchCount = 0;
        if (FAILED(findResult) || !matches || FAILED(matches->get_Length(&matchCount)))
            return FailHr(error, L"menu UIA identity query failed", findResult);
        if (matchCount != 1)
            return Fail(error, L"projected menu AutomationId is missing or duplicated");
        if (FAILED(matches->GetElement(0, &found)) || !found)
            return Fail(error, L"projected menu element is unavailable");
        return true;
    };
    // MenuFlyout descendants live in transient popup HWNDs and are not part of
    // the desktop UIA tree until a user expands their parent. The commit gate
    // validates the persistent menu bar contract only; protocol validation and
    // renderer construction validate the complete closed flyout hierarchy.
    for (const auto& item : snapshot.menu) {
        const auto expectedId = L"FluentShell.Menu." + item.itemId;
        ComPtr<IUIAutomationElement> element;
        if (!findMenuElement(expectedId, element)) return false;
        ElementInfo info;
        if (!ReadElementInfo(element.Get(), info, error)) return false;
        if (item.kind != MenuItemKind::Popup ||
            info.processId != static_cast<int>(options.rendererProcessId) ||
            info.framework != L"XAML" || info.controlType != UIA_MenuItemControlTypeId ||
            info.name != DisplayText(item.text) ||
            info.isEnabled != (item.enabled ? TRUE : FALSE) || !info.isControl)
            return Fail(error, L"projected top-level menu role, name, or enabled state is incorrect");
        if (!HasPattern(element.Get(), RequiredPattern::ExpandCollapse, error)) return false;
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
            { snapshot.bounds.left + width / 2, snapshot.bounds.top + 12 },
            { snapshot.bounds.left + width / 2, snapshot.bounds.top + height / 2 },
            { snapshot.bounds.left + width / 4, snapshot.bounds.top + height / 3 },
            { snapshot.bounds.left + width * 3 / 4, snapshot.bounds.top + height / 3 },
            { snapshot.bounds.left + width / 4, snapshot.bounds.top + height * 2 / 3 },
            { snapshot.bounds.left + width * 3 / 4, snapshot.bounds.top + height * 2 / 3 },
        };
        bool exposedProxy = false;
        bool resolvedProxy = false;
        for (const auto& point : samples) {
            const HWND before = WindowFromPoint(point);
            ComPtr<IUIAutomationElement> hit;
            const HRESULT hitResult = automation->ElementFromPoint(point, &hit);
            const HWND after = WindowFromPoint(point);
            if (before && after &&
                GetAncestor(before, GA_ROOT) == options.proxy &&
                GetAncestor(after, GA_ROOT) == options.proxy) {
                exposedProxy = true;
            }
            if (FAILED(hitResult) || !hit) continue;
            int hitPid = 0;
            if (SUCCEEDED(hit->get_CurrentProcessId(&hitPid)) &&
                hitPid == static_cast<int>(options.rendererProcessId) &&
                IsDescendant(automation.Get(), root.Get(), hit.Get())) {
                resolvedProxy = true;
                break;
            }
        }
        if (!ScreenHitTestMatchesExposure(exposedProxy, resolvedProxy))
            return Fail(error, L"screen hit-test does not resolve to the exposed proxy tree");
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
    if (g_projectedUiaWorkerPoisoned.load(std::memory_order_acquire))
        return Fail(error, L"UIA validation disabled after an abandoned provider worker");

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
        // A projected surface is validated cross-process while it is rasterizing
        // for the first time, so the budget has to cover a provider that is still
        // warming up.  It stays bounded: an abandoned worker still poisons UIA for
        // the session rather than being joined.
        if (future.wait_for(std::chrono::milliseconds(5000)) != std::future_status::ready) {
            g_projectedUiaWorkerPoisoned.store(true, std::memory_order_release);
            worker.detach();
            return Fail(error,
                L"UIA validation deadline expired; worker abandoned and session poisoned");
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
