#pragma once

#include "ToolbarRadioGroupTests.h"
#include "ItemImageCaptureTests.h"
#include <array>

namespace FluentShell::Tests {
namespace ToolbarImageVariants {

using namespace Bridge::Translation;

struct Fixture final {
    ToolbarRadioGroups::Fixture controls;
    std::array<HIMAGELIST, 4> lists{};
    int listId = 0;
    bool ready = false;

    Fixture(int dimension = 16, int id = 0) : listId(id) {
        if (!controls.ready) return;
        constexpr std::array<UINT, 4> setters = {
            TB_SETIMAGELIST, TB_SETHOTIMAGELIST, TB_SETDISABLEDIMAGELIST, TB_SETPRESSEDIMAGELIST };
        for (size_t state = 0; state < lists.size(); ++state) {
            lists[state] = ImageList_Create(dimension, dimension, ILC_COLOR32 | ILC_MASK, 2, 1);
            if (!lists[state]) return;
            for (int index = 0; index < 2; ++index) {
                HICON icon = ItemImageCapture::NativeIndexIcon(dimension, static_cast<int>(state) * 10 + index);
                const int added = icon ? ImageList_AddIcon(lists[state], icon) : -1;
                if (icon) DestroyIcon(icon);
                if (added != index) return;
            }
            SendMessageW(controls.toolbar, setters[state], id, reinterpret_cast<LPARAM>(lists[state]));
        }
        ready = Add(101);
    }

    ~Fixture() {
        // Destroy the toolbar before releasing lists it borrows.
        if (controls.root) DestroyWindow(controls.root);
        controls.root = nullptr;
        for (auto list : lists) if (list) ImageList_Destroy(list);
    }

