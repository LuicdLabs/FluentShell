#include "WindowCapture.h"

#include "../../Common/FluentShell.h"
#include "ControlAdapters.h"

#include <commctrl.h>

#include <array>
#include <unordered_set>
#include <utility>

namespace FluentShell::Bridge::Translation {
namespace {

constexpr wchar_t kNodeGenerationProperty[] = L"FluentShell.Bridge.NodeGeneration";
constexpr size_t kMaxMenuDepth = 8;
constexpr size_t kMaxMenuItems = 256;

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

// Appended to every rejection so the log names the concrete HWND that failed
// its adapter contract, not just the stage that rejected it.
void AppendWindowEvidence(HWND hwnd, std::wstring& reason) {
    wchar_t classBuffer[kMaxClassNameChars]{};
    reason.append(L" [hwnd=");
    reason.append(std::to_wstring(reinterpret_cast<uintptr_t>(hwnd)));
    reason.append(L" class=");
    reason.append(ClassNameOf(hwnd, classBuffer));
    reason.append(L" style=");
    reason.append(std::to_wstring(
        static_cast<uint64_t>(GetWindowLongPtrW(hwnd, GWL_STYLE))));
    reason.append(L" exStyle=");
    reason.append(std::to_wstring(
        static_cast<uint64_t>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE))));
    reason.push_back(L']');
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
    if ((exStyle & WS_EX_MDICHILD) != 0) {
        reason = L"MDI child top-level window";
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

struct Enumeration final {
    std::vector<HWND> handles;
};

BOOL CALLBACK CollectChild(HWND hwnd, LPARAM raw) {
    auto& enumeration = *reinterpret_cast<Enumeration*>(raw);
    enumeration.handles.push_back(hwnd);
    return enumeration.handles.size() <= Ipc::kMaxNodes;
}

bool IsEffectivelyEnabled(HWND root, HWND child) noexcept {
    for (HWND current = child; current; current = GetParent(current)) {
        if (!IsWindowEnabled(current)) return false;
        if (current == root) return true;
    }
    return false;
}

bool IsDialogControlParent(HWND child) noexcept {
    wchar_t classBuffer[kMaxClassNameChars]{};
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(child, GWL_STYLE));
    const auto exStyle = static_cast<DWORD>(GetWindowLongPtrW(child, GWL_EXSTYLE));
    return FluentShell::EqualsIgnoreCase(ClassNameOf(child, classBuffer), L"#32770") &&
        (style & (WS_CHILD | DS_CONTROL)) == (WS_CHILD | DS_CONTROL) &&
        (exStyle & WS_EX_CONTROLPARENT) != 0;
}

bool CaptureNativeTabOrder(
    HWND root,
    const std::vector<HWND>& handles,
    std::unordered_map<HWND, int>& tabIndexes,
    std::wstring& reason) {
    std::unordered_set<HWND> candidates;
    for (const HWND child : handles) {
        if (IsCompositeImplementationChild(child)) continue;
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(child, GWL_STYLE));
        if (!IsDialogControlParent(child) && IsWindowVisible(child) &&
            IsEffectivelyEnabled(root, child) &&
            (style & WS_TABSTOP) != 0) {
            candidates.insert(child);
        }
    }
    if (candidates.empty()) return true;

    const auto seed = std::find_if(handles.begin(), handles.end(), [root](HWND child) {
        return GetParent(child) == root;
    });
    if (seed == handles.end()) {
        reason = L"native dialog manager has no direct child traversal seed";
        return false;
    }

    // GetNextDlgTabItem is the native dialog manager's source of truth. A raw
    // WS_TABSTOP that this walk omits is not an effective dialog tab stop; keep
    // its style as evidence, but do not invent an order from HWND enumeration.
    std::unordered_set<HWND> visited;
    HWND current = *seed;
    for (size_t attempt = 0; attempt <= handles.size() + candidates.size(); ++attempt) {
        const HWND next = GetNextDlgTabItem(root, current, FALSE);
        if (!next) break;
        if (!IsChild(root, next)) {
            reason = L"native dialog manager returned a tab stop outside the source window";
            return false;
        }
        if (!visited.insert(next).second) break;
        if (candidates.contains(next)) {
            tabIndexes.emplace(next, static_cast<int>(tabIndexes.size()));
        }
        current = next;
    }
    return true;
}

