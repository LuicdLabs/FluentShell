#pragma once

#include "../../src/Bridge/Translation/ControlAdapters.h"
#include "../../src/Bridge/Translation/SourceThreadAgent.h"
#include "ItemImageCaptureTests.h"

#include <array>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace FluentShell::Tests {
namespace OwnerDataListView {

using namespace Bridge::Translation;

struct Fixture final {
    HWND root = nullptr;
    HWND list = nullptr;
    HIMAGELIST images = nullptr;
    DWORD threadId = GetCurrentThreadId();
    std::vector<std::array<std::wstring, 2>> rows{
        { L"First virtual row", L"Running" }, { L"Second virtual row", L"Stopped" }
    };
    int textReads = 0;
    int imageReads = 0;
    int unsupportedTextReads = 0;
    int nativeIdReads = 0;
    int stateNotifications = 0;
    int editNotifications = 0;
    bool returnTextPointer = false;
    bool changeCountDuringRead = false;
    UINT callbackMaskDuringRead = 0;
    bool acceptRename = true;
    bool wrongThread = false;
    bool ready = false;

    explicit Fixture(DWORD mode) {
        root = CreateWindowExW(0, L"Static", L"owner-data-list-view-regression",
            WS_OVERLAPPEDWINDOW, 0, 0, 440, 260, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!root) return;
        SetWindowSubclass(root, Owner, 991, reinterpret_cast<DWORD_PTR>(this));
        list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE |
            LVS_SHAREIMAGELISTS | LVS_OWNERDATA | LVS_EDITLABELS | mode, 0, 0, 400, 210,
            root, reinterpret_cast<HMENU>(992), GetModuleHandleW(nullptr), nullptr);
        if (!list) return;
        SetWindowSubclass(list, ObserveMessages, 992, reinterpret_cast<DWORD_PTR>(this));
        SendMessageW(list, CCM_SETUNICODEFORMAT, TRUE, 0);
        for (int index = 0; index < 2; ++index) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<LPWSTR>(index == 0 ? L"Name" : L"State");
            column.cx = 170;
            if (ListView_InsertColumn(list, index, &column) != index) return;
        }
        images = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 1, 1);
        HICON icon = ItemImageCapture::NativeIndexIcon(16, 77);
        const int imageIndex = images && icon ? ImageList_AddIcon(images, icon) : -1;
        if (icon) DestroyIcon(icon);
        if (imageIndex != 0) return;
        SendMessageW(list, LVM_SETIMAGELIST, LVSIL_SMALL, reinterpret_cast<LPARAM>(images));
        SendMessageW(list, LVM_SETIMAGELIST, LVSIL_NORMAL, reinterpret_cast<LPARAM>(images));
        SetCount();
        ready = true;
    }

    ~Fixture() {
        if (root) DestroyWindow(root);
        if (images) ImageList_Destroy(images);
    }

    void SetCount() const { SendMessageW(list, LVM_SETITEMCOUNT, rows.size(), 0); }

    bool Capture(ControlNode& node, std::wstring& error) const {
        node.kind = ControlKind::ListView;
        node.nodeId = 71;
        node.generation = 1;
        node.hwnd = list;
        return CaptureControlDetail(list, node, error);
    }

    static LRESULT CALLBACK ObserveMessages(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
        auto& fixture = *reinterpret_cast<Fixture*>(data);
        if (message == LVM_GETITEMTEXTW || message == LVM_GETITEMTEXTA)
            ++fixture.unsupportedTextReads;
        if (message == LVM_MAPINDEXTOID && wParam != static_cast<WPARAM>(-1))
            ++fixture.nativeIdReads;
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ObserveMessages, id);
        return DefSubclassProc(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK Owner(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
        auto& fixture = *reinterpret_cast<Fixture*>(data);
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, Owner, id);
        const auto* header = message == WM_NOTIFY ? reinterpret_cast<const NMHDR*>(lParam) : nullptr;
        if (!header || header->hwndFrom != fixture.list)
            return DefSubclassProc(window, message, wParam, lParam);
        fixture.wrongThread |= GetCurrentThreadId() != fixture.threadId;
        if (header->code == LVN_GETDISPINFOW) {
            auto& item = reinterpret_cast<NMLVDISPINFOW*>(lParam)->item;
            if (item.iItem < 0 || static_cast<size_t>(item.iItem) >= fixture.rows.size()) return 0;
            if ((item.mask & LVIF_TEXT) != 0) {
                ++fixture.textReads;
                const auto& value = fixture.rows[static_cast<size_t>(item.iItem)]
                    [static_cast<size_t>(std::clamp(item.iSubItem, 0, 1))];
                if (fixture.returnTextPointer) item.pszText = const_cast<LPWSTR>(value.c_str());
                else if (item.pszText && item.cchTextMax > 0)
                    lstrcpynW(item.pszText, value.c_str(), item.cchTextMax);
                if (fixture.changeCountDuringRead && item.iItem == 1) {
                    fixture.changeCountDuringRead = false;
                    SendMessageW(fixture.list, LVM_SETITEMCOUNT, fixture.rows.size() + 1, 0);
                }
                if (fixture.callbackMaskDuringRead && item.iItem == 1) {
                    SendMessageW(fixture.list, LVM_SETCALLBACKMASK, fixture.callbackMaskDuringRead, 0);
                    fixture.callbackMaskDuringRead = 0;
                }
            }
            if ((item.mask & LVIF_IMAGE) != 0) {
                ++fixture.imageReads;
                item.iImage = item.iItem == 0 ? 0 : I_IMAGENONE;
            }
            return 0;
        }
        if (header->code == LVN_ITEMCHANGED || header->code == LVN_ODSTATECHANGED)
            ++fixture.stateNotifications;
        if (header->code == LVN_BEGINLABELEDITW) return FALSE;
        if (header->code == LVN_ENDLABELEDITW) {
            ++fixture.editNotifications;
            const auto& item = reinterpret_cast<const NMLVDISPINFOW*>(lParam)->item;
            if (fixture.acceptRename && item.pszText && item.iItem >= 0 &&
                static_cast<size_t>(item.iItem) < fixture.rows.size()) {
                fixture.rows[static_cast<size_t>(item.iItem)][0] = item.pszText;
                return TRUE;
            }
            return FALSE;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }
};