    bool Add(int command) const {
        if (!controls.Add(command, BTNS_BUTTON, TBSTATE_ENABLED, L"Connect")) return false;
        TBBUTTONINFOW info{};
        info.cbSize = sizeof(info);
        info.dwMask = TBIF_IMAGE;
        info.iImage = MAKELONG(1, listId);
        return SendMessageW(controls.toolbar, TB_SETBUTTONINFOW, command,
            reinterpret_cast<LPARAM>(&info)) != FALSE;
    }
};

inline void CheckWire(const ControlNode& node, void (*check)(bool, const char*)) {
    WindowSnapshot snapshot;
    snapshot.nodes = { node };
    const auto json = SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    const auto wire = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(json))
        .GetNamedObject(L"window").GetNamedArray(L"nodes").GetObjectAt(0)
        .GetNamedArray(L"toolbarItems").GetObjectAt(0);
    if (!wire.HasKey(L"hotImage") || !wire.HasKey(L"disabledImage") || !wire.HasKey(L"pressedImage")) {
        check(false, "toolbar state image missing from wire payload");
        return;
    }
    check(wire.GetNamedObject(L"hotImage").GetNamedNumber(L"imageWidth") == 16 &&
        wire.GetNamedObject(L"disabledImage").GetNamedString(L"imageData") !=
            wire.GetNamedObject(L"pressedImage").GetNamedString(L"imageData"),
        "toolbar state image metadata or distinct pixels were lost on the wire");
}

struct ReentrantListChange final {
    HIMAGELIST replacement = nullptr;
    bool changed = false;
};

inline LRESULT CALLBACK ReplaceEarlierImage(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR id, DWORD_PTR reference) {
    auto& state = *reinterpret_cast<ReentrantListChange*>(reference);
    if (message == TB_GETBUTTONINFOW && wParam == 102 && !state.changed) {
        state.changed = true;
        SendMessageW(window, TB_SETHOTIMAGELIST, 0, reinterpret_cast<LPARAM>(state.replacement));
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ReplaceEarlierImage, id);
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace ToolbarImageVariants

inline void TestToolbarImageVariants(void (*check)(bool, const char*)) {
    using namespace ToolbarImageVariants;
    for (int listId : { 0, 3 }) {
        Fixture fixture(16, listId);
        check(fixture.ready, "toolbar state-image fixture could not initialize");
        if (!fixture.ready) continue;
        ControlKind kind{};
        std::wstring error;
        check(ClassifyControl(fixture.controls.toolbar, kind, error) && kind == ControlKind::Toolbar,
            "toolbar state image lists were refused by the adapter probe");
        ControlNode node;
        const bool captured = fixture.controls.Capture(node, error);
        if (!captured) std::wcerr << L"Toolbar state capture: " << error << L'\n';
        check(captured && node.toolbarItems.size() == 1, "toolbar state image capture failed");
        if (!captured || node.toolbarItems.empty()) continue;
        const auto& item = node.toolbarItems[0];
        check(item.imageData[0] == 1 && item.hotImage && item.disabledImage && item.pressedImage &&
            ItemImageCapture::HasNativeIndexPixels(*item.hotImage, 11) &&
            ItemImageCapture::HasNativeIndexPixels(*item.disabledImage, 21) &&
            ItemImageCapture::HasNativeIndexPixels(*item.pressedImage, 31),
            "toolbar state images did not use the referenced list ID and image index");
        CheckWire(node, check);
        WindowSnapshot before;
        before.nodes = { node };
        const auto original = SerializeWindowOpen(L"00112233445566778899aabbccddeeff", before);
        SendMessageW(fixture.controls.toolbar, TB_SETHOTIMAGELIST, listId,
            reinterpret_cast<LPARAM>(fixture.lists[3]));
        fixture.controls.SetState(101, 0);
        ControlNode changed;
        check(fixture.controls.Capture(changed, error) && !changed.toolbarItems[0].enabled &&
            changed.toolbarItems[0].hotImage &&
            ItemImageCapture::HasNativeIndexPixels(*changed.toolbarItems[0].hotImage, 31),
            "toolbar capture did not observe native list replacement and disabled state");
        before.nodes = { changed };
        check(original != SerializeWindowOpen(L"00112233445566778899aabbccddeeff", before),
            "toolbar state image replacement was absent from the snapshot fingerprint");
        for (UINT message : { TB_SETHOTIMAGELIST, TB_SETDISABLEDIMAGELIST, TB_SETPRESSEDIMAGELIST })
            SendMessageW(fixture.controls.toolbar, message, listId, 0);
        check(fixture.controls.Capture(changed, error) && !changed.toolbarItems[0].hotImage &&
            !changed.toolbarItems[0].disabledImage && !changed.toolbarItems[0].pressedImage,
            "toolbar state images survived removal of their native lists");
        SendMessageW(fixture.controls.toolbar, TB_SETHOTIMAGELIST, listId,
            reinterpret_cast<LPARAM>(fixture.lists[1]));
        ImageList_Remove(fixture.lists[1], 1);
        check(!fixture.controls.Capture(changed, error),
            "toolbar accepted an alternate list without its referenced image");
    }
    {
        Fixture fixture;
        Fixture other(32);
        check(fixture.ready && other.ready, "toolbar mismatched-image fixtures could not initialize");
        if (fixture.ready && other.ready) {
            SendMessageW(fixture.controls.toolbar, TB_SETDISABLEDIMAGELIST, 0,
                reinterpret_cast<LPARAM>(other.lists[2]));
            ControlNode node;
            std::wstring error;
            check(!fixture.controls.Capture(node, error), "toolbar accepted mismatched state image dimensions");
            SendMessageW(fixture.controls.toolbar, TB_SETDISABLEDIMAGELIST, 0,
                reinterpret_cast<LPARAM>(fixture.lists[2]));
            const bool added = fixture.Add(102);
            ReentrantListChange state{ fixture.lists[3] };
            const bool hooked = SetWindowSubclass(fixture.controls.toolbar, ReplaceEarlierImage,
                1, reinterpret_cast<DWORD_PTR>(&state)) != FALSE;
            check(added && hooked && !fixture.controls.Capture(node, error) && state.changed &&
                error.find(L"image lists changed") != std::wstring::npos,
                "toolbar published mixed image lists after a reentrant application callback");
            if (hooked) RemoveWindowSubclass(fixture.controls.toolbar, ReplaceEarlierImage, 1);
        }
    }
    {
        Fixture fixture(96);
        check(fixture.ready, "toolbar pixel-budget fixture could not initialize");
        if (!fixture.ready) return;
        bool added = true;
        for (int command = 102; command <= 116; ++command) added = fixture.Add(command) && added;
        ControlNode node;
        std::wstring error;
        check(added && fixture.controls.Capture(node, error),
            "toolbar rejected images at the aggregate pixel budget");
        check(fixture.Add(117) && !fixture.controls.Capture(node, error) &&
            error.find(L"aggregate pixel budget") != std::wstring::npos,
            "toolbar alternate images bypassed the aggregate pixel budget");
    }
}

} // namespace FluentShell::Tests
