#pragma once

#include <fcntl.h>
#include <io.h>

// Include in main.cpp's test namespace after Check(). Diagnostic, not a test:
//
//   FluentShell.Native.Tests.exe --probe-shell-properties <path>
//
// opens the shell's own Properties sheet for <path> in this process (SHObjectProperties runs it on
// a thread of its own, exactly as Explorer does for a folder's Properties), then runs the Bridge's
// real capture on that thread through a SourceThreadAgent, once per page. Every page is a separate
// question -- which control, if any, keeps the sheet native -- so the probe selects each tab in turn
// and prints the capture's verdict and its nodes. Nothing is changed: the sheet is cancelled.

struct ShellPropertiesWindowSearch final {
    DWORD pid = 0;
    HWND found = nullptr;
};

inline BOOL CALLBACK FindShellPropertiesSheet(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<ShellPropertiesWindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    wchar_t name[64]{};
    if (pid == search.pid && IsWindowVisible(window) &&
        GetClassNameW(window, name, static_cast<int>(std::size(name))) && std::wstring_view(name) == L"#32770" &&
        FindWindowExW(window, nullptr, WC_TABCONTROLW, nullptr)) {
        search.found = window;
        return FALSE;
    }
    return TRUE;
}

inline void PrintShellPropertiesSnapshot(const Translation::WindowSnapshot& snapshot) {
    std::wcout << L"  surface title='" << snapshot.title << L"' nodes=" << snapshot.nodes.size() << L'\n';
    for (const auto& node : snapshot.nodes) {
        wchar_t name[64]{};
        if (node.hwnd) GetClassNameW(node.hwnd, name, static_cast<int>(std::size(name)));
        std::wcout << L"    " << Translation::ControlKindName(node.kind) << L" id=" << node.controlId
                   << L" class=" << name << L" style=0x" << std::hex << node.style << std::dec
                   << L" rect=" << node.rect.left << L',' << node.rect.top << L',' << node.rect.right << L','
                   << node.rect.bottom << L" checked=" << node.checked << L" enabled=" << node.enabled
                   << L" text='" << node.text.substr(0, 60) << L"'";
        if (!node.chromeRegions.empty()) std::wcout << L" chromeRegions=" << node.chromeRegions.size();
        if (!node.islandItems.empty()) std::wcout << L" islandItems=" << node.islandItems.size();
        std::wcout << L'\n';
        for (const auto& item : node.islandItems)
            std::wcout << L"      island item '" << item.name << L"'\n";
    }
}

inline void DumpShellPropertiesMsaa(IAccessible* accessible, int depth) {
    if (!accessible || depth > 4) return;
    long count = 0;
    accessible->get_accChildCount(&count);
    if (count <= 0 || count > 200) return;
    std::vector<VARIANT> children(static_cast<size_t>(count));
    long obtained = 0;
    if (FAILED(AccessibleChildren(accessible, 0, count, children.data(), &obtained))) return;
    for (long index = 0; index < obtained; ++index) {
        VARIANT& child = children[static_cast<size_t>(index)];
        IAccessible* element = accessible;
        VARIANT self{};
        self.vt = VT_I4;
        self.lVal = CHILDID_SELF;
        IAccessible* nested = nullptr;
        if (child.vt == VT_DISPATCH && child.pdispVal) {
            child.pdispVal->QueryInterface(IID_IAccessible, reinterpret_cast<void**>(&nested));
            element = nested;
        } else self = child;
        if (!element) { VariantClear(&child); continue; }
        BSTR name = nullptr;
        element->get_accName(self, &name);
        VARIANT role{}, state{};
        element->get_accRole(self, &role);
        element->get_accState(self, &state);
        long x = 0, y = 0, w = 0, h = 0;
        element->accLocation(&x, &y, &w, &h, self);
        std::wcout << std::wstring(static_cast<size_t>(depth) * 2 + 6, L' ') << L"msaa role=" << (role.vt == VT_I4 ? role.lVal : -1)
                   << L" state=0x" << std::hex << (state.vt == VT_I4 ? state.lVal : 0) << std::dec
                   << L" at " << x << L',' << y << L' ' << w << L'x' << h
                   << L" name='" << (name ? name : L"") << L"'\n";
        if (name) SysFreeString(name);
        if (nested) { DumpShellPropertiesMsaa(nested, depth + 1); nested->Release(); }
        VariantClear(&child);
    }
}