void HashRange(uint64_t& hash, const void* data, size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
}

template <typename T>
void HashBytes(uint64_t& hash, const T& value) noexcept {
    HashRange(hash, &value, sizeof(T));
}

// A ListView can carry 256 KiB of text and the fingerprint runs on every
// reconcile tick, so string content is folded in one contiguous pass rather
// than one call per character.  The byte order, and therefore the hash, is
// identical to the per-character walk.
void HashString(uint64_t& hash, std::wstring_view value) noexcept {
    HashRange(hash, value.data(), value.size() * sizeof(wchar_t));
}

bool HasMdiClient(HWND root) {
    return FindWindowExW(root, nullptr, L"MDIClient", nullptr) != nullptr;
}

struct MenuCaptureState final {
    size_t count = 0;
    std::unordered_set<HMENU> menus;
    std::unordered_set<uint32_t> commandIds;
};

bool CaptureMenuLevel(
    HMENU menu,
    size_t depth,
    std::wstring_view path,
    bool topLevel,
    bool ancestorEnabled,
    MenuCaptureState& state,
    std::vector<MenuItemSnapshot>& result,
    std::wstring& reason) {
    if (!menu || !IsMenu(menu) || depth > kMaxMenuDepth || !state.menus.insert(menu).second) {
        reason = L"invalid, deep, or cyclic menu shape";
        return false;
    }
    MENUINFO menuInfo{sizeof(menuInfo)};
    menuInfo.fMask = MIM_STYLE;
    if (!GetMenuInfo(menu, &menuInfo) ||
        (menuInfo.dwStyle & (MNS_NOTIFYBYPOS | MNS_MODELESS | MNS_DRAGDROP)) != 0) {
        reason = L"callback or modeless menu semantics are not supported";
        return false;
    }
    const int count = GetMenuItemCount(menu);
    if (count < 0 || state.count + static_cast<size_t>(count) > kMaxMenuItems) {
        reason = L"invalid or excessive menu item count";
        return false;
    }
    bool foundDefault = false;
    result.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW info{sizeof(info)};
        std::wstring text(Ipc::kMaxStringChars + 1, L'\0');
        info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU |
            MIIM_STRING | MIIM_BITMAP | MIIM_CHECKMARKS | MIIM_DATA;
        info.dwTypeData = text.data();
        info.cch = static_cast<UINT>(text.size());
        if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info) ||
            info.cch > Ipc::kMaxStringChars) {
            reason = L"invalid or excessive menu label";
            return false;
        }
        text.resize(info.cch);
        const UINT unsupportedType = MFT_OWNERDRAW | MFT_BITMAP | MFT_MENUBARBREAK |
            MFT_MENUBREAK | MFT_RIGHTJUSTIFY;
        if ((info.fType & unsupportedType) != 0 || info.hbmpItem != nullptr ||
            info.hbmpChecked != nullptr || info.hbmpUnchecked != nullptr ||
            info.dwItemData != 0) {
            reason = L"owner-draw, bitmap, or custom menu item";
            return false;
        }
        MenuItemSnapshot item;
        item.itemId = path.empty()
            ? std::to_wstring(index)
            : std::wstring(path) + L"." + std::to_wstring(index);
        item.text = std::move(text);
        item.enabled = ancestorEnabled &&
            (info.fState & (MFS_DISABLED | MFS_GRAYED)) == 0;
        item.checked = (info.fState & MFS_CHECKED) != 0;
        item.radio = (info.fType & MFT_RADIOCHECK) != 0;
        item.isDefault = (info.fState & MFS_DEFAULT) != 0;
        if (item.isDefault && foundDefault) {
            reason = L"menu level has multiple default items";
            return false;
        }
        foundDefault = foundDefault || item.isDefault;
        ++state.count;
        if ((info.fType & MFT_SEPARATOR) != 0) {
            if (topLevel || info.hSubMenu || !item.text.empty()) {
                reason = L"invalid menu separator shape";
                return false;
            }
            item.kind = MenuItemKind::Separator;
        } else if (info.hSubMenu) {
            if (item.text.empty()) {
                reason = L"popup menu has no textual label";
                return false;
            }
            item.kind = MenuItemKind::Popup;
            if (!CaptureMenuLevel(info.hSubMenu, depth + 1, item.itemId,
                    false, item.enabled, state, item.items, reason) || item.items.empty()) return false;
        } else {
            if (topLevel || item.text.empty() || info.wID == 0 || info.wID >= 0xf000) {
                reason = L"menu command cannot use standard WM_COMMAND semantics";
                return false;
            }
            item.kind = MenuItemKind::Command;
            item.commandId = info.wID;
            if (!state.commandIds.insert(item.commandId).second) {
                reason = L"ambiguous duplicate executable menu command ID";
                return false;
            }
        }
        result.push_back(std::move(item));
    }
    return true;
}

