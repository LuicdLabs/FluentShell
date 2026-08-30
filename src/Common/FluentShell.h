#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace FluentShell {

inline constexpr wchar_t kBridgeDllName[] = L"FluentShell.Bridge.dll";
inline constexpr wchar_t kLogPrefix[] = L"[FluentShell] ";

enum class Backdrop : uint32_t {
    Auto = 0,
    None = 1,
    Mica = 2,
    MicaAlt = 3,
    Acrylic = 4,
};

struct DwmStyle {
    bool darkMode = true;
    Backdrop backdrop = Backdrop::Mica;
    bool roundCorners = true;
    bool extendFrame = false;
};

struct MatchRule {
    std::wstring exe;      // empty = any
    std::wstring className; // empty = any
    std::wstring title;     // empty = any; substring match
    DwmStyle dwm{};
};

// Diagnostics are the only view into a projection that failed inside an injected
// process, so this must never lose or truncate a record: rejection reasons carry
// several hundred characters of HWND/style evidence.
inline void Log(std::wstring_view msg) {
    std::wstring line;
    try {
        line.reserve(msg.size() + 32);
        line.append(kLogPrefix);
        line.append(msg);
        line.push_back(L'\n');
    } catch (...) {
        OutputDebugStringW(L"[FluentShell] log allocation failed\n");
        return;
    }
    // std::wstring_view is not guaranteed to be null-terminated, so the debugger
    // string is always built here instead of passing msg.data() through.
    OutputDebugStringW(line.c_str());

    // Also append to %TEMP%\FluentShell.log for field diagnosis without a debugger.
    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) == 0) return;
    std::wstring path = temp;
    path += L"FluentShell.log";
    const HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[24]{};
    swprintf_s(stamp, L"%02u:%02u:%02u.%03u ",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    std::wstring record = stamp;
    record.append(line, 0, line.size() - 1);
    record.append(L"\r\n");

    // UTF-8 for notepad-friendly logs.  Size the conversion from the record
    // rather than a fixed buffer so a long reason cannot be cut in half.
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, record.c_str(),
        static_cast<int>(record.size()), nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
        std::string utf8(static_cast<size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, record.c_str(),
                static_cast<int>(record.size()), utf8.data(), bytes,
                nullptr, nullptr) == bytes) {
            DWORD written = 0;
            WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }
    CloseHandle(file);
}

inline std::wstring GetProcessImagePath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 4]{};
    DWORD size = static_cast<DWORD>(std::size(buf));
    std::wstring result;
    if (QueryFullProcessImageNameW(h, 0, buf, &size)) {
        result.assign(buf, size);
    }
    CloseHandle(h);
    return result;
}

inline std::wstring FileNameOf(std::wstring_view path) {
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring_view::npos) return std::wstring(path);
    return std::wstring(path.substr(pos + 1));
}

inline bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (towlower(a[i]) != towlower(b[i])) return false;
    }
    return true;
}

inline bool ContainsIgnoreCase(std::wstring_view hay, std::wstring_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (towlower(hay[i + j]) != towlower(needle[j])) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

} // namespace FluentShell
