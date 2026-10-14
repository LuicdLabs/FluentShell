#include "OwnerDrawComboBox.h"
#include "AccessibleIsland.h"
#include "PaintedPixel.h"

#include <commctrl.h>
#include <algorithm>
#include <cstring>

namespace FluentShell::Bridge::Translation {
namespace {

bool Refuse(std::wstring& reason, const wchar_t* detail) {
    reason = L"owner-draw ComboBox: ";
    reason += detail;
    return false;
}

class NativeDpiContext final {
public:
    explicit NativeDpiContext(HWND window)
        : previous_(SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(window))) {}
    ~NativeDpiContext() { if (previous_) SetThreadDpiAwarenessContext(previous_); }
private:
    DPI_AWARENESS_CONTEXT previous_;
};

class ItemCanvas final {
public:
    ItemCanvas(int width, int height) : width_(width), height_(height) {
        dc_ = CreateCompatibleDC(nullptr);
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = width;
        bitmap.bmiHeader.biHeight = -height;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap_ = dc_ ? CreateDIBSection(dc_, &bitmap, DIB_RGB_COLORS, &bits_, nullptr, 0) : nullptr;
        previous_ = bitmap_ ? SelectObject(dc_, bitmap_) : nullptr;
    }
    ~ItemCanvas() {
        if (previous_) SelectObject(dc_, previous_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
    }
    bool Paint(HWND combo, HWND owner, DRAWITEMSTRUCT draw, int nativeWidth, int nativeHeight,
               ImageListEntry& face) {
        if (!dc_ || !bitmap_ || !bits_ || !previous_ || previous_ == HGDI_ERROR) return false;
        const size_t bytes = static_cast<size_t>(width_) * height_ * 4;
        std::vector<uint8_t> black(bytes);
        bool drawn = false;
        for (int pass = 0; pass < 2; ++pass) {
            std::memset(bits_, pass == 0 ? 0 : 255, bytes);
            const int saved = SaveDC(dc_);
            if (!saved) return false;
            SetMapMode(dc_, MM_ANISOTROPIC);
            SetWindowExtEx(dc_, nativeWidth, nativeHeight, nullptr);
            SetViewportExtEx(dc_, width_, height_, nullptr);
            IntersectClipRect(dc_, 0, 0, nativeWidth, nativeHeight);
            const auto font = reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0));
            if (font) SelectObject(dc_, font);
            SetBkMode(dc_, TRANSPARENT);
            SetTextColor(dc_, GetSysColor(draw.itemState & ODS_DISABLED ? COLOR_GRAYTEXT :
                draw.itemState & ODS_SELECTED ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
            SetBkColor(dc_, GetSysColor(draw.itemState & ODS_SELECTED ? COLOR_HIGHLIGHT : COLOR_WINDOW));
            draw.hDC = dc_;
            draw.rcItem = { 0, 0, nativeWidth, nativeHeight };
            const LRESULT handled = SendMessageW(owner, WM_DRAWITEM, draw.CtlID,
                reinterpret_cast<LPARAM>(&draw));
            GdiFlush();
            RestoreDC(dc_, saved);
            if (!IsWindow(combo) || !IsWindow(owner)) return false;
            // A dialog procedure answers WM_DRAWITEM through DWL_MSGRESULT, which the sender reads as 0
            // even when the procedure painted the item (Performance Monitor's property pages), so
            // painting is evidence of handling too. Either pass may leave its own fill alone (a black
            // swatch over the black pass), so the two are judged together.
            drawn = drawn || handled != 0 || Painted(pass == 0 ? 0 : 255, bytes);
            if (pass == 0) std::memcpy(black.data(), bits_, bytes);
        }
        if (!drawn) return false;
        face.imageWidth = static_cast<uint32_t>(width_);
        face.imageHeight = static_cast<uint32_t>(height_);
        face.imageFormat = L"bgra8-premultiplied";
        face.imageData = std::move(black);
        const auto white = static_cast<const uint8_t*>(bits_);
        for (size_t offset = 0; offset < bytes; offset += 4)
            face.imageData[offset + 3] = RecoverPaintAlpha(face.imageData.data() + offset, white + offset);
        return true;
    }
private:
    // Whether any colour channel of the canvas differs from the fill the pass started with.
    bool Painted(uint8_t fill, size_t bytes) const noexcept {
        const auto* pixels = static_cast<const uint8_t*>(bits_);
        for (size_t offset = 0; offset + 2 < bytes; offset += 4)
            if (pixels[offset] != fill || pixels[offset + 1] != fill || pixels[offset + 2] != fill) return true;
        return false;
    }

    int width_;
    int height_;
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    void* bits_ = nullptr;
};

} // namespace

bool CaptureOwnerDrawComboBox(HWND combo, ControlNode& node, std::wstring& reason,
    const std::vector<std::wstring>* labels) {
    const DWORD thread = GetWindowThreadProcessId(combo, nullptr);
    const HWND owner = GetParent(combo);
    if (!thread || thread != GetCurrentThreadId() || !owner ||
        GetWindowThreadProcessId(owner, nullptr) != thread)
        return Refuse(reason, L"owner and control must share the current GUI thread");
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(combo, GWL_STYLE));
    const DWORD drawStyle = style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE);
    const DWORD type = style & 3u;
    if ((drawStyle != CBS_OWNERDRAWFIXED && drawStyle != CBS_OWNERDRAWVARIABLE) ||
        (type != CBS_DROPDOWN && type != CBS_DROPDOWNLIST))
        return Refuse(reason, L"requires one fixed or variable draw style and a dropdown type");
    RECT physicalClient{};
    if (!GetClientRect(combo, &physicalClient)) return Refuse(reason, L"client bounds are unavailable");
    NativeDpiContext dpi(combo);
    RECT nativeClient{};
    COMBOBOXINFO info{ sizeof(info) };
    if (!GetClientRect(combo, &nativeClient) || nativeClient.right <= 0 ||
        physicalClient.right <= 0 || !GetComboBoxInfo(combo, &info))
        return Refuse(reason, L"native item or list geometry is unavailable");
    const auto scale = [&](int value) { return MulDiv(value, physicalClient.right, nativeClient.right); };
    const LRESULT rawCount = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    if (rawCount < 0 || rawCount > static_cast<LRESULT>(kMaxOwnerDrawComboItems))
        return Refuse(reason, L"item count exceeds 64 or cannot be read");
    const int count = static_cast<int>(rawCount);
    if (labels && labels->size() != static_cast<size_t>(count))
        return Refuse(reason, L"owner's item labels do not match the item count");
    const int selected = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (selected < -1 || selected >= count) return Refuse(reason, L"selection is outside the item range");
    const int listWidth = static_cast<int>(SendMessageW(combo, CB_GETDROPPEDWIDTH, 0, 0));
    RECT listClient{};
    if (listWidth <= 0 || !info.hwndList || !GetClientRect(info.hwndList, &listClient))
        return Refuse(reason, L"dropdown width is unavailable");
    // A popup whose horizontal extent exceeds its width needs a horizontal
    // viewport contract; silently clipping the owner face would lose content.
    if (SendMessageW(combo, CB_GETHORIZONTALEXTENT, 0, 0) > listWidth)
        return Refuse(reason, L"horizontally scrolling item faces are unsupported");
    const int itemWidth = std::max(1, listWidth - 2 * GetSystemMetrics(SM_CXEDGE));
    const bool enabled = IsWindowEnabled(combo) != FALSE;
    const UINT cues = static_cast<UINT>(SendMessageW(combo, WM_QUERYUISTATE, 0, 0));
    const UINT baseState = ODS_NOFOCUSRECT | (enabled ? 0 : ODS_DISABLED) |
        ((cues & UISF_HIDEACCEL) ? ODS_NOACCEL : 0);
    OwnerDrawComboSnapshot capture;
    capture.mode = drawStyle == CBS_OWNERDRAWFIXED ? L"fixed" : L"variable";
    std::vector<ULONG_PTR> identities;
    std::vector<int> heights;
    node.items.clear();
    size_t textBytes = 0;
    size_t pixelBytes = 0;
    auto paint = [&](int index, int width, int height, UINT state, ULONG_PTR data,
                     ImageListEntry& image) {
        const int physicalWidth = scale(width);
        const int physicalHeight = scale(height);
        if (width <= 0 || height <= 0 || physicalWidth <= 0 || physicalHeight <= 0 ||
            physicalWidth > kMaxOwnerDrawComboWidth || physicalHeight > kMaxOwnerDrawComboHeight)
            return Refuse(reason, L"item face exceeds the 512 by 128 pixel bound");
        const size_t bytes = static_cast<size_t>(physicalWidth) * physicalHeight * 4;
        if (bytes > kMaxOwnerDrawComboBytes - pixelBytes)
            return Refuse(reason, L"item faces exceed the 1 MiB decoded pixel budget");
        pixelBytes += bytes;
        DRAWITEMSTRUCT draw{};
        draw.CtlType = ODT_COMBOBOX;
        draw.CtlID = static_cast<UINT>(GetDlgCtrlID(combo));
        draw.itemID = static_cast<UINT>(index);
        draw.itemAction = ODA_DRAWENTIRE;
        draw.itemState = state;
        draw.hwndItem = combo;
        draw.itemData = data;
        ItemCanvas canvas(physicalWidth, physicalHeight);
        if (!canvas.Paint(combo, owner, draw, width, height, image))
            return Refuse(reason, L"owner did not handle WM_DRAWITEM or destroyed its control");
        return true;
    };
    for (int index = 0; index < count; ++index) {
        std::wstring label;
        if (labels) {
            label = (*labels)[static_cast<size_t>(index)];
        } else if (style & CBS_HASSTRINGS) {
            const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, index, 0);
            if (length < 0 || length > static_cast<LRESULT>(Ipc::kMaxStringChars))
                return Refuse(reason, L"item string length is unavailable or exceeds the text cap");
            label.resize(static_cast<size_t>(length) + 1);
            const LRESULT copied = SendMessageW(combo, CB_GETLBTEXT, index,
                reinterpret_cast<LPARAM>(label.data()));
            if (copied < 0 || copied > length)
                return Refuse(reason, L"item string changed while being read");
            label.resize(static_cast<size_t>(copied));
        }
        if (label.empty()) label = AccessibleChildName(info.hwndList, index + 1);
        if (label.empty() || label.size() > Ipc::kMaxStringChars)
            return Refuse(reason, L"item has no bounded native string or accessibility name");
        if (label.size() > 256 * 1024 - textBytes)
            return Refuse(reason, L"item labels exceed the aggregate text cap");
        textBytes += label.size();
        node.items.push_back(std::move(label));
        const auto data = static_cast<ULONG_PTR>(SendMessageW(combo, CB_GETITEMDATA, index, 0));
        const int height = static_cast<int>(SendMessageW(combo, CB_GETITEMHEIGHT, index, 0));
        identities.push_back(data);
        heights.push_back(height);
        OwnerDrawComboItemSnapshot item;
        if (!paint(index, itemWidth, height, baseState, data, item.normal) ||
            !paint(index, itemWidth, height, baseState | ODS_SELECTED, data, item.selected)) return false;
        capture.items.push_back(std::move(item));
    }
    node.editable = type == CBS_DROPDOWN;
    if (!node.editable) {
        ImageListEntry face;
        const ULONG_PTR data = selected < 0 ? 0 : identities[static_cast<size_t>(selected)];
        const int faceWidth = info.rcItem.right - info.rcItem.left;
        const int faceHeight = info.rcItem.bottom - info.rcItem.top;
        if (!paint(selected, faceWidth, faceHeight, baseState | ODS_COMBOBOXEDIT, data, face)) {
            // With nothing selected an owner may rightly draw nothing (mstsc's empty computer box,
            // a fresh drive list): the field shows its background, so the face is blank.
            if (selected >= 0 || !IsWindow(combo) || !IsWindow(owner)) return false;
            const int width = scale(faceWidth);
            const int height = scale(faceHeight);
            if (width <= 0 || height <= 0 || width > kMaxOwnerDrawComboWidth || height > kMaxOwnerDrawComboHeight)
                return false;
            reason.clear();
            face = {};
            face.imageWidth = static_cast<uint32_t>(width);
            face.imageHeight = static_cast<uint32_t>(height);
            face.imageFormat = L"bgra8-premultiplied";
            face.imageData.assign(static_cast<size_t>(width) * height * 4, 0);
        }
        capture.selectionFace = std::move(face);
    }
    if (!IsWindow(combo) || GetParent(combo) != owner ||
        static_cast<DWORD>(GetWindowLongPtrW(combo, GWL_STYLE)) != style ||
        SendMessageW(combo, CB_GETCOUNT, 0, 0) != count ||
        SendMessageW(combo, CB_GETCURSEL, 0, 0) != selected ||
        SendMessageW(combo, CB_GETDROPPEDWIDTH, 0, 0) != listWidth)
        return Refuse(reason, L"owner changed control identity, shape, or selection while painting");
    for (int index = 0; index < count; ++index)
        if (static_cast<ULONG_PTR>(SendMessageW(combo, CB_GETITEMDATA, index, 0)) != identities[index] ||
            SendMessageW(combo, CB_GETITEMHEIGHT, index, 0) != heights[index])
            return Refuse(reason, L"owner changed item identity or height while painting");
    node.selectedIndex = selected;
    node.ownerDrawCombo = std::move(capture);
    return true;
}

