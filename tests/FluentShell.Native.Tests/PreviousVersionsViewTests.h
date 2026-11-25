#pragma once

// Include after main.cpp's Check helper, inside its test namespace.
//
// The Previous Versions page (twext.dll) of a shell Properties sheet shows its versions in a
// shell folder view whose DirectUI items view is read through MSAA.  Only the empty view is
// projected: a list root with one header of column headers and one message.  The shape
// decision is a pure function of the MSAA description, so it is driven here with the shape
// measured on the real sheet (zh-TW) and its refusals, without twext.  The identity check is
// driven with look-alike windows this test registers, which it must refuse.

namespace PreviousVersionsViewFixture {

using Element = Translation::PreviousVersionsElement;

Element Make(long role, long state, std::wstring name, RECT rect, std::vector<Element> children = {}) {
    Element element;
    element.role = role;
    element.state = state;
    element.name = std::move(name);
    element.rect = rect;
    element.childCount = static_cast<long>(children.size());
    element.children = std::move(children);
    return element;
}

// The island's client area in screen pixels, as measured: 366 by 274 at 1519,609.
constexpr RECT kClient{ 1519, 609, 1885, 883 };

Element Header(std::vector<Element> columns) {
    return Make(ROLE_SYSTEM_LIST, STATE_SYSTEM_READONLY, L"標頭", RECT{ 1519, 609, 1885, 634 },
        std::move(columns));
}

std::vector<Element> Columns() {
    constexpr long state = STATE_SYSTEM_FOCUSABLE | STATE_SYSTEM_READONLY;
    return {
        Make(ROLE_SYSTEM_SPLITBUTTON, state, L"名稱", RECT{ 1519, 609, 1724, 634 }),
        Make(ROLE_SYSTEM_SPLITBUTTON, state, L"修改日期", RECT{ 1724, 609, 1868, 634 }),
    };
}

Element Message(std::wstring text = L"沒有先前版本可供使用。") {
    return Make(ROLE_SYSTEM_STATICTEXT,
        STATE_SYSTEM_FOCUSABLE | STATE_SYSTEM_READONLY | STATE_SYSTEM_FOCUSED,
        std::move(text), RECT{ 1519, 640, 1885, 673 });
}

// The provider's two invisible window children (each owning seven elements of its own,
// which are never read because the element is invisible).
Element HiddenWindow() {
    Element element = Make(ROLE_SYSTEM_WINDOW,
        STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN | STATE_SYSTEM_FOCUSABLE, L"",
        RECT{});
    return element;
}

Element EmptyView(std::vector<Element> extra = {}) {
    std::vector<Element> children{ Header(Columns()), Message(), HiddenWindow(), HiddenWindow() };
    for (auto& element : extra) children.push_back(std::move(element));
    return Make(ROLE_SYSTEM_LIST, 0, L"項目檢視", kClient, std::move(children));
}

bool Decide(const Element& root, std::vector<Translation::AccessibleIslandItem>& items,
    std::wstring& reason) {
    reason.clear();
    return Translation::DecidePreviousVersionsView(root, kClient, items, reason);
}

bool RefusedWith(const Element& root, std::wstring_view expected) {
    std::vector<Translation::AccessibleIslandItem> items;
    std::wstring reason;
    return !Decide(root, items, reason) && items.empty() &&
        reason.find(expected) != std::wstring::npos;
}

LRESULT CALLBACK LookAlikeProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace PreviousVersionsViewFixture

void TestPreviousVersionsViewShape() {
    using namespace PreviousVersionsViewFixture;
    using Translation::AccessibleItemKind;
    std::vector<Translation::AccessibleIslandItem> items;
    std::wstring reason;

    // The measured empty view: the two column names and the message, inert, where they are.
    Check(Decide(EmptyView(), items, reason), "previous versions empty view was refused");
    Check(items.size() == 3, "previous versions empty view did not publish three texts");
    if (items.size() == 3) {
        Check(items[0].kind == AccessibleItemKind::Text && items[0].name == L"名稱" &&
            items[0].rect.left == 0 && items[0].rect.top == 0 &&
            items[0].rect.right == 205 && items[0].rect.bottom == 25,
            "previous versions first column is not published at its rectangle");
        Check(items[1].name == L"修改日期" && items[1].rect.left == 205 && items[1].rect.right == 349,
            "previous versions second column is not published at its rectangle");
        Check(items[2].kind == AccessibleItemKind::Text && items[2].name == L"沒有先前版本可供使用。" &&
            items[2].rect.top == 31 && items[2].rect.bottom == 64 && items[2].rect.right == 366,
            "previous versions message is not published at its rectangle");
        bool inert = true;
        for (const auto& item : items) {
            if (!item.actionName.empty() || item.dropDown || item.selected || item.expanded)
                inert = false;
        }
        Check(inert, "previous versions items carry an action");
    }

    // An empty view with no message (still being filled) is the same contract.
    {
        Element root = EmptyView();
        root.children.erase(root.children.begin() + 1);
        root.childCount = static_cast<long>(root.children.size());
        Check(Decide(root, items, reason) && items.size() == 2,
            "previous versions view without a message was refused");
    }

    // A listed version keeps the whole window native.
    Check(RefusedWith(EmptyView({ Make(ROLE_SYSTEM_LISTITEM, STATE_SYSTEM_SELECTABLE,
            L"dev", RECT{ 1519, 640, 1885, 662 }) }), L"previous versions list has entries"),
        "previous versions list item was not refused");
    // An invisible element is skipped, whatever it is.
    Check(Decide(EmptyView({ Make(ROLE_SYSTEM_LISTITEM, STATE_SYSTEM_INVISIBLE, L"", RECT{}) }),
            items, reason) && items.size() == 3,
        "previous versions invisible element was not skipped");
    Check(RefusedWith(EmptyView({ Message(L"第二則訊息") }), L"more than one message"),
        "previous versions second message was not refused");
    {
        std::vector<Element> columns = Columns();
        columns.push_back(Make(ROLE_SYSTEM_PUSHBUTTON, 0, L"更多", RECT{ 1868, 609, 1885, 634 }));
        Element root = EmptyView();
        root.children[0] = Header(std::move(columns));
        Check(RefusedWith(root, L"non-column child"),
            "previous versions header with a non-column child was not refused");
    }
    {
        Element root = EmptyView();
        root.children[0] = Header({});
        Check(RefusedWith(root, L"header has no columns"),
            "previous versions header without columns was not refused");
    }
    Check(RefusedWith(EmptyView({ Header(Columns()) }), L"more than one header"),
        "previous versions second header was not refused");
    {
        Element root = EmptyView();
        root.children.erase(root.children.begin());
        root.childCount = static_cast<long>(root.children.size());
        Check(RefusedWith(root, L"has no header"), "previous versions view without a header was not refused");
    }
    Check(RefusedWith(EmptyView({ Make(ROLE_SYSTEM_PUSHBUTTON, 0, L"還原",
            RECT{ 1519, 700, 1600, 720 }) }), L"role 43"),
        "previous versions unexpected role was not refused");
    {
        Element root = EmptyView();
        root.role = ROLE_SYSTEM_PANE;
        Check(RefusedWith(root, L"root is not a list"), "previous versions non-list root was not refused");
    }
    {
        Element root = EmptyView();
        root.children[1].rect.bottom = kClient.bottom + 10;
        Check(RefusedWith(root, L"falls outside the view"),
            "previous versions message outside the view was not refused");
    }
    {
        Element root = EmptyView();
        root.children[1].children.push_back(Make(ROLE_SYSTEM_LINK, 0, L"x", RECT{ 1519, 640, 1530, 650 }));
        root.children[1].childCount = 1;
        Check(RefusedWith(root, L"message owns children"),
            "previous versions message with children was not refused");
    }
    {
        // The provider counted more children than could be read.
        Element root = EmptyView();
        root.childCount += 1;
        Check(RefusedWith(root, L"could not all be read"),
            "previous versions partially read view was not refused");
    }
}

void TestPreviousVersionsViewIdentity() {
    using namespace PreviousVersionsViewFixture;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    // Look-alikes of the shell's classes, registered by this test executable.  Local classes are
    // per module, so these are found first for windows created with this instance.
    for (const wchar_t* name : { L"SHELLDLL_DefView", L"DirectUIHWND" }) {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = &LookAlikeProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = name;
        const bool registered = RegisterClassW(&windowClass) != 0 ||
            GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        Check(registered, "previous versions look-alike class was not registered");
    }
    HWND root = CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"previous-versions-identity",
        WS_OVERLAPPEDWINDOW, 20, 20, 460, 520, nullptr, nullptr, instance, nullptr);
    Check(root != nullptr, "previous versions identity root was not created");
    if (!root) return;
    // A child #32770 page with twext's template ids, but this executable's instance.
    HWND page = CreateWindowExW(WS_EX_CONTROLPARENT, L"#32770", L"以前的版本",
        WS_CHILD | WS_VISIBLE, 0, 0, 400, 440, root, nullptr, instance, nullptr);
    Check(page != nullptr, "previous versions identity page was not created");
    if (page) {
        struct Control final { int id; const wchar_t* className; DWORD style; };
        const Control controls[]{
            { 201, L"Static", SS_ICON }, { 202, WC_LINK, 0 }, { 203, L"Static", 0 },
            { 207, L"Static", 0 }, { 204, L"Button", BS_SPLITBUTTON }, { 209, L"Button", BS_SPLITBUTTON },
        };
        for (const Control& control : controls) {
            CreateWindowExW(0, control.className, L"", WS_CHILD | control.style, 0, 0, 10, 10, page,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(control.id)), instance, nullptr);
        }
        HWND defView = CreateWindowExW(0, L"SHELLDLL_DefView", L"", WS_CHILD | WS_VISIBLE | WS_BORDER,
            14, 113, 368, 276, page, nullptr, instance, nullptr);
        HWND island = defView ? CreateWindowExW(0, L"DirectUIHWND", L"", WS_CHILD | WS_VISIBLE,
            0, 0, 366, 274, defView, nullptr, instance, nullptr) : nullptr;
        Check(defView && island, "previous versions look-alike view was not created");
        if (defView && island) {
            Check(!Translation::WindowClassRegisteredBySystemModule(defView, L"shell32.dll"),
                "previous versions look-alike view claims shell32's class");
            Check(!Translation::IsPreviousVersionsFolderView(defView),
                "previous versions identity accepted a folder view registered by the test");
            Check(!Translation::IsPreviousVersionsView(island),
                "previous versions identity accepted an island under a look-alike folder view");
            // The folder view is not a container anywhere but on twext's page.
            Translation::ControlKind kind{};
            std::wstring reason;
            Check(!Translation::ClassifyControl(defView, kind, reason) &&
                    reason.find(L"Previous Versions page") != std::wstring::npos,
                "a shell folder view outside the Previous Versions page was admitted");
            // And the look-alike island never reaches the Previous Versions reader's actions.
            std::wstring invokeReason;
            Check(!Translation::InvokeAccessibleIslandItem(island, 0, L"名稱", L"", invokeReason) &&
                    invokeReason.find(L"previous versions") == std::wstring::npos,
                "a look-alike island was treated as the Previous Versions view");
        }
    }
    DestroyWindow(root);
}