// Per-window state the child loop threads through node capture.  Passing it as
// one value keeps the child capture stages independent of the loop itself.
struct ChildCaptureScope final {
    HWND root = nullptr;
    DWORD rootThread = 0;
    const std::unordered_map<HWND, int>* tabIndexes = nullptr;
    const std::unordered_map<HWND, uint64_t>* visibleNodeIds = nullptr;
    int zIndex = 0;
};

bool CaptureTopLevelFacets(
    HWND root,
    const CaptureContext& context,
    WindowSnapshot& next,
    std::wstring& reason) {
    next.surfaceId = context.surfaceId;
    next.surfaceKind = SurfaceKind::Window;
    next.modal = false;
    next.canCancel = true;
    next.generation = context.generation;
    next.revision = context.revision;
    next.nativeHwnd = root;
    next.ownerHwnd = GetWindow(root, GW_OWNER);
    if (!WindowText(root, next.title, reason)) return false;
    next.dpi = GetDpiForWindow(root);
    if (next.dpi == 0) next.dpi = 96;
    const bool minimized = IsIconic(root) != FALSE;
    const bool maximized = IsZoomed(root) != FALSE;
    if (!CaptureTopLevelBounds(root, minimized, maximized, next.bounds)) {
        reason = L"native window has no valid restore bounds";
        return false;
    }
    GetClientRect(root, &next.clientBounds);
    next.windowStyle = static_cast<uint64_t>(GetWindowLongPtrW(root, GWL_STYLE));
    next.windowExStyle = static_cast<uint64_t>(GetWindowLongPtrW(root, GWL_EXSTYLE));
    next.visible = IsWindowVisible(root) != FALSE;
    next.enabled = IsWindowEnabled(root) != FALSE;
    next.state = minimized ? L"minimized" : (maximized ? L"maximized" : L"normal");
    next.showInTaskbar =
        next.ownerHwnd == nullptr || (next.windowExStyle & WS_EX_APPWINDOW) != 0;
    next.rtl = (next.windowExStyle & WS_EX_LAYOUTRTL) != 0;
    return CaptureTopLevelMenu(root, next.menu, reason);
}

// Node IDs must survive a reconcile pass but never outlive the HWND they name.
// A bridge-owned window property carries the lifecycle generation, so a
// recreated control at the same address is issued a fresh node ID.
bool AssignNodeIdentity(
    HWND child,
    CaptureContext& context,
    ControlNode& node,
    std::wstring& reason) {
    auto generation = reinterpret_cast<uintptr_t>(
        GetPropW(child, kNodeGenerationProperty));
    if (generation == 0) {
        generation = static_cast<uintptr_t>(context.nextNodeGeneration++);
        if (!SetPropW(child, kNodeGenerationProperty,
                reinterpret_cast<HANDLE>(generation))) {
            reason = L"cannot assign bridge-owned child generation";
            return false;
        }
    }
    auto identity = context.nodeIds.find(child);
    if (identity == context.nodeIds.end() || identity->second.generation != generation) {
        identity = context.nodeIds.insert_or_assign(
            child, CaptureContext::NodeIdentity{ generation, context.nextNodeId++ }).first;
    }
    node.nodeId = identity->second.nodeId;
    node.generation = generation;
    node.hwnd = child;
    return true;
}

