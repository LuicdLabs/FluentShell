#include "Protocol.h"

#include <bcrypt.h>
#include <objbase.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")

namespace FluentShell::Bridge::Ipc {

bool IsKnownMessageType(uint16_t value) noexcept {
    return value >= static_cast<uint16_t>(MessageType::Hello) &&
           value <= static_cast<uint16_t>(MessageType::Shutdown);
}

bool ValidateHeader(
    const FrameHeader& header,
    uint64_t previousSequence,
    std::wstring& error) noexcept {
    if (header.magic != kFrameMagic) {
        error = L"invalid frame magic";
        return false;
    }
    if (header.major != kProtocolMajor) {
        error = L"incompatible protocol major";
        return false;
    }
    if (!IsKnownMessageType(header.type)) {
        error = L"unknown message type";
        return false;
    }
    if (header.flags != 0) {
        error = L"unsupported frame flags";
        return false;
    }
    if (header.payloadLength > kMaxPayloadBytes) {
        error = L"frame payload exceeds limit";
        return false;
    }
    if (header.sequence == 0 || header.sequence <= previousSequence) {
        error = L"duplicate or out-of-order sequence";
        return false;
    }
    return true;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size) != size) {
        return {};
    }
    return result;
}

std::wstring HwndToString(HWND hwnd) {
    std::wostringstream stream;
    stream << L"0x" << std::uppercase << std::hex << std::setw(16)
           << std::setfill(L'0')
           << static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd));
    return stream.str();
}

bool TryParseHwnd(std::wstring_view value, HWND& hwnd) noexcept {
    hwnd = nullptr;
    if (value.size() < 3 || value[0] != L'0' || (value[1] != L'x' && value[1] != L'X')) {
        return false;
    }
    unsigned long long raw = 0;
    for (size_t index = 2; index < value.size(); ++index) {
        const wchar_t c = value[index];
        unsigned digit = 0;
        if (c >= L'0' && c <= L'9') digit = static_cast<unsigned>(c - L'0');
        else if (c >= L'a' && c <= L'f') digit = 10u + static_cast<unsigned>(c - L'a');
        else if (c >= L'A' && c <= L'F') digit = 10u + static_cast<unsigned>(c - L'A');
        else return false;
        if (raw > (std::numeric_limits<unsigned long long>::max() - digit) / 16u) return false;
        raw = raw * 16u + digit;
    }
    hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(raw));
    return true;
}

std::wstring UInt64ToString(uint64_t value) {
    return std::to_wstring(value);
}

bool TryParseUInt64(std::wstring_view value, uint64_t& result) noexcept {
    if (value.empty() || value.size() > 20 || (value.size() > 1 && value.front() == L'0')) {
        return false;
    }
    uint64_t parsed = 0;
    for (const wchar_t c : value) {
        if (c < L'0' || c > L'9') return false;
        const uint64_t digit = static_cast<uint64_t>(c - L'0');
        if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    result = parsed;
    return true;
}

std::wstring NewGuidString() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return {};
    wchar_t text[40]{};
    if (StringFromGUID2(guid, text, static_cast<int>(std::size(text))) <= 2) return {};
    std::wstring result(text);
    if (!result.empty() && result.front() == L'{') result.erase(result.begin());
    if (!result.empty() && result.back() == L'}') result.pop_back();
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(std::towlower(value));
    });
    return result;
}

std::wstring NewNonceHex() {
    std::array<unsigned char, 16> bytes{};
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        return {};
    }
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char value : bytes) {
        result.push_back(kHex[value >> 4]);
        result.push_back(kHex[value & 0x0f]);
    }
    return result;
}

uint64_t ProcessCreationTime(HANDLE process) noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!process || !GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return value.QuadPart;
}

} // namespace FluentShell::Bridge::Ipc
