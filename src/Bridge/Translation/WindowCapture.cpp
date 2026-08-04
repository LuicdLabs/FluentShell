#include "WindowCapture.h"

#include "../../Common/FluentShell.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <unordered_set>
#include <utility>

namespace FluentShell::Bridge::Translation {
namespace {

constexpr wchar_t kNodeGenerationProperty[] = L"FluentShell.Bridge.NodeGeneration";

class PhysicalCoordinateScope final {
public:
    PhysicalCoordinateScope() noexcept
        : previous_(SetThreadDpiAwarenessContext(
              DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~PhysicalCoordinateScope() {
        if (previous_) SetThreadDpiAwarenessContext(previous_);
    }
    bool IsValid() const noexcept { return previous_ != nullptr; }

private:
    DPI_AWARENESS_CONTEXT previous_ = nullptr;
};

bool IsUsableTopLevelBounds(const RECT& bounds) {
    return bounds.right > bounds.left && bounds.bottom > bounds.top &&
        !(bounds.left == -32000 && bounds.top == -32000);
}

bool CaptureTopLevelBounds(HWND root, bool minimized, bool maximized, RECT& bounds) {
    if (!GetWindowRect(root, &bounds)) return false;
    if (!minimized && !maximized) return IsUsableTopLevelBounds(bounds);

    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(root, &placement)) return false;

    RECT normal = placement.rcNormalPosition;
    if (!IsUsableTopLevelBounds(normal)) return false;
    const auto exStyle = static_cast<uint64_t>(GetWindowLongPtrW(root, GWL_EXSTYLE));
    if ((exStyle & WS_EX_TOOLWINDOW) == 0) {
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        const HMONITOR monitor = MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST);
        if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return false;
        OffsetRect(&normal,
            monitorInfo.rcWork.left - monitorInfo.rcMonitor.left,
            monitorInfo.rcWork.top - monitorInfo.rcMonitor.top);
    }
    bounds = normal;
    return true;
}

std::wstring WindowClass(HWND hwnd) {
    wchar_t value[256]{};
    const int length = GetClassNameW(hwnd, value, static_cast<int>(std::size(value)));
    return length > 0 ? std::wstring(value, static_cast<size_t>(length)) : std::wstring();
}

bool WindowText(HWND hwnd, std::wstring& value, std::wstring& reason) {
    const int reported = GetWindowTextLengthW(hwnd);
    if (reported < 0 || static_cast<size_t>(reported) > Ipc::kMaxStringChars) {
        reason = L"window text exceeds the 64 KiB protocol string limit";
        return false;
    }
    std::wstring captured(static_cast<size_t>(reported) + 1, L'\0');
    const int length = GetWindowTextW(hwnd, captured.data(), static_cast<int>(captured.size()));
    if (length < 0 || static_cast<size_t>(length) > Ipc::kMaxStringChars) {
        reason = L"window text exceeds the 64 KiB protocol string limit";
        return false;
    }
    captured.resize(static_cast<size_t>(length));
    value = std::move(captured);
    return true;
}

bool IsStandardTopLevel(HWND root, std::wstring& reason) {
    if (!root || !IsWindow(root) || GetAncestor(root, GA_ROOT) != root) {
        reason = L"not a top-level window";
        return false;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(root, &processId);
    if (processId != GetCurrentProcessId()) {
        reason = L"foreign top-level window";
        return false;
    }
    const auto style = static_cast<uint64_t>(GetWindowLongPtrW(root, GWL_STYLE));
    const auto exStyle = static_cast<uint64_t>(GetWindowLongPtrW(root, GWL_EXSTYLE));
    if ((style & WS_CHILD) != 0 || (exStyle & WS_EX_LAYERED) != 0) {
        reason = L"child or layered top-level window";
        return false;
    }
    if (GetWindow(root, GW_OWNER) != nullptr) {
        reason = L"owned top-level window is outside the v1 owner graph";
        return false;
    }
    if (GetMenu(root)) {
        reason = L"top-level menu is not supported in v1";
        return false;
    }
    HRGN region = CreateRectRgn(0, 0, 0, 0);
    const int regionType = region ? GetWindowRgn(root, region) : ERROR;
    if (region) DeleteObject(region);
    if (regionType != ERROR) {
        reason = L"nonrectangular top-level window";
        return false;
    }
    return true;
}

bool ClassifyControl(
    HWND hwnd,
    ControlKind& kind,
    std::wstring& reason) {
    const auto className = WindowClass(hwnd);
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));