// Geometry, styles, focusability, and text: the facets every projected control
// shares regardless of which adapter owns it.
bool CaptureCommonNodeFacets(
    HWND child,
    const ChildCaptureScope& scope,
    ControlNode& node,
    std::wstring& reason) {
    const HWND parent = GetParent(child);
    if (parent != scope.root) {
        const auto parentId = scope.visibleNodeIds->find(parent);
        if (parentId == scope.visibleNodeIds->end()) {
            reason = L"unsupported or hidden intermediate parent";
            return false;
        }
        node.parentNodeId = parentId->second;
    }
    node.controlId = GetDlgCtrlID(child);
    node.zIndex = scope.zIndex;
    RECT childRect{};
    GetWindowRect(child, &childRect);
    MapWindowPoints(nullptr, scope.root, reinterpret_cast<POINT*>(&childRect), 2);
    node.rect = childRect;
    node.style = static_cast<uint64_t>(GetWindowLongPtrW(child, GWL_STYLE));
    node.exStyle = static_cast<uint64_t>(GetWindowLongPtrW(child, GWL_EXSTYLE));
    node.visible = true;
    node.enabled = IsEffectivelyEnabled(scope.root, child);
    const bool nativeTabStop = (node.style & WS_TABSTOP) != 0;
    node.tabStop = nativeTabStop &&
        (!node.enabled || scope.tabIndexes->contains(child));
    if (const auto tab = scope.tabIndexes->find(child); tab != scope.tabIndexes->end()) {
        node.tabIndex = tab->second;
    }
    node.dialogCode = static_cast<uint32_t>(SendMessageW(child, WM_GETDLGCODE, 0, 0));
    // A multiline Edit legitimately wants all keys; anything else asking for raw
    // key routing implements private keyboard behavior we cannot project.
    const bool standardTextKeyboard =
        (node.kind == ControlKind::Edit || node.kind == ControlKind::Password) &&
        (static_cast<DWORD>(node.style) & ES_MULTILINE) != 0;
    if ((node.dialogCode & (DLGC_WANTALLKEYS | DLGC_WANTMESSAGE)) != 0 &&
        !standardTextKeyboard) {
        reason = L"control requires custom keyboard routing code=" +
            std::to_wstring(node.dialogCode);
        return false;
    }
    if (!WindowText(child, node.text, reason)) return false;
    // Never place a clear-text password in the UIA Name property.  The
    // PasswordBox still receives the canonical value through its typed view
    // model, while accessibility exposes only its role.
    node.automationName = node.kind == ControlKind::Password
        ? L"Password edit"
        : node.text;
    node.groupStart = (node.style & WS_GROUP) != 0;
    if (node.kind == ControlKind::DialogContainer) {
        node.tabStop = false;
        node.tabIndex = -1;
    }
    return true;
}

// Classify, identify, common facets, adapter-specific state.  Every stage
// rejects with its own reason and the caller appends the HWND evidence once.
bool CaptureChildNode(
    HWND child,
    CaptureContext& context,
    const ChildCaptureScope& scope,
    ControlNode& node,
    std::wstring& reason) {
    DWORD childProcess = 0;
    const DWORD childThread = GetWindowThreadProcessId(child, &childProcess);
    if (childProcess != GetCurrentProcessId() || childThread != scope.rootThread) {
        reason = L"foreign-process or foreign-thread child HWND";
        return false;
    }
    return ClassifyControl(child, node.kind, reason) &&
        AssignNodeIdentity(child, context, node, reason) &&
        CaptureCommonNodeFacets(child, scope, node, reason) &&
        CaptureControlDetail(child, node, reason);
}

