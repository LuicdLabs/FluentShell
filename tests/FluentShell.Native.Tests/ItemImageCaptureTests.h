#pragma once

#include "../../src/Bridge/Translation/ControlAdapters.h"

#include <commctrl.h>
#include <iostream>

#include <string>
#include <vector>

namespace FluentShell::Tests {
namespace ItemImageCapture {

using namespace FluentShell::Bridge;

inline HICON NativeIndexIcon(int dimension, int nativeIndex) {
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = dimension;
    bitmap.bmiHeader.biHeight = -dimension;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!color || !pixels) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    // Opaque BGRA pixels encode the native index, so the remap assertions also
    // distinguish the actual copied icon from another valid image-list entry.
    for (int index = 0; index < dimension * dimension; ++index)
        static_cast<uint32_t*>(pixels)[index] = 0xffb46400u | static_cast<uint8_t>(nativeIndex);
    const size_t maskStride = (static_cast<size_t>(dimension) + 15u) / 16u * 2u;
    const std::vector<uint8_t> maskBits(maskStride * dimension, 0);
    HBITMAP mask = CreateBitmap(dimension, dimension, 1, 1, maskBits.data());
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = mask ? CreateIconIndirect(&info) : nullptr;
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return icon;
}

inline bool HasNativeIndexPixels(const Translation::ImageListEntry& image, int nativeIndex) {
    return image.imageWidth == 16 && image.imageHeight == 16 &&
        image.imageFormat == L"bgra8-premultiplied" && image.imageData.size() == 16 * 16 * 4 &&
        image.imageData[0] == static_cast<uint8_t>(nativeIndex) && image.imageData[1] == 100 &&
        image.imageData[2] == 180 && image.imageData[3] == 255;
}

// Hidden controls exercise the native capture messages without desktop input.
struct Fixture final {
    HWND window = nullptr;
    HWND control = nullptr;
    HIMAGELIST images = nullptr;
    bool tree = true;
    bool ready = false;

    Fixture(bool treeControl, int dimension, int imageCount) : tree(treeControl) {
        window = CreateWindowExW(0, L"Static", L"item-image-tests", WS_OVERLAPPEDWINDOW,
            0, 0, 360, 260, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) return;
        const DWORD style = WS_CHILD | (tree
            ? TVS_HASBUTTONS | TVS_HASLINES
            : LVS_REPORT | LVS_SHAREIMAGELISTS);
        control = CreateWindowExW(0, tree ? WC_TREEVIEWW : WC_LISTVIEWW, L"", style,
            0, 0, 320, 220, window, reinterpret_cast<HMENU>(901),
            GetModuleHandleW(nullptr), nullptr);
        if (!control) return;
        SendMessageW(control, CCM_SETUNICODEFORMAT, TRUE, 0);
        images = ImageList_Create(dimension, dimension, ILC_COLOR32 | ILC_MASK,
            imageCount, 1);
        if (!images) return;
        for (int index = 0; index < imageCount; ++index) {
            HICON icon = NativeIndexIcon(dimension, index);
            const int added = icon ? ImageList_AddIcon(images, icon) : -1;
            if (icon) DestroyIcon(icon);
            if (added != index) return;
        }
        SendMessageW(control, tree ? TVM_SETIMAGELIST : LVM_SETIMAGELIST,
            tree ? TVSIL_NORMAL : LVSIL_SMALL, reinterpret_cast<LPARAM>(images));
        if (!tree) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            column.pszText = const_cast<LPWSTR>(L"Name");
            column.cx = 180;
            column.fmt = LVCFMT_LEFT;
            if (SendMessageW(control, LVM_INSERTCOLUMNW, 0,
                    reinterpret_cast<LPARAM>(&column)) != 0) return;
        }
        ready = true;
    }