inline void TestMode(DWORD mode, const wchar_t* name, void (*check)(bool, const char*)) {
    Fixture fixture(mode);
    check(fixture.ready, "owner-data ListView fixture was not created");
    if (!fixture.ready) return;
    ControlKind kind{};
    std::wstring error;
    check(ClassifyControl(fixture.list, kind, error) && kind == ControlKind::ListView,
        "bounded owner-data ListView was rejected by admission");
    ListView_SetItemState(fixture.list, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ControlNode node;
    const bool captured = fixture.Capture(node, error);
    if (!captured) std::wcerr << L"owner-data ListView " << name << L": " << error << L'\n';
    check(captured, "owner-data ListView did not capture native callbacks");
    if (!captured) return;
    check(node.listViewMode == name && node.items == std::vector<std::wstring>{
        fixture.rows[0][0], fixture.rows[1][0] } && node.selectedIndices == std::vector<int>{1} &&
        node.focusedIndex == 1 && node.multiSelect && node.editableLabels,
        "owner-data ListView lost its native text, selection, focus or label-edit capability");
    check(fixture.textReads >= 2 && fixture.imageReads >= 2 && !fixture.wrongThread &&
        fixture.unsupportedTextReads == 0 && fixture.nativeIdReads == 0,
        "virtual ListView bypassed its source-thread callbacks or requested unsupported native item identities");
    check(node.itemNativeIds.empty() && !node.itemActivationSupported,
        "owner-data indexes were advertised as stable activation identities");
    check(node.itemImages == std::vector<int>{0, -1} && node.imageList.size() == 1 &&
        !node.imageList[0].imageData.empty() && node.imageList[0].imageData[0] == 77,
        "owner-data ListView did not capture the callback image and native image list");
    if (mode == LVS_REPORT)
        check(node.columns == std::vector<std::wstring>{L"Name", L"State"} &&
            node.rows.size() == 2 && node.rows[1][1] == L"Stopped" && node.itemRects.empty(),
            "virtual report columns or subitem callbacks were omitted");
    else
        check(node.rows.empty() && node.columns.empty() && node.itemRects.size() == 2,
            "virtual non-report mode lost native geometry or invented report columns");

    check(SetListViewFocusedIndex(fixture.list, 0) && fixture.Capture(node, error) &&
        node.focusedIndex == 0 && node.selectedIndices == std::vector<int>{1},
        "owner-data focus action changed selection or failed canonical readback");
    check(fixture.stateNotifications > 0, "owner-data state changes did not reach the application's notification handler");
    WindowSnapshot snapshot;
    snapshot.nodes = { node };
    const auto json = SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    check(json.find("\"itemNativeIds\":[]") != std::string::npos,
        "virtual ListView serialized invented native item identities");
    ActionRequest action;
    action.nodeId = node.nodeId;
    action.action = L"setSelection";
    action.integerValues = { 0, 1 };
    check(ValidateActionForSnapshot(action, snapshot, error), "virtual multi-selection was not admitted by the existing contract");
    action.action = L"activateItem";
    action.itemIndex = 0;
    check(!ValidateActionForSnapshot(action, snapshot, error), "virtual item activation was allowed without stable identity");

    const int textReadsBefore = fixture.textReads;
    bool rejectedOffThread = false;
    std::thread worker([&] {
        std::wstring text;
        std::wstring reason;
        rejectedOffThread = !ReadListViewItemText(fixture.list, 0, 0, text, reason);
    });
    worker.join();
    check(rejectedOffThread && fixture.textReads == textReadsBefore,
        "virtual item reader dispatched a callback away from the owning GUI thread");

    // The owner may return a pointer rather than copy into LVITEM's buffer, and
    // long callback labels must grow without truncation in either form.
    fixture.rows[0][0] = std::wstring(700, L'\x4e2d');
    for (const bool pointer : { false, true }) {
        fixture.returnTextPointer = pointer;
        check(fixture.Capture(node, error) && node.items[0] == fixture.rows[0][0],
            "virtual callback text was truncated or lost when the provider returned a pointer");
    }
    fixture.rows[0][0] = L"Before rename";
    ShowWindow(fixture.root, SW_SHOWNOACTIVATE);
    check(RenameListViewItem(fixture.list, 0, L"Renamed virtual row") &&
        fixture.rows[0][0] == L"Renamed virtual row" && fixture.editNotifications == 1,
        "virtual rename did not commit through the application's label-edit notification");
    fixture.acceptRename = false;
    check(!RenameListViewItem(fixture.list, 0, L"Refused name") &&
        fixture.rows[0][0] == L"Renamed virtual row" && fixture.editNotifications == 2,
        "virtual rename bypassed the application's veto");
    check(fixture.unsupportedTextReads == 0, "virtual rename readback used unsupported GETITEMTEXT");
    if (mode == LVS_REPORT) {
        CaptureContext context;
        context.surfaceId = L"22222222-2222-3333-4444-555555555555";
        context.generation = 1;
        context.revision = 1;
        const bool wholeWindow = CaptureWindow(fixture.root, context, snapshot, error);
        if (!wholeWindow) std::wcerr << L"owner-data whole-window capture: " << error << L'\n';
        check(wholeWindow && std::any_of(snapshot.nodes.begin(), snapshot.nodes.end(),
            [&](const ControlNode& candidate) { return candidate.hwnd == fixture.list &&
                candidate.rows.size() == fixture.rows.size() && !candidate.itemActivationSupported; }),
            "bounded owner-data ListView did not survive the complete HWND-tree capture");
    }
}

inline void TestScroll(void (*check)(bool, const char*)) {
    Fixture fixture(LVS_LIST);
    check(fixture.ready, "owner-data scroll fixture was not created");
    if (!fixture.ready) return;
    fixture.rows.assign(80, {L"Scrollable virtual item", L"State"});
    fixture.SetCount();
    ListView_SetItemState(fixture.list, 1, LVIS_SELECTED, LVIS_SELECTED);
    ControlNode before;
    ControlNode after;
    std::wstring error;
    const bool capturedBefore = fixture.Capture(before, error);
    const bool scrolled = capturedBefore && ScrollListViewBy(fixture.list, 180, 0);
    const bool capturedAfter = scrolled && fixture.Capture(after, error);
    check(capturedAfter && before.itemRects.size() == 80 && after.itemRects.size() == 80 &&
        after.itemRects[0].left < before.itemRects[0].left &&
        after.selectedIndices == std::vector<int>{1},
        "virtual non-report scroll did not preserve native viewport geometry and selection");
}

inline void TestBounds(void (*check)(bool, const char*)) {
    Fixture fixture(LVS_REPORT);
    check(fixture.ready, "owner-data bounds fixture was not created");
    if (!fixture.ready) return;
    ControlNode node;
    std::wstring error;
    SendMessageW(fixture.list, LVM_SETITEMCOUNT, Bridge::Ipc::kMaxListItems + 1, 0);
    const int readsBefore = fixture.textReads;
    check(!fixture.Capture(node, error) && error.find(L"item count") != std::wstring::npos &&
        fixture.textReads == readsBefore, "virtual item-count cap was bypassed or read before admission");
    fixture.SetCount();
    fixture.rows[0][0].assign(Bridge::Ipc::kMaxStringChars + 1, L'x');
    check(!fixture.Capture(node, error) && error.find(L"string limit") != std::wstring::npos,
        "virtual item text exceeded the protocol cap without rejection");
    fixture.rows.assign(9, {std::wstring(32760, L'x'), L"State"});
    fixture.SetCount();
    check(!fixture.Capture(node, error) && error.find(L"bounded adapter payload") != std::wstring::npos,
        "virtual callback text exceeded the aggregate payload budget");
    fixture.rows = {{L"First", L"A"}, {L"Second", L"B"}};
    fixture.SetCount();
    fixture.changeCountDuringRead = true;
    check(!fixture.Capture(node, error) && error.find(L"changed during capture") != std::wstring::npos,
        "a virtual data-set resize during callbacks produced an inconsistent snapshot");
    fixture.SetCount();
    ControlKind kind{};
    for (const UINT callbackMask : { LVIS_STATEIMAGEMASK, LVIS_OVERLAYMASK, LVIS_CUT, LVIS_DROPHILITED }) {
        SendMessageW(fixture.list, LVM_SETCALLBACKMASK, callbackMask, 0);
        const int beforeCallbacks = fixture.textReads;
        check(!ClassifyControl(fixture.list, kind, error) &&
            error.find(L"callback state") != std::wstring::npos &&
            !fixture.Capture(node, error) && fixture.textReads == beforeCallbacks,
            "virtual callback-owned state imagery was admitted without projection semantics");
    }
    SendMessageW(fixture.list, LVM_SETCALLBACKMASK, 0, 0);
    fixture.callbackMaskDuringRead = LVIS_OVERLAYMASK;
    check(!fixture.Capture(node, error) && error.find(L"callback state") != std::wstring::npos,
        "virtual callbacks installed unmodeled state during capture without rejection");
    SendMessageW(fixture.list, LVM_SETCALLBACKMASK, 0, 0);
    ListView_SetExtendedListViewStyleEx(fixture.list, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
    check(!ClassifyControl(fixture.list, kind, error) && error.find(L"LVS_EX_CHECKBOXES") != std::wstring::npos,
        "virtual application-owned checkboxes were offered as stored state images");
}

} // namespace OwnerDataListView

inline void TestOwnerDataListViews(void (*check)(bool, const char*)) {
    OwnerDataListView::TestMode(LVS_REPORT, L"report", check);
    OwnerDataListView::TestMode(LVS_ICON, L"largeIcon", check);
    OwnerDataListView::TestMode(LVS_SMALLICON, L"smallIcon", check);
    OwnerDataListView::TestMode(LVS_LIST, L"list", check);
    OwnerDataListView::TestScroll(check);
    OwnerDataListView::TestBounds(check);
}

} // namespace FluentShell::Tests
