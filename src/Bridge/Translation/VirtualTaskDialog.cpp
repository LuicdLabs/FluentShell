#include "VirtualTaskDialog.h"

#include "../../Common/FluentShell.h"
#include "../Ipc/Protocol.h"

#include <algorithm>
#include <unordered_set>

namespace FluentShell::Bridge::Translation {
namespace {

constexpr wchar_t kHostClass[] = L"FluentShell.TaskDialogHost";
// A renderer action, posted to the host with an owned ActionRequest in lParam.
constexpr UINT kActionMessage = WM_APP + 0x51;
// Sent after a navigation has been applied, so TDN_NAVIGATED follows the TDM_
// NAVIGATE_PAGE call the way comctl32 orders it.
constexpr UINT kNavigatedMessage = WM_APP + 0x52;
constexpr UINT_PTR kCallbackTimer = 1;
constexpr UINT kCallbackTimerPeriod = 200;

HMODULE ThisModule() noexcept {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ThisModule), &module);
    return module;
}

bool ResolveString(PCWSTR value, HINSTANCE instance, std::wstring& result) {
    if (!value) {
        result.clear();
        return true;
    }
    if (!IS_INTRESOURCE(value)) {
        result = value;
        return result.size() <= Ipc::kMaxStringChars;
    }
    if (!instance) instance = GetModuleHandleW(nullptr);
    const wchar_t* resource = nullptr;
    const UINT resourceId = static_cast<UINT>(reinterpret_cast<ULONG_PTR>(value) & 0xffffu);
    // LoadString with a zero buffer length returns a read-only pointer to the
    // resource itself, which has no fixed size cap.
    const int length = LoadStringW(instance, resourceId, reinterpret_cast<LPWSTR>(&resource), 0);
    if (length <= 0 || !resource || length > static_cast<int>(Ipc::kMaxStringChars)) return false;
    result.assign(resource, static_cast<size_t>(length));
    return true;
}

bool IsStandardTaskDialogIcon(PCWSTR icon) noexcept {
    return icon && TaskDialogIcon(icon) != L"none";
}