    if (FluentShell::EqualsIgnoreCase(className, L"Static")) {
        const DWORD type = style & SS_TYPEMASK;
        if (type != SS_LEFT && type != SS_CENTER && type != SS_RIGHT &&
            type != SS_SIMPLE && type != SS_LEFTNOWORDWRAP &&
            type != SS_ETCHEDHORZ && type != SS_ETCHEDVERT) {
            reason = L"unsupported Static draw style";
            return false;
        }
        kind = (type == SS_ETCHEDHORZ || type == SS_ETCHEDVERT)
            ? ControlKind::Separator
            : ControlKind::StaticText;
        return true;
    }

    if (FluentShell::EqualsIgnoreCase(className, L"Button")) {
        const DWORD type = style & BS_TYPEMASK;
        if (type == BS_OWNERDRAW || (style & (BS_BITMAP | BS_ICON)) != 0) {
            reason = L"unsupported Button draw style";
            return false;
        }
        switch (type) {
        case BS_PUSHBUTTON:
        case BS_DEFPUSHBUTTON:
            kind = ControlKind::Button;
            return true;
        case BS_CHECKBOX:
        case BS_AUTOCHECKBOX:
            kind = ControlKind::CheckBox;
            return true;
        case BS_3STATE:
        case BS_AUTO3STATE:
            kind = ControlKind::ThreeState;
            return true;
        case BS_RADIOBUTTON:
        case BS_AUTORADIOBUTTON:
            kind = ControlKind::RadioButton;
            return true;
        default:
            reason = L"unsupported Button type";
            return false;
        }
    }

    if (FluentShell::EqualsIgnoreCase(className, L"Edit")) {
        kind = (style & ES_PASSWORD) != 0 ? ControlKind::Password : ControlKind::Edit;
        return true;
    }

    if (FluentShell::EqualsIgnoreCase(className, L"ComboBox")) {
        if ((style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) != 0) {
            reason = L"owner-draw ComboBox";
            return false;
        }
        if ((style & 0x0003u) != CBS_DROPDOWNLIST) {
            reason = L"editable ComboBox is not supported in v1";
            return false;
        }
        kind = ControlKind::ComboBox;
        return true;
    }

    if (FluentShell::EqualsIgnoreCase(className, L"ListBox")) {
        if ((style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE | LBS_NODATA |
                      LBS_MULTIPLESEL | LBS_EXTENDEDSEL | LBS_MULTICOLUMN | LBS_NOSEL)) != 0) {
            reason = L"owner-draw, virtual, multi-select, or multi-column ListBox";
            return false;
        }
        kind = ControlKind::ListBox;
        return true;
    }

    reason = L"unsupported visible control class: " + className;
    return false;
}

bool ReadStringItems(
    HWND hwnd,
    bool combo,
    std::vector<std::wstring>& items,
    std::wstring& reason) {
    const UINT countMessage = combo ? CB_GETCOUNT : LB_GETCOUNT;
    const UINT lengthMessage = combo ? CB_GETLBTEXTLEN : LB_GETTEXTLEN;
    const UINT textMessage = combo ? CB_GETLBTEXT : LB_GETTEXT;
    const LRESULT rawCount = SendMessageW(hwnd, countMessage, 0, 0);
    if (rawCount < 0 || rawCount > static_cast<LRESULT>(Ipc::kMaxListItems)) {
        reason = L"invalid or excessive string item count";
        return false;
    }
    items.reserve(static_cast<size_t>(rawCount));
    for (LRESULT index = 0; index < rawCount; ++index) {
        const LRESULT length = SendMessageW(hwnd, lengthMessage, index, 0);
        if (length < 0 || length > static_cast<LRESULT>(Ipc::kMaxStringChars)) {
            reason = L"invalid string item length";
            return false;
        }
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        const LRESULT copied = SendMessageW(
            hwnd, textMessage, index, reinterpret_cast<LPARAM>(text.data()));
        if (copied < 0) {
            reason = L"failed to read string item";
            return false;
        }
        text.resize(static_cast<size_t>(copied));
        items.push_back(std::move(text));
    }
    return true;
}

