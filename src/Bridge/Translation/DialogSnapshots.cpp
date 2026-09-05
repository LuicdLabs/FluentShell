#include "DialogSnapshots.h"

#include <algorithm>

#include "../Ipc/Protocol.h"

namespace FluentShell::Bridge::Translation {

RECT CenteredBounds(HWND owner, LONG width, LONG height) {
    RECT reference{};
    if (!owner || !GetWindowRect(owner, &reference)) {
        const HMONITOR monitor = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{ sizeof(info) };
        GetMonitorInfoW(monitor, &info);
        reference = info.rcWork;
    }
    // The Win32 min/max macros are in scope here, so the algorithm calls are
    // parenthesized to keep them from being expanded.
    const LONG x = reference.left + (std::max)(0L, (reference.right - reference.left - width) / 2);
    const LONG y = reference.top + (std::max)(0L, (reference.bottom - reference.top - height) / 2);
    return { x, y, x + width, y + height };
}

std::wstring MessageBoxIcon(UINT type) {
    switch (type & MB_ICONMASK) {
    case MB_ICONHAND: return L"error";
    case MB_ICONQUESTION: return L"question";
    case MB_ICONEXCLAMATION: return L"warning";
    case MB_ICONASTERISK: return L"info";
    default: return L"none";
    }
}

std::wstring TaskDialogIcon(PCWSTR icon) {
    if (!icon) return L"none";
    if (icon == TD_WARNING_ICON) return L"warning";
    if (icon == TD_ERROR_ICON) return L"error";
    if (icon == TD_INFORMATION_ICON) return L"info";
    if (icon == TD_SHIELD_ICON) return L"shield";
    return L"none";
}

WindowSnapshot BuildMessageBoxSnapshot(
    HWND owner,
    std::wstring_view text,
    std::wstring_view caption,
    UINT type,
    std::unordered_map<uint64_t, int>& results) {
    WindowSnapshot snapshot;
    snapshot.surfaceId = Ipc::NewGuidString();
    snapshot.surfaceKind = SurfaceKind::MessageBox;
    snapshot.modal = true;
    snapshot.generation = GetTickCount64();
    snapshot.revision = 1;
    snapshot.ownerHwnd = owner;
    snapshot.title = caption.empty() ? L"Message" : std::wstring(caption);
    snapshot.dpi = owner ? GetDpiForWindow(owner) : 96;
    if (!snapshot.dpi) snapshot.dpi = 96;
    snapshot.bounds = CenteredBounds(owner, 540, 300);
    snapshot.clientBounds = { 0, 0, 540, 300 };
    snapshot.windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    snapshot.windowExStyle = WS_EX_DLGMODALFRAME;
    snapshot.showInTaskbar = owner == nullptr;
    snapshot.icon = MessageBoxIcon(type);
    snapshot.rtl = (type & (MB_RTLREADING | MB_RIGHT)) != 0;

    std::vector<std::pair<int, std::wstring>> buttons;
    switch (type & MB_TYPEMASK) {
    case MB_OKCANCEL: buttons = {{ IDOK, L"OK" }, { IDCANCEL, L"Cancel" }}; break;
    case MB_ABORTRETRYIGNORE: buttons = {{ IDABORT, L"Abort" }, { IDRETRY, L"Retry" }, { IDIGNORE, L"Ignore" }}; break;
    case MB_YESNOCANCEL: buttons = {{ IDYES, L"Yes" }, { IDNO, L"No" }, { IDCANCEL, L"Cancel" }}; break;
    case MB_YESNO: buttons = {{ IDYES, L"Yes" }, { IDNO, L"No" }}; break;
    case MB_RETRYCANCEL: buttons = {{ IDRETRY, L"Retry" }, { IDCANCEL, L"Cancel" }}; break;
    case MB_CANCELTRYCONTINUE: buttons = {{ IDCANCEL, L"Cancel" }, { IDTRYAGAIN, L"Try Again" }, { IDCONTINUE, L"Continue" }}; break;
    default: buttons = {{ IDOK, L"OK" }}; break;
    }
    snapshot.canCancel = std::any_of(buttons.begin(), buttons.end(), [](const auto& button) {
        return button.first == IDCANCEL;
    });

    uint64_t nodeId = 1;
    ControlNode content;
    content.nodeId = nodeId++;
    content.generation = snapshot.generation;
    content.kind = ControlKind::StaticText;
    content.rect = { 32, 40, 508, 190 };
    content.text = std::wstring(text);
    content.automationName = content.text;
    snapshot.nodes.push_back(std::move(content));

    int defaultIndex = static_cast<int>((type & MB_DEFMASK) >> 8);
    defaultIndex = (std::clamp)(defaultIndex, 0, static_cast<int>(buttons.size()) - 1);
    const LONG buttonWidth = 112;
    const LONG gap = 12;
    LONG left = 508 - static_cast<LONG>(buttons.size()) * buttonWidth -
        static_cast<LONG>(buttons.size() - 1) * gap;
    for (size_t index = 0; index < buttons.size(); ++index) {
        ControlNode button;
        button.nodeId = nodeId++;
        button.generation = snapshot.generation;
        button.kind = ControlKind::Button;
        button.controlId = buttons[index].first;
        // Paint order doubles as the z-index, which the protocol requires to be
        // unique per node: nodes.size() is the count already appended, so each node
        // lands one step above the previous one.
        button.zIndex = static_cast<int>(snapshot.nodes.size());
        button.rect = { left, 224, left + buttonWidth, 264 };
        button.visible = true;
        button.enabled = true;
        button.tabStop = true;
        button.tabIndex = static_cast<int>(index);
        button.isDefault = static_cast<int>(index) == defaultIndex;
        button.text = buttons[index].second;
        button.automationName = button.text;
        results.emplace(button.nodeId, buttons[index].first);
        snapshot.nodes.push_back(std::move(button));
        left += buttonWidth + gap;
    }
    return snapshot;
}

WindowSnapshot BuildTaskDialogSnapshot(
    const TASKDIALOGCONFIG& config,
    const std::vector<std::pair<int, std::wstring>>& buttons,
    std::wstring_view title,
    std::wstring_view instruction,
    std::wstring_view content,
    std::wstring_view footer,
    std::wstring_view verification,
    std::unordered_map<uint64_t, int>& results,
    std::optional<uint64_t>& verificationNode) {
    WindowSnapshot snapshot;
    snapshot.surfaceId = Ipc::NewGuidString();
    snapshot.surfaceKind = SurfaceKind::TaskDialog;
    snapshot.modal = true;
    snapshot.canCancel = (config.dwFlags & TDF_ALLOW_DIALOG_CANCELLATION) != 0 ||
        std::any_of(buttons.begin(), buttons.end(), [](const auto& value) { return value.first == IDCANCEL; });
    snapshot.icon = TaskDialogIcon(config.pszMainIcon);
    snapshot.generation = GetTickCount64();
    snapshot.revision = 1;
    snapshot.ownerHwnd = config.hwndParent;
    snapshot.title = title.empty() ? L"Dialog" : std::wstring(title);
    snapshot.dpi = config.hwndParent ? GetDpiForWindow(config.hwndParent) : 96;
    if (!snapshot.dpi) snapshot.dpi = 96;
    snapshot.bounds = CenteredBounds(config.hwndParent, 620, 410);
    snapshot.clientBounds = { 0, 0, 620, 410 };
    snapshot.windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    snapshot.windowExStyle = WS_EX_DLGMODALFRAME;
    snapshot.showInTaskbar = config.hwndParent == nullptr;
    snapshot.rtl = (config.dwFlags & TDF_RTL_LAYOUT) != 0;

    uint64_t nodeId = 1;
    // Paint order doubles as the z-index, which the protocol requires to be unique
    // per node: nodes.size() is the count already appended, so each node lands one
    // step above the previous one.
    auto addText = [&](std::wstring_view value, const RECT& rect) {
        if (value.empty()) return;
        ControlNode node;
        node.nodeId = nodeId++;
        node.generation = snapshot.generation;
        node.kind = ControlKind::StaticText;
        node.zIndex = static_cast<int>(snapshot.nodes.size());
        node.rect = rect;
        node.text = std::wstring(value);
        node.automationName = node.text;
        snapshot.nodes.push_back(std::move(node));
    };
    addText(instruction, { 32, 32, 588, 82 });
    addText(content, { 32, 88, 588, 205 });
    addText(footer, { 32, 252, 588, 292 });
    if (!verification.empty()) {
        ControlNode check;
        check.nodeId = nodeId++;
        check.generation = snapshot.generation;
        check.kind = ControlKind::CheckBox;
        check.controlId = -1;
        check.zIndex = static_cast<int>(snapshot.nodes.size());
        check.rect = { 32, 212, 588, 244 };
        check.tabStop = true;
        check.tabIndex = 0;
        check.text = std::wstring(verification);
        check.automationName = check.text;
        check.checked = (config.dwFlags & TDF_VERIFICATION_FLAG_CHECKED) != 0 ? 1 : 0;
        verificationNode = check.nodeId;
        snapshot.nodes.push_back(std::move(check));
    }
    const LONG buttonWidth = 120;
    const LONG gap = 12;
    LONG left = 588 - static_cast<LONG>(buttons.size()) * buttonWidth -
        static_cast<LONG>(buttons.empty() ? 0 : buttons.size() - 1) * gap;
    int buttonTabIndex = verification.empty() ? 0 : 1;
    for (const auto& [id, label] : buttons) {
        ControlNode button;
        button.nodeId = nodeId++;
        button.generation = snapshot.generation;
        button.kind = ControlKind::Button;
        button.controlId = id;
        button.zIndex = static_cast<int>(snapshot.nodes.size());
        button.rect = { left, 324, left + buttonWidth, 366 };
        button.tabStop = true;
        button.tabIndex = buttonTabIndex++;
        button.isDefault = id == config.nDefaultButton ||
            (config.nDefaultButton == 0 && results.empty());
        button.text = label;
        button.automationName = label;
        results.emplace(button.nodeId, id);
        snapshot.nodes.push_back(std::move(button));
        left += buttonWidth + gap;
    }
    return snapshot;
}

}  // namespace FluentShell::Bridge::Translation