bool CaptureOwnerDrawListBox(HWND list, ControlNode& node, std::wstring& reason) {
    const auto refuse = [&](const wchar_t* detail) {
        reason = L"owner-draw ListBox: ";
        reason += detail;
        return false;
    };
    const DWORD thread = GetWindowThreadProcessId(list, nullptr);
    const HWND owner = GetParent(list);
    if (!thread || thread != GetCurrentThreadId() || !owner ||
        GetWindowThreadProcessId(owner, nullptr) != thread)
        return refuse(L"owner and control must share the current GUI thread");
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(list, GWL_STYLE));
    const DWORD drawStyle = style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE);
    if (drawStyle != LBS_OWNERDRAWFIXED && drawStyle != LBS_OWNERDRAWVARIABLE)
        return refuse(L"requires one fixed or variable draw style");
    RECT physicalClient{};
    if (!GetClientRect(list, &physicalClient)) return refuse(L"client bounds are unavailable");
    NativeDpiContext dpi(list);
    RECT nativeClient{};
    if (!GetClientRect(list, &nativeClient) || nativeClient.right <= 0 || physicalClient.right <= 0)
        return refuse(L"native row geometry is unavailable");
    const auto scale = [&](int value) { return MulDiv(value, physicalClient.right, nativeClient.right); };
    const LRESULT rawCount = SendMessageW(list, LB_GETCOUNT, 0, 0);
    if (rawCount < 0 || rawCount > static_cast<LRESULT>(kMaxOwnerDrawComboItems))
        return refuse(L"item count exceeds 64 or cannot be read");
    const int count = static_cast<int>(rawCount);
    // A list wider than its window scrolls sideways, which needs a horizontal viewport contract.
    if (SendMessageW(list, LB_GETHORIZONTALEXTENT, 0, 0) > nativeClient.right)
        return refuse(L"horizontally scrolling rows are unsupported");
    const bool multiple = (style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0;
    std::vector<int> selection;
    if (multiple) {
        const LRESULT selected = SendMessageW(list, LB_GETSELCOUNT, 0, 0);
        if (selected == LB_ERR || selected > count) return refuse(L"selection could not be read");
        selection.assign(static_cast<size_t>(selected), 0);
        if (selected > 0 && SendMessageW(list, LB_GETSELITEMS, static_cast<WPARAM>(selected),
                reinterpret_cast<LPARAM>(selection.data())) != selected)
            return refuse(L"selection could not be read");
        std::sort(selection.begin(), selection.end());
    } else {
        const int current = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (current < -1 || current >= count) return refuse(L"selection is outside the item range");
        if (current >= 0) selection.push_back(current);
    }
    const bool enabled = IsWindowEnabled(list) != FALSE;
    const UINT cues = static_cast<UINT>(SendMessageW(list, WM_QUERYUISTATE, 0, 0));
    const UINT baseState = (enabled ? 0 : ODS_DISABLED) | ((cues & UISF_HIDEACCEL) ? ODS_NOACCEL : 0);
    OwnerDrawListSnapshot capture;
    capture.mode = drawStyle == LBS_OWNERDRAWFIXED ? L"fixed" : L"variable";
    std::vector<std::wstring> labels;
    std::vector<ULONG_PTR> identities;
    std::vector<int> heights;
    size_t textBytes = 0;
    size_t pixelBytes = 0;
    const auto paint = [&](int index, int height, UINT state, ULONG_PTR data, ImageListEntry& image) {
        const int width = nativeClient.right;
        const int physicalWidth = scale(width);
        const int physicalHeight = scale(height);
        if (height <= 0 || physicalWidth <= 0 || physicalHeight <= 0 ||
            physicalWidth > kMaxOwnerDrawComboWidth || physicalHeight > kMaxOwnerDrawComboHeight)
            return refuse(L"row face exceeds the 512 by 128 pixel bound");
        const size_t bytes = static_cast<size_t>(physicalWidth) * physicalHeight * 4;
        if (bytes > kMaxOwnerDrawComboBytes - pixelBytes)
            return refuse(L"row faces exceed the 1 MiB decoded pixel budget");
        pixelBytes += bytes;
        DRAWITEMSTRUCT draw{};
        draw.CtlType = ODT_LISTBOX;
        draw.CtlID = static_cast<UINT>(GetDlgCtrlID(list));
        draw.itemID = static_cast<UINT>(index);
        draw.itemAction = ODA_DRAWENTIRE;
        draw.itemState = state;
        draw.hwndItem = list;
        draw.itemData = data;
        ItemCanvas canvas(physicalWidth, physicalHeight);
        if (!canvas.Paint(list, owner, draw, width, height, image))
            return refuse(L"owner did not handle WM_DRAWITEM or destroyed its control");
        return true;
    };
    for (int index = 0; index < count; ++index) {
        std::wstring label;
        if (style & LBS_HASSTRINGS) {
            const LRESULT length = SendMessageW(list, LB_GETTEXTLEN, index, 0);
            if (length < 0 || length > static_cast<LRESULT>(Ipc::kMaxStringChars))
                return refuse(L"item string length is unavailable or exceeds the text cap");
            label.resize(static_cast<size_t>(length) + 1);
            const LRESULT copied = SendMessageW(list, LB_GETTEXT, index,
                reinterpret_cast<LPARAM>(label.data()));
            if (copied < 0 || copied > length) return refuse(L"item string changed while being read");
            label.resize(static_cast<size_t>(copied));
        }
        if (label.empty()) label = AccessibleChildName(list, index + 1);
        if (label.empty() || label.size() > Ipc::kMaxStringChars)
            return refuse(L"item has no bounded native string or accessibility name");
        if (label.size() > 256 * 1024 - textBytes) return refuse(L"item labels exceed the aggregate text cap");
        textBytes += label.size();
        labels.push_back(std::move(label));
        const auto data = static_cast<ULONG_PTR>(SendMessageW(list, LB_GETITEMDATA, index, 0));
        const int height = static_cast<int>(SendMessageW(list, LB_GETITEMHEIGHT, index, 0));
        identities.push_back(data);
        heights.push_back(height);
        OwnerDrawComboItemSnapshot item;
        if (!paint(index, height, baseState, data, item.normal)) return false;
        if (std::binary_search(selection.begin(), selection.end(), index) &&
            !paint(index, height, baseState | ODS_SELECTED, data, item.selected)) return false;
        capture.items.push_back(std::move(item));
    }
    if (!IsWindow(list) || GetParent(list) != owner ||
        static_cast<DWORD>(GetWindowLongPtrW(list, GWL_STYLE)) != style ||
        SendMessageW(list, LB_GETCOUNT, 0, 0) != count ||
        (!multiple && SendMessageW(list, LB_GETCURSEL, 0, 0) != (selection.empty() ? LB_ERR : selection.front())))
        return refuse(L"owner changed control identity, shape, or selection while painting");
    for (int index = 0; index < count; ++index)
        if (static_cast<ULONG_PTR>(SendMessageW(list, LB_GETITEMDATA, index, 0)) != identities[index] ||
            SendMessageW(list, LB_GETITEMHEIGHT, index, 0) != heights[index])
            return refuse(L"owner changed item identity or height while painting");
    node.items = std::move(labels);
    node.multiSelect = multiple;
    node.selectedIndices = multiple ? selection : std::vector<int>{};
    node.selectedIndex = selection.empty() ? -1 : selection.front();
    node.ownerDrawList = std::move(capture);
    return true;
}

} // namespace FluentShell::Bridge::Translation
