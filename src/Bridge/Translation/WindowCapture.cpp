#include "WindowCapture.h"
#include "CaptureTrace.h"
#include "GuiThreadDispatch.h"
#include "NativeDocument.h"

#include "../../Common/FluentShell.h"
#include "ControlAdapters.h"
#include "MenuBarCapture.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdlib>
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
    // An owned top-level travels only when the application is running it modally
    // against an owner that is on screen: the proxy can then inherit that owner and
    // block it, which is the same contract a translated MessageBox already meets.
    // A modeless owned window has no such contract, and cloaking its owner would
    // strand it behind an unrelated renderer HWND.
    if (EffectiveTopLevelOwner(root) != nullptr && !IsModalOwnedTopLevel(root)) {
        reason = L"modeless owned top-level window is outside the v1 owner graph";
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

// A child dialog that is not part of its parent's dialog-manager walk -- no DS_CONTROL, no
// WS_EX_CONTROLPARENT, as with the pages of an OLE property frame -- runs its own IsDialogMessage loop:
// Tab cycles inside it, and its controls are unreachable from the parent's walk.
bool IsIndependentDialogPage(HWND window) noexcept {
    wchar_t classBuffer[kMaxClassNameChars]{};
    return FluentShell::EqualsIgnoreCase(ClassNameOf(window, classBuffer), L"#32770") &&
        (static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)) & WS_CHILD) != 0 &&
        !IsDialogControlParent(window);
}

bool IsMdiChildWindow(HWND window) noexcept {
    return (static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)) &
        WS_EX_MDICHILD) != 0;
}

bool IsMdiNavigationContainer(HWND window) noexcept {
    if (IsMdiChildWindow(window)) return true;
    wchar_t classBuffer[kMaxClassNameChars]{};
    return FluentShell::EqualsIgnoreCase(ClassNameOf(window, classBuffer), L"MDIClient");
}

HWND NativeTabScope(HWND root, HWND child) noexcept {
    for (HWND parent = GetParent(child); parent && parent != root; parent = GetParent(parent)) {
        if (IsMdiChildWindow(parent) || IsIndependentDialogPage(parent)) return parent;
    }
    return root;
}

