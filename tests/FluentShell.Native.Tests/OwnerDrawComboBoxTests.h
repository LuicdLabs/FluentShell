#pragma once

#include "../../src/Bridge/Translation/ControlAdapters.h"
#include "../../src/Bridge/Translation/OwnerDrawComboBox.h"
#include "../../src/Bridge/Translation/WindowCapture.h"

#include <oleacc.h>
#include <algorithm>
#include <vector>
#include <filesystem>
#include <fstream>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace FluentShell::Tests {
namespace OwnerDrawCombos {

using namespace Bridge::Translation;

class NamedItems final : public IAccessible {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IDispatch && id != IID_IAccessible) return E_NOINTERFACE;
        *out = static_cast<IAccessible*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --references_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override { if (count) *count = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** out) override { if (out) *out = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override { if (count) *count = 2; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** out) override { if (out) *out = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override {
        if (!name || child.vt != VT_I4 || child.lVal <= 0) return E_INVALIDARG;
        *name = SysAllocString(child.lVal == 1 ? L"Accessible first" : L"Accessible second");
        return *name ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT, VARIANT*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT, VARIANT*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR*, VARIANT, long*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE accSelect(long, VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accLocation(long*, long*, long*, long*, VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accNavigate(long, VARIANT, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accHitTest(long, long, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }
private:
    ULONG references_ = 1;
};

struct Fixture final {
    HWND root = nullptr;
    HWND combo = nullptr;
    HWND list = nullptr;
    bool ready = false;
    bool active = false;
    bool refuseDraw = false;
    bool changeData = false;
    bool transparent = false;
    bool accessibleNames = false;
    unsigned unsafeTextReads = 0;
    DWORD sourceThread = GetCurrentThreadId();
    bool wrongThread = false;
    std::vector<DRAWITEMSTRUCT> draws;

    static LRESULT CALLBACK Parent(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR id, DWORD_PTR data) {
        auto& fixture = *reinterpret_cast<Fixture*>(data);
        if (message == WM_MEASUREITEM && lParam) {
            auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (item.CtlType == ODT_COMBOBOX) { item.itemHeight = 16 + item.itemID % 2 * 8; return TRUE; }
        }
        if (message == WM_DRAWITEM && lParam) {
            auto& draw = *reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (draw.CtlType == ODT_COMBOBOX) {
                if (fixture.active) {
                    fixture.draws.push_back(draw);
                    fixture.wrongThread = fixture.wrongThread || GetCurrentThreadId() != fixture.sourceThread;
                    if (fixture.refuseDraw) return FALSE;
                    if (fixture.changeData) {
                        fixture.changeData = false;
                        SendMessageW(draw.hwndItem, CB_SETITEMDATA, draw.itemID, 0xa999);
                    }
                }
                RECT rect = draw.rcItem;
                if (fixture.transparent) rect.right = std::max<LONG>(1, rect.right / 2);
                const COLORREF color = (draw.itemState & ODS_SELECTED) ? RGB(180, 30, 10) :
                    (draw.itemState & ODS_COMBOBOXEDIT) ? RGB(30, 180, 10) : RGB(10, 30, 180);
                HBRUSH brush = CreateSolidBrush(color);
                FillRect(draw.hDC, &rect, brush);
                DeleteObject(brush);
                return TRUE;
            }
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, Parent, id);
        return DefSubclassProc(window, message, wParam, lParam);
    }
    static LRESULT CALLBACK Child(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR id, DWORD_PTR data) {
        auto& fixture = *reinterpret_cast<Fixture*>(data);
        if (window == fixture.list && message == WM_GETOBJECT &&
            static_cast<LONG>(lParam) == OBJID_CLIENT && fixture.accessibleNames) {
            auto* provider = new NamedItems;
            const LRESULT result = LresultFromObject(IID_IAccessible, wParam, provider);
            provider->Release();
            return result;
        }
        if (fixture.active && window == fixture.combo && fixture.accessibleNames &&
            (message == CB_GETLBTEXT || message == CB_GETLBTEXTLEN)) ++fixture.unsafeTextReads;
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, Child, id);
        return DefSubclassProc(window, message, wParam, lParam);
    }
    Fixture(DWORD style = CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
        int width = 180, int count = 2) {
        root = CreateWindowExW(0, L"Static", L"owner-draw-combo-regression", WS_OVERLAPPEDWINDOW,
            0, 0, 800, 420, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!root || !SetWindowSubclass(root, Parent, 1, reinterpret_cast<DWORD_PTR>(this))) return;
        combo = CreateWindowExW(0, L"ComboBox", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
            10, 10, width, 200, root, reinterpret_cast<HMENU>(731), GetModuleHandleW(nullptr), nullptr);
        COMBOBOXINFO info{ sizeof(info) };
        if (!combo || !GetComboBoxInfo(combo, &info)) return;
        list = info.hwndList;
        accessibleNames = (style & CBS_HASSTRINGS) == 0;
        if (!SetWindowSubclass(combo, Child, 1, reinterpret_cast<DWORD_PTR>(this)) ||
            !SetWindowSubclass(list, Child, 1, reinterpret_cast<DWORD_PTR>(this))) return;
        for (int index = 0; index < count; ++index) {
            const LPARAM value = style & CBS_HASSTRINGS ?
                reinterpret_cast<LPARAM>(index % 2 ? L"Second" : L"First") : 0xa100 + index;
            if (SendMessageW(combo, CB_ADDSTRING, 0, value) != index) return;
            SendMessageW(combo, CB_SETITEMDATA, index, 0xa100 + index);
        }
        SendMessageW(combo, CB_SETCURSEL, count > 1 ? 1 : count ? 0 : -1, 0);
        ready = true;
    }
    ~Fixture() { if (root) DestroyWindow(root); }
    bool Capture(ControlNode& node, std::wstring& error) {
        node = {};
        node.kind = ControlKind::ComboBox;
        node.nodeId = 3;
        node.generation = 1;
        node.hwnd = combo;
        node.style = static_cast<uint64_t>(GetWindowLongPtrW(combo, GWL_STYLE));
        draws.clear();
        error.clear();
        active = true;
        const bool success = CaptureControlDetail(combo, node, error);
        active = false;
        return success;
    }
};

} // namespace OwnerDrawCombos

inline void TestOwnerDrawComboBoxes(void (*check)(bool, const char*)) {
    using namespace OwnerDrawCombos;
    for (DWORD mode : { CBS_OWNERDRAWFIXED, CBS_OWNERDRAWVARIABLE }) {
        Fixture fixture(CBS_DROPDOWNLIST | mode | CBS_HASSTRINGS);
        check(fixture.ready, "owner-draw ComboBox fixture could not initialize");
        if (!fixture.ready) continue;
        ControlNode node;
        std::wstring error;
        const bool captured = fixture.Capture(node, error);
        check(captured && node.ownerDrawCombo && node.ownerDrawCombo->items.size() == 2 &&
            node.items == std::vector<std::wstring>{ L"First", L"Second" } && node.selectedIndex == 1,
            "fixed/variable owner-draw ComboBox did not retain native labels and selection");
        if (!captured || !node.ownerDrawCombo) continue;
        const auto& painted = *node.ownerDrawCombo;
        check(painted.mode == (mode == CBS_OWNERDRAWFIXED ? L"fixed" : L"variable") &&
            painted.selectionFace && !node.editable && fixture.draws.size() == 10 && !fixture.wrongThread,
            "ComboBox callback mode, draw count, closed field, or GUI-thread ownership was wrong");
        for (const auto& draw : fixture.draws) {
            check(draw.CtlType == ODT_COMBOBOX && draw.CtlID == 731 && draw.hwndItem == fixture.combo &&
                draw.itemAction == ODA_DRAWENTIRE && (draw.itemState & ODS_NOFOCUSRECT) &&
                !(draw.itemState & ODS_FOCUS) && draw.itemData == 0xa100 + draw.itemID,
                "WM_DRAWITEM did not carry original ComboBox identity, item data, or focus state");
            if (draw.itemState & ODS_COMBOBOXEDIT)
                check(draw.itemID == 1 && !(draw.itemState & ODS_SELECTED),
                    "ComboBox closed field used popup selected state or the wrong native item");
        }
        check(painted.items[0].normal.imageData != painted.items[0].selected.imageData &&
            painted.selectionFace->imageData[1] == 180 &&
            painted.items[0].normal.imageData[3] == 255,
            "ComboBox popup and closed-field owner pixels were not captured separately");
        if (mode == CBS_OWNERDRAWVARIABLE)
            check(painted.items[0].normal.imageHeight != painted.items[1].normal.imageHeight,
                "variable owner-draw ComboBox lost native per-item heights");
        WindowSnapshot snapshot;
        snapshot.nodes = { node };
        const auto json = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(
            SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot)));
        const auto serialized = json.GetNamedObject(L"window").GetNamedArray(L"nodes")
            .GetObjectAt(0).GetNamedObject(L"ownerDrawCombo");
        check(serialized.GetNamedArray(L"items").Size() == 2 && serialized.HasKey(L"selectionFace"),
            "native ComboBox serialization omitted native state faces");
        const auto original = SnapshotFingerprint(snapshot);
        snapshot.nodes[0].ownerDrawCombo->items[0].selected.imageData[0] ^= 1;
        check(original != SnapshotFingerprint(snapshot), "ComboBox owner pixels were absent from reconcile fingerprint");
        EnableWindow(fixture.combo, FALSE);
        check(fixture.Capture(node, error) && std::all_of(fixture.draws.begin(), fixture.draws.end(),
            [](const auto& draw) { return (draw.itemState & ODS_DISABLED) != 0; }),
            "disabled ComboBox owner was painted with enabled item state");
    }
    {
        Fixture fixture(CBS_DROPDOWN | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS);
        ControlNode node;
        std::wstring error;
        check(fixture.ready && fixture.Capture(node, error) && node.editable &&
            node.ownerDrawCombo && !node.ownerDrawCombo->selectionFace && fixture.draws.size() == 8,
            "editable owner-draw ComboBox replaced its text editor with a painted selection face");
    }
    {
        Fixture fixture(CBS_DROPDOWNLIST | CBS_OWNERDRAWVARIABLE);
        ControlNode node;
        std::wstring error;
        check(fixture.ready && fixture.Capture(node, error) && fixture.unsafeTextReads == 0 &&
            node.items == std::vector<std::wstring>{ L"Accessible first", L"Accessible second" },
            "non-string owner-draw ComboBox did not use MSAA labels without reading item data as text");
        fixture.accessibleNames = false;
        check(!fixture.Capture(node, error) && error.find(L"accessibility name") != std::wstring::npos,
            "unnamed non-string owner-draw ComboBox was admitted");
    }
    {
        Fixture fixture;
        ControlNode node;
        std::wstring error;
        fixture.refuseDraw = true;
        check(fixture.ready && !fixture.Capture(node, error) && error.find(L"WM_DRAWITEM") != std::wstring::npos,
            "owner-draw ComboBox ignored owner callback refusal");
        fixture.refuseDraw = false;
        fixture.changeData = true;
        check(!fixture.Capture(node, error) && error.find(L"identity") != std::wstring::npos,
            "owner-draw ComboBox published mixed item identities after a reentrant owner callback");
        fixture.transparent = true;
        check(fixture.Capture(node, error) && node.ownerDrawCombo &&
            node.ownerDrawCombo->items[0].normal.imageData.back() == 0,
            "owner-draw ComboBox failed to preserve unpainted transparent pixels");
        SendMessageW(fixture.combo, CB_SETCURSEL, -1, 0);
        check(fixture.Capture(node, error) && node.selectedIndex == -1 &&
            std::any_of(fixture.draws.begin(), fixture.draws.end(), [](const auto& draw) {
                return draw.itemID == static_cast<UINT>(-1) && draw.itemData == 0 &&
                    (draw.itemState & ODS_COMBOBOXEDIT); }),
            "empty ComboBox selection did not use native UINT(-1) draw semantics");
    }
    {
        Fixture count(CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, 180, 65);
        Fixture width(CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, 800);
        Fixture budget(CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, 500, 3);
        ControlNode node;
        std::wstring error;
        check(count.ready && !count.Capture(node, error) && count.draws.empty() &&
            error.find(L"64") != std::wstring::npos, "owner-draw ComboBox item cap was not enforced before painting");
        check(width.ready && !width.Capture(node, error) && error.find(L"512 by 128") != std::wstring::npos,
            "owner-draw ComboBox face dimensions escaped the pixel cap");
        SendMessageW(budget.combo, CB_SETITEMHEIGHT, 0, 128);
        check(budget.ready && !budget.Capture(node, error) && error.find(L"budget") != std::wstring::npos,
            "owner-draw ComboBox state variants bypassed the aggregate pixel budget");
    }
    {
        const auto path = std::filesystem::current_path() / L"tests" / L"ProtocolFixtures" /
            L"window.open.owner-draw-combo.json";
        std::ifstream input(path, std::ios::binary);
        const std::string bytes{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
        winrt::Windows::Data::Json::JsonObject fixture;
        const bool parsed = !bytes.empty() && winrt::Windows::Data::Json::JsonObject::TryParse(
            winrt::to_hstring(bytes), fixture);
        check(parsed && fixture.GetNamedObject(L"window").GetNamedArray(L"nodes").GetObjectAt(0)
            .GetNamedObject(L"ownerDrawCombo").GetNamedString(L"mode") == L"fixed",
            "shared owner-draw ComboBox protocol fixture could not be read by the native codec");
    }
}

} // namespace FluentShell::Tests
