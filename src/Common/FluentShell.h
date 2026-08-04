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

inline void Log(std::wstring_view msg) {
    OutputDebugStringW(kLogPrefix);
    OutputDebugStringW(msg.data());
    OutputDebugStringW(L"\n");

    // Also append to %TEMP%\FluentShell.log for field diagnosis without a debugger.
    wchar_t temp[MAX_PATH]{};
    if (GetTempPathW(static_cast<DWORD>(std::size(temp)), temp) > 0) {
        std::wstring path = temp;
        path += L"FluentShell.log";
        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            SYSTEMTIME st{};
            GetLocalTime(&st);
            wchar_t line[1024]{};
            swprintf_s(line, L"%02u:%02u:%02u.%03u %s%.*s\r\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                kLogPrefix,
                static_cast<int>(msg.size()), msg.data());
            DWORD written = 0;
            // UTF-8 for notepad-friendly logs.
            char utf8[2048]{};
            const int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
            if (n > 1) {
                WriteFile(h, utf8, static_cast<DWORD>(n - 1), &written, nullptr);
            }
            CloseHandle(h);
        }
    }
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