// The icon of a TASKDIALOGCONFIG member: a TD_* icon by name, or the pixels of
// the application's HICON or icon resource, scaled to `size`.
bool ResolveIcon(bool useHandle, HICON handle, PCWSTR name, HINSTANCE instance, int size,
    TaskDialogIconModel& icon) {
    icon = {};
    if (useHandle) {
        if (!handle) return true;
        HICON scaled = static_cast<HICON>(CopyImage(handle, IMAGE_ICON, size, size, LR_COPYFROMRESOURCE));
        if (!scaled) scaled = static_cast<HICON>(CopyImage(handle, IMAGE_ICON, size, size, 0));
        const bool captured = CaptureDialogIcon(scaled ? scaled : handle, icon.image);
        if (scaled) DestroyIcon(scaled);
        return captured;
    }
    if (!name) return true;
    if (IsStandardTaskDialogIcon(name)) {
        icon.standard = TaskDialogIcon(name);
        return true;
    }
    // An application icon: from the module the configuration names, or a system
    // icon (IDI_*) when it names none.
    return LoadDialogIcon(instance, name, size, icon.image) ||
        (instance && LoadDialogIcon(nullptr, name, size, icon.image));
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* dialog = reinterpret_cast<VirtualTaskDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!dialog || message == WM_NCCREATE || message == WM_NCDESTROY) {
        if (message == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return dialog->HandleMessage(hwnd, message, wParam, lParam);
}

bool RegisterHostClass() {
    static const bool registered = [] {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = HostProc;
        windowClass.hInstance = ThisModule();
        windowClass.lpszClassName = kHostClass;
        return RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

} // namespace

std::unique_ptr<VirtualTaskDialog> VirtualTaskDialog::Create(
    const TASKDIALOGCONFIG& config, HWND owner, std::wstring& reason) {
    std::unique_ptr<VirtualTaskDialog> dialog(new VirtualTaskDialog());
    dialog->owner_ = owner;
    dialog->dpi_ = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    if (!dialog->dpi_) dialog->dpi_ = 96;
    if (!dialog->Load(config, reason)) return nullptr;
    return dialog;
}

VirtualTaskDialog::~VirtualTaskDialog() {
    if (host_ && IsWindow(host_)) {
        SetWindowLongPtrW(host_, GWLP_USERDATA, 0);
        DestroyWindow(host_);
    }
}

bool VirtualTaskDialog::Load(const TASKDIALOGCONFIG& config, std::wstring& reason) {
    if (config.cbSize != sizeof(TASKDIALOGCONFIG)) {
        reason = L"TASKDIALOGCONFIG has an unknown size";
        return false;
    }
    TaskDialogModel model;
    const HINSTANCE instance = config.hInstance;
    if (!ResolveString(config.pszWindowTitle, instance, model.title) ||
        !ResolveString(config.pszMainInstruction, instance, model.instruction) ||
        !ResolveString(config.pszContent, instance, model.content) ||
        !ResolveString(config.pszExpandedInformation, instance, model.expandedInformation) ||
        !ResolveString(config.pszFooter, instance, model.footer) ||
        !ResolveString(config.pszVerificationText, instance, model.verification) ||
        !ResolveString(config.pszExpandedControlText, instance, model.expandedControlText) ||
        !ResolveString(config.pszCollapsedControlText, instance, model.collapsedControlText)) {
        reason = L"a task dialog string resource could not be loaded";
        return false;
    }
    const TASKDIALOG_FLAGS flags = config.dwFlags;
    const bool commandLinks = (flags & (TDF_USE_COMMAND_LINKS | TDF_USE_COMMAND_LINKS_NO_ICON)) != 0;
    std::unordered_set<int> ids;
    if (config.cButtons != 0) {
        if (!config.pButtons || config.cButtons > 64) {
            reason = L"task dialog custom buttons are missing or too many";
            return false;
        }
        for (UINT index = 0; index < config.cButtons; ++index) {
            TaskDialogButtonModel button;
            button.id = config.pButtons[index].nButtonID;
            if (!ids.insert(button.id).second) continue;
            if (!ResolveString(config.pButtons[index].pszButtonText, instance, button.text)) {
                reason = L"a task dialog button label could not be loaded";
                return false;
            }
            button.commandLink = commandLinks;
            if (commandLinks) {
                // A command link's first line is its label and the rest its note.
                const size_t newline = button.text.find(L'\n');
                if (newline != std::wstring::npos) {
                    button.note = button.text.substr(newline + 1);
                    button.text.erase(newline);
                    if (!button.text.empty() && button.text.back() == L'\r') button.text.pop_back();
                }
            }
            model.buttons.push_back(std::move(button));
        }
    }
    static constexpr std::pair<TASKDIALOG_COMMON_BUTTON_FLAGS, int> kCommon[] = {
        { TDCBF_OK_BUTTON, IDOK }, { TDCBF_YES_BUTTON, IDYES }, { TDCBF_NO_BUTTON, IDNO },
        { TDCBF_RETRY_BUTTON, IDRETRY }, { TDCBF_CANCEL_BUTTON, IDCANCEL }, { TDCBF_CLOSE_BUTTON, IDCLOSE },
    };
    for (const auto& [flag, id] : kCommon) {
        if ((config.dwCommonButtons & flag) == 0 || !ids.insert(id).second) continue;
        TaskDialogButtonModel button;
        button.id = id;
        button.text = TaskDialogButtonLabel(id);
        model.buttons.push_back(std::move(button));
    }
    // A dialog that declares no button at all gets an OK button.
    if (model.buttons.empty()) {
        TaskDialogButtonModel button;
        button.id = IDOK;
        button.text = TaskDialogButtonLabel(IDOK);
        model.buttons.push_back(std::move(button));
    }
    model.defaultButton = config.nDefaultButton;
    if (model.defaultButton != 0 &&
        std::none_of(model.buttons.begin(), model.buttons.end(),
            [&](const auto& button) { return button.id == model.defaultButton; })) {
        model.defaultButton = 0;
    }

    if (config.cRadioButtons != 0) {
        if (!config.pRadioButtons || config.cRadioButtons > 64) {
            reason = L"task dialog radio buttons are missing or too many";
            return false;
        }
        for (UINT index = 0; index < config.cRadioButtons; ++index) {
            TaskDialogRadioModel radio;
            radio.id = config.pRadioButtons[index].nButtonID;
            if (!ResolveString(config.pRadioButtons[index].pszButtonText, instance, radio.text)) {
                reason = L"a task dialog radio button label could not be loaded";
                return false;
            }
            model.radios.push_back(std::move(radio));
        }
        if ((flags & TDF_NO_DEFAULT_RADIO_BUTTON) == 0) {
            const bool known = std::any_of(model.radios.begin(), model.radios.end(),
                [&](const auto& radio) { return radio.id == config.nDefaultRadioButton; });
            model.selectedRadio = known ? config.nDefaultRadioButton : model.radios.front().id;
        }
    }

    const int iconSize = MulDiv(32, static_cast<int>(dpi_), 96);
    const int footerIconSize = MulDiv(16, static_cast<int>(dpi_), 96);
    if (!ResolveIcon((flags & TDF_USE_HICON_MAIN) != 0, config.hMainIcon, config.pszMainIcon,
            instance, iconSize, model.mainIcon) ||
        !ResolveIcon((flags & TDF_USE_HICON_FOOTER) != 0, config.hFooterIcon, config.pszFooterIcon,
            instance, footerIconSize, model.footerIcon)) {
        // An icon that cannot be read is left out rather than refusing the dialog.
        FluentShell::Log(L"Task dialog icon could not be read; showing the dialog without it");
    }

    model.canCancel = (flags & TDF_ALLOW_DIALOG_CANCELLATION) != 0 ||
        std::any_of(model.buttons.begin(), model.buttons.end(),
            [](const auto& button) { return button.id == IDCANCEL; });
    model.verificationChecked = (flags & TDF_VERIFICATION_FLAG_CHECKED) != 0;
    model.expanded = (flags & TDF_EXPANDED_BY_DEFAULT) != 0;
    model.expandFooterArea = (flags & TDF_EXPAND_FOOTER_AREA) != 0;
    model.hyperlinks = (flags & TDF_ENABLE_HYPERLINKS) != 0;
    model.rtl = (flags & TDF_RTL_LAYOUT) != 0;
    model.minimizable = (flags & TDF_CAN_BE_MINIMIZED) != 0;
    model.sizeToContent = (flags & TDF_SIZE_TO_CONTENT) != 0;
    model.commandLinkIcons = (flags & TDF_USE_COMMAND_LINKS_NO_ICON) == 0;
    model.widthDlu = config.cxWidth;
    if ((flags & TDF_SHOW_MARQUEE_PROGRESS_BAR) != 0) model.progress = TaskDialogProgress::Marquee;
    else if ((flags & TDF_SHOW_PROGRESS_BAR) != 0) model.progress = TaskDialogProgress::Normal;

    model_ = std::move(model);
    callback_ = config.pfCallback;
    callbackData_ = config.lpCallbackData;
    instance_ = instance;
    flags_ = flags;
    return true;
}

WindowSnapshot VirtualTaskDialog::InitialSnapshot() {
    const HWND reference = (flags_ & TDF_POSITION_RELATIVE_TO_WINDOW) != 0 ? owner_ : nullptr;
    auto snapshot = BuildTaskDialogSnapshot(model_, owner_, reference, dpi_, std::nullopt, elements_);
    surfaceId_ = snapshot.surfaceId;
    // The surface stands for the host window the callback addresses.
    snapshot.nativeHwnd = host_;
    generation_ = snapshot.generation;
    return snapshot;
}

bool VirtualTaskDialog::Start(Sink sink) {
    sink_ = std::move(sink);
    if (!RegisterHostClass()) return false;
    // Unowned and never shown: a window the application opens over the dialog
    // (owned by this HWND) must not inherit the cloak of the projected owner.
    host_ = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, model_.title.c_str(), WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, ThisModule(), this);
    if (!host_) return false;
    timerBase_ = GetTickCount64();
    if ((flags_ & TDF_CALLBACK_TIMER) != 0) SetTimer(host_, kCallbackTimer, kCallbackTimerPeriod, nullptr);
    return true;
}

void VirtualTaskDialog::Announce() {
    announced_ = true;
    Notify(TDN_DIALOG_CONSTRUCTED);
    if (!completed_) Notify(TDN_CREATED);
    // comctl32 clicks the default radio button once the dialog is up.
    if (!completed_ && model_.selectedRadio != 0)
        Notify(TDN_RADIO_BUTTON_CLICKED, static_cast<WPARAM>(model_.selectedRadio));
}

void VirtualTaskDialog::PostAction(const ActionRequest& action) {
    const HWND host = host_;
    if (!host) return;
    auto copy = std::make_unique<ActionRequest>(action);
    if (PostMessageW(host, kActionMessage, 0, reinterpret_cast<LPARAM>(copy.get()))) copy.release();
}

void VirtualTaskDialog::Finish() {
    if (!host_) return;
    KillTimer(host_, kCallbackTimer);
    // A dialog that never announced itself (its projection did not open) was never
    // created as far as the callback knows.
    if (announced_ && !destroyed_) {
        destroyed_ = true;
        Notify(TDN_DESTROYED);
    }
    // Actions still queued for the host own their payloads.
    MSG message{};
    while (PeekMessageW(&message, host_, kActionMessage, kActionMessage, PM_REMOVE)) {
        delete reinterpret_cast<ActionRequest*>(message.lParam);
    }
    SetWindowLongPtrW(host_, GWLP_USERDATA, 0);
    DestroyWindow(host_);
    host_ = nullptr;
}

TaskDialogResult VirtualTaskDialog::Result() const noexcept {
    return { button_, model_.selectedRadio, model_.verificationChecked };
}

HRESULT VirtualTaskDialog::Notify(UINT notification, WPARAM wParam, LPARAM lParam) {
    if (!callback_ || !host_) return S_OK;
    return callback_(host_, notification, wParam, lParam, callbackData_);
}

void VirtualTaskDialog::Rebuild(uint64_t eventId) {
    if (completed_ || !sink_.publish) return;
    const HWND reference = (flags_ & TDF_POSITION_RELATIVE_TO_WINDOW) != 0 ? owner_ : nullptr;
    // The session keeps the window where it is now; only the size comes from here.
    POINT origin{ 0, 0 };
    auto snapshot = BuildTaskDialogSnapshot(model_, owner_, reference, dpi_, origin, elements_);
    snapshot.surfaceId = surfaceId_;
    snapshot.generation = generation_;
    snapshot.nativeHwnd = host_;
    for (auto& node : snapshot.nodes) node.generation = generation_;
    sink_.publish(std::move(snapshot), eventId);
}

void VirtualTaskDialog::ClickButton(int id, uint64_t eventId) {
    if (completed_) return;
    const auto button = std::find_if(model_.buttons.begin(), model_.buttons.end(),
        [&](const auto& candidate) { return candidate.id == id; });
    const bool known = button != model_.buttons.end();
    if (known && !button->enabled) return;
    if (!known && !(id == IDCANCEL && model_.canCancel)) return;
    // S_FALSE keeps the dialog open; anything else lets the click close it.
    if (Notify(TDN_BUTTON_CLICKED, static_cast<WPARAM>(id)) == S_FALSE) {
        if (eventId) Rebuild(eventId);
        return;
    }
    if (completed_) return;
    button_ = id;
    completed_ = true;
    KillTimer(host_, kCallbackTimer);
    if (sink_.complete) sink_.complete();
}

void VirtualTaskDialog::ClickRadio(int id, uint64_t eventId) {
    const auto radio = std::find_if(model_.radios.begin(), model_.radios.end(),
        [&](const auto& candidate) { return candidate.id == id; });
    if (radio == model_.radios.end() || !radio->enabled) {
        if (eventId) Rebuild(eventId);
        return;
    }
    model_.selectedRadio = id;
    Rebuild(eventId);
    Notify(TDN_RADIO_BUTTON_CLICKED, static_cast<WPARAM>(id));
}

void VirtualTaskDialog::ClickVerification(bool checked, uint64_t eventId) {
    if (model_.verification.empty()) return;
    model_.verificationChecked = checked;
    Rebuild(eventId);
    Notify(TDN_VERIFICATION_CLICKED, checked ? TRUE : FALSE);
}

void VirtualTaskDialog::ApplyAction(const ActionRequest& action) {
    if (completed_) return;
    if (action.action == L"close") {
        ClickButton(IDCANCEL, action.eventId);
        return;
    }
    if (!action.nodeId) return;
    const auto found = elements_.find(*action.nodeId);
    if (found == elements_.end()) {
        Rebuild(action.eventId);
        return;
    }
    const TaskDialogElement element = found->second;
    switch (element.kind) {
    case TaskDialogElementKind::Button:
        if (action.action == L"invoke") ClickButton(element.id, action.eventId);
        break;
    case TaskDialogElementKind::Radio:
        if (action.action == L"setCheck" && action.integerValue == 1) ClickRadio(element.id, action.eventId);
        else Rebuild(action.eventId);
        break;
    case TaskDialogElementKind::Verification:
        if (action.action == L"setCheck") ClickVerification(action.integerValue != 0, action.eventId);
        break;
    case TaskDialogElementKind::Expando:
        if (action.action == L"invoke") {
            model_.expanded = !model_.expanded;
            Rebuild(action.eventId);
            Notify(TDN_EXPANDO_BUTTON_CLICKED, model_.expanded ? TRUE : FALSE);
        }
        break;
    case TaskDialogElementKind::Text: {
        // A link is named by its index: activateItem carries it, and a plain invoke
        // is the text's only (first) link.
        size_t index = 0;
        if (action.action == L"activateItem") index = static_cast<size_t>((std::max)(0, action.itemIndex));
        else if (action.action != L"invoke") break;
        if (index < element.hrefs.size()) {
            const std::wstring href = element.hrefs[index];
            Notify(TDN_HYPERLINK_CLICKED, 0, reinterpret_cast<LPARAM>(href.c_str()));
        }
        // The renderer holds the click as pending until a patch names its event.
        Rebuild(action.eventId);
        break;
    }
    }
}

bool VirtualTaskDialog::UpdateIcon(bool footer, LPARAM icon) {
    const int size = MulDiv(footer ? 16 : 32, static_cast<int>(dpi_), 96);
    const bool handle = (flags_ & (footer ? TDF_USE_HICON_FOOTER : TDF_USE_HICON_MAIN)) != 0;
    TaskDialogIconModel model;
    const bool resolved = ResolveIcon(handle, reinterpret_cast<HICON>(icon),
        reinterpret_cast<PCWSTR>(icon), instance_, size, model);
    (footer ? model_.footerIcon : model_.mainIcon) = std::move(model);
    Rebuild();
    return resolved;
}

bool VirtualTaskDialog::SetElementText(WPARAM element, LPARAM text) {
    std::wstring value;
    if (!ResolveString(reinterpret_cast<PCWSTR>(text), instance_, value)) return false;
    switch (element) {
    case TDE_CONTENT: model_.content = std::move(value); break;
    case TDE_EXPANDED_INFORMATION: model_.expandedInformation = std::move(value); break;
    case TDE_FOOTER: model_.footer = std::move(value); break;
    case TDE_MAIN_INSTRUCTION: model_.instruction = std::move(value); break;
    default: return false;
    }
    Rebuild();
    return true;
}

LRESULT VirtualTaskDialog::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case kActionMessage: {
        std::unique_ptr<ActionRequest> action(reinterpret_cast<ActionRequest*>(lParam));
        if (action) ApplyAction(*action);
        return 0;
    }
    case kNavigatedMessage:
        if (!completed_) Notify(TDN_NAVIGATED);
        return 0;
    case WM_TIMER:
        if (wParam == kCallbackTimer && !completed_) {
            const ULONGLONG now = GetTickCount64();
            const auto elapsed = static_cast<WPARAM>((std::min<ULONGLONG>)(now - timerBase_, 0xffffffffu));
            if (Notify(TDN_TIMER, elapsed) == S_FALSE) timerBase_ = now;
            return 0;
        }
        break;
    case WM_CLOSE:
        ClickButton(IDCANCEL);
        return 0;
    case WM_SETTEXT: {
        const LRESULT result = DefWindowProcW(hwnd, message, wParam, lParam);
        if (lParam) model_.title = reinterpret_cast<PCWSTR>(lParam);
        Rebuild();
        return result;
    }
    case TDM_NAVIGATE_PAGE: {
        const auto* config = reinterpret_cast<const TASKDIALOGCONFIG*>(lParam);
        std::wstring reason;
        if (!config || !Load(*config, reason)) {
            FluentShell::Log(L"Task dialog navigation refused: " + reason);
            return 0;
        }
        timerBase_ = GetTickCount64();
        KillTimer(host_, kCallbackTimer);
        if ((flags_ & TDF_CALLBACK_TIMER) != 0) SetTimer(host_, kCallbackTimer, kCallbackTimerPeriod, nullptr);
        Rebuild();
        PostMessageW(host_, kNavigatedMessage, 0, 0);
        return 0;
    }
    case TDM_CLICK_BUTTON:
        ClickButton(static_cast<int>(wParam));
        return 0;
    case TDM_SET_MARQUEE_PROGRESS_BAR:
        model_.progress = wParam ? TaskDialogProgress::Marquee : TaskDialogProgress::Normal;
        Rebuild();
        return 0;
    case TDM_SET_PROGRESS_BAR_STATE:
        // Normal, error and paused draw alike; the position is what moves.
        return TRUE;
    case TDM_SET_PROGRESS_BAR_RANGE: {
        const LRESULT previous = MAKELPARAM(model_.progressMinimum, model_.progressMaximum);
        const int minimum = LOWORD(lParam);
        const int maximum = HIWORD(lParam);
        if (maximum > minimum && (minimum != model_.progressMinimum || maximum != model_.progressMaximum)) {
            model_.progressMinimum = minimum;
            model_.progressMaximum = maximum;
            Rebuild();
        }
        return previous;
    }
    case TDM_SET_PROGRESS_BAR_POS: {
        const LRESULT previous = model_.progressPosition;
        const int position = static_cast<int>(wParam);
        if (position != model_.progressPosition) {
            model_.progressPosition = position;
            if (model_.progress == TaskDialogProgress::Normal) Rebuild();
        }
        return previous;
    }
    case TDM_SET_PROGRESS_BAR_MARQUEE:
        model_.marqueeRunning = wParam != 0;
        return 0;
    case TDM_SET_ELEMENT_TEXT:
    case TDM_UPDATE_ELEMENT_TEXT:
        return SetElementText(wParam, lParam) ? TRUE : FALSE;
    case TDM_CLICK_RADIO_BUTTON:
        ClickRadio(static_cast<int>(wParam));
        return 0;
    case TDM_ENABLE_BUTTON:
        for (auto& button : model_.buttons) {
            if (button.id == static_cast<int>(wParam) && button.enabled != (lParam != 0)) {
                button.enabled = lParam != 0;
                Rebuild();
            }
        }
        return 0;
    case TDM_ENABLE_RADIO_BUTTON:
        for (auto& radio : model_.radios) {
            if (radio.id == static_cast<int>(wParam) && radio.enabled != (lParam != 0)) {
                radio.enabled = lParam != 0;
                Rebuild();
            }
        }
        return 0;
    case TDM_CLICK_VERIFICATION:
        ClickVerification(wParam != 0);
        return 0;
    case TDM_SET_BUTTON_ELEVATION_REQUIRED_STATE:
        for (auto& button : model_.buttons) {
            if (button.id == static_cast<int>(wParam) && button.elevated != (lParam != 0)) {
                button.elevated = lParam != 0;
                Rebuild();
            }
        }
        return 0;
    case TDM_UPDATE_ICON:
        return UpdateIcon(wParam == TDIE_ICON_FOOTER, lParam) ? TRUE : FALSE;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace FluentShell::Bridge::Translation
