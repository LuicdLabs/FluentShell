#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace FluentShell::Bridge::Ipc {

inline constexpr uint32_t kFrameMagic = 0x48534C46u; // "FLSH" in little endian.
inline constexpr uint16_t kProtocolMajor = 1;
// Writers emit this minor; same-major readers accept any uint16 minor and ignore unknown JSON fields.
// Minor 1 requires the renderer-side provisional interaction gate. A Bridge
// must not pair with a minor-0 renderer because that peer would treat the
// pre-UIA provisional commit as interactive.
inline constexpr uint16_t kProtocolMinor = 1;
inline constexpr uint32_t kMaxPayloadBytes = 4u * 1024u * 1024u;
inline constexpr size_t kMaxJsonDepth = 32;
inline constexpr size_t kMaxStringChars = 65536;
inline constexpr size_t kMaxNodes = 512;
inline constexpr size_t kMaxListItems = 4096;

enum class MessageType : uint16_t {
    Hello = 1,
    WindowOpen = 2,
    WindowPatch = 3,
    ActionInvoke = 4,
    ActionResult = 5,
    SurfaceReady = 6,
    SurfaceCommit = 7,
    WindowClose = 8,
    Heartbeat = 9,
    Error = 10,
    Shutdown = 11,
};

#pragma pack(push, 1)
struct FrameHeader final {
    uint32_t magic = kFrameMagic;
    uint16_t major = kProtocolMajor;
    uint16_t minor = kProtocolMinor;
    uint16_t type = 0;
    uint16_t flags = 0;
    uint32_t payloadLength = 0;
    uint64_t sequence = 0;
    uint64_t revision = 0;
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 32, "FLSH frame header must remain 32 bytes");

struct Frame final {
    FrameHeader header{};
    std::string payload;
};

bool IsKnownMessageType(uint16_t value) noexcept;
bool ValidateHeader(const FrameHeader& header, uint64_t previousSequence, std::wstring& error) noexcept;
std::string WideToUtf8(std::wstring_view value);
std::wstring Utf8ToWide(std::string_view value);
std::wstring HwndToString(HWND hwnd);
bool TryParseHwnd(std::wstring_view value, HWND& hwnd) noexcept;
std::wstring UInt64ToString(uint64_t value);
bool TryParseUInt64(std::wstring_view value, uint64_t& result) noexcept;
std::wstring NewGuidString();
std::wstring NewNonceHex();
uint64_t ProcessCreationTime(HANDLE process) noexcept;

} // namespace FluentShell::Bridge::Ipc
