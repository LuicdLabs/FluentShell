#pragma once

// Include after main.cpp's Check helper, inside its test namespace.
//
// aclui.dll's permission checklist (CHECKLIST_ACLUI) on a folder's Security page, rebuilt with the
// layout measured on the real sheet: a Static label per permission at id row*4, an Allow cell at
// row*4+1 and a Deny cell at row*4+2.  On the read-only page a checked cell is a 16x16 SS_ICON and
// an unchecked one a 0x0 SS_ICON window; in the Edit dialog every cell is a BS_AUTOCHECKBOX.  The
// layout is what lets the checklist be admitted without a pixel verdict; the class's identity is
// the registering module, which a look-alike registered by this test does not have.

namespace AclChecklistFixture {

struct Window final {
    HWND handle = nullptr;
    ~Window() { if (handle) DestroyWindow(handle); }
};

constexpr wchar_t kLookAlikeClass[] = L"CHECKLIST_ACLUI";

bool RegisterLookAlike() {
    WNDCLASSEXW type{ sizeof(type) };
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    type.lpszClassName = kLookAlikeClass;
    return RegisterClassExW(&type) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// Rows of { label, allow checked, deny checked }.  editable builds the Edit dialog's check boxes.
HWND BuildChecklist(HWND parent, bool editable, int rows, bool strayChild = false, int misalignedRow = -1,
    int missingCellRow = -1) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const HWND checklist = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_CONTROLPARENT, kLookAlikeClass, L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_TABSTOP, 12, 236, 356, 136, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(1004)), instance, nullptr);
    if (!checklist) return nullptr;
    for (int row = 0; row < rows; ++row) {
        const int top = 6 + row * 21;
        const auto id = [&](int column) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(row * 4 + column)); };
        CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"Static", L"Permission", 0x5001C18C,
            12, top, 222, 15, checklist, id(0), instance, nullptr);
        for (int column = 1; column <= 2; ++column) {
            if (row == missingCellRow && column == 2) continue;
            const int left = (column == 1 ? 247 : 279) + (row == misalignedRow && column == 2 ? 5 : 0);
            if (editable) {
                CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"Button", L"Allow Permission", 0x5001C003,
                    left, top, 16, 15, checklist, id(column), instance, nullptr);
            } else {
                const bool checked = column == 1;
                CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"Static", checked ? L"Allow Permission" : L"",
                    0x50000083, left, top, checked ? 16 : 0, checked ? 16 : 0, checklist, id(column),
                    instance, nullptr);
            }
        }
    }
    if (strayChild)
        CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"Edit", L"", WS_CHILD | WS_VISIBLE, 12, 120, 80, 15,
            checklist, reinterpret_cast<HMENU>(static_cast<INT_PTR>(400)), instance, nullptr);
    return checklist;
}

} // namespace AclChecklistFixture

void TestAclChecklistLayout() {
    using namespace AclChecklistFixture;
    Check(RegisterLookAlike(), "checklist look-alike class could not be registered");
    Window dialog{ CreateWindowExW(0, WC_DIALOG, L"Security", WS_POPUP | WS_CAPTION, 40, 40, 420, 520,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr) };
    Check(dialog.handle != nullptr, "checklist host dialog could not be created");
    if (!dialog.handle) return;

    // Whether the verdict matched; the reason is printed only when it did not (test.ps1 treats a
    // native test's stderr as a failure under Windows PowerShell).
    const auto layout = [&](bool expected, bool editable, int rows, bool stray, int misaligned, int missing) {
        Window checklist{ BuildChecklist(dialog.handle, editable, rows, stray, misaligned, missing) };
        std::wstring reason;
        const bool accepted = checklist.handle && Translation::HasAclChecklistLayout(checklist.handle, reason);
        if (accepted != expected) std::wcerr << L"checklist layout verdict unexpected: " << reason << L'\n';
        return accepted == expected;
    };
    Check(layout(true, false, 7, false, -1, -1), "the read-only Security page's checklist layout was refused");
    Check(layout(true, true, 7, false, -1, -1), "the Edit dialog's check box checklist layout was refused");
    Check(layout(true, false, 0, false, -1, -1), "an empty checklist (no principal selected) was refused");
    Check(layout(false, false, 7, true, -1, -1), "a checklist with a stray child was accepted");
    Check(layout(false, true, 7, false, 3, -1), "a checklist with a misaligned column was accepted");
    Check(layout(false, true, 7, false, -1, 4), "a checklist row without its Deny cell was accepted");

    // The identity is the registering module: this look-alike's class is the test executable's, so
    // the contract never applies to it and it is judged as an unregistered class always was.
    Window lookAlike{ BuildChecklist(dialog.handle, false, 3) };
    Check(lookAlike.handle && !Translation::WindowClassRegisteredBySystemModule(lookAlike.handle, L"aclui.dll"),
        "a CHECKLIST_ACLUI registered outside System32's aclui.dll passed as aclui's checklist");
}