bool CaptureChildNodes(
    HWND root,
    CaptureContext& context,
    WindowSnapshot& next,
    std::wstring& reason) {
    Enumeration enumeration;
    EnumChildWindows(root, CollectChild, reinterpret_cast<LPARAM>(&enumeration));
    if (enumeration.handles.size() > Ipc::kMaxNodes) {
        reason = L"window exceeds the 512 node limit";
        return false;
    }
    // Probe adapters first so an unsupported focusable class reports its real
    // support-boundary failure instead of a secondary dialog-navigation shape.
    const DWORD rootThread = GetWindowThreadProcessId(root, nullptr);
    for (const HWND child : enumeration.handles) {
        if (!IsWindowVisible(child) || IsCompositeImplementationChild(child)) continue;
        DWORD childProcess = 0;
        const DWORD childThread = GetWindowThreadProcessId(child, &childProcess);
        if (childProcess != GetCurrentProcessId() || childThread != rootThread) {
            reason = L"foreign-process or foreign-thread child HWND";
            AppendWindowEvidence(child, reason);
            return false;
        }
        ControlKind kind{};
        if (!ClassifyControl(child, kind, reason)) {
            AppendWindowEvidence(child, reason);
            return false;
        }
    }
    std::unordered_map<HWND, int> tabIndexes;
    if (!CaptureNativeTabOrder(root, enumeration.handles, tabIndexes, reason)) return false;

    std::unordered_map<HWND, uint64_t> visibleNodeIds;
    ChildCaptureScope scope;
    scope.root = root;
    scope.rootThread = rootThread;
    scope.tabIndexes = &tabIndexes;
    scope.visibleNodeIds = &visibleNodeIds;
    for (const HWND child : enumeration.handles) {
        if (!IsWindowVisible(child) || IsCompositeImplementationChild(child)) continue;
        ControlNode node;
        if (!CaptureChildNode(child, context, scope, node, reason)) {
            AppendWindowEvidence(child, reason);
            return false;
        }
        ++scope.zIndex;
        visibleNodeIds.emplace(child, node.nodeId);
        next.nodes.push_back(std::move(node));
    }
    return true;
}

} // namespace

bool CaptureTopLevelMenu(
    HWND root,
    std::vector<MenuItemSnapshot>& menu,
    std::wstring& rejectionReason) noexcept {
    try {
        menu.clear();
        const HMENU nativeMenu = GetMenu(root);
        if (!nativeMenu) return true;
        if (HasMdiClient(root) || nativeMenu == GetSystemMenu(root, FALSE)) {
            rejectionReason = L"MDI or system menu cannot be projected as a menu bar";
            return false;
        }
        MenuCaptureState state;
        return CaptureMenuLevel(nativeMenu, 1, L"", true, true,
            state, menu, rejectionReason) &&
            !menu.empty();
    } catch (...) {
        rejectionReason = L"exception while capturing native menu";
        return false;
    }
}