inline void DumpShellPropertiesTree(HWND parent, int depth) {
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t name[96]{}, text[160]{};
        GetClassNameW(child, name, static_cast<int>(std::size(name)));
        GetWindowTextW(child, text, static_cast<int>(std::size(text)));
        RECT rect{};
        GetWindowRect(child, &rect);
        MapWindowPoints(nullptr, GetAncestor(child, GA_ROOT), reinterpret_cast<POINT*>(&rect), 2);
        std::wcout << std::wstring(static_cast<size_t>(depth) * 2 + 4, L' ') << name << L" id=" << GetDlgCtrlID(child)
                   << L" style=0x" << std::hex << GetWindowLongPtrW(child, GWL_STYLE) << L" ex=0x"
                   << GetWindowLongPtrW(child, GWL_EXSTYLE) << std::dec << L" visible=" << (IsWindowVisible(child) != FALSE)
                   << L" rect=" << rect.left << L',' << rect.top << L',' << rect.right << L',' << rect.bottom
                   << L" text='" << text << L"'\n";
        if (!IsWindowVisible(child)) continue;
        const std::wstring_view type(name);
        if (type == L"CHECKLIST_ACLUI" || type == L"DirectUIHWND" || type == L"SysListView32") {
            IAccessible* accessible = nullptr;
            if (SUCCEEDED(AccessibleObjectFromWindow(child, static_cast<DWORD>(OBJID_CLIENT), IID_IAccessible,
                    reinterpret_cast<void**>(&accessible))) && accessible) {
                DumpShellPropertiesMsaa(accessible, depth + 1);
                accessible->Release();
            }
        }
        DumpShellPropertiesTree(child, depth + 1);
    }
}