struct Enumeration final {
    std::vector<HWND> handles;
};

BOOL CALLBACK CollectChild(HWND hwnd, LPARAM raw) {
    auto& enumeration = *reinterpret_cast<Enumeration*>(raw);
    enumeration.handles.push_back(hwnd);
    return enumeration.handles.size() <= Ipc::kMaxNodes;
}

bool CaptureNativeTabOrder(
    HWND root,
    const std::vector<HWND>& handles,
    std::unordered_map<HWND, int>& tabIndexes,
    std::wstring& reason) {
    std::unordered_set<HWND> candidates;
    for (const HWND child : handles) {
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(child, GWL_STYLE));
        if (IsWindowVisible(child) && IsWindowEnabled(child) &&
            (style & WS_TABSTOP) != 0) {
            candidates.insert(child);
        }
    }
    if (candidates.empty()) return true;

    // GetNextDlgTabItem is the native dialog manager's source of truth. Do
    // not silently substitute EnumChildWindows order when it cannot describe
    // every currently focusable candidate; that would produce a misleading
    // focus contract and should instead trigger whole-window fallback.
    std::unordered_set<HWND> visited;
    HWND current = nullptr;
    for (size_t attempt = 0; attempt <= handles.size() + candidates.size(); ++attempt) {
        const HWND next = GetNextDlgTabItem(root, current, FALSE);
        if (!next) {
            reason = L"native dialog manager did not expose a complete tab order";
            return false;
        }
        if (!visited.insert(next).second) break;
        if (candidates.contains(next)) {
            tabIndexes.emplace(next, static_cast<int>(tabIndexes.size()));
        }
        current = next;
    }
    if (tabIndexes.size() != candidates.size()) {
        reason = L"native dialog manager tab order omitted a focusable control";
        return false;
    }
    return true;
}

template <typename T>
void HashBytes(uint64_t& hash, const T& value) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (size_t index = 0; index < sizeof(T); ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}

void HashString(uint64_t& hash, std::wstring_view value) noexcept {
    for (const wchar_t character : value) HashBytes(hash, character);
}

} // namespace

