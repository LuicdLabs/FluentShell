#include "InjectorCore.h"
#include "Watcher.h"

#include <cerrno>
#include <cwctype>
#include <iostream>
#include <optional>
#include <string>

namespace {

using FluentShell::Injector::InjectOptions;

bool ParsePid(const wchar_t* text, DWORD& result) {
    if (!text || !*text) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long value = wcstoul(text, &end, 10);
    if (errno != 0 || !end || *end != L'\0' || value == 0 || value > MAXDWORD) {
        return false;
    }
    result = static_cast<DWORD>(value);
    return true;
}

bool IsSha256Text(std::wstring_view value) {
    if (value.size() != 64) return false;
    for (const wchar_t c : value) {
        if (!iswxdigit(c)) return false;
    }
    return true;
}

std::optional<InjectOptions> ParseInjectOptions(int argc, wchar_t** argv) {
    if (argc < 3) return std::nullopt;

    InjectOptions options;
    options.requestedImagePath = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::wstring_view option = argv[i];
        if (option == L"--pid") {
            if (options.pid || i + 1 >= argc) return std::nullopt;
            DWORD pid = 0;
            if (!ParsePid(argv[++i], pid)) return std::nullopt;
            options.pid = pid;
        } else if (option == L"--sha256") {
            if (options.sha256 || i + 1 >= argc) return std::nullopt;
            std::wstring hash = argv[++i];
            if (!IsSha256Text(hash)) return std::nullopt;
            for (auto& c : hash) c = static_cast<wchar_t>(towlower(c));
            options.sha256 = std::move(hash);
        } else if (option == L"--signer") {
            std::wcerr << L"--signer is not implemented; use --sha256 to pin the target binary.\n";
            return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    return options;
}

void PrintUsage() {
    std::wcout <<
        L"FluentShell.Injector - explicit Win32 to WinUI translation\n\n"
        L"Usage:\n"
        L"  FluentShell.Injector inject <absolute-image-path> [--pid <pid>] [--sha256 <hex>]\n"
        L"  FluentShell.Injector watch [--dry-run]  global apply until the panic hotkey or the GUI stops it;\n"
        L"                                        --dry-run reports eligibility and injects nothing\n"
        L"  FluentShell.Injector watch-warning    print the risk warning that must be accepted first\n\n"
        L"Injection requires one canonical image path. If more than one matching process is\n"
        L"running, --pid is mandatory. Bridge is loaded directly; Renderer\\FluentShell.Renderer.exe\n"
        L"must be in the production root's Renderer folder. 32-bit targets are served by x86\\.\n"
        L"Name-only injection is intentionally unsupported. watch applies only to windows of this\n"
        L"user's session, at this user's integrity level, and never starts at logon.\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::wstring_view command = argv[1];
    if (command == L"inject") {
        const auto options = ParseInjectOptions(argc, argv);
        if (!options) {
            PrintUsage();
            return 1;
        }
        FluentShell::Injector::EnableDebugPrivilege();
        return FluentShell::Injector::InjectRequestedTarget(*options);
    }
    if (command == L"watch" && (argc == 2 || (argc == 3 && std::wstring_view(argv[2]) == L"--dry-run"))) {
        FluentShell::Injector::EnableDebugPrivilege();
        return FluentShell::Injector::RunWatch(argc == 3);
    }
    if (command == L"watch-warning" && argc == 2) {
        return FluentShell::Injector::PrintRiskWarning();
    }
    PrintUsage();
    return 1;
}
