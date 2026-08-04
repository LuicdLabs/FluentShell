#include "FrameCodec.h"

#include <limits>

namespace FluentShell::Bridge::Ipc {
namespace {

bool CompleteIo(
    HANDLE pipe,
    OVERLAPPED& overlapped,
    DWORD timeoutMs,
    DWORD& transferred,
    std::wstring_view operation,
    std::wstring& error) noexcept {
    const DWORD wait = WaitForSingleObject(overlapped.hEvent, timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        error = std::wstring(operation) + L" timed out";
        return false;
    }
    if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) {
        error = std::wstring(operation) + L" failed (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    return true;
}

bool ReadExact(
    HANDLE pipe,
    void* destination,
    DWORD size,
    ULONGLONG deadline,
    bool infinite,
    std::wstring& error) noexcept {
    auto* cursor = static_cast<unsigned char*>(destination);
    DWORD remaining = size;
    while (remaining != 0) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
            error = L"pipe read event allocation failed";
            return false;
        }
        DWORD read = 0;
        BOOL ok = ReadFile(pipe, cursor, remaining, nullptr, &overlapped);
        DWORD lastError = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && lastError == ERROR_IO_PENDING) {
            DWORD waitMs = INFINITE;
            if (!infinite) {
                const ULONGLONG now = GetTickCount64();
                const ULONGLONG remainingMs = deadline > now ? deadline - now : 0;
                waitMs = remainingMs >= INFINITE
                    ? INFINITE - 1
                    : static_cast<DWORD>(remainingMs);
            }
            ok = CompleteIo(pipe, overlapped, waitMs, read, L"pipe read", error);
        } else if (ok) {
            ok = GetOverlappedResult(pipe, &overlapped, &read, TRUE);
        }
        if (!ok || read == 0) lastError = GetLastError();
        CloseHandle(overlapped.hEvent);
        if (!ok || read == 0) {
            if (error.empty()) error = L"pipe read failed (" + std::to_wstring(lastError) + L")";
            return false;
        }
        cursor += read;
        remaining -= read;
    }
    return true;
}

bool WriteExact(
    HANDLE pipe,
    const void* source,
    DWORD size,
    ULONGLONG deadline,
    bool infinite,
    std::wstring& error) noexcept {
    const auto* cursor = static_cast<const unsigned char*>(source);
    DWORD remaining = size;
    while (remaining != 0) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
            error = L"pipe write event allocation failed";
            return false;
        }
        DWORD written = 0;
        BOOL ok = WriteFile(pipe, cursor, remaining, nullptr, &overlapped);
        DWORD lastError = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && lastError == ERROR_IO_PENDING) {
            DWORD waitMs = INFINITE;
            if (!infinite) {
                const ULONGLONG now = GetTickCount64();
                const ULONGLONG remainingMs = deadline > now ? deadline - now : 0;
                waitMs = remainingMs >= INFINITE
                    ? INFINITE - 1
                    : static_cast<DWORD>(remainingMs);
            }
            ok = CompleteIo(pipe, overlapped, waitMs, written, L"pipe write", error);
        } else if (ok) {
            ok = GetOverlappedResult(pipe, &overlapped, &written, TRUE);
        }
        if (!ok || written == 0) lastError = GetLastError();
        CloseHandle(overlapped.hEvent);
        if (!ok || written == 0) {
            if (error.empty()) error = L"pipe write failed (" + std::to_wstring(lastError) + L")";
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

} // namespace

bool ReadFrame(
    HANDLE pipe,
    uint64_t previousSequence,
    Frame& frame,
    std::wstring& error,
    DWORD timeoutMs) noexcept {
    frame = {};
    const bool infinite = timeoutMs == INFINITE;
    const ULONGLONG deadline = infinite ? 0 : GetTickCount64() + timeoutMs;
    if (!ReadExact(
            pipe, &frame.header, sizeof(frame.header), deadline, infinite, error)) {
        return false;
    }
    if (!ValidateHeader(frame.header, previousSequence, error)) return false;
    frame.payload.resize(frame.header.payloadLength);
    if (!frame.payload.empty() &&
        !ReadExact(
            pipe, frame.payload.data(), frame.header.payloadLength,
            deadline, infinite, error)) {
        return false;
    }
    return true;
}

bool WriteFrame(
    HANDLE pipe,
    MessageType type,
    uint64_t sequence,
    uint64_t revision,
    std::string_view payload,
    std::wstring& error,
    DWORD timeoutMs) noexcept {
    if (!pipe || pipe == INVALID_HANDLE_VALUE) {
        error = L"pipe is not connected";
        return false;
    }
    if (sequence == 0 || payload.size() > kMaxPayloadBytes ||
        payload.size() > std::numeric_limits<uint32_t>::max()) {
        error = L"invalid outbound frame";
        return false;
    }
    FrameHeader header{};
    header.type = static_cast<uint16_t>(type);
    header.payloadLength = static_cast<uint32_t>(payload.size());
    header.sequence = sequence;
    header.revision = revision;
    const bool infinite = timeoutMs == INFINITE;
    const ULONGLONG deadline = infinite ? 0 : GetTickCount64() + timeoutMs;
    if (!WriteExact(pipe, &header, sizeof(header), deadline, infinite, error)) return false;
    if (!payload.empty() &&
        !WriteExact(pipe, payload.data(), static_cast<DWORD>(payload.size()),
            deadline, infinite, error)) {
        return false;
    }
    return true;
}

} // namespace FluentShell::Bridge::Ipc