bool CaptureNativeTabOrder(
    HWND root,
    const std::vector<HWND>& handles,
    std::unordered_map<HWND, int>& tabIndexes,
    std::wstring& reason) {
    std::unordered_map<HWND, HWND> candidates;
    for (const HWND child : handles) {
        auto candidate = std::make_shared<HWND>(nullptr);
        if (!DispatchToWindowThread(child, [root, child, candidate] {
                if (IsCompositeImplementationChild(child) || IsMdiNavigationContainer(child)) return;
                const auto style = static_cast<DWORD>(GetWindowLongPtrW(child, GWL_STYLE));
                if (!IsDialogControlParent(child) && !IsIndependentDialogPage(child) && IsWindowVisible(child) &&
                    IsEffectivelyEnabled(root, child) && (style & WS_TABSTOP) != 0)
                    *candidate = NativeTabScope(root, child);
            }, reason)) return false;
        if (*candidate) candidates.emplace(child, *candidate);
    }
    if (candidates.empty()) return true;

    // Dialog-manager traversal is per container: an MDI child frame runs its own
    // IsDialogMessage loop, so the controls inside it are not reachable from the
    // frame's walk, and neither are the controls of a child dialog that is no
    // control parent (an OLE property page). Each control belongs to its nearest
    // such container, or the root when it has none. Even a control-parent style
    // that exposes an MDI subtree to the root's walk must not assign that
    // subtree the root's tab order.
    std::vector<HWND> containers{ root };
    for (const HWND child : handles) {
        if ((IsMdiChildWindow(child) || IsIndependentDialogPage(child)) && IsWindowVisible(child)) {
            containers.push_back(child);
        }
    }

    bool seeded = false;
    for (const HWND container : containers) {
        const auto seed = std::find_if(handles.begin(), handles.end(), [container](HWND child) {
            return GetParent(child) == container;
        });
        if (seed == handles.end()) continue;
        seeded = true;
        // GetNextDlgTabItem is the native dialog manager's source of truth. A raw
        // WS_TABSTOP that this walk omits is not an effective dialog tab stop; keep
        // its style as evidence, but do not invent an order from HWND enumeration.
        std::unordered_set<HWND> visited;
        // A null start answers the container's first tab stop, so the order begins where the native one
        // does even when the container's first child is itself a tab stop (a walk begun at that child
        // would number it last).
        HWND current = nullptr;
        for (size_t attempt = 0; attempt <= handles.size() + candidates.size(); ++attempt) {
            auto nextValue = std::make_shared<HWND>(nullptr);
            if (!DispatchToWindowThread(container, [container, current, nextValue] {
                    *nextValue = GetNextDlgTabItem(container, current, FALSE);
                }, reason)) return false;
            const HWND next = *nextValue;
            if (!next) break;
            if (!IsChild(container, next)) {
                reason = L"native dialog manager returned a tab stop outside its container";
                return false;
            }
            if (!visited.insert(next).second) break;
            if (const auto candidate = candidates.find(next);
                candidate != candidates.end() && candidate->second == container) {
                tabIndexes.emplace(next, static_cast<int>(tabIndexes.size()));
            }
            current = next;
        }
    }
    if (!seeded) {
        reason = L"native dialog manager has no direct child traversal seed";
        return false;
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
    bool requireUniqueCommands = true;
};

bool CaptureMenuLevel(
    HMENU menu,
    size_t depth,
    std::wstring_view path,
    bool topLevel,
    bool ancestorEnabled,
    bool mdiFrame,
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
        // A maximized MDI child makes DefFrameProc insert its own window chrome
        // into the frame's menu bar: the child's icon plus minimize, restore, and
        // close bitmaps.  That chrome is the same contract the projected MDI
        // child's caption already offers, so a bitmap-backed top-level item on an
        // MDI frame is skipped instead of being projected twice or refusing the
        // frame.  Textual items still go through every ordinary rule below.
        if (mdiFrame && topLevel &&
            (info.hbmpItem != nullptr || (info.fType & MFT_BITMAP) != 0)) {
            continue;
        }
        const UINT unsupportedType = MFT_OWNERDRAW | MFT_BITMAP | MFT_MENUBARBREAK |
            MFT_MENUBREAK | MFT_RIGHTJUSTIFY;
        // `dwItemData` is application-private storage the menu never interprets: the
        // projection posts the same WM_COMMAND a real selection produces and the
        // application looks up its own data exactly as it always does.  An icon beside a
        // label is decoration on top of the label, which is the item's meaning, so a
        // string item that also carries a bitmap is projected from its string.  What is
        // still refused is an item with no readable label at all -- owner-draw, or a
        // bitmap standing in place of text -- and a break that changes the menu's shape.
        if ((info.fType & unsupportedType) != 0) {
            wchar_t evidence[192]{};
            swprintf_s(evidence,
                L"menu item type 0x%08X has no projectable label ('%.32s')",
                static_cast<unsigned>(info.fType), text.c_str());
            reason = evidence;
            return false;
        }
        if (text.empty() && (info.fType & MFT_SEPARATOR) == 0) {
            reason = L"menu item carries no label";
            return false;
        }
        MenuItemSnapshot item;
        // Identity is the item's position in the *projected* level, not in the
        // native one: MDI window chrome is skipped above, and a projected path has
        // to stay contiguous for the renderer to admit it.  Nothing resolves a
        // click through this path -- menu commands travel as their WM_COMMAND ID.
        item.itemId = path.empty()
            ? std::to_wstring(result.size())
            : std::wstring(path) + L"." + std::to_wstring(result.size());
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
                    false, item.enabled, mdiFrame, state, item.items, reason) ||
                item.items.empty()) return false;
        } else {
            if (topLevel || item.text.empty() || info.wID == 0 || IsSystemCommandId(info.wID)) {
                wchar_t evidence[192]{};
                swprintf_s(evidence,
                    L"menu command '%.32s' id=%u cannot use standard WM_COMMAND semantics",
                    item.text.c_str(), static_cast<unsigned>(info.wID));
                reason = evidence;
                return false;
            }
            item.kind = MenuItemKind::Command;
            item.commandId = info.wID;
            if (state.requireUniqueCommands && !state.commandIds.insert(item.commandId).second) {
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
    // The kind of every node already captured, so a child can be checked against the
    // parent that will own it in the projection.
    const std::unordered_map<HWND, ControlKind>* visibleNodeKinds = nullptr;
    // The next tab index after the dialog manager's walk, for a Windows Forms tab stop
    // the walk does not reach (WinForms moves focus with Tab itself).
    int* nextExtraTabIndex = nullptr;
    int zIndex = 0;
};

bool CaptureTopLevelFacets(
    HWND root,
    const CaptureContext& context,
    WindowSnapshot& next,
    std::wstring& reason) {
    next.surfaceId = context.surfaceId;
    next.surfaceKind = SurfaceKind::Window;
    // Modality is not a style bit: the dialog manager states it by disabling the
    // owner for the dialog's lifetime, and that is what the projection has to
    // reproduce on the owner's proxy.
    next.modal = IsModalOwnedTopLevel(root);
    // Escape cancels a dialog because the dialog manager maps it to IDCANCEL; an
    // ordinary top-level window receives the keystroke and normally ignores it.
    // Reporting cancel semantics for every window would make Escape close
    // applications that never close on Escape.  A Windows Forms dialog (Event Viewer's
    // filter, Task Scheduler's task properties) is cancelled by Escape through its
    // CancelButton, which no window evidence names; it is a dialog when it is modal or
    // has the dialog frame (WS_EX_DLGMODALFRAME: FormBorderStyle.FixedDialog), and the
    // dialog it nearly always is closes the same way the form's own close button does.
    {
        wchar_t rootClass[kMaxClassNameChars]{};
        GetClassNameW(root, rootClass, static_cast<int>(std::size(rootClass)));
        const bool dialogFrame = (static_cast<DWORD>(GetWindowLongPtrW(root, GWL_EXSTYLE)) & WS_EX_DLGMODALFRAME) != 0;
        next.canCancel = FluentShell::EqualsIgnoreCase(rootClass, L"#32770") ||
            ((next.modal || dialogFrame) && std::wstring_view(rootClass).starts_with(L"WindowsForms10."));
    }
    next.generation = context.generation;
    next.revision = context.revision;
    next.nativeHwnd = root;
    // The protocol field carries the owner relationship the proxy has to
    // reproduce, so it names the owner that can actually hold the screen.  A
    // window whose whole owner chain is hidden projects as an unowned window.
    next.ownerHwnd = EffectiveTopLevelOwner(root);
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
    // Taskbar presence follows the shell's own rule, which reads the raw owner:
    // a hidden owner still suppresses the button unless WS_EX_APPWINDOW asks for
    // one.  This is deliberately not the effective owner above.
    next.showInTaskbar = GetWindow(root, GW_OWNER) == nullptr ||
        (next.windowExStyle & WS_EX_APPWINDOW) != 0;
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
        // The projection places a child inside its parent's own element, which only
        // exists for a container kind.  A control nested inside anything else would be
        // described by a snapshot the renderer cannot lay out, so it is refused here
        // with the evidence rather than faulting the surface across the wire.
        const auto parentKind = scope.visibleNodeKinds->find(parent);
        if (parentKind == scope.visibleNodeKinds->end() ||
            !IsProjectedContainerKind(parentKind->second)) {
            reason = L"visible control is nested inside " +
                std::wstring(parentKind == scope.visibleNodeKinds->end()
                    ? L"an uncaptured window" : ControlKindName(parentKind->second)) +
                L", which frames no child nodes";
            return false;
        }
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
        (!node.enabled || scope.tabIndexes->contains(child) || IsWindowsFormsControl(child));
    if (const auto tab = scope.tabIndexes->find(child); tab != scope.tabIndexes->end()) {
        node.tabIndex = tab->second;
    } else if (node.tabStop && node.enabled && scope.nextExtraTabIndex) {
        // Windows Forms traversal follows the walk's controls, in capture order.
        node.tabIndex = (*scope.nextExtraTabIndex)++;
    }
    node.dialogCode = static_cast<uint32_t>(SendMessageW(child, WM_GETDLGCODE, 0, 0));
    // A multiline Edit legitimately wants all keys; anything else asking for raw
    // key routing implements private keyboard behavior we cannot project.
    const bool standardTextKeyboard =
        (node.kind == ControlKind::Edit || node.kind == ControlKind::Password ||
         node.kind == ControlKind::RichText) &&
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
    // WS_MAXIMIZEBOX aliases WS_TABSTOP. On an MDI frame that bit describes its
    // caption, including while a modal dialog disables the owner; it must never
    // turn into a disabled dialog tab stop. Keep the raw style for caption
    // commands, and keep both structural MDI kinds outside dialog traversal.
    // A progress bar takes no keyboard input; a WS_TABSTOP a dialog template gave it
    // would only make Tab land on nothing.  An inert painted surface is the same: a
    // focusable header strip that publishes no action has nothing for focus to act on.
    if (node.kind == ControlKind::DialogContainer || node.kind == ControlKind::ProgressBar ||
        node.kind == ControlKind::MdiClient || node.kind == ControlKind::MdiChild ||
        node.kind == ControlKind::StaticBitmap || node.kind == ControlKind::GroupBox) {
        node.tabStop = false;
        node.tabIndex = -1;
    }
    // A container frames other windows; the projection puts focus on the panes
    // inside it, and its splitters are reached by the pointer or by their own
    // keyboard affordance rather than by dialog traversal.  A container whose children
    // tile it has nowhere to paint text, so whatever text it carries is a name for the
    // region rather than content: it becomes the pane's accessible name.
    if (node.kind == ControlKind::PaneContainer) {
        node.tabStop = false;
        node.tabIndex = -1;
        node.automationName = node.text;
        node.text.clear();
    }
    // An accessible island's elements are not focusable natively either -- the
    // provider reports IsKeyboardFocusable false for them -- so the island itself is
    // not a dialog traversal stop; its projected items carry their own focus.
    if (node.kind == ControlKind::AccessibleIsland) {
        node.tabStop = false;
        node.tabIndex = -1;
        node.automationName = node.text;
        node.text.clear();
    }
    return true;
}

void TraceCaptureStage(HWND child, const wchar_t* stage) noexcept {
    if (!CaptureTraceEnabled()) return;
    try {
        wchar_t className[64]{};
        GetClassNameW(child, className, static_cast<int>(std::size(className)));
        CaptureTrace(std::wstring(stage) + L" hwnd=" + Ipc::HwndToString(child) + L" class=" + className);
    } catch (...) {
    }
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
    if (childProcess != GetCurrentProcessId()) {
        reason = L"foreign-process child HWND";
        return false;
    }
    if (childThread != GetCurrentThreadId()) {
        struct RemoteCapture final {
            CaptureContext context;
            ChildCaptureScope scope;
            std::unordered_map<HWND, int> tabs;
            std::unordered_map<HWND, uint64_t> ids;
            std::unordered_map<HWND, ControlKind> kinds;
            ControlNode node;
            std::wstring error;
            bool success = false;
        };
        auto work = std::make_shared<RemoteCapture>();
        work->context = context;
        work->scope = scope;
        work->tabs = *scope.tabIndexes;
        work->ids = *scope.visibleNodeIds;
        work->kinds = *scope.visibleNodeKinds;
        work->scope.tabIndexes = &work->tabs;
        work->scope.visibleNodeIds = &work->ids;
        work->scope.visibleNodeKinds = &work->kinds;
        if (!DispatchToWindowThread(child, [child, work] {
                work->success = CaptureChildNode(child, work->context, work->scope, work->node, work->error);
            }, reason)) return false;
        if (!work->success) { reason = std::move(work->error); return false; }
        context.nodeIds = std::move(work->context.nodeIds);
        context.nextNodeId = work->context.nextNodeId;
        context.nextNodeGeneration = work->context.nextNodeGeneration;
        node = std::move(work->node);
        return true;
    }
    TraceCaptureStage(child, L"classify");
    if (!ClassifyControl(child, node.kind, reason)) return false;
    TraceCaptureStage(child, L"identity");
    if (!AssignNodeIdentity(child, context, node, reason)) return false;
    TraceCaptureStage(child, L"facets");
    if (!CaptureCommonNodeFacets(child, scope, node, reason)) return false;
    TraceCaptureStage(child, L"detail");
    return CaptureControlDetail(child, node, reason);
}

// A WS_VISIBLE child draws nothing when its ancestors' client areas clip it away
// entirely -- a collapsed pane, a control parked outside its parent.  It is skipped
// like a hidden one, and its own children are clipped with it.  A minimized root has
// no client area at all, so clipping is not evaluated against it.
bool DrawsInsideRoot(HWND root, HWND child) noexcept {
    if (IsIconic(root)) return true;
    RECT visible{};
    if (!GetWindowRect(child, &visible)) return false;
    for (HWND ancestor = GetParent(child); ancestor; ancestor = GetParent(ancestor)) {
        RECT client{};
        POINT origin{ 0, 0 };
        if (!GetClientRect(ancestor, &client) || !ClientToScreen(ancestor, &origin)) return false;
        OffsetRect(&client, origin.x, origin.y);
        if (!IntersectRect(&visible, &visible, &client)) return false;
        if (ancestor == root) return true;
    }
    return false;
}

// True when a WS_CLIPSIBLINGS child lies entirely under one visible sibling that is
// above it in z-order: it cannot paint over that sibling, so nothing of it shows.
bool CoveredBySiblingAbove(HWND child) noexcept {
    if ((GetWindowLongPtrW(child, GWL_STYLE) & WS_CLIPSIBLINGS) == 0) return false;
    RECT own{};
    if (!GetWindowRect(child, &own)) return false;
    for (HWND above = GetWindow(child, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
        RECT covering{};
        RECT joined{};
        if (!IsWindowVisible(above) || !GetWindowRect(above, &covering)) continue;
        if ((GetWindowLongPtrW(above, GWL_EXSTYLE) & (WS_EX_TRANSPARENT | WS_EX_LAYERED)) != 0)
            continue;
        if (UnionRect(&joined, &covering, &own) && EqualRect(&joined, &covering)) return true;
    }
    return false;
}

// A wizard-mode property sheet keeps its tab control visible but shows no tab: the
// page above covers the strip, and under comctl32 v6 every item rectangle is empty
// as well.  It draws nothing a projection could reproduce.  A tab control with real,
// uncovered tabs keeps the ordinary contract.
bool IsHiddenTabStrip(HWND child) noexcept {
    wchar_t className[32]{};
    if (GetClassNameW(child, className, 32) <= 0 ||
        !FluentShell::EqualsIgnoreCase(className, WC_TABCONTROLW)) return false;
    if (CoveredBySiblingAbove(child)) return true;
    const auto count = static_cast<int>(SendMessageW(child, TCM_GETITEMCOUNT, 0, 0));
    if (count <= 0) return false;
    for (int index = 0; index < count; ++index) {
        RECT item{};
        if (SendMessageW(child, TCM_GETITEMRECT, index, reinterpret_cast<LPARAM>(&item)) &&
            !IsRectEmpty(&item)) return false;
    }
    return true;
}

bool CapturedChild(HWND root, HWND child) noexcept {
    return IsWindowVisible(child) && !IsCompositeImplementationChild(child) &&
        DrawsInsideRoot(root, child) && !IsHiddenTabStrip(child);
}

// The outermost window between `child` and `root` that a sibling above covers but for
// its edges, or null.
HWND OutermostObscuredHost(HWND root, HWND child) noexcept {
    HWND host = nullptr;
    for (HWND current = child; current && current != root; current = GetParent(current)) {
        if (ObscuredBySiblingAbove(current)) host = current;
    }
    return host;
}

bool ProbeChildOnOwner(HWND root, HWND child, bool& kept, ControlKind& kind, std::wstring& reason) {
    struct Probe final { bool kept = false; bool valid = true; ControlKind kind{}; std::wstring error; };
    auto probe = std::make_shared<Probe>();
    if (!DispatchToWindowThread(child, [root, child, probe] {
            probe->kept = CapturedChild(root, child);
            if (probe->kept) probe->valid = ClassifyControl(child, probe->kind, probe->error);
        }, reason)) return false;
    kept = probe->kept;
    kind = probe->kind;
    reason = std::move(probe->error);
    return probe->valid;
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
    // Every rejection in the tree is logged, not only the first: a window with
    // several unsupported children otherwise takes one rebuild per blocker to
    // enumerate.
    const DWORD rootThread = GetWindowThreadProcessId(root, nullptr);
    std::wstring firstRejection;
    size_t rejections = 0;
    std::unordered_set<HWND> keptChildren;
    std::unordered_set<HWND> droppedHosts;
    std::vector<HWND> nativeDocuments;
    for (const HWND child : enumeration.handles) {
        DWORD childProcess = 0;
        const DWORD childThread = GetWindowThreadProcessId(child, &childProcess);
        if (!childThread || childProcess != GetCurrentProcessId()) {
            reason = L"destroyed or foreign-process child HWND";
            AppendWindowEvidence(child, reason);
            return false;
        }
        ControlKind kind{};
        std::wstring childReason;
        bool kept = false;
        bool documentChild = false;
        for (const HWND document : nativeDocuments) {
            if (!IsChild(document, child)) continue;
            auto owned = std::make_shared<bool>(false);
            if (!DispatchToWindowThread(document, [document, child, owned] {
                    *owned = NativeDocumentOwnsDescendant(document, child);
                }, childReason)) { reason = std::move(childReason); return false; }
            if (*owned) { documentChild = true; break; }
        }
        if (documentChild) continue;
        if (ProbeChildOnOwner(root, child, kept, kind, childReason)) {
            if (kept) {
                keptChildren.insert(child);
                if (kind == ControlKind::NativeDocument) nativeDocuments.push_back(child);
            }
            continue;
        }
        // What the capture cannot project inside a host a sibling covers but for its
        // edges is never on screen; the whole host is left out instead of the window.
        // A covered host whose content is supported keeps its nodes, since a covered
        // control is still reachable from the keyboard.
        if (const HWND host = OutermostObscuredHost(root, child)) {
            droppedHosts.insert(host);
            continue;
        }
        AppendWindowEvidence(child, childReason);
        if (rejections++ < 8) {
            try {
                FluentShell::Log(L"Capture rejected child: " + childReason);
            } catch (...) {}
        }
        if (firstRejection.empty()) firstRejection = childReason;
    }
    if (!firstRejection.empty()) {
        reason = std::move(firstRejection);
        return false;
    }
    if (!droppedHosts.empty()) {
        std::erase_if(keptChildren, [&](HWND child) {
            return std::any_of(droppedHosts.begin(), droppedHosts.end(), [child](HWND host) {
                return child == host || IsChild(host, child);
            });
        });
    }
    std::unordered_map<HWND, int> tabIndexes;
    if (!CaptureNativeTabOrder(root, enumeration.handles, tabIndexes, reason)) return false;

    std::unordered_map<HWND, uint64_t> visibleNodeIds;
    std::unordered_map<HWND, ControlKind> visibleNodeKinds;
    ChildCaptureScope scope;
    scope.root = root;
    scope.rootThread = rootThread;
    scope.tabIndexes = &tabIndexes;
    scope.visibleNodeIds = &visibleNodeIds;
    scope.visibleNodeKinds = &visibleNodeKinds;
    int nextExtraTabIndex = static_cast<int>(tabIndexes.size());
    scope.nextExtraTabIndex = &nextExtraTabIndex;
    for (const HWND child : enumeration.handles) {
        if (!keptChildren.contains(child)) continue;
        // The menu-bar toolbar is projected as the surface's menu, so it must not also
        // appear as a control: one native menu bar becomes one projected menu bar.
        if (child == context.menuBarToolbar && !context.menuBarToolbarMenu.empty()) continue;
        ControlNode node;
        if (!CaptureChildNode(child, context, scope, node, reason)) {
            AppendWindowEvidence(child, reason);
            return false;
        }
        ++scope.zIndex;
        visibleNodeIds.emplace(child, node.nodeId);
        visibleNodeKinds.emplace(child, node.kind);
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
        if (nativeMenu == GetSystemMenu(root, FALSE)) {
            rejectionReason = L"system menu cannot be projected as a menu bar";
            return false;
        }
        MenuCaptureState state;
        if (!CaptureMenuLevel(nativeMenu, 1, L"", true, true,
                HasMdiClient(root), state, menu, rejectionReason)) return false;
        if (menu.empty()) {
            rejectionReason = L"native menu bar has no projectable items";
            return false;
        }
        return true;
    } catch (...) {
        rejectionReason = L"exception while capturing native menu";
        return false;
    }
}

bool CaptureMenuHandle(
    HMENU menu,
    std::wstring_view itemIdPath,
    std::vector<MenuItemSnapshot>& items,
    std::wstring& rejectionReason,
    bool requireUniqueCommands) noexcept {
    try {
        items.clear();
        if (!menu || !IsMenu(menu)) {
            rejectionReason = L"menu handle is not a menu";
            return false;
        }
        MenuCaptureState state;
        state.requireUniqueCommands = requireUniqueCommands;
        // Depth 1 with topLevel false: these are the items of a popup, not the bar.
        return CaptureMenuLevel(menu, 1, itemIdPath, false, true, false,
            state, items, rejectionReason);
    } catch (...) {
        rejectionReason = L"exception while capturing a menu handle";
        return false;
    }
}

void ObserveMenuBarToolbar(HWND root, CaptureContext& context) noexcept {
    try {
        HWND toolbar = FindMenuBarToolbar(root);
        ControlNode identity;
        std::wstring reason;
        std::vector<MenuBarButton> buttons;
        if (toolbar) {
            struct Observation final {
                CaptureContext context;
                ControlNode identity;
                std::vector<MenuBarButton> buttons;
                bool success = false;
            };
            auto observed = std::make_shared<Observation>();
            observed->context = context;
            if (!DispatchToWindowThread(toolbar, [toolbar, observed] {
                    std::wstring error;
                    observed->success = AssignNodeIdentity(toolbar, observed->context,
                        observed->identity, error) && ReadMenuBarButtons(toolbar, observed->buttons);
                }, reason)) toolbar = nullptr;
            else {
                context.nodeIds = std::move(observed->context.nodeIds);
                context.nextNodeId = observed->context.nextNodeId;
                context.nextNodeGeneration = observed->context.nextNodeGeneration;
                if (!observed->success) toolbar = nullptr;
                else {
                    identity = std::move(observed->identity);
                    buttons = std::move(observed->buttons);
                }
            }
        }
        if (!toolbar) {
            identity.generation = 0;
            buttons.clear();
        }
        if (toolbar == context.menuBarToolbar &&
            identity.generation == context.menuBarToolbarGeneration &&
            buttons == context.menuBarButtons) return;
        context.menuBarToolbar = toolbar;
        context.menuBarToolbarGeneration = identity.generation;
        context.menuBarButtons = std::move(buttons);
        context.menuBarToolbarMenu.clear();
        context.menuBarToolbarCommands.clear();
        context.menuBarSilentButtons.clear();
        context.menuBarRefresh.Invalidate(GetTickCount64());
    } catch (...) {
        context.menuBarToolbar = nullptr;
        context.menuBarToolbarGeneration = 0;
        context.menuBarButtons.clear();
        context.menuBarToolbarMenu.clear();
        context.menuBarToolbarCommands.clear();
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
        if (!CaptureTopLevelFacets(root, context, next, rejectionReason)) return false;
        // Observation is cheap and never opens a popup. The post-commit source-thread
        // command refreshes the contents only after meaningful application changes.
        ObserveMenuBarToolbar(root, context);
        if (context.menuBarToolbar) {
            if (GetMenu(root)) {
                rejectionReason =
                    L"window has both an HMENU menu bar and a menu-bar toolbar";
                return false;
            }
            next.menu = context.menuBarToolbarMenu;
            next.menuBindingGeneration = context.menuBarBindingGeneration;
        }
        if (!CaptureChildNodes(root, context, next, rejectionReason)) {
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

std::wstring DescribeSnapshotDifference(const WindowSnapshot& before, const WindowSnapshot& after) {
    if (before.nodes.size() != after.nodes.size())
        return L"node count " + std::to_wstring(before.nodes.size()) + L" -> " +
            std::to_wstring(after.nodes.size());
    const auto rectText = [](const RECT& rect) {
        return std::to_wstring(rect.left) + L"," + std::to_wstring(rect.top) + L"," +
            std::to_wstring(rect.right) + L"," + std::to_wstring(rect.bottom);
    };
    for (size_t index = 0; index < before.nodes.size(); ++index) {
        const auto& a = before.nodes[index];
        const auto& b = after.nodes[index];
        std::wstring field;
        if (a.hwnd != b.hwnd || a.kind != b.kind) field = L"identity";
        else if (!EqualRect(&a.rect, &b.rect)) field = L"rect " + rectText(a.rect) + L" -> " + rectText(b.rect);
        else if (a.text != b.text) field = L"text";
        else if (a.visible != b.visible || a.enabled != b.enabled) field = L"visibility/enabled";
        else if (a.checked != b.checked || a.selectedIndex != b.selectedIndex ||
                 a.focusedIndex != b.focusedIndex || a.position != b.position) field = L"state";
        else if (a.items != b.items || a.itemRects.size() != b.itemRects.size()) field = L"items";
        else if (a.chromeRegions.size() != b.chromeRegions.size()) field = L"chrome region count";
        else {
            for (size_t region = 0; region < a.chromeRegions.size() && field.empty(); ++region) {
                const auto& before = a.chromeRegions[region].imageData;
                const auto& after = b.chromeRegions[region].imageData;
                if (before == after) continue;
                // How much moved: a few levels on a few bytes is render noise, a whole glyph is
                // an animation.
                size_t differing = 0;
                int largest = 0;
                const size_t common = (std::min)(before.size(), after.size());
                for (size_t byte = 0; byte < common; ++byte) {
                    const int delta = std::abs(static_cast<int>(before[byte]) - static_cast<int>(after[byte]));
                    if (delta != 0) ++differing;
                    largest = (std::max)(largest, delta);
                }
                field = L"chrome pixels " + rectText(a.chromeRegions[region].rect) + L" (" +
                    std::to_wstring(differing) + L" of " + std::to_wstring(common) +
                    L" bytes differ, largest step " + std::to_wstring(largest) + L")";
            }
            for (size_t bar = 0; bar < a.scrollBars.size() && bar < b.scrollBars.size() && field.empty(); ++bar)
                if (a.scrollBars[bar].position != b.scrollBars[bar].position ||
                    a.scrollBars[bar].maximum != b.scrollBars[bar].maximum)
                    field = L"scroll bar";
            if (field.empty() && a.imageData != b.imageData) field = L"image pixels";
        }
        if (!field.empty()) {
            wchar_t className[64]{};
            GetClassNameW(a.hwnd, className, 64);
            return L"node " + std::to_wstring(index) + L" (" + className + L") " + field;
        }
    }
    return {};
}

uint64_t SnapshotFingerprint(const WindowSnapshot& snapshot) noexcept {
    uint64_t hash = 1469598103934665603ull;
    const auto hashImage = [&](const ImageListEntry& image) {
        HashBytes(hash, image.imageWidth);
        HashBytes(hash, image.imageHeight);
        HashString(hash, image.imageFormat);
        HashBytes(hash, image.imageData.size());
        if (!image.imageData.empty()) HashRange(hash, image.imageData.data(), image.imageData.size());
    };
    const auto hashOptionalImage = [&](const std::optional<ImageListEntry>& image) {
        HashBytes(hash, image.has_value());
        if (image) hashImage(*image);
    };
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
    HashBytes(hash, snapshot.menuBindingGeneration);
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
    HashBytes(hash, snapshot.popupMenu.has_value());
    if (snapshot.popupMenu) {
        HashBytes(hash, snapshot.popupMenu->popupId);
        HashBytes(hash, snapshot.popupMenu->nodeId);
        HashBytes(hash, snapshot.popupMenu->itemIndex);
        hashMenu(hashMenu, snapshot.popupMenu->items);
    }
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
        HashBytes(hash, node.active);
        HashString(hash, node.windowState);
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
        HashString(hash, node.listViewMode);
        HashBytes(hash, node.groupViewEnabled);
        HashBytes(hash, node.listViewGroups.size());
        for (const auto& group : node.listViewGroups) {
            HashBytes(hash, group.groupId);
            HashString(hash, group.header);
            HashString(hash, group.footer);
            HashString(hash, group.headerAlignment);
            HashString(hash, group.footerAlignment);
            HashBytes(hash, group.collapsible);
            HashBytes(hash, group.collapsed);
            HashBytes(hash, group.hidden);
            HashBytes(hash, group.headerRect);
            HashBytes(hash, group.footerRect);
        }
        HashBytes(hash, node.itemGroupIds.size());
        for (const int groupId : node.itemGroupIds) HashBytes(hash, groupId);
        HashBytes(hash, node.ownerDrawCombo.has_value());
        if (node.ownerDrawCombo) {
            HashString(hash, node.ownerDrawCombo->mode);
            HashBytes(hash, node.ownerDrawCombo->items.size());
            for (const auto& item : node.ownerDrawCombo->items) {
                hashImage(item.normal);
                hashImage(item.selected);
            }
            hashOptionalImage(node.ownerDrawCombo->selectionFace);
        }
        HashBytes(hash, node.document.has_value());
        if (node.document) {
            HashString(hash, node.document->documentId);
            HashBytes(hash, node.document->regions.size());
            for (const auto& region : node.document->regions) {
                HashString(hash, region.key);
                HashString(hash, region.kind);
                HashBytes(hash, region.rect);
                HashBytes(hash, region.contentWidth);
                HashBytes(hash, region.contentHeight);
                HashBytes(hash, region.maximumX);
                HashBytes(hash, region.maximumY);
                HashBytes(hash, region.offsetX);
                HashBytes(hash, region.offsetY);
            }
            HashBytes(hash, node.document->items.size());
            for (const auto& item : node.document->items) {
                HashString(hash, item.key);
                HashString(hash, item.kind);
                HashBytes(hash, item.rect);
                HashBytes(hash, item.regionIndex);
                HashBytes(hash, item.reportRegionIndex);
                HashString(hash, item.name);
                HashString(hash, item.actionIdentity);
                HashBytes(hash, item.enabled);
                hashOptionalImage(item.image);
                HashBytes(hash, item.reportRows.size());
                for (const auto& row : item.reportRows) {
                    HashString(hash, row.path);
                    HashBytes(hash, row.value);
                    HashBytes(hash, row.available);
                }
            }
        }
        HashBytes(hash, node.itemRects.size());
        for (const auto& rect : node.itemRects) HashBytes(hash, rect);
        HashBytes(hash, node.itemNativeIds.size());
        for (const uint32_t nativeId : node.itemNativeIds) HashBytes(hash, nativeId);
        HashBytes(hash, node.itemActivationSupported);
        HashBytes(hash, node.itemDefaultActivation);
        HashBytes(hash, node.columns.size());
        for (const auto& column : node.columns) HashString(hash, column);
        HashBytes(hash, node.columnWidths.size());
        for (const int width : node.columnWidths) HashBytes(hash, width);
        HashBytes(hash, node.columnOrder.size());
        for (const int logical : node.columnOrder) HashBytes(hash, logical);
        HashBytes(hash, node.rows.size());
        for (const auto& row : node.rows) {
            HashBytes(hash, row.size());
            for (const auto& cell : row) HashString(hash, cell);
        }
        HashBytes(hash, node.itemCount);
        HashBytes(hash, node.itemWindowStart);
        HashBytes(hash, node.columnHeadersVisible);
        HashBytes(hash, node.checkBoxes);
        HashBytes(hash, node.checkedIndices.size());
        for (const int index : node.checkedIndices) HashBytes(hash, index);
        HashBytes(hash, node.itemDepths.size());
        for (const int depth : node.itemDepths) HashBytes(hash, depth);
        HashBytes(hash, node.itemExpanded.size());
        for (const bool expanded : node.itemExpanded) HashBytes(hash, expanded);
        HashBytes(hash, node.itemHasChildren.size());
        for (const bool hasChildren : node.itemHasChildren) HashBytes(hash, hasChildren);
        HashBytes(hash, node.imageList.size());
        for (const auto& entry : node.imageList) {
            HashBytes(hash, entry.imageWidth);
            HashBytes(hash, entry.imageHeight);
            HashString(hash, entry.imageFormat);
            HashBytes(hash, entry.imageData.size());
            if (!entry.imageData.empty())
                HashRange(hash, entry.imageData.data(), entry.imageData.size());
        }
        HashBytes(hash, node.itemImages.size());
        for (const int image : node.itemImages) HashBytes(hash, image);
        HashBytes(hash, node.itemSelectedImages.size());
        for (const int image : node.itemSelectedImages) HashBytes(hash, image);
        HashBytes(hash, node.stateImageList.size());
        for (const auto& entry : node.stateImageList) {
            HashBytes(hash, entry.imageWidth);
            HashBytes(hash, entry.imageHeight);
            HashString(hash, entry.imageFormat);
            HashBytes(hash, entry.imageData.size());
            if (!entry.imageData.empty())
                HashRange(hash, entry.imageData.data(), entry.imageData.size());
        }
        HashBytes(hash, node.itemStateImages.size());
        for (const int image : node.itemStateImages) HashBytes(hash, image);
        HashBytes(hash, node.editableLabels);
        HashBytes(hash, node.editingIndex);
        HashBytes(hash, node.imageWidth);
        HashBytes(hash, node.imageHeight);
        HashString(hash, node.imageFormat);
        HashBytes(hash, node.imageData.size());
        if (!node.imageData.empty()) HashRange(hash, node.imageData.data(), node.imageData.size());
        HashBytes(hash, node.toolbarItems.size());
        for (const auto& item : node.toolbarItems) {
            HashBytes(hash, item.radioGroup);
            HashBytes(hash, item.kind);
            HashBytes(hash, item.commandId);
            HashBytes(hash, item.rect);
            HashString(hash, item.text);
            HashBytes(hash, item.enabled);
            HashBytes(hash, item.hidden);
            HashBytes(hash, item.checked);
            HashBytes(hash, item.dropDown);
            HashBytes(hash, item.wholeDropDown);
            HashBytes(hash, item.paintedFace);
            hashOptionalImage(item.hotImage);
            hashOptionalImage(item.disabledImage);
            hashOptionalImage(item.pressedImage);
            HashBytes(hash, item.imageWidth);
            HashBytes(hash, item.imageHeight);
            HashString(hash, item.imageFormat);
            HashBytes(hash, item.imageData.size());
            if (!item.imageData.empty()) HashRange(hash, item.imageData.data(), item.imageData.size());
        }
        HashString(hash, node.adapterId);
        HashBytes(hash, node.splits.size());
        for (const auto& split : node.splits) {
            HashBytes(hash, split.vertical);
            HashBytes(hash, split.position);
            HashBytes(hash, split.thickness);
            HashBytes(hash, split.minimum);
            HashBytes(hash, split.maximum);
        }
        HashBytes(hash, node.scrollBars.size());
        for (const auto& bar : node.scrollBars) {
            HashBytes(hash, bar.vertical);
            HashBytes(hash, bar.enabled);
            HashBytes(hash, bar.minimum);
            HashBytes(hash, bar.maximum);
            HashBytes(hash, bar.page);
            HashBytes(hash, bar.position);
            HashBytes(hash, bar.rect);
        }
        if (!node.scrollBars.empty()) HashBytes(hash, node.clientRect);
        // The chrome pixels are part of the fingerprint, so a container that repaints
        // its own band reaches the renderer as a patch instead of going stale.
        HashBytes(hash, node.chromeRegions.size());
        for (const auto& region : node.chromeRegions) {
            HashBytes(hash, region.rect);
            HashBytes(hash, region.imageWidth);
            HashBytes(hash, region.imageHeight);
            HashString(hash, region.imageFormat);
            HashBytes(hash, region.imageData.size());
            if (!region.imageData.empty())
                HashRange(hash, region.imageData.data(), region.imageData.size());
        }
        HashBytes(hash, node.islandItems.size());
        for (const auto& item : node.islandItems) {
            HashString(hash, item.kind);
            HashBytes(hash, item.rect);
            HashString(hash, item.name);
            HashString(hash, item.description);
            HashString(hash, item.actionName);
            HashBytes(hash, item.enabled);
            HashBytes(hash, item.dropDown);
            HashBytes(hash, item.selected);
            HashBytes(hash, item.imageWidth);
            HashBytes(hash, item.imageHeight);
            HashString(hash, item.imageFormat);
            HashBytes(hash, item.imageData.size());
            if (!item.imageData.empty())
                HashRange(hash, item.imageData.data(), item.imageData.size());
        }
        HashBytes(hash, node.richParagraphs.size());
        for (const auto& paragraph : node.richParagraphs) {
            HashString(hash, paragraph.alignment);
            HashBytes(hash, paragraph.bullet);
            HashBytes(hash, paragraph.leftIndent);
            HashBytes(hash, paragraph.firstLineIndent);
            HashBytes(hash, paragraph.spaceBefore);
            HashBytes(hash, paragraph.spaceAfter);
            HashBytes(hash, paragraph.runs.size());
            for (const auto& run : paragraph.runs) {
                HashString(hash, run.text);
                HashBytes(hash, run.bold);
                HashBytes(hash, run.italic);
                HashBytes(hash, run.underline);
                HashBytes(hash, run.strike);
                HashBytes(hash, run.size);
                HashBytes(hash, run.color);
            }
        }
        HashBytes(hash, node.performanceGraph.has_value());
        if (node.performanceGraph) {
            const auto& graph = *node.performanceGraph;
            HashString(hash, graph.displayType);
            HashBytes(hash, graph.minimum);
            HashBytes(hash, graph.maximum);
            HashBytes(hash, graph.sampleCount);
            HashBytes(hash, graph.intervalMs);
            HashBytes(hash, graph.scroll);
            HashBytes(hash, graph.barSlot);
            HashBytes(hash, graph.wrapped);
            HashBytes(hash, graph.startTimeMs);
            HashBytes(hash, graph.endTimeMs);
            HashBytes(hash, graph.showVerticalGrid);
            HashBytes(hash, graph.showHorizontalGrid);
            HashBytes(hash, graph.showScaleLabels);
            HashBytes(hash, graph.showTimeLabels);
            HashString(hash, graph.title);
            HashString(hash, graph.yAxisLabel);
            HashBytes(hash, graph.backColor);
            HashBytes(hash, graph.controlColor);
            HashBytes(hash, graph.gridColor);
            HashBytes(hash, graph.textColor);
            HashBytes(hash, graph.barColor);
            HashBytes(hash, graph.series.size());
            for (const auto& series : graph.series) {
                HashString(hash, series.name);
                HashBytes(hash, series.color);
                HashBytes(hash, series.width);
                HashString(hash, series.style);
                HashBytes(hash, series.scale);
                HashBytes(hash, series.visible);
                HashBytes(hash, series.highlighted);
                HashBytes(hash, series.values.size());
                for (const auto& value : series.values) {
                    HashBytes(hash, value.has_value());
                    if (value) HashBytes(hash, *value);
                }
            }
        }
        HashBytes(hash, node.performanceLegend.has_value());
        if (node.performanceLegend) {
            const auto& legend = *node.performanceLegend;
            HashString(hash, legend.displayType);
            HashBytes(hash, legend.columns.size());
            for (const auto& column : legend.columns) {
                HashString(hash, column.title);
                HashBytes(hash, column.width);
            }
            HashBytes(hash, legend.rows.size());
            for (const auto& row : legend.rows) {
                HashBytes(hash, row.visible);
                HashBytes(hash, row.color);
                HashBytes(hash, row.width);
                HashString(hash, row.style);
                HashBytes(hash, row.selected);
                HashBytes(hash, row.cells.size());
                for (const auto& cell : row.cells) HashString(hash, cell);
            }
        }
        HashBytes(hash, node.performanceValueBar.has_value());
        if (node.performanceValueBar) {
            HashBytes(hash, node.performanceValueBar->fields.size());
            for (const auto& field : node.performanceValueBar->fields) {
                HashString(hash, field.label);
                HashString(hash, field.value);
            }
        }
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

// True when `next` differs from the canonical snapshot only in the pixels some container's chrome
// regions hold (so the fingerprints differ and nothing else moved).  The pixel buffers are swapped
// for the length of the comparison, never copied.
bool OnlyChromePixelsChanged(WindowSnapshot& canonical, WindowSnapshot& next,
    uint64_t canonicalFingerprint) noexcept {
    if (canonical.nodes.size() != next.nodes.size()) return false;
    struct Swap final { std::vector<uint8_t>* canonical; std::vector<uint8_t>* next; };
    std::vector<Swap> swaps;
    try {
        for (size_t index = 0; index < canonical.nodes.size(); ++index) {
            auto& before = canonical.nodes[index].chromeRegions;
            auto& after = next.nodes[index].chromeRegions;
            if (before.size() != after.size()) return false;
            for (size_t region = 0; region < before.size(); ++region) {
                if (before[region].imageData != after[region].imageData)
                    swaps.push_back({ &before[region].imageData, &after[region].imageData });
            }
        }
    } catch (...) {
        return false;
    }
    if (swaps.empty()) return false;
    for (const auto& pair : swaps) pair.canonical->swap(*pair.next);
    // `next` now carries the canonical pixels: it fingerprints like the canonical snapshot exactly
    // when the pixels were all that differed.
    const bool onlyPixels = SnapshotFingerprint(next) == canonicalFingerprint;
    for (const auto& pair : swaps) pair.canonical->swap(*pair.next);
    return onlyPixels;
}

} // namespace FluentShell::Bridge::Translation