    ~Fixture() {
        if (window) DestroyWindow(window);
        if (images) ImageList_Destroy(images);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    HTREEITEM AddTreeItem(const wchar_t* text, int normal, int selected) const {
        TVINSERTSTRUCTW item{};
        item.hParent = TVI_ROOT;
        item.hInsertAfter = TVI_LAST;
        item.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        item.item.pszText = const_cast<LPWSTR>(text);
        item.item.iImage = normal;
        item.item.iSelectedImage = selected;
        return reinterpret_cast<HTREEITEM>(SendMessageW(
            control, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
    }

    bool SetTreeImages(HTREEITEM handle, int normal, int selected) const {
        TVITEMW item{};
        item.mask = TVIF_HANDLE | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        item.hItem = handle;
        item.iImage = normal;
        item.iSelectedImage = selected;
        return SendMessageW(control, TVM_SETITEMW, 0,
            reinterpret_cast<LPARAM>(&item)) != FALSE;
    }

    bool AddListItem(int index, int image) const {
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = index;
        item.pszText = const_cast<LPWSTR>(L"Item");
        item.iImage = image;
        return SendMessageW(control, LVM_INSERTITEMW, 0,
            reinterpret_cast<LPARAM>(&item)) == index;
    }

    bool Capture(Translation::ControlNode& node, std::wstring& reason) const {
        node.kind = tree ? Translation::ControlKind::TreeView : Translation::ControlKind::ListView;
        node.style = static_cast<uint64_t>(GetWindowLongPtrW(control, GWL_STYLE));
        reason.clear();
        return Translation::CaptureControlDetail(control, node, reason);
    }
};

inline size_t PixelBytes(const Translation::ControlNode& node) {
    size_t result = 0;
    for (const auto& icon : node.imageList) result += icon.imageData.size();
    return result;
}

struct CallbackState final {
    HIMAGELIST images = nullptr;
    int normal = 139;
    int selected = 138;
    int normalReads = 0;
    int selectedReads = 0;
    int itemReads = 0;
    bool appendImage = false;
    bool unresolved = false;
};

inline LRESULT CALLBACK CountItemReads(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR reference) {
    auto* state = reinterpret_cast<CallbackState*>(reference);
    if (message == TVM_GETITEMW) ++state->itemReads;
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, CountItemReads, subclassId);
    return result;
}

inline LRESULT CALLBACK ResolveCallbackImages(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR reference) {
    auto* state = reinterpret_cast<CallbackState*>(reference);
    if (message == WM_NOTIFY && lParam) {
        auto* info = reinterpret_cast<NMTVDISPINFOW*>(lParam);
        if (info->hdr.code == TVN_GETDISPINFOW) {
            if ((info->item.mask & TVIF_IMAGE) != 0) {
                ++state->normalReads;
                if (state->appendImage) {
                    state->normal = ImageList_AddIcon(
                        state->images, LoadIconW(nullptr, IDI_APPLICATION));
                    state->appendImage = false;
                }
                info->item.iImage = state->unresolved ? I_IMAGECALLBACK : state->normal;
            }
            if ((info->item.mask & TVIF_SELECTEDIMAGE) != 0) {
                ++state->selectedReads;
                info->item.iSelectedImage = state->unresolved ? I_IMAGECALLBACK : state->selected;
            }
            return 0;
        }
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, ResolveCallbackImages, subclassId);
    return result;
}

inline void TestSparseTree(void (*check)(bool, const char*)) {
    Fixture fixture(true, 16, 140);
    check(fixture.ready, "large native TreeView image list fixture was not created");
    if (!fixture.ready) return;
    const HTREEITEM first = fixture.AddTreeItem(L"First", 139, 138);
    const HTREEITEM second = fixture.AddTreeItem(L"Second", 0, 139);
    const HTREEITEM none = fixture.AddTreeItem(L"No icon", I_IMAGENONE, I_IMAGENONE);
    check(first && second && none, "sparse TreeView image items were not inserted");
    Translation::ControlNode node;
    std::wstring reason;
    const bool captured = fixture.Capture(node, reason);
    if (!captured) {
        TVITEMW observed{};
        observed.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        observed.hItem = none;
        SendMessageW(fixture.control, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&observed));
        std::wcerr << L"Sparse tree capture: " << reason << L"; no-icon indexes="
            << observed.iImage << L"," << observed.iSelectedImage << L'\n';
    }
    check(captured, "sparse TreeView rejected a 140-entry native image list");
    check(node.imageList.size() == 3 &&
          node.itemImages == std::vector<int>{2, 0, -1} &&
          node.itemSelectedImages == std::vector<int>{1, 2, -1},
        "TreeView normal and selected native references were not compacted together");
    check(PixelBytes(node) == 3 * 16 * 16 * 4,
        "unused TreeView image-list entries consumed the pixel budget");
    check(node.imageList.size() == 3 && HasNativeIndexPixels(node.imageList[0], 0) &&
          HasNativeIndexPixels(node.imageList[1], 138) && HasNativeIndexPixels(node.imageList[2], 139),
        "TreeView compact icon pixels did not match their normal and selected native indexes");

    check(fixture.SetTreeImages(first, 137, 137) && fixture.SetTreeImages(second, 0, 0),
        "TreeView image references could not be changed");
    check(fixture.Capture(node, reason) && node.imageList.size() == 2 &&
          node.itemImages == std::vector<int>{1, 0, -1} &&
          node.itemSelectedImages == std::vector<int>{1, 0, -1},
        "TreeView recapture retained old pixels or compact image indexes");
    check(node.imageList.size() == 2 && HasNativeIndexPixels(node.imageList[0], 0) &&
          HasNativeIndexPixels(node.imageList[1], 137),
        "TreeView recapture copied stale native icon pixels after references changed");

    check(fixture.SetTreeImages(first, 137, 140), "invalid TreeView selected index was not stored");
    check(!fixture.Capture(node, reason) && reason.find(L"outside the native image list") != std::wstring::npos,
        "TreeView accepted a selected image outside the native list while its normal image was valid");
    SendMessageW(fixture.control, TVM_SETIMAGELIST, TVSIL_NORMAL, 0);
    check(fixture.Capture(node, reason) && node.imageList.empty() &&
          node.itemImages == std::vector<int>{-1, -1, -1} &&
          node.itemSelectedImages == std::vector<int>{-1, -1, -1},
        "TreeView without a native image list retained item imagery");
}

inline void TestSparseList(void (*check)(bool, const char*)) {
    Fixture fixture(false, 16, 140);
    check(fixture.ready, "large native ListView image list fixture was not created");
    if (!fixture.ready) return;
    check(fixture.AddListItem(0, 139) && fixture.AddListItem(1, 0) &&
          fixture.AddListItem(2, I_IMAGENONE), "sparse ListView image items were not inserted");
    Translation::ControlNode node;
    std::wstring reason;
    check(fixture.Capture(node, reason) && node.imageList.size() == 2 &&
          node.itemImages == std::vector<int>{1, 0, -1},
        "ListView image references were not compacted from the native small image list");
    check(node.imageList.size() == 2 && HasNativeIndexPixels(node.imageList[0], 0) &&
          HasNativeIndexPixels(node.imageList[1], 139),
        "ListView compact icon pixels did not match their native row indexes");
}

inline void TestSmallIconsAndBudget(void (*check)(bool, const char*)) {
    {
        Fixture fixture(true, 16, 140);
        check(fixture.ready, "many small TreeView icons fixture was not created");
        if (!fixture.ready) return;
        for (int index = 0; index < 140; ++index)
            check(fixture.AddTreeItem(L"Item", index, index) != nullptr,
                "small-icon TreeView item was not inserted");
        Translation::ControlNode node;
        std::wstring reason;
        check(fixture.Capture(node, reason) && node.imageList.size() == 140 &&
              node.itemImages.size() == 140 && node.itemSelectedImages.size() == 140 &&
              node.itemImages.back() == 139 && node.itemSelectedImages.back() == 139 &&
              PixelBytes(node) == 140 * 16 * 16 * 4,
            "TreeView rejected 140 referenced small icons below the pixel budget");
    }
    {
        Fixture fixture(true, 64, 65);
        check(fixture.ready, "TreeView image pixel-budget fixture was not created");
        if (!fixture.ready) return;
        for (int index = 0; index < 64; ++index)
            check(fixture.AddTreeItem(L"Item", index, index) != nullptr,
                "pixel-budget TreeView item was not inserted");
        Translation::ControlNode node;
        std::wstring reason;
        check(fixture.Capture(node, reason) && PixelBytes(node) == Ipc::kMaxImageListBytes,
            "TreeView rejected the exact decoded-pixel budget");
        check(fixture.AddTreeItem(L"Over budget", 64, 64) != nullptr,
            "over-budget TreeView item was not inserted");
        check(!fixture.Capture(node, reason) && node.imageList.empty() &&
              reason.find(L"decoded pixel budget") != std::wstring::npos,
            "TreeView did not reject its complete image budget before copying icons");
    }
}

inline void TestProviderReadOnce(void (*check)(bool, const char*)) {
    Fixture fixture(true, 16, 140);
    check(fixture.ready, "callback TreeView icon fixture was not created");
    if (!fixture.ready) return;
    CallbackState state;
    state.images = fixture.images;
    const bool hooked = SetWindowSubclass(fixture.window, ResolveCallbackImages, 77,
        reinterpret_cast<DWORD_PTR>(&state)) &&
        SetWindowSubclass(fixture.control, CountItemReads, 77,
            reinterpret_cast<DWORD_PTR>(&state));
    check(hooked, "callback TreeView test subclasses were not installed");
    if (hooked) {
        check(fixture.AddTreeItem(L"Callback item", I_IMAGECALLBACK, I_IMAGECALLBACK) != nullptr,
            "callback TreeView image item was not inserted");
        state.itemReads = state.normalReads = state.selectedReads = 0;
        state.appendImage = true;
        Translation::ControlNode node;
        std::wstring reason;
        const bool captured = fixture.Capture(node, reason);
        check(captured && state.normal == 140 && node.imageList.size() == 2 &&
              node.itemImages == std::vector<int>{1} && node.itemSelectedImages == std::vector<int>{0},
            "TreeView did not capture an icon added by the item provider during its read");
        check(state.itemReads == 1 && state.normalReads == 1 && state.selectedReads == 1,
            "compacting TreeView imagery queried an item provider more than once");
        state.unresolved = true;
        check(!fixture.Capture(node, reason) && reason.find(L"callback item image") != std::wstring::npos,
            "TreeView accepted an unresolved callback image");
    }
    RemoveWindowSubclass(fixture.control, CountItemReads, 77);
    RemoveWindowSubclass(fixture.window, ResolveCallbackImages, 77);
}

} // namespace ItemImageCapture

inline void TestReferencedItemImagery(void (*check)(bool, const char*)) {
    ItemImageCapture::TestSparseTree(check);
    ItemImageCapture::TestSparseList(check);
    ItemImageCapture::TestSmallIconsAndBudget(check);
    ItemImageCapture::TestProviderReadOnce(check);
}

} // namespace FluentShell::Tests
