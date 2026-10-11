#pragma once

#include "RendererSession.h"

#include <optional>

namespace FluentShell::Bridge::Translation {

std::optional<int> TranslateMessageBox(
    const std::shared_ptr<RendererSession>& session,
    HWND owner,
    LPCWSTR text,
    LPCWSTR caption,
    UINT type,
    WORD languageId = 0);

// MessageBoxIndirect adds a resource text and an application icon (MB_USERICON).
std::optional<int> TranslateMessageBoxIndirect(
    const std::shared_ptr<RendererSession>& session,
    const MSGBOXPARAMSW* params);

std::optional<TaskDialogResult> TranslateTaskDialog(
    const std::shared_ptr<RendererSession>& session,
    const TASKDIALOGCONFIG* config);

} // namespace FluentShell::Bridge::Translation
