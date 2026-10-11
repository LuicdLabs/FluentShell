#include "DialogTranslator.h"

#include "../../Common/FluentShell.h"

namespace FluentShell::Bridge::Translation {
namespace {

// MessageBox flags whose semantics the projection does not reproduce: a help
// button reports WM_HELP to a callback, and the service, desktop and system/task
// modal forms are not owned by a window the projection can stand in for.
constexpr UINT kNativeOnlyMessageBoxFlags =
    MB_HELP | MB_SERVICE_NOTIFICATION | MB_DEFAULT_DESKTOP_ONLY | MB_SYSTEMMODAL | MB_TASKMODAL;

bool KeepsUserLanguage(WORD languageId) noexcept {
    // V1 keeps the target's normal user language.  Explicit alternate language
    // requests remain native rather than silently relabeling.
    return languageId == 0 || languageId == MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL) ||
        languageId == GetUserDefaultUILanguage();
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
        (type & kNativeOnlyMessageBoxFlags) != 0 || (type & MB_USERICON) == MB_USERICON ||
        !KeepsUserLanguage(languageId)) {
        return std::nullopt;
    }
    return session->ShowMessageBox(
        owner, text, caption ? std::wstring_view(caption) : std::wstring_view(), type, languageId);
}

std::optional<int> TranslateMessageBoxIndirect(
    const std::shared_ptr<RendererSession>& session,
    const MSGBOXPARAMSW* params) {
    if (!session || !session->IsReady() || !params || params->cbSize != sizeof(MSGBOXPARAMSW) ||
        !params->lpszText || (params->dwStyle & kNativeOnlyMessageBoxFlags) != 0 ||
        !KeepsUserLanguage(static_cast<WORD>(params->dwLanguageId))) {
        return std::nullopt;
    }
    auto resolve = [&](LPCWSTR value, std::wstring& result) {
        if (!value) return true;
        if (!IS_INTRESOURCE(value)) {
            result = value;
            return true;
        }
        const wchar_t* resource = nullptr;
        const int length = LoadStringW(params->hInstance,
            static_cast<UINT>(reinterpret_cast<ULONG_PTR>(value) & 0xffffu),
            reinterpret_cast<LPWSTR>(&resource), 0);
        if (length <= 0 || !resource) return false;
        result.assign(resource, static_cast<size_t>(length));
        return true;
    };
    std::wstring text;
    std::wstring caption;
    if (!resolve(params->lpszText, text) || !resolve(params->lpszCaption, caption)) return std::nullopt;
    UINT type = params->dwStyle;
    DialogIconImage icon;
    if ((type & MB_USERICON) == MB_USERICON) {
        const HWND owner = params->hwndOwner;
        const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
        const int size = MulDiv(32, static_cast<int>(dpi ? dpi : 96), 96);
        // An icon that cannot be read leaves the message box native rather than
        // showing it without the picture the application chose.
        if (!LoadDialogIcon(params->hInstance, params->lpszIcon, size, icon)) return std::nullopt;
        type &= ~MB_ICONMASK;
    }
    return session->ShowMessageBox(params->hwndOwner, text, caption, type,
        static_cast<WORD>(params->dwLanguageId), icon.Empty() ? nullptr : &icon);
}

std::optional<TaskDialogResult> TranslateTaskDialog(
    const std::shared_ptr<RendererSession>& session,
    const TASKDIALOGCONFIG* config) {
    if (!session || !session->IsReady() || !config) return std::nullopt;
    return session->ShowTaskDialog(*config);
}

} // namespace FluentShell::Bridge::Translation