inline int ProbeShellProperties(const wchar_t* path) {
    using ShObjectProperties = BOOL(WINAPI*)(HWND, DWORD, PCWSTR, PCWSTR);
    const HMODULE shell = LoadLibraryW(L"shell32.dll");
    const auto open = shell ? reinterpret_cast<ShObjectProperties>(GetProcAddress(shell, "SHObjectProperties")) : nullptr;
    if (!open) { std::wcerr << L"SHObjectProperties unavailable\n"; return 2; }
    if (!open(nullptr, 0x2 /* SHOP_FILEPATH */, path, nullptr)) {
        std::wcerr << L"SHObjectProperties refused " << path << L'\n';
        return 2;
    }
    ShellPropertiesWindowSearch search{ GetCurrentProcessId() };
    for (int attempt = 0; attempt < 100 && !search.found; ++attempt) {
        Sleep(100);
        EnumWindows(FindShellPropertiesSheet, reinterpret_cast<LPARAM>(&search));
    }
    if (!search.found) { std::wcerr << L"no Properties sheet appeared\n"; return 2; }
    const HWND sheet = search.found;
    // The General page counts the folder's size on a thread of its own; let it settle.
    Sleep(2500);
    const HWND tabs = FindWindowExW(sheet, nullptr, WC_TABCONTROLW, nullptr);
    const int pages = tabs ? static_cast<int>(SendMessageW(tabs, TCM_GETITEMCOUNT, 0, 0)) : 0;
    std::wcout << L"sheet 0x" << std::hex << reinterpret_cast<uintptr_t>(sheet) << std::dec
               << L" style=0x" << std::hex << GetWindowLongPtrW(sheet, GWL_STYLE)
               << L" exStyle=0x" << GetWindowLongPtrW(sheet, GWL_EXSTYLE) << std::dec
               << L" owner=0x" << std::hex << reinterpret_cast<uintptr_t>(GetWindow(sheet, GW_OWNER)) << std::dec
               << L" pages=" << pages << L'\n';
    if (const HWND owner = GetWindow(sheet, GW_OWNER)) {
        wchar_t ownerClass[64]{};
        GetClassNameW(owner, ownerClass, static_cast<int>(std::size(ownerClass)));
        std::wcout << L"  owner class=" << ownerClass << L" visible=" << (IsWindowVisible(owner) != FALSE)
                   << L" enabled=" << (IsWindowEnabled(owner) != FALSE) << L'\n';
    }
    std::wcout << L"sheet thread " << GetWindowThreadProcessId(sheet, nullptr) << L" pid " << GetCurrentProcessId() << std::endl;
    // FS_PROBE_LOOP=n repeats the first page's attach n times, so a stall lasts long enough to sample.
    wchar_t loop[16]{};
    if (GetEnvironmentVariableW(L"FS_PROBE_LOOP", loop, 16)) {
        for (int round = _wtoi(loop); round > 0; --round) {
            bool transient = false;
            auto agent = Translation::SourceThreadAgent::Attach(sheet, GetModuleHandleW(nullptr), &transient);
            std::wcout << L"loop attach " << (agent ? L"ok" : L"refused") << std::endl;
            if (agent && !agent->Shutdown()) Translation::RetainSourceThreadAgent(agent);
        }
    }
    int refused = 0;
    for (int page = 0; page < (std::max)(pages, 1); ++page) {
        if (pages) {
            SendMessageW(sheet, PSM_SETCURSEL, static_cast<WPARAM>(page), 0);
            Sleep(1500);
        }
        wchar_t label[128]{};
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = label;
        item.cchTextMax = static_cast<int>(std::size(label));
        if (tabs) SendMessageW(tabs, TCM_GETITEMW, static_cast<WPARAM>(page), reinterpret_cast<LPARAM>(&item));
        std::wcout << L"page " << page << L" '" << label << L"'\n";
        bool transient = false;
        auto agent = Translation::SourceThreadAgent::Attach(sheet, GetModuleHandleW(nullptr), &transient);
        if (!agent) {
            std::wcout << L"  REFUSED at attach (transient=" << transient << L")\n";
            DumpShellPropertiesTree(sheet, 0);
            ++refused;
            continue;
        }
        Translation::WindowSnapshot snapshot;
        std::wstring error;
        bool timedOut = false;
        if (agent->Capture(snapshot, error, 4000, nullptr, &timedOut)) PrintShellPropertiesSnapshot(snapshot);
        else {
            std::wcout << L"  REFUSED: " << error << L" (timedOut=" << timedOut << L")\n";
            ++refused;
        }
        std::wcout.flush();
        // The Bridge's own teardown: shut down on the source thread, or keep the agent alive.
        if (!agent->Shutdown()) Translation::RetainSourceThreadAgent(agent);
        agent.reset();
    }
    // FS_PROBE_HOLD_PAGE=n leaves page n open for a minute, for an outside inspector (UIA, MSAA).
    wchar_t hold[16]{};
    if (GetEnvironmentVariableW(L"FS_PROBE_HOLD_PAGE", hold, 16)) {
        SendMessageW(sheet, PSM_SETCURSEL, static_cast<WPARAM>(_wtoi(hold)), 0);
        std::wcout << L"holding page " << _wtoi(hold) << std::endl;
        Sleep(60000);
    }
    PostMessageW(sheet, WM_SYSCOMMAND, SC_CLOSE, 0);
    for (int attempt = 0; attempt < 50 && IsWindow(sheet); ++attempt) Sleep(100);
    std::wcout << L"refused pages: " << refused << L'\n';
    return refused ? 1 : 0;
}
