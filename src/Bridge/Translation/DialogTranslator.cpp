#include "DialogTranslator.h"

#include "../../Common/FluentShell.h"

#include <algorithm>
#include <unordered_set>

namespace FluentShell::Bridge::Translation {
namespace {

bool ResolveString(
    PCWSTR value,
    HINSTANCE instance,
    std::wstring& result) {
    if (!value) {
        result.clear();
        return true;
    }
    if (!IS_INTRESOURCE(value)) {
        result = value;
        return result.size() <= Ipc::kMaxStringChars;
    }
    if (!instance) instance = GetModuleHandleW(nullptr);
    wchar_t buffer[65537]{};
    const UINT resourceId = static_cast<UINT>(
        reinterpret_cast<ULONG_PTR>(value) & 0xffffu);
    const int length = LoadStringW(
        instance, resourceId, buffer,
        static_cast<int>(std::size(buffer)));
    if (length <= 0 || length > static_cast<int>(Ipc::kMaxStringChars)) return false;
    result.assign(buffer, static_cast<size_t>(length));
    return true;
}

bool HasUnsupportedTaskDialogFlags(const TASKDIALOGCONFIG& config) {
    constexpr TASKDIALOG_FLAGS kUnsupported =
        TDF_ENABLE_HYPERLINKS |
        TDF_USE_COMMAND_LINKS |
        TDF_USE_COMMAND_LINKS_NO_ICON |
        TDF_SHOW_PROGRESS_BAR |
        TDF_SHOW_MARQUEE_PROGRESS_BAR |
        TDF_CALLBACK_TIMER |
        TDF_EXPAND_FOOTER_AREA |
        TDF_EXPANDED_BY_DEFAULT |
        TDF_VERIFICATION_FLAG_CHECKED;
    // TDF_VERIFICATION_FLAG_CHECKED is handled, not rejected.
    return (config.dwFlags & (kUnsupported & ~TDF_VERIFICATION_FLAG_CHECKED)) != 0 ||
        (config.dwFlags & (TDF_USE_HICON_MAIN | TDF_USE_HICON_FOOTER)) != 0 ||
        config.pfCallback != nullptr || config.cRadioButtons != 0 ||
        config.pRadioButtons != nullptr || config.pszExpandedInformation != nullptr ||
        (config.pszMainIcon != nullptr &&
         config.pszMainIcon != TD_WARNING_ICON &&
         config.pszMainIcon != TD_ERROR_ICON &&
         config.pszMainIcon != TD_INFORMATION_ICON &&
         config.pszMainIcon != TD_SHIELD_ICON);
}

bool BuildTaskButtons(
    const TASKDIALOGCONFIG& config,
    std::vector<std::pair<int, std::wstring>>& buttons) {
    std::unordered_set<int> ids;
    auto add = [&](int id, PCWSTR label) {
        if (!ids.insert(id).second) return false;
        std::wstring text;
        if (!ResolveString(label, config.hInstance, text) || text.empty()) return false;
        buttons.emplace_back(id, std::move(text));
        return true;
    };
    if (config.cButtons != 0 || config.pButtons != nullptr) {
        if (!config.pButtons || config.cButtons > 64) return false;
        for (UINT index = 0; index < config.cButtons; ++index) {
            if (!add(config.pButtons[index].nButtonID, config.pButtons[index].pszButtonText)) return false;
        }
    }
    const auto common = config.dwCommonButtons;
    if ((common & TDCBF_OK_BUTTON) && !add(IDOK, L"OK")) return false;
    if ((common & TDCBF_YES_BUTTON) && !add(IDYES, L"Yes")) return false;
    if ((common & TDCBF_NO_BUTTON) && !add(IDNO, L"No")) return false;
    if ((common & TDCBF_RETRY_BUTTON) && !add(IDRETRY, L"Retry")) return false;
    if ((common & TDCBF_CANCEL_BUTTON) && !add(IDCANCEL, L"Cancel")) return false;
    if ((common & TDCBF_CLOSE_BUTTON) && !add(IDCLOSE, L"Close")) return false;
    if (buttons.empty()) buttons.emplace_back(IDOK, L"OK");
    return true;
}

} // namespace

std::optional<int> TranslateMessageBox(
    const std::shared_ptr<RendererSession>& session,
    HWND owner,
    LPCWSTR text,
    LPCWSTR caption,
    UINT type,
    WORD languageId) {
    if (!session || !session->IsReady() || !text ||
        (type & (MB_HELP | MB_SERVICE_NOTIFICATION | MB_DEFAULT_DESKTOP_ONLY |
                 MB_SYSTEMMODAL | MB_TASKMODAL)) != 0) {
        return std::nullopt;
    }
    if (languageId != 0 && languageId != MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL)) {
        // V1 keeps the target's normal user language.  Explicit alternate
        // language requests remain native rather than silently relabeling.
        return std::nullopt;
    }
    return session->ShowMessageBox(
        owner, text, caption ? std::wstring_view(caption) : std::wstring_view(), type, languageId);
}

std::optional<TaskDialogResult> TranslateTaskDialog(
    const std::shared_ptr<RendererSession>& session,
    const TASKDIALOGCONFIG* config) {
    if (!session || !session->IsReady() || !config ||
        config->cbSize < sizeof(TASKDIALOGCONFIG) ||
        HasUnsupportedTaskDialogFlags(*config)) {
        return std::nullopt;
    }
    std::wstring title;
    std::wstring instruction;
    std::wstring content;
    std::wstring footer;
    std::wstring verification;
    if (!ResolveString(config->pszWindowTitle, config->hInstance, title) ||
        !ResolveString(config->pszMainInstruction, config->hInstance, instruction) ||
        !ResolveString(config->pszContent, config->hInstance, content) ||
        !ResolveString(config->pszFooter, config->hInstance, footer) ||
        !ResolveString(config->pszVerificationText, config->hInstance, verification)) {
        return std::nullopt;
    }
    std::vector<std::pair<int, std::wstring>> buttons;
    if (!BuildTaskButtons(*config, buttons)) return std::nullopt;
    if (config->nDefaultButton != 0 &&
        std::none_of(buttons.begin(), buttons.end(), [&](const auto& button) {
            return button.first == config->nDefaultButton;
        })) {
        return std::nullopt;
    }
    return session->ShowTaskDialog(*config, buttons, title, instruction, content, footer, verification);
}

} // namespace FluentShell::Bridge::Translation
