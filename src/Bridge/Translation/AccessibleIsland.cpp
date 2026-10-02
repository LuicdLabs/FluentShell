#include "AccessibleIsland.h"
#include "MmcHtmlDocument.h"

#include "../../Common/FluentShell.h"
#include "../Ipc/Protocol.h"

#include <oleacc.h>
#include <oleauto.h>

#include <algorithm>
#include <array>

namespace FluentShell::Bridge::Translation {
namespace {

bool Reject(std::wstring& reason, std::wstring_view text) {
    reason = text;
    return false;
}

// Releases an IAccessible without pulling a COM smart pointer into this translation
// unit, which keeps the adapter free of winrt/ATL dependencies.
struct AccessibleRef final {
    IAccessible* value = nullptr;
    AccessibleRef() = default;
    explicit AccessibleRef(IAccessible* raw) noexcept : value(raw) {}
    AccessibleRef(const AccessibleRef&) = delete;
    AccessibleRef& operator=(const AccessibleRef&) = delete;
    AccessibleRef(AccessibleRef&& other) noexcept : value(other.value) { other.value = nullptr; }
    AccessibleRef& operator=(AccessibleRef&& other) noexcept {
        if (this != &other) {
            if (value) value->Release();
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    ~AccessibleRef() { if (value) value->Release(); }
    IAccessible* operator->() const noexcept { return value; }
    explicit operator bool() const noexcept { return value != nullptr; }
};

struct BstrRef final {
    BSTR value = nullptr;
    BstrRef() = default;
    BstrRef(const BstrRef&) = delete;
    BstrRef& operator=(const BstrRef&) = delete;
    ~BstrRef() { if (value) SysFreeString(value); }
    std::wstring Text() const {
        return value ? std::wstring(value, SysStringLen(value)) : std::wstring();
    }
};

VARIANT SelfId() noexcept {
    VARIANT self{};
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;
    return self;
}

// One island element, addressed either as a full IAccessible or as a simple child id
// of the island itself.  Both forms are answered by real providers, so both are read.
struct IslandElement final {
    AccessibleRef object;
    VARIANT id = SelfId();

    IAccessible* Target(IAccessible* container) const noexcept {
        return object ? object.value : container;
    }
};

std::wstring TrimmedText(std::wstring text) {
    const auto isSpace = [](wchar_t character) {
        return character == L' ' || character == L'\t' || character == L'\r' ||
            character == L'\n';
    };
    while (!text.empty() && isSpace(text.back())) text.pop_back();
    size_t start = 0;
    while (start < text.size() && isSpace(text[start])) ++start;
    return text.substr(start);
}

// A container role the island's root may legitimately have.  Anything else means the
// window is not framing a set of elements and is refused.
bool IsContainerRole(long role) noexcept {
    return role == ROLE_SYSTEM_CLIENT || role == ROLE_SYSTEM_PANE ||
        role == ROLE_SYSTEM_GROUPING || role == ROLE_SYSTEM_WINDOW ||
        role == ROLE_SYSTEM_TOOLBAR || role == ROLE_SYSTEM_PAGETABLIST;
}

bool IsPageTabHost(HWND window) noexcept {
    wchar_t name[64]{};
    return GetClassNameW(window, name, 64) > 0 &&
        FluentShell::EqualsIgnoreCase(name, L"AMCCustomTab");
}

bool MapItemKind(long role, AccessibleItemKind& kind, bool& dropDown) noexcept {
    dropDown = false;
    switch (role) {
    case ROLE_SYSTEM_STATICTEXT:
        kind = AccessibleItemKind::Text;
        return true;
    case ROLE_SYSTEM_PUSHBUTTON:
        kind = AccessibleItemKind::Button;
        return true;
    case ROLE_SYSTEM_BUTTONDROPDOWN:
    case ROLE_SYSTEM_BUTTONDROPDOWNGRID:
    case ROLE_SYSTEM_BUTTONMENU:
    case ROLE_SYSTEM_MENUPOPUP:
        // These complete by opening a menu of their own rather than in place, which
        // the projection has to draw so the affordance is not silently lost.
        kind = AccessibleItemKind::Button;
        dropDown = true;
        return true;
    case ROLE_SYSTEM_LINK:
        kind = AccessibleItemKind::Link;
        return true;
    case ROLE_SYSTEM_PAGETAB:
        kind = AccessibleItemKind::PageTab;
        return true;
    default:
        return false;
    }
}

bool OpenIsland(HWND island, AccessibleRef& root, std::wstring& reason) {
    IAccessible* raw = nullptr;
    // OBJID_CLIENT is what the provider answers for its own content.  A window with no
    // provider returns a failure here, which refuses the island rather than projecting
    // an empty one.
    const HRESULT result = AccessibleObjectFromWindow(
        island, static_cast<DWORD>(OBJID_CLIENT), IID_IAccessible,
        reinterpret_cast<void**>(&raw));
    if (FAILED(result) || !raw) {
        return Reject(reason, L"island window answers no accessible object");
    }
    root = AccessibleRef(raw);
    return true;
}

bool ReadRole(IAccessible* target, const VARIANT& id, long& role) noexcept {
    VARIANT value{};
    if (FAILED(target->get_accRole(id, &value))) return false;
    const bool ok = value.vt == VT_I4;
    if (ok) role = value.lVal;
    VariantClear(&value);
    return ok;
}

bool ReadState(IAccessible* target, const VARIANT& id, long& state) noexcept {
    VARIANT value{};
    if (FAILED(target->get_accState(id, &value))) return false;
    const bool ok = value.vt == VT_I4;
    if (ok) state = value.lVal;
    VariantClear(&value);
    return ok;
}

// The island's children, skipping the ones the provider declares invisible.  A child
// that owns children of its own is refused: a nested tree is a different contract
// than a flat list of elements and would need its own admission rules.
bool CollectElements(
    IAccessible* root,
    std::vector<IslandElement>& elements,
    std::wstring& reason) {
    long childCount = 0;
    if (FAILED(root->get_accChildCount(&childCount)) || childCount <= 0) {
        return Reject(reason, L"island exposes no accessible children");
    }
    if (static_cast<size_t>(childCount) > Ipc::kMaxIslandItems) {
        return Reject(reason, L"island exceeds the protocol item bound");
    }
    std::vector<VARIANT> raw(static_cast<size_t>(childCount));
    long obtained = 0;
    if (FAILED(AccessibleChildren(root, 0, childCount, raw.data(), &obtained)) ||
        obtained <= 0) {
        return Reject(reason, L"island accessible children could not be enumerated");
    }
    bool ok = true;
    for (long index = 0; index < obtained; ++index) {
        VARIANT& entry = raw[static_cast<size_t>(index)];
        IslandElement element;
        if (entry.vt == VT_DISPATCH && entry.pdispVal) {
            IAccessible* child = nullptr;
            if (SUCCEEDED(entry.pdispVal->QueryInterface(IID_IAccessible,
                    reinterpret_cast<void**>(&child))) && child) {
                element.object = AccessibleRef(child);
            } else {
                ok = Reject(reason, L"island child is not an accessible element");
            }
        } else if (entry.vt == VT_I4) {
            element.id.lVal = entry.lVal;
        } else {
            ok = Reject(reason, L"island child has an unsupported accessible form");
        }
        VariantClear(&entry);
        if (!ok) continue;
        IAccessible* target = element.Target(root);
        long state = 0;
        if (!ReadState(target, element.id, state)) {
            ok = Reject(reason, L"island child state is unavailable");
            continue;
        }
        if ((state & STATE_SYSTEM_INVISIBLE) != 0) continue;
        if (element.object) {
            long nested = 0;
            if (SUCCEEDED(element.object->get_accChildCount(&nested)) && nested > 0) {
                ok = Reject(reason, L"island child owns an accessible subtree");
                continue;
            }
        }
        elements.push_back(std::move(element));
    }
    if (!ok) return false;
    if (elements.empty()) {
        return Reject(reason, L"island exposes no visible accessible children");
    }
    return true;
}

bool ReadItem(
    IAccessible* root,
    const IslandElement& element,
    const RECT& islandScreenRect,
    AccessibleIslandItem& item,
    std::wstring& reason) {
    IAccessible* target = element.Target(root);
    long role = 0;
    if (!ReadRole(target, element.id, role)) {
        return Reject(reason, L"island child role is unavailable");
    }
    if (!MapItemKind(role, item.kind, item.dropDown)) {
        wchar_t evidence[112]{};
        swprintf_s(evidence, L"island child has unsupported accessible role %ld", role);
        return Reject(reason, evidence);
    }
    BstrRef name;
    if (FAILED(target->get_accName(element.id, &name.value))) {
        return Reject(reason, L"island child name is unavailable");
    }
    item.name = TrimmedText(name.Text());
    if (item.name.empty()) {
        return Reject(reason, L"island child has no accessible name");
    }
    if (item.name.size() > Ipc::kMaxStringChars) {
        return Reject(reason, L"island child name exceeds the protocol cap");
    }
    BstrRef description;
    if (SUCCEEDED(target->get_accDescription(element.id, &description.value))) {
        item.description = TrimmedText(description.Text());
        if (item.description.size() > Ipc::kMaxStringChars) item.description.clear();
    }
    BstrRef action;
    if (SUCCEEDED(target->get_accDefaultAction(element.id, &action.value))) {
        item.actionName = TrimmedText(action.Text());
        if (item.actionName.size() > Ipc::kMaxStringChars) {
            return Reject(reason, L"island child action name exceeds the protocol cap");
        }
    }
    long state = 0;
    if (!ReadState(target, element.id, state)) {
        return Reject(reason, L"island child state is unavailable");
    }
    if ((state & STATE_SYSTEM_INVISIBLE) != 0) {
        return Reject(reason, L"island child became invisible during capture");
    }
    item.enabled = (state & STATE_SYSTEM_UNAVAILABLE) == 0;
    // Disabled MMC commands can omit their default action. Preserve the disabled
    // button, and require the provider's action as soon as it becomes enabled.
    if (item.kind != AccessibleItemKind::Text && item.enabled && item.actionName.empty())
        return Reject(reason, L"island child offers no accessible default action");
    if (item.kind == AccessibleItemKind::Text && !item.actionName.empty())
        return Reject(reason, L"island text child unexpectedly offers an action");
    item.selected = item.kind == AccessibleItemKind::PageTab &&
        (state & STATE_SYSTEM_SELECTED) != 0;
    if (item.kind == AccessibleItemKind::PageTab && (state & STATE_SYSTEM_SELECTABLE) == 0)
        return Reject(reason, L"island page tab is not selectable");
    long left = 0;
    long top = 0;
    long width = 0;
    long height = 0;
    if (FAILED(target->accLocation(&left, &top, &width, &height, element.id)) ||
        width <= 0 || height <= 0) {
        return Reject(reason, L"island child has no accessible bounds");
    }
    // accLocation is in screen pixels; the item is published relative to the island's
    // own client area, matching how a Toolbar publishes its button rectangles.
    item.rect.left = left - islandScreenRect.left;
    item.rect.top = top - islandScreenRect.top;
    item.rect.right = item.rect.left + width;
    item.rect.bottom = item.rect.top + height;
    return true;
}

}  // namespace

std::wstring AccessibleChildName(HWND window, int childIndex) noexcept {
    try {
        if (!window || !IsWindow(window) || childIndex <= 0) return {};
        AccessibleRef root;
        std::wstring ignored;
        if (!OpenIsland(window, root, ignored)) return {};
        VARIANT id{};
        id.vt = VT_I4;
        id.lVal = childIndex;
        BstrRef name;
        if (FAILED(root->get_accName(id, &name.value))) return {};
        std::wstring text = TrimmedText(name.Text());
        if (text.size() > Ipc::kMaxStringChars) return {};
        return text;
    } catch (...) {
        return {};
    }
}

bool IsAccessibleIslandClass(std::wstring_view className) noexcept {
    static constexpr std::array kHostClasses{
        std::wstring_view{ L"DirectUIHWND" },
        std::wstring_view{ L"AMCCustomTab" },
        std::wstring_view{ L"Internet Explorer_Server" },
    };
    for (const auto& candidate : kHostClasses) {
        if (FluentShell::EqualsIgnoreCase(className, candidate)) return true;
    }
    return false;
}

std::wstring DescribeAccessibleIsland(HWND window) noexcept {
    try {
        AccessibleRef root;
        std::wstring reason;
        if (!OpenIsland(window, root, reason)) return reason;
        std::wstring result;
        size_t remaining = 32;
        const auto visit = [&](auto&& recurse, IAccessible* object,
                               const VARIANT& id, unsigned depth) -> void {
            if (remaining == 0) return;
            --remaining;
            long role = -1, state = -1, count = 0;
            ReadRole(object, id, role);
            ReadState(object, id, state);
            if (id.vt == VT_I4 && id.lVal == CHILDID_SELF)
                object->get_accChildCount(&count);
            BstrRef name, action;
            object->get_accName(id, &name.value);
            object->get_accDefaultAction(id, &action.value);
            auto shortText = [](std::wstring text) {
                if (text.size() > 80) text.resize(80);
                for (auto& character : text)
                    if (character < L' ') character = L' ';
                return text;
            };
            result += L" {depth=" + std::to_wstring(depth) + L" role=" +
                std::to_wstring(role) + L" state=" + std::to_wstring(state) +
                L" children=" + std::to_wstring(count) + L" name='" +
                shortText(name.Text()) + L"' action='" + shortText(action.Text()) + L"'}";
            if (depth >= 4 || count <= 0 || remaining == 0) return;
            const long requested = (std::min)(count, static_cast<long>(remaining));
            std::vector<VARIANT> children(static_cast<size_t>(requested));
            long obtained = 0;
            if (SUCCEEDED(AccessibleChildren(object, 0, requested, children.data(), &obtained))) {
                for (long index = 0; index < (std::min)(obtained, requested); ++index) {
                    const auto& child = children[static_cast<size_t>(index)];
                    if (child.vt == VT_DISPATCH && child.pdispVal) {
                        IAccessible* raw = nullptr;
                        if (SUCCEEDED(child.pdispVal->QueryInterface(IID_IAccessible,
                                reinterpret_cast<void**>(&raw))) && raw) {
                            AccessibleRef ref(raw);
                            recurse(recurse, ref.value, SelfId(), depth + 1);
                        }
                    } else if (child.vt == VT_I4 && child.lVal != CHILDID_SELF) {
                        recurse(recurse, object, child, depth + 1);
                    }
                }
            }
            for (auto& child : children) VariantClear(&child);
        };
        visit(visit, root.value, SelfId(), 0);
        return result;
    } catch (...) {
        return L"accessible diagnostic failed";
    }
}

bool ReadAccessibleIslandItems(
    HWND island,
    std::vector<AccessibleIslandItem>& items,
    std::wstring& reason) noexcept {
    items.clear();
    try {
        if (!island || !IsWindow(island)) {
            return Reject(reason, L"island window is gone");
        }
        wchar_t className[64]{};
        GetClassNameW(island, className, 64);
        if (FluentShell::EqualsIgnoreCase(className, L"Internet Explorer_Server"))
            return ReadMmcHtmlDocument(island, items, reason);
        RECT screenRect{};
        if (!GetWindowRect(island, &screenRect)) {
            return Reject(reason, L"island bounds are unavailable");
        }
        // The client origin is what item rectangles are relative to, so a window with
        // a border does not shift every item.
        POINT clientOrigin{ 0, 0 };
        if (!ClientToScreen(island, &clientOrigin)) {
            return Reject(reason, L"island client origin is unavailable");
        }
        screenRect.left = clientOrigin.x;
        screenRect.top = clientOrigin.y;
        AccessibleRef root;
        if (!OpenIsland(island, root, reason)) return false;
        long rootRole = 0;
        const VARIANT self = SelfId();
        if (!ReadRole(root.value, self, rootRole) || !IsContainerRole(rootRole)) {
            return Reject(reason, L"island accessible root is not a container");
        }
        if (IsPageTabHost(island) != (rootRole == ROLE_SYSTEM_PAGETABLIST))
            return Reject(reason, L"island tab host has an inconsistent root role");
        std::vector<IslandElement> elements;
        if (!CollectElements(root.value, elements, reason)) return false;
        items.reserve(elements.size());
        size_t selectedTabs = 0;
        for (const IslandElement& element : elements) {
            AccessibleIslandItem item;
            if (!ReadItem(root.value, element, screenRect, item, reason)) return false;
            if ((rootRole == ROLE_SYSTEM_PAGETABLIST) !=
                (item.kind == AccessibleItemKind::PageTab))
                return Reject(reason, L"island page tabs require a homogeneous tab list");
            if (item.selected) ++selectedTabs;
            if (item.kind == AccessibleItemKind::PageTab) {
                RECT client{};
                if (!GetClientRect(island, &client) || item.rect.left < 0 || item.rect.top < 0 ||
                    item.rect.right > client.right || item.rect.bottom > client.bottom)
                    return Reject(reason, L"island page tab bounds fall outside its host");
                for (const auto& previous : items) {
                    RECT intersection{};
                    // MMC's slanted page tabs overlap at their edges (the observed
                    // 73 px and 70 px tabs overlap by 8 px). Keep those rectangles;
                    // only containment, duplicate bands, or cross-row overlap is invalid.
                    if (IntersectRect(&intersection, &previous.rect, &item.rect) &&
                        (previous.rect.top != item.rect.top || previous.rect.bottom != item.rect.bottom ||
                         previous.rect.left == item.rect.left || previous.rect.right == item.rect.right ||
                         ((previous.rect.left < item.rect.left) != (previous.rect.right < item.rect.right))))
                        return Reject(reason, L"island page tab bounds overlap inconsistently");
                }
            }
            items.push_back(std::move(item));
        }
        if (rootRole == ROLE_SYSTEM_PAGETABLIST && selectedTabs != 1)
            return Reject(reason, L"island tab list must have exactly one selected tab");
        return true;
    } catch (...) {
        items.clear();
        return Reject(reason, L"island accessible read raised an exception");
    }
}

bool InvokeAccessibleIslandItem(
    HWND island,
    int index,
    const std::wstring& expectedName,
    const std::wstring& expectedAction,
    std::wstring& reason) noexcept {
    try {
        if (!island || !IsWindow(island) || !IsWindowVisible(island) ||
            !IsWindowEnabled(island)) {
            return Reject(reason, L"island window is unavailable for an action");
        }
        POINT clientOrigin{};
        if (!ClientToScreen(island, &clientOrigin)) {
            return Reject(reason, L"island client origin is unavailable");
        }
        AccessibleRef root;
        if (!OpenIsland(island, root, reason)) return false;
        long rootRole = 0;
        if (!ReadRole(root.value, SelfId(), rootRole) || !IsContainerRole(rootRole)) {
            return Reject(reason, L"island accessible root is not a container");
        }
        if (IsPageTabHost(island) != (rootRole == ROLE_SYSTEM_PAGETABLIST))
            return Reject(reason, L"island tab host role changed before invocation");
        std::vector<IslandElement> elements;
        if (!CollectElements(root.value, elements, reason)) return false;
        if (index < 0 || static_cast<size_t>(index) >= elements.size()) {
            return Reject(reason, L"island item index no longer exists");
        }
        const IslandElement& element = elements[static_cast<size_t>(index)];
        AccessibleIslandItem current;
        const RECT origin{ clientOrigin.x, clientOrigin.y, clientOrigin.x, clientOrigin.y };
        if (!ReadItem(root.value, element, origin, current, reason)) return false;
        if ((rootRole == ROLE_SYSTEM_PAGETABLIST) !=
            (current.kind == AccessibleItemKind::PageTab))
            return Reject(reason, L"island page tab role changed before invocation");
        // The published name and action are the identity of the element the user
        // clicked.  If either moved, the projection acts on nothing rather than on
        // whatever slid into that position.
        if (current.name != expectedName || current.actionName != expectedAction) {
            return Reject(reason, L"island item identity changed before the action ran");
        }
        if (!current.enabled) {
            return Reject(reason, L"island item became disabled before the action ran");
        }
        if (current.actionName.empty()) {
            return Reject(reason, L"island item offers no accessible default action");
        }
        // Invoke the same object whose current identity and state were read above;
        // reopening the provider here could resolve the index to another element.
        IAccessible* target = element.Target(root.value);
        const HRESULT performed = target->accDoDefaultAction(element.id);
        if (FAILED(performed)) {
            return Reject(reason, L"the island element refused its own default action");
        }
        return true;
    } catch (...) {
        return Reject(reason, L"island action raised an exception");
    }
}

}  // namespace FluentShell::Bridge::Translation