bool CaptureWindow(
    HWND root,
    CaptureContext& context,
    WindowSnapshot& snapshot,
    std::wstring& rejectionReason) noexcept {
    try {
        PhysicalCoordinateScope dpiScope;
        if (!dpiScope.IsValid()) {
            rejectionReason = L"cannot establish physical-coordinate DPI context";
            return false;
        }
        if (!IsStandardTopLevel(root, rejectionReason)) return false;

        WindowSnapshot next;
        next.surfaceId = context.surfaceId;
        next.surfaceKind = SurfaceKind::Window;
        next.modal = false;
        next.canCancel = true;
        next.generation = context.generation;
        next.revision = context.revision;
        next.nativeHwnd = root;
        next.ownerHwnd = GetWindow(root, GW_OWNER);
        if (!WindowText(root, next.title, rejectionReason)) return false;
        next.dpi = GetDpiForWindow(root);
        if (next.dpi == 0) next.dpi = 96;
        const bool minimized = IsIconic(root) != FALSE;
        const bool maximized = IsZoomed(root) != FALSE;
        if (!CaptureTopLevelBounds(root, minimized, maximized, next.bounds)) {
            rejectionReason = L"native window has no valid restore bounds";
            return false;
        }
        RECT client{};
        GetClientRect(root, &client);
        next.clientBounds = client;
        next.windowStyle = static_cast<uint64_t>(GetWindowLongPtrW(root, GWL_STYLE));
        next.windowExStyle = static_cast<uint64_t>(GetWindowLongPtrW(root, GWL_EXSTYLE));
        next.visible = IsWindowVisible(root) != FALSE;
        next.enabled = IsWindowEnabled(root) != FALSE;
        next.state = minimized ? L"minimized" : (maximized ? L"maximized" : L"normal");
        next.showInTaskbar = next.ownerHwnd == nullptr || (next.windowExStyle & WS_EX_APPWINDOW) != 0;
        next.rtl = (next.windowExStyle & WS_EX_LAYOUTRTL) != 0;

        Enumeration enumeration;
        EnumChildWindows(root, CollectChild, reinterpret_cast<LPARAM>(&enumeration));
        if (enumeration.handles.size() > Ipc::kMaxNodes) {
            rejectionReason = L"window exceeds the 512 node limit";
            return false;
        }

        std::unordered_map<HWND, uint64_t> visibleNodeIds;
        std::unordered_map<HWND, int> nativeTabIndexes;
        if (!CaptureNativeTabOrder(root, enumeration.handles, nativeTabIndexes, rejectionReason)) {
            return false;
        }
        const DWORD rootThread = GetWindowThreadProcessId(root, nullptr);
        int zIndex = 0;
        for (const HWND child : enumeration.handles) {
            if (!IsWindowVisible(child)) continue;
            DWORD childProcess = 0;
            const DWORD childThread = GetWindowThreadProcessId(child, &childProcess);
            if (childProcess != GetCurrentProcessId() || childThread != rootThread) {
                rejectionReason = L"foreign-process or foreign-thread child HWND";
                return false;
            }

            ControlNode node;
            if (!ClassifyControl(child, node.kind, rejectionReason)) return false;
            uint64_t lifecycleGeneration = reinterpret_cast<uintptr_t>(
                GetPropW(child, kNodeGenerationProperty));
            if (lifecycleGeneration == 0) {
                lifecycleGeneration = context.nextNodeGeneration++;
                if (!SetPropW(child, kNodeGenerationProperty,
                        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(lifecycleGeneration)))) {
                    rejectionReason = L"cannot assign bridge-owned child generation";
                    return false;
                }
            }
            auto id = context.nodeIds.find(child);
            if (id == context.nodeIds.end() || id->second.generation != lifecycleGeneration) {
                CaptureContext::NodeIdentity identity{
                    lifecycleGeneration, context.nextNodeId++ };
                id = context.nodeIds.insert_or_assign(child, identity).first;
            }
            node.nodeId = id->second.nodeId;
            node.generation = lifecycleGeneration;
            node.hwnd = child;
            const HWND parent = GetParent(child);
            if (parent != root) {
                const auto parentId = visibleNodeIds.find(parent);
                if (parentId == visibleNodeIds.end()) {
                    rejectionReason = L"unsupported or hidden intermediate parent";
                    return false;
                }
                node.parentNodeId = parentId->second;
            }
            node.controlId = GetDlgCtrlID(child);
            node.zIndex = zIndex++;
            RECT childRect{};
            GetWindowRect(child, &childRect);
            MapWindowPoints(nullptr, root, reinterpret_cast<POINT*>(&childRect), 2);
            node.rect = childRect;
            node.style = static_cast<uint64_t>(GetWindowLongPtrW(child, GWL_STYLE));
            node.exStyle = static_cast<uint64_t>(GetWindowLongPtrW(child, GWL_EXSTYLE));
            node.visible = true;
            node.enabled = IsWindowEnabled(child) != FALSE;
            node.tabStop = (node.style & WS_TABSTOP) != 0;
            if (const auto tab = nativeTabIndexes.find(child); tab != nativeTabIndexes.end()) {
                node.tabIndex = tab->second;
            }
            node.dialogCode = static_cast<uint32_t>(SendMessageW(child, WM_GETDLGCODE, 0, 0));
            if ((node.dialogCode & (DLGC_WANTALLKEYS | DLGC_WANTMESSAGE)) != 0) {
                rejectionReason = L"control requires custom keyboard routing";
                return false;
            }
            if (!WindowText(child, node.text, rejectionReason)) return false;
            // Never place a password's clear text in the UIA Name property.
            // The PasswordBox still receives the canonical value through its
            // typed view model, while accessibility exposes only its role.
            node.automationName = node.kind == ControlKind::Password
                ? L"Password edit"
                : node.text;
            node.groupStart = (node.style & WS_GROUP) != 0;

            const DWORD controlStyle = static_cast<DWORD>(node.style);
            if (node.kind == ControlKind::Button) {
                node.isDefault = (controlStyle & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
            } else if (node.kind == ControlKind::CheckBox ||
                       node.kind == ControlKind::ThreeState ||
                       node.kind == ControlKind::RadioButton) {
                node.checked = static_cast<int>(SendMessageW(child, BM_GETCHECK, 0, 0));
            } else if (node.kind == ControlKind::Edit || node.kind == ControlKind::Password) {
                node.readOnly = (controlStyle & ES_READONLY) != 0;
                node.multiline = (controlStyle & ES_MULTILINE) != 0;
                DWORD start = 0;
                DWORD end = 0;
                SendMessageW(child, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
                node.selectionStart = static_cast<int>(start);
                node.selectionLength = static_cast<int>(end >= start ? end - start : 0);
            } else if (node.kind == ControlKind::ComboBox || node.kind == ControlKind::ListBox) {
                const bool combo = node.kind == ControlKind::ComboBox;
                if (!ReadStringItems(child, combo, node.items, rejectionReason)) return false;
                node.selectedIndex = static_cast<int>(SendMessageW(
                    child, combo ? CB_GETCURSEL : LB_GETCURSEL, 0, 0));
            }

            visibleNodeIds.emplace(child, node.nodeId);
            next.nodes.push_back(std::move(node));
        }

        snapshot = std::move(next);
        return true;
    } catch (...) {
        rejectionReason = L"exception while capturing native window";
        return false;
    }
}

uint64_t SnapshotFingerprint(const WindowSnapshot& snapshot) noexcept {
    uint64_t hash = 1469598103934665603ull;
    HashBytes(hash, snapshot.nativeHwnd);
    HashBytes(hash, snapshot.ownerHwnd);
    HashBytes(hash, snapshot.generation);
    HashString(hash, snapshot.title);
    HashBytes(hash, snapshot.dpi);
    HashBytes(hash, snapshot.bounds);
    HashBytes(hash, snapshot.clientBounds);
    HashBytes(hash, snapshot.windowStyle);
    HashBytes(hash, snapshot.windowExStyle);
    HashBytes(hash, snapshot.visible);
    HashBytes(hash, snapshot.enabled);
    HashString(hash, snapshot.state);
    HashBytes(hash, snapshot.showInTaskbar);
    HashBytes(hash, snapshot.rtl);
    HashBytes(hash, snapshot.nodes.size());
    for (const auto& node : snapshot.nodes) {
        HashBytes(hash, node.nodeId);
        HashBytes(hash, node.generation);
        HashBytes(hash, node.hwnd);
        const bool hasParent = node.parentNodeId.has_value();
        HashBytes(hash, hasParent);
        if (node.parentNodeId) HashBytes(hash, *node.parentNodeId);
        HashBytes(hash, node.kind);
        HashBytes(hash, node.controlId);
        HashBytes(hash, node.zIndex);
        HashBytes(hash, node.tabIndex);
        HashBytes(hash, node.rect);
        HashBytes(hash, node.style);
        HashBytes(hash, node.exStyle);
        HashBytes(hash, node.visible);
        HashBytes(hash, node.enabled);
        HashBytes(hash, node.tabStop);
        HashBytes(hash, node.dialogCode);
        HashString(hash, node.text);
        HashString(hash, node.automationName);
        HashBytes(hash, node.checked);
        HashBytes(hash, node.selectedIndex);
        HashBytes(hash, node.selectionStart);
        HashBytes(hash, node.selectionLength);
        HashBytes(hash, node.readOnly);
        HashBytes(hash, node.multiline);
        HashBytes(hash, node.isDefault);
        HashBytes(hash, node.groupStart);
        HashBytes(hash, node.items.size());
        for (const auto& item : node.items) HashString(hash, item);
    }
    return hash;
}

} // namespace FluentShell::Bridge::Translation
