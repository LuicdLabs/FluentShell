#pragma once

#include "Protocol.h"

#include <string>

namespace FluentShell::Bridge::Ipc {

bool ReadFrame(
    HANDLE pipe,
    uint64_t previousSequence,
    Frame& frame,
    std::wstring& error,
    DWORD timeoutMs = INFINITE) noexcept;
bool WriteFrame(
    HANDLE pipe,
    MessageType type,
    uint64_t sequence,
    uint64_t revision,
    std::string_view payload,
    std::wstring& error,
    DWORD timeoutMs = INFINITE) noexcept;

} // namespace FluentShell::Bridge::Ipc