bool CaptureWindow(
    HWND root,
    CaptureContext& context,
    WindowSnapshot& snapshot,
    std::wstring& rejectionReason) noexcept {
    try {
        // WindowCapture and the renderer exchange physical pixel bounds, so the
        // whole pass runs in one explicit DPI context.
        PhysicalCoordinateScope dpiScope;
        if (!dpiScope.IsValid()) {
            rejectionReason = L"cannot establish physical-coordinate DPI context";
            return false;
        }
        if (!IsStandardTopLevel(root, rejectionReason)) {
            AppendWindowEvidence(root, rejectionReason);
            return false;
        }
        WindowSnapshot next;
        if (!CaptureTopLevelFacets(root, context, next, rejectionReason) ||
            !CaptureChildNodes(root, context, next, rejectionReason)) {
            return false;
        }
        // The snapshot is published only once every stage has accepted, so a
        // rejected capture can never leave a partial tree behind.
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
    HashString(hash, snapshot.adapterId);
    HashString(hash, snapshot.pageId);
    const auto hashMenu = [&](const auto& self,
                              const std::vector<MenuItemSnapshot>& items) -> void {
        HashBytes(hash, items.size());
        for (const auto& item : items) {
            HashString(hash, item.itemId);
            HashBytes(hash, item.kind);
            HashString(hash, item.text);
            HashBytes(hash, item.commandId);
            HashBytes(hash, item.enabled);
            HashBytes(hash, item.checked);
            HashBytes(hash, item.radio);
            HashBytes(hash, item.isDefault);
            self(self, item.items);
        }
    };
    hashMenu(hashMenu, snapshot.menu);
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
        HashBytes(hash, node.selectedIndices.size());
        for (const int index : node.selectedIndices) HashBytes(hash, index);
        HashBytes(hash, node.focusedIndex);
        HashBytes(hash, node.multiSelect);
        HashBytes(hash, node.selectionStart);
        HashBytes(hash, node.selectionLength);
        HashBytes(hash, node.readOnly);
        HashBytes(hash, node.multiline);
        HashBytes(hash, node.editable);
        HashBytes(hash, node.isDefault);
        HashBytes(hash, node.groupStart);
        HashBytes(hash, node.minimum);
        HashBytes(hash, node.maximum);
        HashBytes(hash, node.position);
        HashBytes(hash, node.indeterminate);
        HashBytes(hash, node.smallChange);
        HashBytes(hash, node.largeChange);
        HashBytes(hash, node.vertical);
        HashBytes(hash, node.reversed);
        HashBytes(hash, node.items.size());
        for (const auto& item : node.items) HashString(hash, item);
        HashBytes(hash, node.itemRects.size());
        for (const auto& rect : node.itemRects) HashBytes(hash, rect);
        HashBytes(hash, node.columns.size());
        for (const auto& column : node.columns) HashString(hash, column);
        HashBytes(hash, node.columnWidths.size());
        for (const int width : node.columnWidths) HashBytes(hash, width);
        HashBytes(hash, node.rows.size());
        for (const auto& row : node.rows) {
            HashBytes(hash, row.size());
            for (const auto& cell : row) HashString(hash, cell);
        }
        HashBytes(hash, node.columnHeadersVisible);
        HashBytes(hash, node.checkBoxes);
        HashBytes(hash, node.checkedIndices.size());
        for (const int index : node.checkedIndices) HashBytes(hash, index);
        HashBytes(hash, node.itemDepths.size());
        for (const int depth : node.itemDepths) HashBytes(hash, depth);
        HashBytes(hash, node.itemExpanded.size());
        for (const bool expanded : node.itemExpanded) HashBytes(hash, expanded);
        HashBytes(hash, node.imageWidth);
        HashBytes(hash, node.imageHeight);
        HashString(hash, node.imageFormat);
        HashBytes(hash, node.imageData.size());
        if (!node.imageData.empty()) HashRange(hash, node.imageData.data(), node.imageData.size());
        HashBytes(hash, node.toolbarItems.size());
        for (const auto& item : node.toolbarItems) {
            HashBytes(hash, item.kind);
            HashBytes(hash, item.commandId);
            HashBytes(hash, item.rect);
            HashString(hash, item.text);
            HashBytes(hash, item.enabled);
            HashBytes(hash, item.hidden);
            HashBytes(hash, item.imageWidth);
            HashBytes(hash, item.imageHeight);
            HashString(hash, item.imageFormat);
            HashBytes(hash, item.imageData.size());
            if (!item.imageData.empty()) HashRange(hash, item.imageData.data(), item.imageData.size());
        }
        HashString(hash, node.adapterId);
        HashString(hash, node.pageId);
        HashString(hash, node.semanticKey);
        HashString(hash, node.sourceKind);
        HashString(hash, node.presentationVariant);
        HashBytes(hash, node.supportedActions.size());
        for (const auto& action : node.supportedActions) HashString(hash, action);
        HashString(hash, node.helpText);
        HashString(hash, node.accessKey);
    }
    return hash;
}

} // namespace FluentShell::Bridge::Translation
