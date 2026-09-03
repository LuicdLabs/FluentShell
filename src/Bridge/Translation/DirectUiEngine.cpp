#include "DirectUiEngine.h"

#include "SourceThreadAgent.h"
#include "ControlAdapters.h"
#include "../../Common/FluentShell.h"
#include "../Ipc/Protocol.h"

#include <combaseapi.h>
#include <dwmapi.h>
#include <wincrypt.h>
#include <mscat.h>
#include <winver.h>
#include <wintrust.h>
#include <softpub.h>
#include <commctrl.h>
#include <prsht.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>
#include <unordered_set>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace FluentShell::Bridge::Translation {
namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD kUiaDeadlineMs = 1600;
constexpr size_t kMaxTextChars = 4096;
// Upper bound on the UIA subtree a single admitted page may present. Composite
// controls contribute one element per item, so a list page legitimately exceeds
// a few dozen elements; the bound stays an equality-checked rejection so an
// unexpectedly large provider tree still keeps the surface native.
constexpr int kMaxUiaDescendants = 512;
// Items a single projected composite may own. Larger native lists stay native
// rather than being truncated into a partially represented control.
constexpr size_t kMaxCompositeItems = 256;

bool Fail(std::wstring& error, std::wstring_view value) {
    error.assign(value);
    return false;
}

std::wstring ClassName(HWND window) {
    wchar_t value[128]{};
    const int length = GetClassNameW(window, value, static_cast<int>(std::size(value)));
    return length > 0 ? std::wstring(value, static_cast<size_t>(length)) : std::wstring();
}

bool SameRect(const RECT& left, const RECT& right) noexcept {
    return EqualRect(&left, &right) != FALSE;
}

bool SameImplementationIdentity(
    const std::vector<DirectUiImplementationEvidence>& left,
    const std::vector<DirectUiImplementationEvidence>& right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (left[index].hwnd != right[index].hwnd ||
            left[index].generation != right[index].generation ||
            left[index].parent != right[index].parent ||
            left[index].className != right[index].className ||
            left[index].visible != right[index].visible) return false;
    }
    return true;
}

bool IsPropertySheetButton(int value) noexcept {
    return value == PSBTN_BACK || value == PSBTN_NEXT ||
        value == PSBTN_FINISH || value == PSBTN_CANCEL;
}

HWND CurrentPropertySheetPage(HWND root, const std::vector<HWND>& pageHosts) noexcept {
    if (!root || pageHosts.empty()) return nullptr;
    const HWND page = reinterpret_cast<HWND>(
        SendMessageW(root, PSM_GETCURRENTPAGEHWND, 0, 0));
    return page && std::find(pageHosts.begin(), pageHosts.end(), page) != pageHosts.end()
        ? page : nullptr;
}

bool SameWindow(const DirectUiWindowEvidence& left, const DirectUiWindowEvidence& right) noexcept {
    return left.hwnd == right.hwnd && left.generation == right.generation &&
        SameRect(left.bounds, right.bounds) && left.style == right.style &&
        left.exStyle == right.exStyle && left.visible == right.visible &&
        left.enabled == right.enabled && left.tabIndex == right.tabIndex &&
        left.text == right.text && left.note == right.note &&
        left.controlId == right.controlId && left.dialogCode == right.dialogCode &&
        left.checked == right.checked && left.minimum == right.minimum &&
        left.maximum == right.maximum && left.position == right.position &&
        left.indeterminate == right.indeterminate &&
        left.hasDetail == right.hasDetail &&
        SameDirectUiDetailShape(left, right) &&
        SameDirectUiDetailContent(left, right);
}

bool SameProfileWindow(
    const DirectUiSlot& slot,
    const DirectUiWindowEvidence& left,
    const DirectUiWindowEvidence& right) noexcept {
    const bool styleChanged = left.style != right.style;
    auto normalized = right;
    normalized.style = left.style;
    if (styleChanged && slot.kind == ControlKind::Button)
        normalized.dialogCode = left.dialogCode;
    if (styleChanged && slot.kind == ControlKind::RadioButton && slot.captureBitmap)
        normalized.checked = left.checked;
    if (!SameWindow(left, normalized)) return false;
    if (!styleChanged) return true;
    const uint64_t alternateBits = static_cast<uint64_t>(slot.nativeStyleValue) ^
        static_cast<uint64_t>(slot.nativeStyleAlt);
    const auto admitted = [&](uint64_t style) {
        const uint64_t masked = style & slot.nativeStyleMask;
        return masked == slot.nativeStyleValue || masked == slot.nativeStyleAlt;
    };
    const uint32_t dialogVariation = left.dialogCode ^ right.dialogCode;
    const uint32_t defaultDialogBits = DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON;
    const uint32_t leftDefaultDialog = left.dialogCode & defaultDialogBits;
    const uint32_t rightDefaultDialog = right.dialogCode & defaultDialogBits;
    const bool dialogCodeMatches = slot.kind != ControlKind::Button ||
        ((dialogVariation & ~defaultDialogBits) == 0 &&
         (leftDefaultDialog == DLGC_DEFPUSHBUTTON ||
          leftDefaultDialog == DLGC_UNDEFPUSHBUTTON) &&
         (rightDefaultDialog == DLGC_DEFPUSHBUTTON ||
          rightDefaultDialog == DLGC_UNDEFPUSHBUTTON));
    return alternateBits != 0 && dialogCodeMatches &&
        ((left.style ^ right.style) & ~alternateBits) == 0 &&
        admitted(left.style) && admitted(right.style);
}

bool SameMovedWindow(
    const DirectUiWindowEvidence& before,
    const DirectUiWindowEvidence& after,
    int64_t deltaX,
    int64_t deltaY) noexcept {
    const bool moved =
        static_cast<int64_t>(before.bounds.left) + deltaX == after.bounds.left &&
        static_cast<int64_t>(before.bounds.top) + deltaY == after.bounds.top &&
        static_cast<int64_t>(before.bounds.right) + deltaX == after.bounds.right &&
        static_cast<int64_t>(before.bounds.bottom) + deltaY == after.bounds.bottom;
    return moved && before.hwnd == after.hwnd &&
        before.generation == after.generation && before.style == after.style &&
        before.exStyle == after.exStyle && before.visible == after.visible &&
        before.enabled == after.enabled && before.tabIndex == after.tabIndex &&
        before.text == after.text && before.note == after.note &&
        before.controlId == after.controlId &&
        before.dialogCode == after.dialogCode && before.checked == after.checked &&
        before.minimum == after.minimum && before.maximum == after.maximum &&
        before.position == after.position && before.indeterminate == after.indeterminate &&
        before.hasDetail == after.hasDetail &&
        SameDirectUiDetailShape(before, after) &&
        SameDirectUiDetailContent(before, after);
}

bool CaptureWindowClientPixels(
    HWND window,
    DirectUiWindowEvidence& value,
    std::wstring& error) {
    return CaptureOwnedWindowPixels(window, Ipc::kMaxDirectUiBitmapDimension,
        Ipc::kMaxDirectUiBitmapBytes, value.imageWidth, value.imageHeight,
        value.imageFormat, value.imageData, error);
}

// Reads this backing control's typed state through its own registered Win32
// adapter. The DirectUI lane therefore inherits every proven control contract
// (and every rejection) instead of re-deriving facets per application.
bool CaptureSlotDetail(
    HWND window,
    ControlKind kind,
    DirectUiWindowEvidence& value,
    std::wstring& error) {
    value.hasDetail = false;
    value.detail = ControlNode{};
    value.detail.kind = kind;
    value.detail.hwnd = window;
    value.detail.style = value.style;
    value.detail.exStyle = value.exStyle;
    value.detail.rect = value.bounds;
    value.detail.text = value.text;
    value.detail.enabled = value.enabled;
    value.detail.visible = value.visible;
    value.detail.controlId = value.controlId;
    value.detail.dialogCode = value.dialogCode;
    std::wstring reason;
    if (!CaptureControlDetail(window, value.detail, reason))
        return Fail(error, L"backing control typed state was rejected: " + reason);
    value.hasDetail = true;
    return true;
}

bool SameToolbarItems(
    const std::vector<ToolbarItemSnapshot>& left,
    const std::vector<ToolbarItemSnapshot>& right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.kind != b.kind || a.commandId != b.commandId || !SameRect(a.rect, b.rect) ||
            a.text != b.text || a.enabled != b.enabled || a.hidden != b.hidden ||
            a.imageWidth != b.imageWidth || a.imageHeight != b.imageHeight ||
            a.imageFormat != b.imageFormat || a.imageData != b.imageData) return false;
    }
    return true;
}

// The part of a toolbar that is a contract rather than presentation: which
// command each button posts and the caption that says so. Enabled state,
// visibility, geometry, and images may move between revisions; a command or its
// caption may not, because no UIA evidence corroborates it while cloaked.
bool SameToolbarCommands(
    const std::vector<ToolbarItemSnapshot>& left,
    const std::vector<ToolbarItemSnapshot>& right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (left[index].kind != right[index].kind ||
            left[index].commandId != right[index].commandId ||
            left[index].text != right[index].text) return false;
    }
    return true;
}

bool SameRectList(const std::vector<RECT>& left, const std::vector<RECT>& right) noexcept {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (!SameRect(left[index], right[index])) return false;
    }
    return true;
}

// Reads one HWND facet block. All reads happen on the source GUI thread, so
// the values are canonical the moment they return.
bool ReadWindowEvidence(
    SourceThreadAgent& agent,
    HWND window,
    bool isButton,
    DirectUiWindowEvidence& value,
    std::wstring& error,
    bool isProgress = false,
    bool captureBitmap = false) {
    if (!window || !IsWindow(window) || !GetWindowRect(window, &value.bounds))
        return Fail(error, L"window identity or geometry is unavailable");
    value.hwnd = window;
    value.generation = agent.DirectUiWindowGeneration(window);
    value.style = static_cast<uint64_t>(GetWindowLongPtrW(window, GWL_STYLE));
    value.exStyle = static_cast<uint64_t>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    value.visible = IsWindowVisible(window) != FALSE;
    value.enabled = IsWindowEnabled(window) != FALSE;
    value.controlId = GetDlgCtrlID(window);
    value.dialogCode = static_cast<uint32_t>(SendMessageW(window, WM_GETDLGCODE, 0, 0));
    if (isButton) value.checked = SendMessageW(window, BM_GETCHECK, 0, 0) != 0;
    if (FluentShell::EqualsIgnoreCase(ClassName(window), L"BitmapSwitchClass"))
        value.checked = (static_cast<DWORD>(value.style) & WS_TABSTOP) != 0;
    if (isProgress) {
        PBRANGE range{};
        SendMessageW(window, PBM_GETRANGE, FALSE, reinterpret_cast<LPARAM>(&range));
        value.minimum = range.iLow;
        value.maximum = range.iHigh;
        value.position = static_cast<int>(SendMessageW(window, PBM_GETPOS, 0, 0));
        value.indeterminate = (static_cast<DWORD>(value.style) & PBS_MARQUEE) != 0;
        if (value.maximum <= value.minimum || value.position < value.minimum ||
            value.position > value.maximum)
            return Fail(error, L"progress backing range or position is invalid");
    }
    const int textLength = GetWindowTextLengthW(window);
    if (textLength < 0 || textLength > static_cast<int>(kMaxTextChars))
        return Fail(error, L"window text exceeds bound");
    std::wstring text(static_cast<size_t>(textLength) + 1, L'\0');
    const int copied = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    if (copied < 0) return Fail(error, L"window text is unavailable");
    text.resize(static_cast<size_t>(copied));
    value.text = std::move(text);
    // BCM_GETNOTE only answers on command-link buttons; it is a no-op message
    // for every other class, so asking is always safe.
    if (isButton) {
        const LRESULT noteLength = SendMessageW(window, BCM_GETNOTELENGTH, 0, 0);
        if (noteLength > 0 && noteLength <= static_cast<LRESULT>(kMaxTextChars)) {
            std::wstring note(static_cast<size_t>(noteLength) + 1, L'\0');
            DWORD noteCapacity = static_cast<DWORD>(note.size());
            if (SendMessageW(window, BCM_GETNOTE,
                    reinterpret_cast<WPARAM>(&noteCapacity),
                    reinterpret_cast<LPARAM>(note.data()))) {
                note.resize(static_cast<size_t>(
                    std::find(note.begin(), note.end(), L'\0') - note.begin()));
                value.note = std::move(note);
            }
        }
    }
    if (captureBitmap && !CaptureWindowClientPixels(window, value, error)) return false;
    return value.generation != 0 || Fail(error, L"window generation is unavailable");
}

bool ReadBasicWindowEvidence(
    SourceThreadAgent& agent,
    HWND window,
    DirectUiWindowEvidence& value,
    std::wstring& error) {
    if (!window || !IsWindow(window) || !GetWindowRect(window, &value.bounds))
        return Fail(error, L"window identity or geometry is unavailable");
    value.hwnd = window;
    value.generation = agent.DirectUiWindowGeneration(window);
    value.style = static_cast<uint64_t>(GetWindowLongPtrW(window, GWL_STYLE));
    value.exStyle = static_cast<uint64_t>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    value.visible = IsWindowVisible(window) != FALSE;
    value.enabled = IsWindowEnabled(window) != FALSE;
    value.controlId = GetDlgCtrlID(window);
    return value.generation != 0 || Fail(error, L"window generation is unavailable");
}

// Kinds whose UIA provider publishes one element per item. Their items are
// absorbed into the owner slot instead of each becoming its own slot, so the
// engine needs a native census to hold the item count to an equality.
bool IsDirectUiCompositeKind(ControlKind kind) noexcept {
    switch (kind) {
    case ControlKind::ComboBox:
    case ControlKind::ListBox:
    case ControlKind::ListView:
    case ControlKind::TabControl:
    case ControlKind::StatusBar:
    case ControlKind::Toolbar:
        return true;
    default:
        return false;
    }
}

// The item collection each composite adapter fills. SIZE_MAX means the node is
// not a composite, which is what rejects an unexpected absorption attempt.
size_t DirectUiNativeItemCount(const ControlNode& node) noexcept {
    switch (node.kind) {
    case ControlKind::ListView:
        return node.rows.size();
    case ControlKind::Toolbar:
        return node.toolbarItems.size();
    case ControlKind::ComboBox:
    case ControlKind::ListBox:
    case ControlKind::TabControl:
    case ControlKind::StatusBar:
        return node.items.size();
    default:
        return static_cast<size_t>(-1);
    }
}

// UIA descendants an already-slotted control legitimately owns, so the child is
// absorbed into that slot instead of becoming a slot of its own. Absorption is
// deliberately narrow: the point of it is that the owner's projected control
// already represents and routes the child, so any role the owner cannot
// represent must reject the surface rather than silently disappear. The owner's
// own container element is never in this set, which is what keeps exactly one
// retained semantic per backing HWND.
bool IsAbsorbedChildControlType(ControlKind owner, int controlType) noexcept {
    // Scroll chrome belongs to whichever control draws it, and the projected
    // WinUI control brings its own scrolling.
    if (controlType == UIA_ScrollBarControlTypeId ||
        controlType == UIA_ThumbControlTypeId)
        return true;
    switch (owner) {
    case ControlKind::ComboBox:
        // The drop-down button, the edit field of an editable combo, the list
        // popup, and every item in it.
        return controlType == UIA_ListItemControlTypeId ||
            controlType == UIA_EditControlTypeId ||
            controlType == UIA_ListControlTypeId ||
            controlType == UIA_ButtonControlTypeId;
    case ControlKind::ListBox:
        return controlType == UIA_ListItemControlTypeId;
    case ControlKind::ListView:
        return controlType == UIA_ListItemControlTypeId ||
            controlType == UIA_DataItemControlTypeId ||
            controlType == UIA_HeaderControlTypeId ||
            controlType == UIA_HeaderItemControlTypeId ||
            controlType == UIA_TextControlTypeId ||
            controlType == UIA_ImageControlTypeId;
    case ControlKind::TabControl:
        return controlType == UIA_TabItemControlTypeId;
    case ControlKind::StatusBar:
        return controlType == UIA_TextControlTypeId ||
            controlType == UIA_SeparatorControlTypeId ||
            controlType == UIA_ImageControlTypeId;
    case ControlKind::Toolbar:
        return controlType == UIA_ButtonControlTypeId ||
            controlType == UIA_SeparatorControlTypeId ||
            controlType == UIA_ImageControlTypeId;
    case ControlKind::SysLink:
        // The control element itself is the Hyperlink; the text run below it is
        // the same label the slot already publishes.
        return controlType == UIA_TextControlTypeId;
    case ControlKind::Edit:
    case ControlKind::Password:
        // A multiline edit publishes its own scroll chrome, handled above.
        return false;
    default:
        return false;
    }
}

// Behavior an absorbed child may carry. Selection, invocation, and toggling are
// exactly what the owner slot's own routes dispatch; anything else (a writable
// Value on an item, say) would be an unrepresented mutation path.
constexpr uint32_t kAbsorbedChildPatternMask = DirectUiPatternInvoke |
    DirectUiPatternToggle | DirectUiPatternSelectionItem |
    DirectUiPatternExpandCollapse;

bool CaptureDirectUiBootstrapCore(
    SourceThreadAgent& agent,
    DirectUiBootstrapEvidence& evidence,
    std::wstring& error) {
    const HWND root = agent.Root();
    DWORD rootProcess = 0;
    const DWORD rootThread = GetWindowThreadProcessId(root, &rootProcess);
    if (!rootThread || rootThread != agent.ThreadId() || rootProcess != GetCurrentProcessId())
        return Fail(error, L"bootstrap: root thread or process identity mismatch");

    evidence = {};
    evidence.rootClass = ClassName(root);
    if (evidence.rootClass.empty())
        return Fail(error, L"bootstrap: root class is unavailable");
    evidence.native.mutationEpoch = agent.MutationEpoch();
    DWORD cloakReasons = 0;
    if (FAILED(DwmGetWindowAttribute(root, DWMWA_CLOAKED,
            &cloakReasons, sizeof(cloakReasons))))
        return Fail(error, L"bootstrap: native cloak state is unavailable");
    evidence.native.cloaked = (cloakReasons & DWM_CLOAKED_APP) != 0;
    evidence.native.dpi = GetDpiForWindow(root);
    if (!evidence.native.dpi ||
        !ReadWindowEvidence(agent, root, false, evidence.native.root, error)) return false;
    evidence.native.ownerHwnd = GetWindow(root, GW_OWNER);
    evidence.native.title = evidence.native.root.text;
    if (!GetClientRect(root, &evidence.native.clientBounds))
        return Fail(error, L"bootstrap: root client geometry unavailable");
    evidence.native.clientOriginScreen = { 0, 0 };
    if (!ClientToScreen(root, &evidence.native.clientOriginScreen))
        return Fail(error, L"bootstrap: root client origin unavailable");

    std::vector<HWND> descendants;
    EnumChildWindows(root, [](HWND window, LPARAM raw) -> BOOL {
        auto& values = *reinterpret_cast<std::vector<HWND>*>(raw);
        values.push_back(window);
        return values.size() <= 256;
    }, reinterpret_cast<LPARAM>(&descendants));
    if (descendants.empty() || descendants.size() > 256)
        return Fail(error, L"bootstrap: descendant count is outside the generic bound");

    for (HWND child : descendants) {
        DWORD process = 0;
        if (GetWindowThreadProcessId(child, &process) != rootThread || process != rootProcess)
            return Fail(error, L"bootstrap: descendant escaped the source thread/process");
        DirectUiBootstrapWindowEvidence value;
        value.parent = GetParent(child);
        value.className = ClassName(child);
        if (value.className.empty() ||
            !ReadBasicWindowEvidence(agent, child, value.window, error)) return false;
        // A composite control's own implementation child is absorbed by its owner
        // slot rather than projected. The probe interrogates the parent with
        // messages, so it can only run here, on the control's own GUI thread. It
        // is evaluated for hidden children too: a closed ComboBox keeps its list
        // child hidden.
        value.compositeImplementationChild = IsCompositeImplementationChild(child);
        if (value.window.visible) {
            std::wstring ignored;
            value.controlSupported = ClassifyControl(
                child, value.controlKind, ignored);
            // Item census for composite controls, read through the control's own
            // adapter here so profile generation on the supervisor thread can pin
            // an equality against the UIA item count without a cross-thread send.
            // A rejected read leaves SIZE_MAX, which refuses the absorption.
            if (value.controlSupported && !value.compositeImplementationChild &&
                IsDirectUiCompositeKind(value.controlKind)) {
                ControlNode probe;
                std::wstring ignoredDetail;
                if (CaptureDirectUiSlotNode(child, value.controlKind, probe, ignoredDetail))
                    value.nativeItemCount = DirectUiNativeItemCount(probe);
            }
        }
        evidence.native.implementationWindows.push_back({
            value.window.hwnd, value.window.generation, value.parent,
            value.className, value.window.visible });
        if (FluentShell::EqualsIgnoreCase(value.className, L"DirectUIHWND") &&
            value.window.visible && value.parent == root) {
            if (evidence.native.directUi.hwnd ||
                !ReadWindowEvidence(agent, child, false, evidence.native.directUi, error))
                return Fail(error, L"bootstrap: DirectUI anchor is missing or ambiguous");
        }
        if (FluentShell::EqualsIgnoreCase(value.className, L"#32770") &&
            value.window.visible) evidence.native.pageHosts.push_back(child);
        if (FluentShell::EqualsIgnoreCase(value.className, L"Static") &&
            value.window.visible) evidence.native.pageStatics.push_back(child);
        evidence.descendants.push_back(std::move(value));
    }
    if (!evidence.native.directUi.hwnd)
        return Fail(error, L"bootstrap: one visible root DirectUI anchor is required");
    if (FluentShell::EqualsIgnoreCase(evidence.rootClass, L"NativeHWNDHost")) {
        evidence.native.propertySheetPageHwnd =
            CurrentPropertySheetPage(root, evidence.native.pageHosts);
    }
    evidence.native.mutationEpoch = agent.MutationEpoch();
    evidence.native.lastMutationHwnd = agent.LastMutationHwnd();
    evidence.native.lastMutationMessage = agent.LastMutationMessage();
    return true;
}

std::wstring CanonicalPath(std::wstring_view path) {
    const std::wstring owned(path);
    HANDLE file = CreateFileW(owned.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::wstring result(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(
        file, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(file);
    if (length == 0 || length >= result.size()) return {};
    result.resize(length);
    if (result.starts_with(L"\\\\?\\")) result.erase(0, 4);
    return result;
}

bool ExactFileVersion(std::wstring_view path, const DirectUiWindowProfile& profile) {
    const std::wstring owned(path);
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(owned.c_str(), &ignored);
    if (!size || size > 1024 * 1024) return false;
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(owned.c_str(), 0, size, data.data())) return false;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
        !info || infoSize < sizeof(*info) || info->dwSignature != VS_FFI_SIGNATURE) return false;
    return HIWORD(info->dwFileVersionMS) == profile.fileVersion[0] &&
        LOWORD(info->dwFileVersionMS) == profile.fileVersion[1] &&
        HIWORD(info->dwFileVersionLS) == profile.fileVersion[2] &&
        LOWORD(info->dwFileVersionLS) == profile.fileVersion[3];
}


std::wstring BstrValue(BSTR raw) {
    std::wstring value = raw ? std::wstring(raw, SysStringLen(raw)) : L"";
    if (raw) SysFreeString(raw);
    return value;
}

// Accepts a verified certificate chain when any certificate in it names the
// Microsoft Windows / Microsoft Corporation signer.
bool ChainNamesMicrosoftWindows(const CRYPT_PROVIDER_DATA* provider) {
    if (!provider) return false;
    const auto* signer = WTHelperGetProvSignerFromChain(
        const_cast<CRYPT_PROVIDER_DATA*>(provider), 0, FALSE, 0);
    if (!signer) return false;
    for (DWORD index = 0; index < signer->csCertChain; ++index) {
        const auto certificate = signer->pasCertChain[index].pCert;
        if (!certificate) continue;
        const DWORD length = CertGetNameStringW(certificate,
            CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
        if (length <= 1 || length > 1024) continue;
        std::wstring name(length, L'\0');
        if (CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                0, nullptr, name.data(), length) == length) {
            name.resize(length - 1);
            if (IsMicrosoftWindowsSignerName(name)) return true;
        }
    }
    return false;
}

// Runs one WinVerifyTrust pass and reports whether the verified chain names
// the Microsoft Windows signer. State is always closed.
bool VerifyTrustOnce(
    WINTRUST_DATA& trust,
    GUID& policy,
    bool& microsoftSigned,
    LONG& status) {
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    status = WinVerifyTrust(nullptr, &policy, &trust);
    microsoftSigned = false;
    if (status == ERROR_SUCCESS && trust.hWVTStateData) {
        const auto* provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
        microsoftSigned = ChainNamesMicrosoftWindows(provider);
    }
    const LONG closeStatus = [&]() {
        WINTRUST_DATA closing = trust;
        closing.dwStateAction = WTD_STATEACTION_CLOSE;
        return WinVerifyTrust(nullptr, &policy, &closing);
    }();
    (void)closeStatus;
    return status == ERROR_SUCCESS;
}

// Verifies a System32 executable that may be embedded-signed or catalog-signed.
// Catalog-signed binaries (the common case for inbox Windows tools) need an
// explicit catalog lookup: enumerate the security catalogs that contain the
// file's hash and verify the signature on each until one names Microsoft.
bool VerifyMicrosoftSignature(std::wstring_view path, std::wstring& error) {
    const std::wstring owned(path);

    // First try embedded Authenticode.
    WINTRUST_FILE_INFO file{ sizeof(file) };
    file.pcwszFilePath = owned.c_str();
    WINTRUST_DATA trust{ sizeof(trust) };
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = 0;
    bool microsoftSigned = false;
    if (VerifyTrustOnce(trust, policy, microsoftSigned, status)) {
        if (microsoftSigned) return true;
        // Embedded signature verified but the signer is not Microsoft; that is
        // a definitive answer, not a catalog case.
        return Fail(error, L"process: trusted signature has no Microsoft Windows signer");
    }

    // Catalog lookup: find catalogs whose members hash to this file.
    HCATADMIN admin = nullptr;
    if (!CryptCATAdminAcquireContext(&admin, nullptr, 0)) {
        return Fail(error, L"process: catalog administrator context unavailable");
    }
    bool verified = false;
    HANDLE fileHandle = INVALID_HANDLE_VALUE;
    do {
        fileHandle = CreateFileW(owned.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fileHandle == INVALID_HANDLE_VALUE) break;
        BYTE hash[64]{};
        DWORD hashSize = sizeof(hash);
        if (!CryptCATAdminCalcHashFromFileHandle(fileHandle, &hashSize, hash, 0)) {
            break;
        }
        std::wstring tag(hashSize * 2, L'\0');
        for (DWORD index = 0; index < hashSize; ++index) {
            swprintf_s(&tag[index * 2], 3, L"%02X", hash[index]);
        }

        // Enumerate every catalog containing this member hash and verify each.
        // The member file handle lets WinVerifyTrust recompute the hash itself,
        // which sidesteps tag-encoding differences between catalog versions.
        HCATINFO current = nullptr;
        while (!verified && (current = CryptCATAdminEnumCatalogFromHash(
                   admin, hash, hashSize, 0, &current)) != nullptr) {
            CATALOG_INFO foundInfo{};
            foundInfo.cbStruct = sizeof(foundInfo);
            if (CryptCATCatalogInfoFromContext(current, &foundInfo, 0)) {
                WINTRUST_DATA catalogTrust{ sizeof(catalogTrust) };
                WINTRUST_CATALOG_INFO catalogChoice{ sizeof(catalogChoice) };
                catalogChoice.pcwszCatalogFilePath = foundInfo.wszCatalogFile;
                catalogChoice.pcwszMemberFilePath = owned.c_str();
                catalogChoice.pcwszMemberTag = tag.c_str();
                catalogChoice.hMemberFile = fileHandle;
                catalogChoice.pbCalculatedFileHash = hash;
                catalogChoice.cbCalculatedFileHash = hashSize;
                catalogTrust.dwUIChoice = WTD_UI_NONE;
                catalogTrust.fdwRevocationChecks = WTD_REVOKE_NONE;
                catalogTrust.dwUnionChoice = WTD_CHOICE_CATALOG;
                catalogTrust.pCatalog = &catalogChoice;
                catalogTrust.dwProvFlags = WTD_REVOCATION_CHECK_NONE;
                LONG catalogStatus = 0;
                bool catalogMicrosoft = false;
                if (VerifyTrustOnce(catalogTrust, policy, catalogMicrosoft, catalogStatus) &&
                    catalogMicrosoft) {
                    verified = true;
                }
            }
        }
        // Enumeration releases the previous context when it advances. If we
        // stopped early after a successful verification, release the current
        // context explicitly as required by the catalog API contract.
        if (current) CryptCATAdminReleaseCatalogContext(admin, current, 0);
    } while (false);
    if (fileHandle != INVALID_HANDLE_VALUE) CloseHandle(fileHandle);
    CryptCATAdminReleaseContext(admin, 0);
    if (!verified) {
        return Fail(error, L"process: catalog signature has no Microsoft Windows signer");
    }
    return true;
}

bool RuntimeId(IUIAutomationElement* element, std::vector<int>& result) {
    SAFEARRAY* raw = nullptr;
    if (FAILED(element->GetRuntimeId(&raw)) || !raw) return false;
    LONG lower = 0;
    LONG upper = -1;
    const bool bounded = SafeArrayGetDim(raw) == 1 &&
        SUCCEEDED(SafeArrayGetLBound(raw, 1, &lower)) &&
        SUCCEEDED(SafeArrayGetUBound(raw, 1, &upper)) && upper >= lower && upper - lower < 32;
    if (bounded) {
        result.reserve(static_cast<size_t>(upper - lower + 1));
        for (LONG index = lower; index <= upper; ++index) {
            int value = 0;
            if (FAILED(SafeArrayGetElement(raw, &index, &value))) {
                result.clear();
                break;
            }
            result.push_back(value);
        }
    }
    SafeArrayDestroy(raw);
    return bounded && !result.empty();
}

bool ReadSemantic(
    IUIAutomationElement* element,
    bool allowOffscreen,
    DirectUiSemanticEvidence& value) {
    BSTR raw = nullptr;
    if (FAILED(element->get_CurrentAutomationId(&raw))) return false;
    value.semanticKey = BstrValue(raw);
    raw = nullptr;
    if (FAILED(element->get_CurrentName(&raw))) return false;
    value.name = BstrValue(raw);
    raw = nullptr;
    if (FAILED(element->get_CurrentHelpText(&raw))) return false;
    value.helpText = BstrValue(raw);
    raw = nullptr;
    if (FAILED(element->get_CurrentAccessKey(&raw))) return false;
    value.accessKey = BstrValue(raw);
    raw = nullptr;
    if (FAILED(element->get_CurrentClassName(&raw))) return false;
    value.className = BstrValue(raw);
    raw = nullptr;
    if (FAILED(element->get_CurrentFrameworkId(&raw))) return false;
    value.frameworkId = BstrValue(raw);
    std::wstring normalized;
    if (!NormalizeDirectUiEvidenceText(value.semanticKey, normalized)) return false;
    value.semanticKey = std::move(normalized);
    if (!NormalizeDirectUiEvidenceText(value.name, normalized)) return false;
    value.name = std::move(normalized);
    if (!NormalizeDirectUiEvidenceText(value.helpText, normalized)) return false;
    value.helpText = std::move(normalized);
    if (!NormalizeDirectUiEvidenceText(value.accessKey, normalized)) return false;
    value.accessKey = std::move(normalized);
    if (!NormalizeDirectUiEvidenceText(value.className, normalized)) return false;
    value.className = std::move(normalized);
    if (!NormalizeDirectUiEvidenceText(value.frameworkId, normalized)) return false;
    value.frameworkId = std::move(normalized);
    UIA_HWND hwnd = nullptr;
    BOOL focusable = FALSE;
    BOOL enabled = FALSE;
    BOOL offscreen = TRUE;
    CONTROLTYPEID type = 0;
    RECT rect{};
    if (FAILED(element->get_CurrentNativeWindowHandle(&hwnd)) ||
        FAILED(element->get_CurrentControlType(&type)) ||
        FAILED(element->get_CurrentIsKeyboardFocusable(&focusable)) ||
        FAILED(element->get_CurrentIsEnabled(&enabled)) ||
        FAILED(element->get_CurrentIsOffscreen(&offscreen)) ||
        FAILED(element->get_CurrentBoundingRectangle(&rect)) ||
        (offscreen && !allowOffscreen)) return false;
    value.backingHwnd = reinterpret_cast<HWND>(hwnd);
    value.controlType = type;
    value.focusable = focusable != FALSE;
    value.enabled = enabled != FALSE;
    value.offscreen = offscreen != FALSE;
    value.bounds = rect;
    return RuntimeId(element, value.runtimeId);
}

bool ReadPatterns(IUIAutomationElement* element, DirectUiSemanticEvidence& semantic) {
    semantic.actionable = false;
    semantic.patternMask = DirectUiPatternNone;
    semantic.capabilityMask = DirectUiCapabilityNone;
    struct PatternProperty final { PROPERTYID property; uint32_t bit; };
    constexpr PatternProperty properties[] = {
        { UIA_IsInvokePatternAvailablePropertyId, DirectUiPatternInvoke },
        { UIA_IsTogglePatternAvailablePropertyId, DirectUiPatternToggle },
        { UIA_IsExpandCollapsePatternAvailablePropertyId, DirectUiPatternExpandCollapse },
        { UIA_IsSelectionItemPatternAvailablePropertyId, DirectUiPatternSelectionItem },
    };
    for (const auto& entry : properties) {
        VARIANT value{};
        VariantInit(&value);
        const HRESULT result = element->GetCurrentPropertyValue(entry.property, &value);
        if (FAILED(result) || value.vt != VT_BOOL) {
            VariantClear(&value);
            return false;
        }
        if (value.boolVal == VARIANT_TRUE) {
            semantic.patternMask |= entry.bit;
            semantic.actionable = true;
        }
        VariantClear(&value);
    }

    // Structural capabilities. A container advertising Selection/Text/Grid does
    // not by itself gain a mutation route, so these are pinned as shape evidence
    // in capabilityMask and never widen patternMask (which is what makes an
    // element count as actionable).
    struct CapabilityProperty final { PROPERTYID property; uint32_t bit; };
    constexpr CapabilityProperty capabilities[] = {
        { UIA_IsSelectionPatternAvailablePropertyId, DirectUiCapabilitySelection },
        { UIA_IsTextPatternAvailablePropertyId, DirectUiCapabilityText },
        { UIA_IsRangeValuePatternAvailablePropertyId, DirectUiCapabilityRangeValue },
        { UIA_IsScrollPatternAvailablePropertyId, DirectUiCapabilityScroll },
        { UIA_IsGridPatternAvailablePropertyId, DirectUiCapabilityGrid },
        { UIA_IsTablePatternAvailablePropertyId, DirectUiCapabilityTable },
    };
    for (const auto& entry : capabilities) {
        VARIANT value{};
        VariantInit(&value);
        const HRESULT result = element->GetCurrentPropertyValue(entry.property, &value);
        if (FAILED(result) || value.vt != VT_BOOL) {
            VariantClear(&value);
            return false;
        }
        if (value.boolVal == VARIANT_TRUE) semantic.capabilityMask |= entry.bit;
        VariantClear(&value);
    }

    if ((semantic.patternMask & DirectUiPatternToggle) != 0) {
        ComPtr<IUIAutomationTogglePattern> toggle;
        ToggleState state = ToggleState_Indeterminate;
        if (FAILED(element->GetCurrentPatternAs(
                UIA_TogglePatternId, IID_PPV_ARGS(&toggle))) || !toggle ||
            FAILED(toggle->get_CurrentToggleState(&state))) return false;
        semantic.toggleState = static_cast<int>(state);
    }

    // DirectUI exposes a read-only Value pattern on presentation-only Image
    // and Text elements. Availability alone is therefore not an action; only
    // a writable Value pattern can mutate canonical state.
    VARIANT available{};
    VariantInit(&available);
    const HRESULT valueResult = element->GetCurrentPropertyValue(
        UIA_IsValuePatternAvailablePropertyId, &available);
    if (FAILED(valueResult) || available.vt != VT_BOOL) {
        VariantClear(&available);
        return false;
    }
    if (available.boolVal == VARIANT_TRUE) {
        semantic.patternMask |= DirectUiPatternValue;
        ComPtr<IUIAutomationValuePattern> valuePattern;
        BOOL readOnly = TRUE;
        if (FAILED(element->GetCurrentPatternAs(
                UIA_ValuePatternId, IID_PPV_ARGS(&valuePattern))) ||
            !valuePattern || FAILED(valuePattern->get_CurrentIsReadOnly(&readOnly))) {
            VariantClear(&available);
            return false;
        }
        semantic.valueReadOnly = readOnly != FALSE;
        semantic.actionable = semantic.actionable || readOnly == FALSE;
    }
    VariantClear(&available);
    return true;
}

// Root nonclient chrome (TitleBar, SystemMenuBar and its items) belongs to the
// OS window frame, not to the projected page. The filter is an exact-shape
// predicate, never a blanket ignore: an unexpected focusable/actionable
// element outside these signatures still rejects the surface.
bool IsRootNonClientChrome(const DirectUiSemanticEvidence& semantic) {
    const bool bare = semantic.backingHwnd == nullptr && semantic.className.empty() &&
        semantic.frameworkId.empty();
    if (!bare) return false;
    if (semantic.controlType == UIA_TitleBarControlTypeId && semantic.semanticKey == L"TitleBar")
        return true;
    if (semantic.controlType == UIA_MenuBarControlTypeId &&
        semantic.semanticKey == L"SystemMenuBar" && !semantic.actionable)
        return true;
    if (semantic.controlType == UIA_ButtonControlTypeId && !semantic.focusable &&
        semantic.actionable &&
        (semantic.semanticKey == L"Close" || semantic.semanticKey == L"Minimize" ||
            semantic.semanticKey == L"Maximize" || semantic.semanticKey == L"Restore" ||
            semantic.semanticKey == L"Help"))
        return true;
    // System menu items are MenuItems parented under SystemMenuBar and always
    // sit outside the DirectUI host bounds.
    if (semantic.controlType == UIA_MenuItemControlTypeId &&
        semantic.semanticKey.empty())
        return true;
    return false;
}

bool CaptureUia(
    HWND rootHwnd,
    HWND directUiHwnd,
    DWORD processId,
    bool allowDeclaredOffscreen,
    const std::vector<std::wstring>& virtualAutomationIds,
    const std::vector<HWND>& backingWindows,
    bool retainAllSemantics,
    DirectUiUiaEvidence& evidence,
    std::wstring& error) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized))
        return Fail(error, L"MTA COM initialization failed");
    struct Uninitialize final { bool active; ~Uninitialize() { if (active) CoUninitialize(); } } scope{
        SUCCEEDED(initialized) };
    ComPtr<IUIAutomation> automation;
    HRESULT result = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    if (FAILED(result) || !automation) return Fail(error, L"UIA8 client creation failed");
    ComPtr<IUIAutomation6> bounded;
    if (FAILED(automation.As(&bounded)) || !bounded ||
        FAILED(bounded->put_ConnectionTimeout(900)) ||
        FAILED(bounded->put_TransactionTimeout(900)))
        return Fail(error, L"bounded UIA timeouts are unavailable");
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(rootHwnd, &root)) || !root)
        return Fail(error, L"root UIA provider is unavailable");
    int rootPid = 0;
    UIA_HWND rootBacking = nullptr;
    if (FAILED(root->get_CurrentProcessId(&rootPid)) || rootPid != static_cast<int>(processId) ||
        FAILED(root->get_CurrentNativeWindowHandle(&rootBacking)) ||
        reinterpret_cast<HWND>(rootBacking) != rootHwnd)
        return Fail(error, L"root UIA identity mismatch");
    evidence.rootHwnd = rootHwnd;
    ComPtr<IUIAutomationElement> directUi;
    int directUiPid = 0;
    UIA_HWND directUiBacking = nullptr;
    if (FAILED(automation->ElementFromHandle(directUiHwnd, &directUi)) || !directUi ||
        FAILED(directUi->get_CurrentProcessId(&directUiPid)) ||
        directUiPid != static_cast<int>(processId) ||
        FAILED(directUi->get_CurrentNativeWindowHandle(&directUiBacking)) ||
        reinterpret_cast<HWND>(directUiBacking) != directUiHwnd)
        return Fail(error, L"DirectUI anchor UIA identity mismatch");
    evidence.directUiHwnd = directUiHwnd;
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation->CreateTrueCondition(&condition)) || !condition)
        return Fail(error, L"UIA descendant condition failed");
    ComPtr<IUIAutomationElementArray> descendants;
    if (FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &descendants)) || !descendants)
        return Fail(error, L"UIA descendant enumeration failed");
    int count = 0;
    if (FAILED(descendants->get_Length(&count)) || count < 1 || count > kMaxUiaDescendants)
        return Fail(error, L"UIA descendant count is outside the exact bound");
    for (int index = 0; index < count; ++index) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(descendants->GetElement(index, &element)) || !element)
            return Fail(error, L"UIA descendant is unavailable");
        int pid = 0;
        BOOL offscreen = TRUE;
        if (FAILED(element->get_CurrentProcessId(&pid)) ||
            (pid != 0 && pid != static_cast<int>(processId)) ||
            FAILED(element->get_CurrentIsOffscreen(&offscreen)))
            return Fail(error, L"UIA descendant process or visibility is unavailable");
        if (offscreen && !allowDeclaredOffscreen) continue;
        DirectUiSemanticEvidence semantic;
        if (!ReadSemantic(element.Get(), allowDeclaredOffscreen, semantic))
            return Fail(error, L"visible UIA semantic evidence read failed");
        if (!ReadPatterns(element.Get(), semantic))
            return Fail(error, L"visible UIA action-pattern evidence read failed");
        const bool actionable = semantic.actionable || semantic.focusable ||
            semantic.controlType == UIA_ButtonControlTypeId ||
            semantic.controlType == UIA_HyperlinkControlTypeId ||
            semantic.controlType == UIA_MenuItemControlTypeId;
        const bool declaredVirtual = std::find(virtualAutomationIds.begin(),
            virtualAutomationIds.end(), semantic.semanticKey) != virtualAutomationIds.end();
        const bool declaredBacking = semantic.backingHwnd &&
            std::find(backingWindows.begin(), backingWindows.end(),
                semantic.backingHwnd) != backingWindows.end();
        const bool retain = retainAllSemantics || declaredVirtual || declaredBacking ||
            (!semantic.offscreen && actionable);
        if (retain &&
            !IsRootNonClientChrome(semantic))
            evidence.semantics.push_back(std::move(semantic));
    }
    return true;
}

const DirectUiSemanticEvidence* FindSemanticByAutomationId(
    const DirectUiUiaEvidence& evidence,
    std::wstring_view automationId) noexcept {
    const auto found = std::find_if(evidence.semantics.begin(), evidence.semantics.end(),
        [&](const auto& value) { return value.semanticKey == automationId; });
    return found == evidence.semantics.end() ? nullptr : &*found;
}

const DirectUiSemanticEvidence* FindSemanticByBacking(
    const DirectUiUiaEvidence& evidence,
    HWND backing) noexcept {
    const auto found = std::find_if(evidence.semantics.begin(), evidence.semantics.end(),
        [&](const auto& value) { return value.backingHwnd == backing; });
    return found == evidence.semantics.end() ? nullptr : &*found;
}

bool RectNear(const RECT& left, const RECT& right, LONG tolerance = 2) noexcept {
    return std::abs(left.left - right.left) <= tolerance &&
        std::abs(left.top - right.top) <= tolerance &&
        std::abs(left.right - right.right) <= tolerance &&
        std::abs(left.bottom - right.bottom) <= tolerance;
}

bool Inside(const RECT& child, const RECT& parent) noexcept {
    return child.left >= parent.left && child.top >= parent.top &&
        child.right <= parent.right && child.bottom <= parent.bottom;
}

std::wstring DisplayText(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == L'&' && index + 1 < value.size()) {
            if (value[index + 1] != L'&') continue;
            ++index;
        }
        result.push_back(value[index]);
    }
    return result;
}

// The protocol action name a route advertises. A ListView selection is a
// canonical index list rather than a single index, so it carries a different
// name from the other list routes even though both are SelectListItem.
std::wstring_view DirectUiActionName(DirectUiAction action, ControlKind kind) noexcept {
    switch (action) {
    case DirectUiAction::HandoffClick:
    case DirectUiAction::HandoffPropertySheetButton:
    case DirectUiAction::HandoffLinkClick:
        return L"invoke";
    case DirectUiAction::ToggleCheck:
    case DirectUiAction::SelectRadio:
        return L"setCheck";
    case DirectUiAction::SetEditText:
        return L"setText";
    case DirectUiAction::SelectListItem:
        return kind == ControlKind::ListView ? L"setSelection" : L"select";
    case DirectUiAction::SetItemCheck:
        return L"setItemCheck";
    case DirectUiAction::ToolbarCommand:
        return L"toolbarCommand";
    default:
        return {};
    }
}

// A secondary route is advertised only when this revision's own typed state says
// the control actually accepts it: an editable combo box, a checkbox ListView.
// The declared route is the ceiling, never the guarantee.
bool DirectUiSecondaryActionApplies(
    DirectUiAction action,
    const ControlNode& node) noexcept {
    switch (action) {
    case DirectUiAction::SetEditText:
        return (node.kind == ControlKind::Edit || node.kind == ControlKind::Password ||
            (node.kind == ControlKind::ComboBox && node.editable)) && !node.readOnly;
    case DirectUiAction::SetItemCheck:
        return node.kind == ControlKind::ListView && node.checkBoxes;
    default:
        return false;
    }
}

void SetDirectUiSupportedActions(const DirectUiSlot& slot, ControlNode& node) {
    node.supportedActions.clear();
    if (!node.enabled) return;
    const std::wstring_view primary = DirectUiActionName(slot.action, slot.kind);
    if (!primary.empty()) node.supportedActions.emplace_back(primary);
    const std::wstring_view secondary =
        DirectUiActionName(slot.secondaryAction, slot.kind);
    if (!secondary.empty() && secondary != primary &&
        DirectUiSecondaryActionApplies(slot.secondaryAction, node))
        node.supportedActions.emplace_back(secondary);
}

RECT ClientRectFor(const RECT& screen, const POINT& origin) noexcept {
    return { screen.left - origin.x, screen.top - origin.y,
        screen.right - origin.x, screen.bottom - origin.y };
}

bool LoadTrustedIconResource(
    std::wstring_view imagePath,
    int resourceId,
    int width,
    int height,
    ControlNode& node,
    std::wstring& error) {
    const std::wstring owned(imagePath);
    HMODULE resources = LoadLibraryExW(owned.c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!resources) return Fail(error, L"resource: application image resource module unavailable");
    HICON icon = reinterpret_cast<HICON>(LoadImageW(resources, MAKEINTRESOURCEW(resourceId),
        IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
    bool captured = false;
    if (icon) {
        captured = CaptureOwnedIconPixels(icon, node.imageWidth, node.imageHeight,
            node.imageFormat, node.imageData, error);
        DestroyIcon(icon);
    } else {
        error = L"resource: application icon resource is unavailable";
    }
    FreeLibrary(resources);
    return captured && node.imageWidth == static_cast<uint32_t>(width) &&
        node.imageHeight == static_cast<uint32_t>(height) ||
        Fail(error, L"resource: application icon did not resolve to owned pixels");
}

// Publishes a backing control's adapter-read typed state onto the projected
// node. Only the facets the projected kind actually owns are copied, so a
// ControlNode field some other adapter uses can never leak into a kind that has
// no meaning for it, and the renderer sees exactly the shape the Win32 lane
// already sends for that kind.
bool ApplyDirectUiDetailToNode(
    const DirectUiSlot& slot,
    const DirectUiWindowEvidence& backing,
    ControlNode& node,
    std::wstring& error) {
    if (!slot.captureDetail) return true;
    if (!backing.hasDetail || backing.detail.kind != slot.kind)
        return Fail(error, L"snapshot: DirectUI backing typed state is unavailable");
    const ControlNode& detail = backing.detail;
    switch (slot.kind) {
    case ControlKind::ThreeState:
        // A BS_3STATE box reaches a value the shared bool facet cannot carry.
        node.checked = detail.checked;
        break;
    case ControlKind::StaticIcon:
        if (detail.imageFormat.empty() || detail.imageData.empty())
            return Fail(error, L"snapshot: DirectUI icon pixels are unavailable");
        node.imageWidth = detail.imageWidth;
        node.imageHeight = detail.imageHeight;
        node.imageFormat = detail.imageFormat;
        node.imageData = detail.imageData;
        break;
    case ControlKind::SysLink:
        // items carries the parsed link caption; the flattened markup stays in
        // text, exactly as the Win32 lane publishes it.
        node.text = detail.text;
        node.items = detail.items;
        node.automationName = detail.automationName;
        // A link item can be disabled while its host HWND is not. Narrowing is
        // monotone, so it cannot contradict the traversal state computed above.
        if (!detail.enabled) {
            node.enabled = false;
            node.tabStop = false;
        }
        break;
    case ControlKind::Edit:
    case ControlKind::Password:
        node.text = detail.text;
        node.readOnly = detail.readOnly;
        node.multiline = detail.multiline;
        node.selectionStart = detail.selectionStart;
        node.selectionLength = detail.selectionLength;
        break;
    case ControlKind::ComboBox:
        node.text = detail.text;
        node.items = detail.items;
        node.selectedIndex = detail.selectedIndex;
        node.editable = detail.editable;
        node.readOnly = detail.readOnly;
        break;
    case ControlKind::ListBox:
        node.items = detail.items;
        node.selectedIndex = detail.selectedIndex;
        node.selectedIndices = detail.selectedIndices;
        node.multiSelect = detail.multiSelect;
        break;
    case ControlKind::ListView:
        node.items = detail.items;
        node.columns = detail.columns;
        node.columnWidths = detail.columnWidths;
        node.rows = detail.rows;
        node.columnHeadersVisible = detail.columnHeadersVisible;
        node.selectedIndex = detail.selectedIndex;
        node.selectedIndices = detail.selectedIndices;
        node.focusedIndex = detail.focusedIndex;
        node.multiSelect = detail.multiSelect;
        node.checkBoxes = detail.checkBoxes;
        node.checkedIndices = detail.checkedIndices;
        break;
    case ControlKind::TabControl:
        node.items = detail.items;
        node.itemRects = detail.itemRects;
        node.selectedIndex = detail.selectedIndex;
        break;
    case ControlKind::StatusBar:
        // Parts carry the text; the adapter clears the bar's own caption so it
        // is not duplicated in the name. Mirror what the Win32 lane sends.
        node.items = detail.items;
        node.columnWidths = detail.columnWidths;
        node.text = detail.text;
        node.automationName = detail.automationName;
        break;
    case ControlKind::Toolbar:
        node.toolbarItems = detail.toolbarItems;
        node.text = detail.text;
        node.automationName = detail.automationName;
        break;
    default:
        return Fail(error, L"snapshot: DirectUI slot kind carries no typed state");
    }
    return true;
}

// Builds the composite renderer snapshot from verified evidence. Every node is
// emitted by profile slot order, so no two profiles can drift apart.
bool BuildCompositeSnapshot(
    SourceThreadAgent& agent,
    const DirectUiWindowProfile& profile,
    std::wstring_view imagePath,
    const DirectUiNativeEvidence& native,
    const DirectUiUiaEvidence& uia,
    WindowSnapshot& snapshot,
    std::unordered_map<uint64_t, DirectUiActionBinding>& bindings,
    std::wstring& error) {
    snapshot.surfaceKind = SurfaceKind::Window;
    snapshot.modal = false;
    snapshot.canCancel = std::any_of(
        profile.slots, profile.slots + profile.slotCount,
        [](const DirectUiSlot& slot) { return slot.cancel; });
    snapshot.icon = L"none";
    snapshot.generation = agent.Generation();
    snapshot.nativeHwnd = native.root.hwnd;
    snapshot.ownerHwnd = native.ownerHwnd;
    snapshot.title = native.title;
    snapshot.dpi = native.dpi;
    snapshot.bounds = native.root.bounds;
    snapshot.clientBounds = native.clientBounds;
    snapshot.windowStyle = native.root.style;
    snapshot.windowExStyle = native.root.exStyle;
    snapshot.visible = native.root.visible;
    snapshot.enabled = native.root.enabled;
    snapshot.state = L"normal";
    snapshot.showInTaskbar = native.ownerHwnd == nullptr ||
        (native.root.exStyle & WS_EX_APPWINDOW) != 0;
    snapshot.rtl = (native.root.exStyle & WS_EX_LAYOUTRTL) != 0;
    snapshot.adapterId = profile.adapterId;
    snapshot.pageId = profile.pageId;
    snapshot.menu.clear();
    snapshot.nodes.clear();
    bindings.clear();

    for (size_t index = 0; index < profile.slotCount; ++index) {
        const DirectUiSlot& slot = profile.slots[index];
        if (!slot.project) continue;
        ControlNode node;
        node.nodeId = DirectUiSemanticNodeId(profile.adapterId, slot.semanticKey);
        node.kind = slot.kind;
        node.zIndex = static_cast<int>(snapshot.nodes.size());
        node.adapterId = profile.adapterId;
        node.pageId = profile.pageId;
        node.semanticKey = slot.semanticKey;
        node.sourceKind = slot.virtualSource ? L"uiaVirtual" : L"nativeBacking";
        node.presentationVariant = slot.presentationVariant;
        node.helpText = L"";
        node.accessKey = L"";
        node.isDefault = slot.defaultButton;
        if (slot.virtualSource) {
            const auto* semantic = FindSemanticByAutomationId(uia,
                slot.uiaAutomationId.empty() ? slot.semanticKey : slot.uiaAutomationId);
            if (!semantic) return Fail(error, L"snapshot: projected virtual slot is missing");
            node.generation = native.root.generation;
            node.tabIndex = slot.tabIndex;
            node.rect = ClientRectFor(semantic->bounds, native.clientOriginScreen);
            node.visible = true;
            node.enabled = native.root.enabled && native.directUi.enabled &&
                semantic->enabled;
            node.tabStop = node.enabled && slot.uiaFocusable;
            node.text = semantic->name;
            node.automationName = semantic->name;
            node.helpText = semantic->helpText;
            node.accessKey = semantic->accessKey;
            if (slot.kind == ControlKind::CheckBox && semantic->toggleState >= 0)
                node.checked = semantic->toggleState == ToggleState_On ? 1 : 0;
            if (slot.iconResourceId) {
                if (!LoadTrustedIconResource(imagePath, slot.iconResourceId,
                        slot.iconWidth, slot.iconHeight, node, error)) return false;
            }
        } else {
            const DirectUiWindowEvidence& backing = native.slotWindows[index];
            const auto* semantic = FindSemanticByBacking(uia, backing.hwnd);
            if (!semantic) return Fail(error, L"snapshot: backing UIA semantic is missing");
            node.generation = backing.generation;
            node.hwnd = backing.hwnd;
            node.controlId = backing.controlId;
            node.tabIndex = backing.tabIndex;
            node.rect = ClientRectFor(backing.bounds, native.clientOriginScreen);
            node.style = backing.style;
            node.exStyle = backing.exStyle;
            node.visible = backing.visible;
            node.enabled = native.root.enabled && native.directUi.enabled &&
                backing.enabled && semantic->enabled;
            node.tabStop = node.enabled && backing.tabIndex >= 0;
            node.dialogCode = backing.dialogCode;
            node.text = backing.text.empty() ? semantic->name : backing.text;
            node.automationName = semantic->name;
            node.helpText = semantic->helpText.empty() ? backing.note : semantic->helpText;
            node.accessKey = semantic->accessKey;
            node.checked = backing.checked ? 1 : 0;
            node.groupStart = (backing.style & WS_GROUP) != 0;
            node.minimum = backing.minimum;
            node.maximum = backing.maximum;
            node.position = backing.position;
            node.indeterminate = backing.indeterminate;
            if (slot.captureBitmap) {
                if (backing.imageFormat != L"bgra8-premultiplied" ||
                    backing.imageWidth == 0 || backing.imageHeight == 0 ||
                    backing.imageData.size() !=
                        static_cast<size_t>(backing.imageWidth) * backing.imageHeight * 4)
                    return Fail(error, L"snapshot: native bitmap pixels are missing");
                node.imageWidth = backing.imageWidth;
                node.imageHeight = backing.imageHeight;
                node.imageFormat = backing.imageFormat;
                node.imageData = backing.imageData;
            }
                // Typed state read through this control's own registered
                // adapter. It runs after the common facets so a composite's
                // canonical collection wins over the generic reading, and
                // before the action census because a secondary route is only
                // advertised when this revision's typed state accepts it.
                if (!ApplyDirectUiDetailToNode(slot, backing, node, error)) return false;
        }
        if (!node.tabStop) node.tabIndex = -1;
        SetDirectUiSupportedActions(slot, node);
        if (slot.action != DirectUiAction::None) {
            bindings.emplace(node.nodeId, DirectUiActionBinding{
                slot.virtualSource ? native.root.hwnd : native.slotWindows[index].hwnd,
                slot.virtualSource ? native.root.generation : native.slotWindows[index].generation,
                index, slot.kind, slot.action, slot.secondaryAction, slot.cancel,
                slot.propertySheetButton });
        }
        snapshot.nodes.push_back(std::move(node));
    }
    return true;
}

bool BuildGenericSemanticProfile(
    const DirectUiBootstrapEvidence& native,
    DirectUiUiaEvidence& uia,
    std::wstring_view imagePath,
    std::shared_ptr<DirectUiOwnedProfile>& owned,
    std::wstring& error) {
    if (uia.semantics.empty() ||
        uia.semantics.size() > static_cast<size_t>(kMaxUiaDescendants))
        return Fail(error, L"generic U: semantic count is outside the capability bound");
    auto result = std::make_shared<DirectUiOwnedProfile>();
    const auto own = [&](std::wstring value) -> std::wstring_view {
        result->strings.push_back(std::move(value));
        return result->strings.back();
    };

    for (const auto& descendant : native.descendants) {
        auto found = std::find_if(result->implementationClasses.begin(),
            result->implementationClasses.end(), [&](const auto& contract) {
                return contract.visible == descendant.window.visible &&
                    FluentShell::EqualsIgnoreCase(contract.className, descendant.className);
            });
        if (found == result->implementationClasses.end()) {
            result->implementationClasses.push_back(
                { own(descendant.className), descendant.window.visible, 1 });
        } else {
            ++found->count;
        }
    }

    // Pass one: the absorption census. A control's provider publishes elements
    // for structure the control already carries in its own typed state - list
    // items, a report header, an editable combo's field, scroll chrome. The
    // admission contract requires exactly one retained semantic per backing
    // HWND, so those children are folded into their owner before any slot is
    // built. Absorption is deliberately narrow: the owner's projected control has
    // to actually represent and route what it swallows, so a role the owner
    // cannot represent rejects the whole surface instead of disappearing.
    const auto findDescendant =
        [&](HWND hwnd) -> const DirectUiBootstrapWindowEvidence* {
        if (!hwnd) return nullptr;
        const auto found = std::find_if(native.descendants.begin(),
            native.descendants.end(), [&](const auto& candidate) {
                return candidate.window.hwnd == hwnd;
            });
        return found == native.descendants.end() ? nullptr : &*found;
    };
    std::vector<bool> absorbed(uia.semantics.size(), false);
    std::unordered_map<HWND, size_t> absorbedCounts;
    for (size_t index = 0; index < uia.semantics.size(); ++index) {
        const auto& semantic = uia.semantics[index];
        const auto* backing = findDescendant(semantic.backingHwnd);
        if (!backing) continue;
        const auto* owner = backing;
        if (backing->compositeImplementationChild) {
            // An implementation child, and everything the provider hangs below
            // it, belongs to the composite that created it.
            owner = findDescendant(backing->parent);
            if (!owner || !owner->controlSupported ||
                !IsDirectUiCompositeKind(owner->controlKind)) continue;
        } else if (!backing->controlSupported ||
                   !IsAbsorbedChildControlType(
                       backing->controlKind, semantic.controlType)) {
            continue;
        }
        const uint32_t childPatterns = semantic.patternMask &
            ~(semantic.valueReadOnly ? DirectUiPatternValue : DirectUiPatternNone);
        if ((childPatterns & ~kAbsorbedChildPatternMask) != 0)
            return Fail(error,
                L"generic U: absorbed composite child exposes an unrepresented pattern");
        if (!semantic.offscreen && !Inside(semantic.bounds, native.native.root.bounds))
            return Fail(error,
                L"generic U: absorbed composite child escaped the admitted root");
        absorbed[index] = true;
        if (++absorbedCounts[owner->window.hwnd] > kMaxCompositeItems)
            return Fail(error,
                L"generic U: absorbed composite child census exceeds the bound");
    }

    std::unordered_set<std::wstring> virtualIds;
    std::unordered_set<HWND> backingHwnds;
    std::vector<DirectUiSemanticEvidence> admittedSemantics;
    admittedSemantics.reserve(uia.semantics.size());
    int nextTabIndex = 0;
    size_t semanticOrdinal = 0;
    const bool propertySheetSurface =
        FluentShell::EqualsIgnoreCase(native.rootClass, L"NativeHWNDHost") &&
        native.native.propertySheetPageHwnd != nullptr;
    for (size_t semanticIndex = 0; semanticIndex < uia.semantics.size(); ++semanticIndex) {
        // Folded into an owner slot in pass one. Dropping it from the retained
        // set is what preserves one semantic per backing HWND.
        if (absorbed[semanticIndex]) continue;
        const auto& semantic = uia.semantics[semanticIndex];
        const auto describeSemantic = [&] {
            return L" (type=" + std::to_wstring(semantic.controlType) +
                L", name=" + semantic.name + L", id=" + semantic.semanticKey +
                L", class=" + semantic.className + L", framework=" + semantic.frameworkId +
                L", hwnd=" + std::to_wstring(reinterpret_cast<uintptr_t>(semantic.backingHwnd)) +
                L", patterns=" +
                std::to_wstring(semantic.patternMask) + L", focusable=" +
                std::to_wstring(semantic.focusable) + L", enabled=" +
                std::to_wstring(semantic.enabled) + L", actionable=" +
                std::to_wstring(semantic.actionable) + L")";
        };
        const uint32_t behavioralPatternMask = semantic.patternMask &
            ~(semantic.valueReadOnly ? DirectUiPatternValue : DirectUiPatternNone);
        const bool insideRoot = Inside(semantic.bounds, native.native.root.bounds);
        const bool insideAnchor = Inside(semantic.bounds, native.native.directUi.bounds);
        if (!insideRoot && !semantic.offscreen)
            return Fail(error, L"generic U: semantic geometry escaped the admitted root");

        DirectUiSlot slot;
        slot.semanticKey = own(L"semantic." + std::to_wstring(semanticOrdinal++));
        slot.uiaControlType = semantic.controlType;
        slot.uiaAutomationId = own(semantic.semanticKey);
        slot.uiaClassName = own(semantic.className);
        slot.uiaFrameworkId = own(semantic.frameworkId);
        slot.uiaEnabled = semantic.enabled;
        slot.uiaFocusable = semantic.focusable;
        slot.uiaActionable = semantic.actionable;
        slot.uiaBoundsScope = insideAnchor
            ? DirectUiBoundsScope::Anchor : DirectUiBoundsScope::Root;
        slot.pinUiaPatterns = true;
        slot.uiaPatternMask = semantic.patternMask;
        // Structural capabilities are pinned beside the behavioral patterns but
        // deliberately never merged into them: a Selection or Table provider is a
        // shape, not a mutation route.
        slot.uiaCapabilityMask = semantic.capabilityMask;
        slot.uiaToggleState = semantic.toggleState;
        slot.uiaOffscreen = semantic.offscreen;

        // DirectUI wizards commonly precreate later-page semantics as offscreen
        // provider nodes. Seal their complete UIA shape, but do not project or
        // route them until a subsequent uncloaked page admission sees them.
        if (semantic.offscreen) {
            if (semantic.semanticKey.empty() ||
                !virtualIds.insert(semantic.semanticKey).second)
                return Fail(error,
                    L"generic U: offscreen semantic AutomationId is empty or duplicated");
            slot.project = false;
            slot.virtualSource = true;
            slot.kind = ControlKind::StaticText;
            slot.presentationVariant = L"standard";
            result->slots.push_back(slot);
            admittedSemantics.push_back(semantic);
            continue;
        }

        const auto backing = std::find_if(native.descendants.begin(),
            native.descendants.end(), [&](const auto& candidate) {
                return candidate.window.hwnd == semantic.backingHwnd;
            });
        const bool hasBacking = backing != native.descendants.end();
        const bool nativeStatic = hasBacking && backing->controlSupported &&
            backing->controlKind == ControlKind::StaticText;
        const bool nativeButton = hasBacking && backing->controlSupported &&
            backing->controlKind == ControlKind::Button;
        const bool nativeCheckBox = hasBacking && backing->controlSupported &&
            backing->controlKind == ControlKind::CheckBox;
        const bool nativeProgress = hasBacking && backing->controlSupported &&
            backing->controlKind == ControlKind::ProgressBar;
        const bool nativeSeparator = hasBacking && backing->controlSupported &&
            backing->controlKind == ControlKind::Separator;
        const bool nativeBitmapDisplay = hasBacking && backing->controlSupported &&
            backing->controlKind == ControlKind::StaticIcon &&
            FluentShell::EqualsIgnoreCase(backing->className, L"BitmapDisplayClass");
        const bool nativeBitmapSwitch = hasBacking && backing->controlSupported &&
            backing->controlKind == ControlKind::RadioButton &&
            FluentShell::EqualsIgnoreCase(backing->className, L"BitmapSwitchClass");
        const bool nativeMonitorPalette = hasBacking && backing->controlSupported &&
            backing->controlKind == ControlKind::StaticIcon &&
            FluentShell::EqualsIgnoreCase(backing->className, L"MonitorPaletteClass");
        const bool nativePane = semantic.controlType == UIA_PaneControlTypeId &&
            hasBacking && backing->controlSupported;
        // The remaining kinds are matched by their registered adapter's verdict
        // alone: the adapter already refused every owner-draw, virtual, and
        // multi-select variant it cannot read, so the DirectUI lane inherits that
        // bound instead of restating style bits here.
        const auto nativeKindIs = [&](ControlKind kind) {
            return hasBacking && backing->controlSupported && backing->controlKind == kind;
        };
        const bool nativeThreeState = nativeKindIs(ControlKind::ThreeState);
        const bool nativeRadioButton = nativeKindIs(ControlKind::RadioButton) &&
            FluentShell::EqualsIgnoreCase(backing->className, L"Button");
        const bool nativeStaticIcon = nativeKindIs(ControlKind::StaticIcon) &&
            FluentShell::EqualsIgnoreCase(backing->className, L"Static");
        const bool nativeGroupBox = nativeKindIs(ControlKind::GroupBox);
        const bool nativeEdit = nativeKindIs(ControlKind::Edit);
        const bool nativePassword = nativeKindIs(ControlKind::Password);
        const bool nativeComboBox = nativeKindIs(ControlKind::ComboBox);
        const bool nativeListBox = nativeKindIs(ControlKind::ListBox);
        const bool nativeListView = nativeKindIs(ControlKind::ListView);
        const bool nativeTabControl = nativeKindIs(ControlKind::TabControl);
        const bool nativeStatusBar = nativeKindIs(ControlKind::StatusBar);
        const bool nativeToolbar = nativeKindIs(ControlKind::Toolbar);
        const bool nativeSysLink = nativeKindIs(ControlKind::SysLink);
        size_t absorbedChildren = 0;
        if (hasBacking) {
            const auto found = absorbedCounts.find(backing->window.hwnd);
            if (found != absorbedCounts.end()) absorbedChildren = found->second;
        }

        bool retain = true;
        if (nativePane && backing->controlKind == ControlKind::StaticText) {
            if (semantic.name.empty() || semantic.focusable || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone)
                return Fail(error, L"generic U: native Static Pane exposes unsupported behavior" +
                    describeSemantic());
            slot.kind = ControlKind::StaticText;
            slot.presentationVariant = L"standard";
        } else if (nativePane && backing->controlKind == ControlKind::Button) {
            if (semantic.name.empty() || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone ||
                (semantic.enabled && !semantic.focusable))
                return Fail(error, L"generic U: native Button Pane lacks an inert UIA contract" +
                    describeSemantic());
            slot.kind = ControlKind::Button;
            slot.presentationVariant =
                ((backing->window.style & BS_TYPEMASK) == BS_COMMANDLINK ||
                 (backing->window.style & BS_TYPEMASK) == BS_DEFCOMMANDLINK)
                ? L"commandLink" : L"standard";
            slot.cancel = FluentShell::EqualsIgnoreCase(
                semantic.semanticKey, L"cancelbutton");
            slot.action = DirectUiAction::HandoffClick;
        } else if (nativePane && backing->controlKind == ControlKind::CheckBox) {
            if (semantic.name.empty() || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone)
                return Fail(error, L"generic U: native CheckBox Pane exposes unsupported behavior" +
                    describeSemantic());
            slot.kind = ControlKind::CheckBox;
            slot.presentationVariant = L"standard";
            slot.action = DirectUiAction::ToggleCheck;
        } else if ((nativePane || semantic.controlType == UIA_TextControlTypeId ||
                    semantic.controlType == UIA_ImageControlTypeId) &&
                   nativeBitmapDisplay) {
            if (semantic.name.empty() || semantic.focusable || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone ||
                (backing->window.style & (WS_TABSTOP | SS_NOTIFY)) != 0)
                return Fail(error, L"generic U: BitmapDisplayClass is interactive or unlabeled" +
                    describeSemantic());
            slot.kind = ControlKind::StaticIcon;
            slot.presentationVariant = L"bitmapDisplay";
            slot.captureBitmap = true;
        } else if ((nativePane || semantic.controlType == UIA_RadioButtonControlTypeId) &&
                   nativeBitmapSwitch) {
            if (semantic.name.empty() ||
                behavioralPatternMask != DirectUiPatternSelectionItem ||
                (backing->window.style & SS_NOTIFY) != 0)
                return Fail(error, L"generic U: BitmapSwitchClass lacks a native radio contract" +
                    describeSemantic());
            slot.kind = ControlKind::RadioButton;
            slot.presentationVariant = L"bitmapSwitch";
            slot.captureBitmap = true;
            slot.action = DirectUiAction::SelectRadio;
        } else if ((nativePane || semantic.controlType == UIA_ImageControlTypeId) &&
                   nativeMonitorPalette) {
            if (semantic.name.empty() || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone ||
                (backing->window.style & SS_NOTIFY) != 0)
                return Fail(error, L"generic U: MonitorPaletteClass is interactive or unlabeled" +
                    describeSemantic());
            slot.kind = ControlKind::StaticIcon;
            slot.presentationVariant = L"monitorPalette";
            slot.captureBitmap = true;
        } else if (semantic.controlType == UIA_GroupControlTypeId && nativeGroupBox) {
            // A BS_GROUPBOX frame. Its caption is the only state it has, and the
            // adapter already refused a tab-stop group box.
            if (semantic.name.empty() || semantic.focusable || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone)
                return Fail(error, L"generic U: GroupBox exposes unsupported behavior" +
                    describeSemantic());
            slot.kind = ControlKind::GroupBox;
            slot.presentationVariant = L"standard";
        } else if ((semantic.controlType == UIA_ImageControlTypeId || nativePane) &&
                   nativeStaticIcon) {
            // SS_ICON static. Its pixels are read through the Static adapter, so
            // the icon travels as owned bitmap data rather than a resource id.
            if (semantic.focusable || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone)
                return Fail(error, L"generic U: Static icon is interactive" +
                    describeSemantic());
            slot.kind = ControlKind::StaticIcon;
            slot.presentationVariant = L"standard";
            slot.captureDetail = true;
        } else switch (semantic.controlType) {
        case UIA_TextControlTypeId:
            if (semantic.focusable || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone ||
                (hasBacking && !nativeStatic) ||
                (semantic.name.empty() &&
                    (!hasBacking || semantic.semanticKey.empty())))
                return Fail(error, L"generic U: Text semantic exposes unsupported behavior" +
                    describeSemantic());
            // Empty native Static providers are layout/implementation evidence,
            // not visible text. Keep their identity in the contract without
            // manufacturing an unnamed WinUI text node.
            slot.project = !semantic.name.empty();
            slot.kind = ControlKind::StaticText;
            slot.presentationVariant = L"standard";
            break;
        case UIA_SeparatorControlTypeId:
            if (semantic.focusable || semantic.actionable ||
                semantic.patternMask != DirectUiPatternNone ||
                (hasBacking && !nativeSeparator))
                return Fail(error, L"generic U: Separator semantic is actionable" +
                    describeSemantic());
            slot.kind = ControlKind::Separator;
            slot.presentationVariant = L"standard";
            break;
        case UIA_ButtonControlTypeId:
            if (semantic.name.empty() ||
                behavioralPatternMask != DirectUiPatternInvoke ||
                (semantic.enabled && !semantic.focusable))
                return Fail(error, L"generic U: Button lacks an exact Invoke contract" +
                    describeSemantic());
            slot.kind = ControlKind::Button;
            if (hasBacking && !nativeButton)
                return Fail(error, L"generic U: Button has an unsupported native backing" +
                    describeSemantic());
            slot.presentationVariant = nativeButton &&
                ((backing->window.style & BS_TYPEMASK) == BS_COMMANDLINK ||
                 (backing->window.style & BS_TYPEMASK) == BS_DEFCOMMANDLINK)
                ? L"commandLink" : L"standard";
            slot.cancel = FluentShell::EqualsIgnoreCase(
                semantic.semanticKey, L"cancelbutton");
            if (nativeButton) {
                slot.action = DirectUiAction::HandoffClick;
            } else if (!hasBacking && propertySheetSurface) {
                if (FluentShell::EqualsIgnoreCase(semantic.semanticKey, L"backbutton"))
                    slot.propertySheetButton = PSBTN_BACK;
                else if (FluentShell::EqualsIgnoreCase(semantic.semanticKey, L"nextbutton"))
                    slot.propertySheetButton = PSBTN_NEXT;
                else if (FluentShell::EqualsIgnoreCase(semantic.semanticKey, L"finishbutton"))
                    slot.propertySheetButton = PSBTN_FINISH;
                else if (slot.cancel)
                    slot.propertySheetButton = PSBTN_CANCEL;
                if (slot.propertySheetButton < 0)
                    return Fail(error, L"generic U: virtual Invoke requires an isolated UIA action broker" +
                        describeSemantic());
                slot.action = DirectUiAction::HandoffPropertySheetButton;
            } else {
                return Fail(error, L"generic U: virtual Invoke is not a proven property-sheet button" +
                    describeSemantic());
            }
            break;
        case UIA_CheckBoxControlTypeId:
            if (semantic.name.empty() ||
                (behavioralPatternMask & DirectUiPatternToggle) == 0 ||
                (behavioralPatternMask &
                    ~(DirectUiPatternToggle | DirectUiPatternInvoke)) != 0 ||
                (semantic.toggleState != ToggleState_Off &&
                 semantic.toggleState != ToggleState_On &&
                 !(nativeThreeState &&
                   semantic.toggleState == ToggleState_Indeterminate)) ||
                (!nativeCheckBox && !nativeThreeState) ||
                (semantic.enabled && !semantic.focusable))
                return Fail(error, L"generic U: CheckBox lacks a native-backed Toggle contract" +
                    describeSemantic());
            // A BS_3STATE box reaches a value a bool cannot carry, so it is read
            // through its own adapter instead of the shared checked facet.
            slot.kind = nativeThreeState ? ControlKind::ThreeState : ControlKind::CheckBox;
            slot.presentationVariant = L"standard";
            slot.action = DirectUiAction::ToggleCheck;
            slot.captureDetail = nativeThreeState;
            break;
        case UIA_ProgressBarControlTypeId:
            if (semantic.name.empty() || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone || !nativeProgress ||
                (backing->window.style & (PBS_VERTICAL | WS_TABSTOP)) != 0)
                return Fail(error, L"generic U: ProgressBar lacks a read-only native backing contract" +
                    describeSemantic());
            slot.kind = ControlKind::ProgressBar;
            slot.presentationVariant = L"standard";
            break;
        case UIA_HyperlinkControlTypeId:
            // The SysLink adapter admits exactly one link, and the source thread
            // already has a proven route that lets the control raise its own
            // NM_RETURN instead of fabricating a parent notification.
            if (semantic.name.empty() || !nativeSysLink ||
                (behavioralPatternMask & DirectUiPatternInvoke) == 0 ||
                (behavioralPatternMask & ~DirectUiPatternInvoke) != 0 ||
                (semantic.enabled && !semantic.focusable))
                return Fail(error, L"generic U: Hyperlink lacks a native SysLink Invoke contract" +
                    describeSemantic());
            slot.kind = ControlKind::SysLink;
            slot.presentationVariant = L"standard";
            slot.captureDetail = true;
            slot.action = DirectUiAction::HandoffLinkClick;
            break;
        case UIA_EditControlTypeId: {
            if (!nativeEdit && !nativePassword)
                return Fail(error, L"generic U: Edit has no admitted native backing" +
                    describeSemantic());
            if (semantic.name.empty() ||
                (behavioralPatternMask & ~DirectUiPatternValue) != 0 ||
                (semantic.enabled && !semantic.focusable))
                return Fail(error, L"generic U: Edit exposes behavior outside its text contract" +
                    describeSemantic());
            // The bridged provider derives IsReadOnly from ES_READONLY. Holding
            // the two to each other keeps a projected TextBox from offering a
            // write the application would refuse, and vice versa.
            const bool readOnly = (backing->window.style & ES_READONLY) != 0;
            if (readOnly == ((behavioralPatternMask & DirectUiPatternValue) != 0))
                return Fail(error, L"generic U: Edit read-only style and UIA Value disagree" +
                    describeSemantic());
            slot.kind = nativePassword ? ControlKind::Password : ControlKind::Edit;
            slot.presentationVariant = nativePassword ? L"password" : L"standard";
            slot.captureDetail = true;
            if (!readOnly) slot.action = DirectUiAction::SetEditText;
            break;
        }
        case UIA_ComboBoxControlTypeId:
            if (semantic.name.empty() || !nativeComboBox ||
                (behavioralPatternMask & ~(DirectUiPatternExpandCollapse |
                    DirectUiPatternValue | DirectUiPatternSelectionItem)) != 0 ||
                (semantic.enabled && !semantic.focusable))
                return Fail(error, L"generic U: ComboBox lacks a native selection contract" +
                    describeSemantic());
            slot.kind = ControlKind::ComboBox;
            slot.presentationVariant = L"standard";
            slot.captureDetail = true;
            slot.action = DirectUiAction::SelectListItem;
            // A CBS_DROPDOWN combo also accepts typed text. Whether the route is
            // advertised is decided per revision from the adapter's own
            // `editable` verdict, not from the style read here.
            slot.secondaryAction = DirectUiAction::SetEditText;
            break;
        case UIA_ListControlTypeId:
            if (!nativeListBox && !nativeListView)
                return Fail(error, L"generic U: List has no admitted native backing" +
                    describeSemantic());
            if (semantic.actionable || behavioralPatternMask != DirectUiPatternNone ||
                (semantic.enabled && !semantic.focusable))
                return Fail(error, L"generic U: List container exposes unsupported behavior" +
                    describeSemantic());
            slot.kind = nativeListView ? ControlKind::ListView : ControlKind::ListBox;
            slot.presentationVariant = L"standard";
            slot.captureDetail = true;
            slot.action = DirectUiAction::SelectListItem;
            if (nativeListView) slot.secondaryAction = DirectUiAction::SetItemCheck;
            break;
        case UIA_DataGridControlTypeId:
        case UIA_TableControlTypeId:
            if (!nativeListView || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone ||
                (semantic.enabled && !semantic.focusable))
                return Fail(error,
                    L"generic U: DataGrid lacks an inert report-mode ListView backing" +
                    describeSemantic());
            slot.kind = ControlKind::ListView;
            slot.presentationVariant = L"standard";
            slot.captureDetail = true;
            slot.action = DirectUiAction::SelectListItem;
            slot.secondaryAction = DirectUiAction::SetItemCheck;
            break;
        case UIA_TabControlTypeId:
            if (!nativeTabControl || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone)
                return Fail(error, L"generic U: Tab lacks an inert native TabControl backing" +
                    describeSemantic());
            slot.kind = ControlKind::TabControl;
            slot.presentationVariant = L"standard";
            slot.captureDetail = true;
            slot.action = DirectUiAction::SelectListItem;
            break;
        case UIA_RadioButtonControlTypeId:
            // BitmapSwitchClass radios are handled above. This is a standard
            // Button-class radio, and only the auto variant maintains its group's
            // exclusivity itself; a plain BS_RADIOBUTTON is refused rather than
            // projected with a selection the application never applies.
            if (semantic.name.empty() || !nativeRadioButton ||
                (backing->window.style & BS_TYPEMASK) != BS_AUTORADIOBUTTON ||
                (behavioralPatternMask & DirectUiPatternSelectionItem) == 0 ||
                (behavioralPatternMask &
                    ~(DirectUiPatternSelectionItem | DirectUiPatternInvoke)) != 0 ||
                (semantic.enabled && !semantic.focusable))
                return Fail(error, L"generic U: RadioButton lacks an auto native group contract" +
                    describeSemantic());
            slot.kind = ControlKind::RadioButton;
            slot.presentationVariant = L"standard";
            slot.action = DirectUiAction::SelectRadio;
            break;
        case UIA_StatusBarControlTypeId:
            // Status bar parts have no name of their own and neither does the
            // bar, so the name requirement every projected role carries does not
            // apply here; the adapter's part text is the whole contract.
            if (!nativeStatusBar || semantic.focusable || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone)
                return Fail(error, L"generic U: StatusBar lacks an inert native backing" +
                    describeSemantic());
            slot.kind = ControlKind::StatusBar;
            slot.presentationVariant = L"standard";
            slot.captureDetail = true;
            break;
        case UIA_ToolBarControlTypeId:
            if (!nativeToolbar || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone)
                return Fail(error, L"generic U: ToolBar lacks an inert native container contract" +
                    describeSemantic());
            slot.kind = ControlKind::Toolbar;
            slot.presentationVariant = L"standard";
            slot.captureDetail = true;
            slot.action = DirectUiAction::ToolbarCommand;
            break;
        case UIA_ImageControlTypeId:
        case UIA_PaneControlTypeId:
        case UIA_GroupControlTypeId:
        case UIA_WindowControlTypeId: {
            // Some Win32/DirectUI providers put a read-only Value pattern on
            // structural roots. It cannot mutate canonical state and is still
            // pinned in the generated evidence contract when the element has
            // a stable identity.
            // A native-backed structural provider may advertise a focus stop
            // solely to delegate focus into its child controls.
            // It has no mutable pattern and is accounted but not projected.
            const bool delegatedStructuralFocus =
                semantic.controlType == UIA_PaneControlTypeId && semantic.focusable &&
                !semantic.actionable && behavioralPatternMask == DirectUiPatternNone &&
                hasBacking &&
                backing->window.hwnd == native.native.propertySheetPageHwnd &&
                FluentShell::EqualsIgnoreCase(backing->className, L"#32770") &&
                !semantic.semanticKey.empty();
            const bool compositePaletteEntry =
                semantic.controlType == UIA_PaneControlTypeId &&
                FluentShell::EqualsIgnoreCase(semantic.className, L"MonitorPaletteEntryClass") &&
                !semantic.focusable && !semantic.actionable &&
                behavioralPatternMask == DirectUiPatternNone;
            if ((semantic.focusable && !delegatedStructuralFocus) || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone)
                return Fail(error, L"generic U: structural semantic is unexpectedly actionable" +
                    describeSemantic());
            if (compositePaletteEntry) {
                slot.project = false;
                slot.kind = ControlKind::StaticText;
                slot.presentationVariant = L"standard";
                retain = false;
                break;
            }
            if (!semantic.name.empty() && !delegatedStructuralFocus)
                return Fail(error, L"generic U: named structural semantic has no visual contract" +
                    describeSemantic());
            slot.project = false;
            slot.kind = ControlKind::StaticText;
            slot.presentationVariant = L"standard";
            retain = !semantic.semanticKey.empty();
            break;
        }
        default:
            return Fail(error, L"generic U: UIA control type is outside the admitted semantic set" +
                describeSemantic());
        }

        if (!retain) continue;
        const bool bindNative = slot.project &&
            ((slot.kind == ControlKind::Button && nativeButton) ||
             (slot.kind == ControlKind::CheckBox && nativeCheckBox) ||
             (slot.kind == ControlKind::ThreeState && nativeThreeState) ||
             (slot.kind == ControlKind::StaticText && nativeStatic) ||
             (slot.kind == ControlKind::ProgressBar && nativeProgress) ||
             (slot.kind == ControlKind::Separator && nativeSeparator) ||
             (slot.kind == ControlKind::StaticIcon &&
              (nativeBitmapDisplay || nativeMonitorPalette || nativeStaticIcon)) ||
             (slot.kind == ControlKind::RadioButton &&
              (nativeBitmapSwitch || nativeRadioButton)) ||
             (slot.kind == ControlKind::GroupBox && nativeGroupBox) ||
             (slot.kind == ControlKind::Edit && nativeEdit) ||
             (slot.kind == ControlKind::Password && nativePassword) ||
             (slot.kind == ControlKind::ComboBox && nativeComboBox) ||
             (slot.kind == ControlKind::ListBox && nativeListBox) ||
             (slot.kind == ControlKind::ListView && nativeListView) ||
             (slot.kind == ControlKind::TabControl && nativeTabControl) ||
             (slot.kind == ControlKind::StatusBar && nativeStatusBar) ||
             (slot.kind == ControlKind::Toolbar && nativeToolbar) ||
             (slot.kind == ControlKind::SysLink && nativeSysLink));
        if (hasBacking && slot.project && !bindNative)
            return Fail(error, L"generic U: projected semantic/native control kinds do not match" +
                describeSemantic());
        if (!bindNative) {
            slot.virtualSource = true;
            if (semantic.semanticKey.empty() || !virtualIds.insert(semantic.semanticKey).second)
                return Fail(error, L"generic U: virtual semantic AutomationId is empty or duplicated");
        } else {
            slot.virtualSource = false;
            if (!backingHwnds.insert(backing->window.hwnd).second)
                return Fail(error, L"generic U: native backing is shared by multiple semantics");
            slot.nativeClass = own(backing->className);
            slot.nativeStyleMask = UINT64_MAX;
            slot.nativeStyleValue = static_cast<uint32_t>(backing->window.style);
            slot.nativeStyleAlt = slot.nativeStyleValue;
            if (slot.kind == ControlKind::Button) {
                const DWORD buttonType = slot.nativeStyleValue & BS_TYPEMASK;
                slot.defaultButton = buttonType == BS_DEFPUSHBUTTON ||
                    buttonType == BS_DEFCOMMANDLINK;
                if (buttonType == BS_PUSHBUTTON || buttonType == BS_DEFPUSHBUTTON) {
                    slot.nativeStyleAlt = (slot.nativeStyleValue & ~BS_TYPEMASK) |
                        (buttonType == BS_PUSHBUTTON ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
                } else if (buttonType == BS_COMMANDLINK || buttonType == BS_DEFCOMMANDLINK) {
                    slot.nativeStyleAlt = (slot.nativeStyleValue & ~BS_TYPEMASK) |
                        (buttonType == BS_COMMANDLINK ? BS_DEFCOMMANDLINK : BS_COMMANDLINK);
                }
            } else if (slot.kind == ControlKind::RadioButton) {
                // Both the BitmapSwitchClass radios and standard auto radios move
                // WS_TABSTOP onto whichever member is selected, so the bit is a
                // legitimate alternate rather than a topology change.
                slot.nativeStyleAlt = slot.nativeStyleValue ^ WS_TABSTOP;
            }
            slot.nativeControlId = backing->window.controlId;
            // Composite census. The provider's child count and the adapter's own
            // item count are two independent observations of one collection: the
            // adapter's count is pinned here and held to an equality across the
            // A/B bracket, and the provider has to publish at least one element
            // per item because headers and scroll chrome only ever add.
            if (IsDirectUiCompositeKind(slot.kind)) {
                if (backing->nativeItemCount == static_cast<size_t>(-1) ||
                    backing->nativeItemCount > kMaxCompositeItems)
                    return Fail(error,
                        L"generic U: composite has no readable native item census" +
                        describeSemantic());
                if (absorbedChildren < backing->nativeItemCount)
                    return Fail(error,
                        L"generic U: composite provider published fewer elements than items" +
                        describeSemantic());
                slot.nativeItemCount = backing->nativeItemCount;
            }
            slot.compositeItemCount = absorbedChildren;
        }
        const bool nativeTabStop = backing != native.descendants.end() &&
            (backing->window.style & WS_TABSTOP) != 0;
        if (slot.project &&
            ((semantic.enabled && semantic.focusable) ||
             (slot.action != DirectUiAction::None && nativeTabStop)))
            slot.tabIndex = nextTabIndex++;
        result->slots.push_back(slot);
        admittedSemantics.push_back(semantic);
    }
    if (result->slots.empty())
        return Fail(error, L"generic U: no projectable or accountable semantics remain");

    auto& profile = result->profile;
    profile.adapterId = L"microsoft.windows.directui.semantic.v1";
    profile.pageId = L"semantic-v1";
    profile.executableBasename = own(std::wstring(FluentShell::FileNameOf(imagePath)));
    profile.rootClass = own(native.rootClass);
    profile.slots = result->slots.data();
    profile.slotCount = result->slots.size();
    profile.directUiOwnsTabOrder = true;
    profile.minDescendants = native.descendants.size();
    profile.maxDescendants = native.descendants.size();
    profile.implementationClasses = result->implementationClasses.data();
    profile.implementationClassCount = result->implementationClasses.size();
    profile.genericSemantic = true;
    uia.semantics = std::move(admittedSemantics);

    std::unordered_map<HWND, std::vector<const DirectUiBootstrapWindowEvidence*>> radioGroups;
    for (const auto& descendant : native.descendants) {
        if (!descendant.window.visible ||
            !FluentShell::EqualsIgnoreCase(descendant.className, L"BitmapSwitchClass")) continue;
        radioGroups[descendant.parent].push_back(&descendant);
    }
    for (const auto& [_, members] : radioGroups) {
        if (members.size() < 2 || members.size() > 16)
            return Fail(error, L"generic U: BitmapSwitchClass radio group size is outside bounds");
        size_t selected = 0;
        size_t grouped = 0;
        for (const auto* member : members) {
            if ((member->window.style & WS_TABSTOP) != 0) ++selected;
            if ((member->window.style & WS_GROUP) != 0) ++grouped;
        }
        if (selected != 1 || grouped == 0)
            return Fail(error, L"generic U: BitmapSwitchClass radio group is not exclusive");
    }
    owned = std::move(result);
    return true;
}

bool CaptureGeneratedProfileEvidence(
    SourceThreadAgent& agent,
    const DirectUiWindowProfile& profile,
    DirectUiNativeEvidence& evidence,
    std::wstring& error) {
    DirectUiBootstrapEvidence bootstrap;
    if (!CaptureDirectUiBootstrapCore(agent, bootstrap, error)) return false;
    if (!FluentShell::EqualsIgnoreCase(ClassName(agent.Root()), profile.rootClass) ||
        bootstrap.descendants.size() < profile.minDescendants ||
        bootstrap.descendants.size() > profile.maxDescendants) {
        return Fail(error, L"generic A/B: root class or descendant count changed"
            L" (expectedRoot=" + std::wstring(profile.rootClass) + L", actualRoot=" +
            ClassName(agent.Root()) + L", expectedCount=" +
            std::to_wstring(profile.minDescendants) + L", actualCount=" +
            std::to_wstring(bootstrap.descendants.size()) + L")");
    }

    size_t accountedClasses = 0;
    for (size_t contractIndex = 0;
         contractIndex < profile.implementationClassCount; ++contractIndex) {
        const auto& contract = profile.implementationClasses[contractIndex];
        const auto count = static_cast<size_t>(std::count_if(
            bootstrap.descendants.begin(), bootstrap.descendants.end(),
            [&](const auto& candidate) {
                return candidate.window.visible == contract.visible &&
                    FluentShell::EqualsIgnoreCase(candidate.className, contract.className);
            }));
        if (count != contract.count)
            return Fail(error, L"generic A/B: implementation HWND inventory changed");
        accountedClasses += count;
    }
    if (accountedClasses != bootstrap.descendants.size())
        return Fail(error, L"generic A/B: implementation HWND class is outside the sealed inventory");

    evidence = std::move(bootstrap.native);
    evidence.slotWindows.assign(profile.slotCount, DirectUiWindowEvidence{});
    std::vector<HWND> backingWindows;
    for (const auto& candidate : bootstrap.descendants) {
        if (!candidate.window.visible || candidate.window.hwnd == evidence.directUi.hwnd) continue;
        // A composite's own implementation child (ComboBox edit/list, ListView
        // header) is owned by its composite slot, not a backing of its own.
        if (candidate.compositeImplementationChild) continue;
        const bool admittedClass = std::any_of(
            profile.slots, profile.slots + profile.slotCount,
            [&](const DirectUiSlot& slot) {
                return !slot.virtualSource &&
                    FluentShell::EqualsIgnoreCase(candidate.className, slot.nativeClass);
            });
        if (admittedClass) backingWindows.push_back(candidate.window.hwnd);
    }
    std::sort(backingWindows.begin(), backingWindows.end(), [](HWND left, HWND right) {
        RECT leftRect{};
        RECT rightRect{};
        if (GetWindowRect(left, &leftRect) && GetWindowRect(right, &rightRect)) {
            if (leftRect.top != rightRect.top) return leftRect.top < rightRect.top;
            if (leftRect.left != rightRect.left) return leftRect.left < rightRect.left;
        }
        return reinterpret_cast<uintptr_t>(left) < reinterpret_cast<uintptr_t>(right);
    });
    const size_t expectedBackings = static_cast<size_t>(std::count_if(
        profile.slots, profile.slots + profile.slotCount,
        [](const DirectUiSlot& slot) { return !slot.virtualSource; }));
    if (backingWindows.size() != expectedBackings)
        return Fail(error, L"generic A/B: visible backing HWND count changed");

    std::vector<bool> filled(profile.slotCount, false);
    for (HWND backing : backingWindows) {
        if (!IsChild(evidence.directUi.hwnd, backing))
            return Fail(error, L"generic A/B: backing HWND escaped the DirectUI anchor");
        const auto backingClass = ClassName(backing);
        DirectUiWindowEvidence value;
        const bool isButton = FluentShell::EqualsIgnoreCase(backingClass, L"Button");
        const bool isProgress = FluentShell::EqualsIgnoreCase(backingClass, PROGRESS_CLASSW);
        if (!ReadWindowEvidence(agent, backing, isButton, value, error, isProgress))
            return false;
        bool matched = false;
        for (size_t index = 0; index < profile.slotCount; ++index) {
            const auto& slot = profile.slots[index];
            if (slot.virtualSource || filled[index] ||
                !FluentShell::EqualsIgnoreCase(backingClass, slot.nativeClass)) continue;
            const bool styleMatches = slot.nativeStyleMask == 0 ||
                (value.style & slot.nativeStyleMask) == slot.nativeStyleValue ||
                (value.style & slot.nativeStyleMask) == slot.nativeStyleAlt;
            const bool idMatches = slot.nativeControlId < 0 ||
                value.controlId == slot.nativeControlId;
            if (!styleMatches || !idMatches) continue;
            value.tabIndex = slot.tabIndex;
            evidence.slotWindows[index] = std::move(value);
            filled[index] = true;
            matched = true;
            break;
        }
        if (!matched)
            return Fail(error, L"generic A/B: backing HWND no longer matches a semantic slot"
                L" (hwnd=" + std::to_wstring(reinterpret_cast<uintptr_t>(backing)) +
                L", class=" + backingClass + L", style=" + std::to_wstring(value.style) +
                L", id=" + std::to_wstring(value.controlId) + L")");
    }
    for (size_t index = 0; index < profile.slotCount; ++index) {
        if (!profile.slots[index].virtualSource && !filled[index])
            return Fail(error, L"generic A/B: semantic backing slot is unavailable");
    }

    if (profile.directUiOwnsTabOrder) {
        const HWND first = GetNextDlgTabItem(agent.Root(), nullptr, FALSE);
        if (first != evidence.directUi.hwnd || GetNextDlgTabItem(agent.Root(), first, FALSE) != first)
            return Fail(error, L"generic A/B: DirectUI anchor is not the native tab cycle");
    }
    evidence.mutationEpoch = agent.MutationEpoch();
    evidence.lastMutationHwnd = agent.LastMutationHwnd();
    evidence.lastMutationMessage = agent.LastMutationMessage();
    for (size_t index = 0; index < profile.slotCount; ++index) {
        const DirectUiSlot& slot = profile.slots[index];
        if (slot.virtualSource) continue;
        DirectUiWindowEvidence& value = evidence.slotWindows[index];
        if (slot.captureBitmap && !CaptureWindowClientPixels(value.hwnd, value, error))
            return false;
        // Typed state through the backing control's own registered adapter. Its
        // rejections are the DirectUI lane's rejections, so an application that
        // grows an owner-draw or virtual variant of an admitted class stays native.
        if (slot.captureDetail && !CaptureSlotDetail(value.hwnd, slot.kind, value, error))
            return false;
    }
    return true;
}

bool MatchBootstrapToGenerated(
    const DirectUiBootstrapEvidence& before,
    const DirectUiNativeEvidence& after,
    std::wstring& error) {
    constexpr std::wstring_view prefix =
        L"generic B: native evidence changed during semantic discovery: ";
    if (before.native.mutationEpoch != after.mutationEpoch)
        return Fail(error, std::wstring(prefix) + L"mutation epoch " +
            std::to_wstring(before.native.mutationEpoch) + L" -> " +
            std::to_wstring(after.mutationEpoch) + L", last hwnd=" +
            std::to_wstring(reinterpret_cast<uintptr_t>(after.lastMutationHwnd)) +
            L", message=" + std::to_wstring(after.lastMutationMessage));
    if (!SameWindow(before.native.root, after.root))
        return Fail(error, std::wstring(prefix) + L"root facets");
    if (!SameWindow(before.native.directUi, after.directUi))
        return Fail(error, std::wstring(prefix) + L"DirectUI anchor facets");
    if (before.native.dpi != after.dpi || before.native.cloaked != after.cloaked ||
        before.native.ownerHwnd != after.ownerHwnd || before.native.title != after.title)
        return Fail(error, std::wstring(prefix) + L"top-level identity");
    if (!SameRect(before.native.clientBounds, after.clientBounds) ||
        before.native.clientOriginScreen.x != after.clientOriginScreen.x ||
        before.native.clientOriginScreen.y != after.clientOriginScreen.y)
        return Fail(error, std::wstring(prefix) + L"client geometry");
    if (before.native.pageHosts != after.pageHosts ||
        before.native.pageStatics != after.pageStatics ||
        before.native.propertySheetPageHwnd != after.propertySheetPageHwnd ||
        !SameImplementationIdentity(
            before.native.implementationWindows, after.implementationWindows))
        return Fail(error, std::wstring(prefix) + L"implementation HWND identity");
    return true;
}

} // namespace

// Shape facets describe which control this is. A projected composite may change
// its selection or content between revisions, but a shape change means the
// native surface is no longer the control the profile admitted.
bool SameDirectUiDetailShape(
    const DirectUiWindowEvidence& left,
    const DirectUiWindowEvidence& right) noexcept {
    if (left.hasDetail != right.hasDetail) return false;
    if (!left.hasDetail) return true;
    const ControlNode& a = left.detail;
    const ControlNode& b = right.detail;
    return a.kind == b.kind && a.multiSelect == b.multiSelect &&
        a.readOnly == b.readOnly && a.multiline == b.multiline &&
        a.editable == b.editable && a.checkBoxes == b.checkBoxes &&
        a.columnHeadersVisible == b.columnHeadersVisible &&
        a.vertical == b.vertical && a.reversed == b.reversed &&
        a.smallChange == b.smallChange && a.largeChange == b.largeChange &&
        a.columns == b.columns && a.columnWidths == b.columnWidths;
}

// Content facets are the ones a still-admitted control may legitimately change:
// selection, item text, caret, item check state, and progress. The A/B bracket
// compares them for equality; only the projected refresh path normalizes them.
bool SameDirectUiDetailContent(
    const DirectUiWindowEvidence& left,
    const DirectUiWindowEvidence& right) noexcept {
    if (left.hasDetail != right.hasDetail) return false;
    if (!left.hasDetail) return true;
    const ControlNode& a = left.detail;
    const ControlNode& b = right.detail;
    return a.selectedIndex == b.selectedIndex &&
        a.selectedIndices == b.selectedIndices &&
        a.focusedIndex == b.focusedIndex &&
        a.selectionStart == b.selectionStart &&
        a.selectionLength == b.selectionLength && a.text == b.text &&
        a.items == b.items && SameRectList(a.itemRects, b.itemRects) &&
        a.rows == b.rows && a.checkedIndices == b.checkedIndices &&
        a.itemDepths == b.itemDepths && a.itemExpanded == b.itemExpanded &&
        a.checked == b.checked && a.minimum == b.minimum &&
        a.maximum == b.maximum && a.position == b.position &&
        a.indeterminate == b.indeterminate &&
        SameToolbarItems(a.toolbarItems, b.toolbarItems);
}

// A composite owner absorbs these UIA item descendants instead of letting each
// item become its own slot. Items have no backing HWND of their own and their
// census is corroborated against the native control's own item count.
bool IsDirectUiCompositeItemControlType(int controlType) noexcept {
    switch (controlType) {
    case UIA_ListItemControlTypeId:
    case UIA_TreeItemControlTypeId:
    case UIA_DataItemControlTypeId:
    case UIA_TabItemControlTypeId:
    case UIA_HeaderControlTypeId:
    case UIA_HeaderItemControlTypeId:
        return true;
    default:
        return false;
    }
}

// The requested protocol action must be one this binding's own slot advertised.
// Resolving through the same name table the snapshot published is what keeps the
// two from drifting: a renderer cannot reach a route the projected node never
// offered, and a route the node did offer is dispatched by its declared kind.
DirectUiAction DirectUiActionForRequest(
    const DirectUiActionBinding& binding,
    std::wstring_view action) noexcept {
    if (action.empty()) return DirectUiAction::None;
    if (binding.action != DirectUiAction::None &&
        DirectUiActionName(binding.action, binding.kind) == action)
        return binding.action;
    if (binding.secondaryAction != DirectUiAction::None &&
        DirectUiActionName(binding.secondaryAction, binding.kind) == action)
        return binding.secondaryAction;
    return DirectUiAction::None;
}

// In-place routes mutate a native-backed control through its own registered
// adapter and its own notification path, so the surface stays projected and the
// recapture accepts only that one control's delta. Handoff routes instead
// restore native visibility first and give the page up.
bool IsDirectUiInPlaceAction(DirectUiAction action) noexcept {
    switch (action) {
    case DirectUiAction::ToggleCheck:
    case DirectUiAction::SelectRadio:
    case DirectUiAction::SetEditText:
    case DirectUiAction::SelectListItem:
    case DirectUiAction::SetItemCheck:
    case DirectUiAction::ToolbarCommand:
        return true;
    default:
        return false;
    }
}

// Reads the facets every adapter expects to find prefilled, then hands the node
// to the control's own registered capture function. Any rejection the adapter
// raises is carried through verbatim so the DirectUI lane inherits the same
// bounded contract the Win32 lane already enforces.
bool CaptureDirectUiSlotNode(
    HWND window,
    ControlKind kind,
    ControlNode& node,
    std::wstring& error) noexcept {
    node = ControlNode{};
    if (!window || !IsWindow(window) || !GetWindowRect(window, &node.rect))
        return Fail(error, L"backing control identity or geometry is unavailable");
    node.kind = kind;
    node.hwnd = window;
    node.style = static_cast<uint64_t>(GetWindowLongPtrW(window, GWL_STYLE));
    node.exStyle = static_cast<uint64_t>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    node.visible = IsWindowVisible(window) != FALSE;
    node.enabled = IsWindowEnabled(window) != FALSE;
    node.controlId = GetDlgCtrlID(window);
    node.dialogCode = static_cast<uint32_t>(
        SendMessageW(window, WM_GETDLGCODE, 0, 0));
    const int textLength = GetWindowTextLengthW(window);
    if (textLength < 0 || textLength > static_cast<int>(kMaxTextChars))
        return Fail(error, L"backing control text exceeds bound");
    std::wstring text(static_cast<size_t>(textLength) + 1, L'\0');
    const int copied = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    if (copied < 0) return Fail(error, L"backing control text is unavailable");
    text.resize(static_cast<size_t>(copied));
    node.text = std::move(text);
    std::wstring reason;
    if (!CaptureControlDetail(window, node, reason))
        return Fail(error, L"backing control typed state was rejected: " + reason);
    return true;
}

const DirectUiWindowProfile* ResolveDirectUiWindowProfile(
    std::wstring& imagePath,
    std::wstring& error) {
    imagePath.clear();
    error.clear();
    wchar_t image[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, image, static_cast<DWORD>(std::size(image)));
    if (!length || length >= std::size(image)) return nullptr;
    const std::wstring_view base = FluentShell::FileNameOf(
        std::wstring_view(image, length));
    const DirectUiWindowProfile* candidate = nullptr;
    for (size_t index = 0; index < kDirectUiProfileCount; ++index) {
        if (FluentShell::EqualsIgnoreCase(base, kDirectUiProfiles[index].executableBasename)) {
            candidate = &kDirectUiProfiles[index];
            break;
        }
    }
    if (!candidate) return nullptr;
    wchar_t systemDirectory[32768]{};
    const UINT systemLength =
        GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
    if (!systemLength || systemLength >= std::size(systemDirectory))
        return Fail(error, L"process: System32 path unavailable") ? nullptr : nullptr;
    std::wstring expected(systemDirectory, systemLength);
    expected += L"\\";
    expected += candidate->executableBasename;
    imagePath = CanonicalPath(std::wstring_view(image, length));
    const auto expectedCanonical = CanonicalPath(expected);
    if (imagePath.empty() || expectedCanonical.empty() ||
        !FluentShell::EqualsIgnoreCase(imagePath, expectedCanonical)) {
        Fail(error, L"process: image is not the canonical System32 executable");
        return nullptr;
    }
    if (!ExactFileVersion(imagePath, *candidate)) {
        Fail(error, L"process: file version does not match the admitted profile");
        return nullptr;
    }
    if (!VerifyMicrosoftSignature(imagePath, error)) return nullptr;
    return candidate;
}

bool ResolveGenericDirectUiImage(std::wstring& imagePath, std::wstring& error) {
    imagePath.clear();
    error.clear();
    wchar_t image[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, image, static_cast<DWORD>(std::size(image)));
    if (!length || length >= std::size(image))
        return Fail(error, L"generic process: image path unavailable");
    const auto base = FluentShell::FileNameOf(std::wstring_view(image, length));
    if (base.empty()) return Fail(error, L"generic process: image basename unavailable");

    wchar_t systemDirectory[32768]{};
    const UINT systemLength =
        GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
    if (!systemLength || systemLength >= std::size(systemDirectory))
        return Fail(error, L"generic process: System32 path unavailable");
    std::wstring expected(systemDirectory, systemLength);
    expected.push_back(L'\\');
    expected.append(base);
    imagePath = CanonicalPath(std::wstring_view(image, length));
    const auto expectedCanonical = CanonicalPath(expected);
    if (imagePath.empty() || expectedCanonical.empty() ||
        !FluentShell::EqualsIgnoreCase(imagePath, expectedCanonical)) {
        return Fail(error, L"generic process: image is not the canonical System32 executable");
    }
    return VerifyMicrosoftSignature(imagePath, error);
}

bool CaptureDirectUiBootstrapEvidenceOnSourceThread(
    SourceThreadAgent& agent,
    DirectUiBootstrapEvidence& evidence,
    std::wstring& error) noexcept {
    try {
        return CaptureDirectUiBootstrapCore(agent, evidence, error);
    } catch (...) {
        return Fail(error, L"bootstrap: native evidence exception");
    }
}

bool CaptureDirectUiNativeEvidenceOnSourceThread(
    SourceThreadAgent& agent,
    const DirectUiWindowProfile& profile,
    DirectUiNativeEvidence& evidence,
    std::wstring& error) noexcept {
    try {
        if (profile.genericSemantic)
            return CaptureGeneratedProfileEvidence(agent, profile, evidence, error);
        const HWND root = agent.Root();
        if (!FluentShell::EqualsIgnoreCase(ClassName(root), profile.rootClass))
            return Fail(error, L"A/B: root class does not match the profile");
        DWORD rootProcess = 0;
        const DWORD rootThread = GetWindowThreadProcessId(root, &rootProcess);
        if (!rootThread || rootThread != agent.ThreadId() || rootProcess != GetCurrentProcessId())
            return Fail(error, L"A/B: root thread or process identity mismatch");
        evidence = {};
        evidence.mutationEpoch = agent.MutationEpoch();
        DWORD cloakReasons = 0;
        if (FAILED(DwmGetWindowAttribute(root, DWMWA_CLOAKED,
                &cloakReasons, sizeof(cloakReasons))))
            return Fail(error, L"A/B: native cloak state is unavailable");
        evidence.cloaked = (cloakReasons & DWM_CLOAKED_APP) != 0;
        evidence.dpi = GetDpiForWindow(root);
        if (!evidence.dpi || !ReadWindowEvidence(agent, root, false, evidence.root, error))
            return false;
        evidence.ownerHwnd = GetWindow(root, GW_OWNER);
        evidence.title = evidence.root.text;
        if (!GetClientRect(root, &evidence.clientBounds))
            return Fail(error, L"A/B: root client geometry unavailable");
        evidence.clientOriginScreen = { 0, 0 };
        if (!ClientToScreen(root, &evidence.clientOriginScreen))
            return Fail(error, L"A/B: root client origin unavailable");

        std::vector<HWND> descendants;
        EnumChildWindows(root, [](HWND window, LPARAM raw) -> BOOL {
            auto& values = *reinterpret_cast<std::vector<HWND>*>(raw);
            values.push_back(window);
            return values.size() <= 64;
        }, reinterpret_cast<LPARAM>(&descendants));
        if (descendants.empty() || descendants.size() < profile.minDescendants ||
            descendants.size() > profile.maxDescendants)
            return Fail(error, L"A/B: descendant count is outside the profile bound");

        // Census of implementation HWNDs. Every class is either part of the
        // declared census or rejects the surface; nothing is skipped silently.
        size_t visibleWrappers = 0;
        size_t hiddenWrappers = 0;
        size_t hiddenScrollBars = 0;
        size_t hiddenLinks = 0;
        size_t hiddenButtons = 0;
        size_t hiddenPageHosts = 0;
        size_t hiddenPageStaticTexts = 0;
        size_t visiblePageHosts = 0;
        size_t pageHostStaticTexts = 0;
        std::vector<HWND> backingWindows;
        evidence.slotWindows.assign(profile.slotCount, DirectUiWindowEvidence{});
        std::vector<bool> slotFilled(profile.slotCount, false);
        for (HWND child : descendants) {
            DWORD process = 0;
            if (GetWindowThreadProcessId(child, &process) != rootThread ||
                process != rootProcess)
                return Fail(error, L"A/B: descendant escaped the source thread/process");
            const auto name = ClassName(child);
            const bool visible = IsWindowVisible(child) != FALSE;
            const uint64_t generation = agent.DirectUiWindowGeneration(child);
            if (!generation)
                return Fail(error, L"A/B: descendant generation is unavailable");
            evidence.implementationWindows.push_back(
                { child, generation, GetParent(child), name, visible });
            if (FluentShell::EqualsIgnoreCase(name, L"DirectUIHWND")) {
                if (!visible || GetParent(child) != root || evidence.directUi.hwnd ||
                    !ReadWindowEvidence(agent, child, false, evidence.directUi, error))
                    return Fail(error, L"A/B: DirectUI anchor shape mismatch");
            } else if (FluentShell::EqualsIgnoreCase(name, L"CtrlNotifySink")) {
                visible ? ++visibleWrappers : ++hiddenWrappers;
            } else if (FluentShell::EqualsIgnoreCase(name, L"ScrollBar")) {
                if (visible) return Fail(error, L"A/B: visible ScrollBar outside the census");
                ++hiddenScrollBars;
            } else if (FluentShell::EqualsIgnoreCase(name, L"SysLink")) {
                if (visible) return Fail(error, L"A/B: visible SysLink outside the census");
                ++hiddenLinks;
            } else if (FluentShell::EqualsIgnoreCase(name, L"Button")) {
                if (!visible) {
                    ++hiddenButtons;
                    continue;
                }
                backingWindows.push_back(child);
            } else if (FluentShell::EqualsIgnoreCase(name, L"#32770")) {
                if (visible) ++visiblePageHosts; else ++hiddenPageHosts;
                if (visible) {
                    evidence.pageHosts.push_back(child);
                    backingWindows.push_back(child);
                }
            } else if (FluentShell::EqualsIgnoreCase(name, L"Static")) {
                const HWND parent = GetParent(child);
                const auto parentClass = parent ? ClassName(parent) : std::wstring();
                if (visible && FluentShell::EqualsIgnoreCase(parentClass, L"#32770")) {
                    ++pageHostStaticTexts;
                    evidence.pageStatics.push_back(child);
                    backingWindows.push_back(child);
                } else if (visible) {
                    // A visible Static must belong to an admitted page host.
                    bool inPageHost = false;
                    for (HWND host : evidence.pageHosts) {
                        if (GetParent(child) == host) { inPageHost = true; break; }
                    }
                    if (!inPageHost)
                        return Fail(error, L"A/B: visible Static outside a page host");
                    ++pageHostStaticTexts;
                    evidence.pageStatics.push_back(child);
                    backingWindows.push_back(child);
                } else {
                    ++hiddenPageStaticTexts;
                }
            } else {
                return Fail(error, L"A/B: unexpected visible or implementation HWND class");
            }
        }
        if (FluentShell::EqualsIgnoreCase(profile.rootClass, L"NativeHWNDHost")) {
            evidence.propertySheetPageHwnd =
                CurrentPropertySheetPage(root, evidence.pageHosts);
        }
        const DirectUiCensus& census = profile.census;
        if (!evidence.directUi.hwnd ||
            visibleWrappers != census.visibleNotifyWrappers ||
            hiddenWrappers != census.hiddenNotifyWrappers ||
            hiddenScrollBars != census.hiddenScrollBars ||
            hiddenLinks != census.hiddenSysLinks ||
            hiddenButtons != census.hiddenButtons ||
            hiddenPageHosts != census.hiddenPageHosts ||
            hiddenPageStaticTexts != census.hiddenPageStaticTexts ||
            visiblePageHosts != census.visiblePageHosts ||
            pageHostStaticTexts != census.pageHostStaticTexts) {
            error = L"A/B: implementation HWND census mismatch (visibleWrappers=" +
                std::to_wstring(visibleWrappers) + L", hiddenWrappers=" +
                std::to_wstring(hiddenWrappers) + L", hiddenScrollBars=" +
                std::to_wstring(hiddenScrollBars) + L", hiddenLinks=" +
                std::to_wstring(hiddenLinks) + L", hiddenButtons=" +
                std::to_wstring(hiddenButtons) + L", hiddenPageHosts=" +
                std::to_wstring(hiddenPageHosts) + L", hiddenPageStaticTexts=" +
                std::to_wstring(hiddenPageStaticTexts) + L", visiblePageHosts=" +
                std::to_wstring(visiblePageHosts) + L", pageHostStaticTexts=" +
                std::to_wstring(pageHostStaticTexts) + L")";
            return false;
        }

        // Bind every visible native backing HWND to a profile slot by class,
        // masked style, and control id. Geometry order makes otherwise
        // identical controls deterministic without application branches.
        std::sort(backingWindows.begin(), backingWindows.end(), [](HWND left, HWND right) {
            RECT leftRect{};
            RECT rightRect{};
            if (GetWindowRect(left, &leftRect) && GetWindowRect(right, &rightRect)) {
                if (leftRect.top != rightRect.top) return leftRect.top < rightRect.top;
                if (leftRect.left != rightRect.left) return leftRect.left < rightRect.left;
            }
            return reinterpret_cast<uintptr_t>(left) < reinterpret_cast<uintptr_t>(right);
        });
        size_t backingSlots = 0;
        for (size_t index = 0; index < profile.slotCount; ++index) {
            if (!profile.slots[index].virtualSource) ++backingSlots;
        }
        if (backingWindows.size() != backingSlots)
            return Fail(error, L"A/B: visible native backing count does not match profile slots");
        for (HWND backingWindow : backingWindows) {
            if (!IsChild(evidence.directUi.hwnd, backingWindow))
                return Fail(error, L"A/B: native backing is not below the DirectUI anchor");
            const std::wstring backingClass = ClassName(backingWindow);
            const bool isButton = FluentShell::EqualsIgnoreCase(backingClass, L"Button");
            DirectUiWindowEvidence value;
            if (!ReadWindowEvidence(agent, backingWindow, isButton, value, error)) return false;
            bool matched = false;
            for (size_t index = 0; index < profile.slotCount; ++index) {
                const DirectUiSlot& slot = profile.slots[index];
                if (slot.virtualSource || slotFilled[index]) continue;
                if (!FluentShell::EqualsIgnoreCase(backingClass, slot.nativeClass)) continue;
                const uint64_t maskedStyle = value.style & slot.nativeStyleMask;
                const bool styleOk = slot.nativeStyleMask == 0 ||
                    maskedStyle == slot.nativeStyleValue ||
                    maskedStyle == slot.nativeStyleAlt;
                const bool idOk = slot.nativeControlId < 0 ||
                    value.controlId == slot.nativeControlId;
                if (styleOk && idOk) {
                    value.tabIndex = -1;
                    evidence.slotWindows[index] = std::move(value);
                    slotFilled[index] = true;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                error = L"A/B: native backing class/style/control id is outside the profile (class=" +
                    backingClass + L", id=" + std::to_wstring(value.controlId) +
                    L", style=" + std::to_wstring(value.style) + L")";
                return false;
            }
        }
        for (size_t index = 0; index < profile.slotCount; ++index) {
            if (!profile.slots[index].virtualSource && !slotFilled[index])
                return Fail(error, L"A/B: profile backing slot has no native window");
        }

        // DirectUI dialogs expose their host as the native dialog-manager tab
        // stop; the provider owns focus traversal inside it. Profiles pin that
        // internal order and UIA corroborates focusability at U.
        if (profile.directUiOwnsTabOrder) {
            const HWND first = GetNextDlgTabItem(root, nullptr, FALSE);
            if (first != evidence.directUi.hwnd ||
                GetNextDlgTabItem(root, first, FALSE) != first)
                return Fail(error, L"A/B: DirectUI host is not the exact native dialog tab cycle");
        } else {
            HWND current = nullptr;
            int index = 0;
            std::unordered_set<HWND> seen;
            for (;;) {
                current = GetNextDlgTabItem(root, current, FALSE);
                if (!current || !seen.insert(current).second) break;
                for (size_t slot = 0; slot < profile.slotCount; ++slot) {
                    if (evidence.slotWindows[slot].hwnd == current)
                        evidence.slotWindows[slot].tabIndex = index++;
                }
            }
        }
        // Declared traversal indexes must be complete and unique for enabled,
        // focusable slots owned by the DirectUI provider.
        std::unordered_set<int> declaredTabIndexes;
        for (size_t index = 0; index < profile.slotCount; ++index) {
            const DirectUiSlot& slot = profile.slots[index];
            if (profile.directUiOwnsTabOrder && slot.project && slot.uiaEnabled &&
                slot.uiaFocusable && slot.tabIndex < 0)
                return Fail(error, L"A/B: focusable DirectUI slot has no declared tab index");
            if (slot.project && slot.tabIndex >= 0) {
                if (slot.virtualSource && !slot.uiaFocusable)
                    return Fail(error, L"A/B: profile declares a tab order for a non-focusable slot");
                if (!declaredTabIndexes.insert(slot.tabIndex).second)
                    return Fail(error, L"A/B: profile declares duplicate tab indexes");
                if (profile.directUiOwnsTabOrder && !slot.virtualSource)
                    evidence.slotWindows[index].tabIndex = slot.tabIndex;
            }
        }
        // Typed state through each backing control's own registered adapter. The
        // flag is lane-independent, so an exact profile row can pin a composite
        // control exactly the way a generated one does.
        for (size_t index = 0; index < profile.slotCount; ++index) {
            const DirectUiSlot& slot = profile.slots[index];
            if (slot.virtualSource || !slot.captureDetail) continue;
            if (!CaptureSlotDetail(evidence.slotWindows[index].hwnd, slot.kind,
                    evidence.slotWindows[index], error))
                return false;
        }
        evidence.mutationEpoch = agent.MutationEpoch();
        return true;
    } catch (...) {
        return Fail(error, L"A/B: native evidence exception");
    }
}

bool MatchDirectUiMutationBracket(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& before,
    const DirectUiNativeEvidence& after,
    std::wstring& error,
    bool requireStableEpoch) noexcept {
    if (before.slotWindows.size() != profile.slotCount ||
        after.slotWindows.size() != profile.slotCount)
        return Fail(error, L"B: DirectUI backing-slot evidence is incomplete");
    if (requireStableEpoch && before.mutationEpoch != after.mutationEpoch)
        return Fail(error, L"B: mutation epoch changed during UIA capture");
    if (before.dpi != after.dpi || !SameWindow(before.root, after.root) ||
        !SameWindow(before.directUi, after.directUi) ||
        before.ownerHwnd != after.ownerHwnd || before.title != after.title ||
        before.cloaked != after.cloaked ||
        !SameRect(before.clientBounds, after.clientBounds) ||
        before.clientOriginScreen.x != after.clientOriginScreen.x ||
        before.clientOriginScreen.y != after.clientOriginScreen.y ||
        before.pageHosts != after.pageHosts || before.pageStatics != after.pageStatics ||
        before.propertySheetPageHwnd != after.propertySheetPageHwnd ||
        !SameImplementationIdentity(
            before.implementationWindows, after.implementationWindows))
        return Fail(error, L"B: root/anchor identity or native facets changed");
    for (size_t index = 0; index < profile.slotCount; ++index) {
        if (profile.slots[index].virtualSource) continue;
        if (!SameProfileWindow(profile.slots[index],
                before.slotWindows[index], after.slotWindows[index])) {
            const auto& oldValue = before.slotWindows[index];
            const auto& newValue = after.slotWindows[index];
            return Fail(error, L"B: backing slot identity or native facets changed (slot=" +
                std::to_wstring(index) + L", kind=" +
                std::wstring(ControlKindName(profile.slots[index].kind)) + L", hwnd=" +
                std::to_wstring(reinterpret_cast<uintptr_t>(oldValue.hwnd)) + L"->" +
                std::to_wstring(reinterpret_cast<uintptr_t>(newValue.hwnd)) + L", generation=" +
                std::to_wstring(oldValue.generation) + L"->" +
                std::to_wstring(newValue.generation) + L", style=" +
                std::to_wstring(oldValue.style) + L"->" + std::to_wstring(newValue.style) +
                L", enabled=" + std::to_wstring(oldValue.enabled) + L"->" +
                std::to_wstring(newValue.enabled) + L", tab=" +
                std::to_wstring(oldValue.tabIndex) + L"->" +
                std::to_wstring(newValue.tabIndex) + L", position=" +
                std::to_wstring(oldValue.position) + L"->" +
                std::to_wstring(newValue.position) + L", indeterminate=" +
                std::to_wstring(oldValue.indeterminate) + L"->" +
                std::to_wstring(newValue.indeterminate) + L")");
        }
    }
    return true;
}

// Accepts exactly one slot's own delta after an in-place action. The acting
// slot's identity, geometry, styles, visibility, enabled state, traversal index,
// and typed shape all stay pinned; only the facets that control legitimately
// changes are taken from the post-action reading. Every other slot, the root,
// the anchor, and the implementation inventory must be untouched, which is what
// keeps an in-place route from being a back door to an unverified page change.
bool MatchDirectUiInPlaceMutation(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& before,
    const DirectUiNativeEvidence& after,
    size_t slotIndex,
    std::wstring& error) noexcept {
    try {
        if (slotIndex >= profile.slotCount ||
            before.slotWindows.size() != profile.slotCount ||
            after.slotWindows.size() != profile.slotCount)
            return Fail(error, L"in-place: acting DirectUI slot is outside the profile");
        const auto& acted = before.slotWindows[slotIndex];
        const auto& result = after.slotWindows[slotIndex];
        if (!SameDirectUiDetailShape(acted, result))
            return Fail(error, L"in-place: acting DirectUI control changed shape");
        auto expected = before;
        auto& normalized = expected.slotWindows[slotIndex];
        normalized.text = result.text;
        normalized.checked = result.checked;
        normalized.position = result.position;
        normalized.minimum = result.minimum;
        normalized.maximum = result.maximum;
        normalized.indeterminate = result.indeterminate;
        normalized.hasDetail = result.hasDetail;
        normalized.detail = result.detail;
        return MatchDirectUiMutationBracket(profile, expected, after, error, false);
    } catch (...) {
        return Fail(error, L"in-place: DirectUI delta comparison exception");
    }
}

bool MatchDirectUiMoveTransition(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& before,
    const DirectUiNativeEvidence& after,
    std::wstring& error) noexcept {
    if (before.slotWindows.size() != profile.slotCount ||
        after.slotWindows.size() != profile.slotCount) {
        return Fail(error, L"move: DirectUI backing-slot evidence is incomplete");
    }
    const int64_t deltaX = static_cast<int64_t>(after.root.bounds.left) -
        before.root.bounds.left;
    const int64_t deltaY = static_cast<int64_t>(after.root.bounds.top) -
        before.root.bounds.top;
    if (!SameMovedWindow(before.root, after.root, deltaX, deltaY) ||
        !SameMovedWindow(before.directUi, after.directUi, deltaX, deltaY) ||
        before.dpi != after.dpi || before.ownerHwnd != after.ownerHwnd ||
        before.title != after.title || before.cloaked != after.cloaked ||
        !SameRect(before.clientBounds, after.clientBounds) ||
        static_cast<int64_t>(before.clientOriginScreen.x) + deltaX !=
            after.clientOriginScreen.x ||
        static_cast<int64_t>(before.clientOriginScreen.y) + deltaY !=
            after.clientOriginScreen.y ||
        before.pageHosts != after.pageHosts || before.pageStatics != after.pageStatics ||
        before.propertySheetPageHwnd != after.propertySheetPageHwnd ||
        !SameImplementationIdentity(
            before.implementationWindows, after.implementationWindows)) {
        return Fail(error, L"move: DirectUI root or anchor changed beyond translation");
    }
    for (size_t index = 0; index < profile.slotCount; ++index) {
        if (profile.slots[index].virtualSource) continue;
        if (!SameMovedWindow(
                before.slotWindows[index], after.slotWindows[index], deltaX, deltaY)) {
            return Fail(error, L"move: DirectUI backing slot changed beyond translation");
        }
    }
    return true;
}

bool MatchDirectUiRefreshTransition(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& before,
    const DirectUiNativeEvidence& after,
    std::wstring& error) noexcept {
    if (before.slotWindows.size() != profile.slotCount ||
        after.slotWindows.size() != profile.slotCount)
        return Fail(error, L"refresh: DirectUI backing-slot evidence is incomplete");
    if (!after.cloaked)
        return Fail(error, L"refresh: native DirectUI root lost its application cloak");

    const int64_t deltaX = static_cast<int64_t>(after.root.bounds.left) -
        before.root.bounds.left;
    const int64_t deltaY = static_cast<int64_t>(after.root.bounds.top) -
        before.root.bounds.top;
    auto root = after.root;
    root.enabled = before.root.enabled;
    auto directUi = after.directUi;
    directUi.enabled = before.directUi.enabled;
    if (!SameMovedWindow(before.root, root, deltaX, deltaY) ||
        !SameMovedWindow(before.directUi, directUi, deltaX, deltaY) ||
        before.dpi != after.dpi || before.ownerHwnd != after.ownerHwnd ||
        before.title != after.title ||
        !SameRect(before.clientBounds, after.clientBounds) ||
        static_cast<int64_t>(before.clientOriginScreen.x) + deltaX !=
            after.clientOriginScreen.x ||
        static_cast<int64_t>(before.clientOriginScreen.y) + deltaY !=
            after.clientOriginScreen.y ||
        before.pageHosts != after.pageHosts || before.pageStatics != after.pageStatics ||
        before.propertySheetPageHwnd != after.propertySheetPageHwnd ||
        !SameImplementationIdentity(
            before.implementationWindows, after.implementationWindows)) {
        return Fail(error,
            L"refresh: DirectUI root, owner, geometry, or implementation identity changed");
    }

    for (size_t index = 0; index < profile.slotCount; ++index) {
        const DirectUiSlot& slot = profile.slots[index];
        if (slot.virtualSource) continue;
        const auto& oldValue = before.slotWindows[index];
        const auto& newValue = after.slotWindows[index];
        auto stable = newValue;
        stable.bounds = oldValue.bounds;
        stable.enabled = oldValue.enabled;
        stable.text = oldValue.text;
        stable.note = oldValue.note;
        stable.checked = oldValue.checked;
        stable.minimum = oldValue.minimum;
        stable.maximum = oldValue.maximum;
        stable.position = oldValue.position;
        stable.indeterminate = oldValue.indeterminate;
        // Typed content a still-admitted control may legitimately change:
        // selection, item text, caret, item checks, and the item census itself
        // as an application populates a collection. The shape facets that say
        // which control this is stay pinned, and they are checked here rather
        // than normalized away.
        if (!SameDirectUiDetailShape(oldValue, newValue))
            return Fail(error, L"refresh: DirectUI backing typed shape changed");
        stable.hasDetail = oldValue.hasDetail;
        stable.detail = oldValue.detail;
        if (!SameProfileWindow(slot, oldValue, stable))
            return Fail(error, L"refresh: DirectUI backing identity or stable facets changed");
        // An actionable label is the contract for what its route does, and UIA
        // cannot corroborate it while the source is cloaked. A rejection here
        // clears the discovery record, so the page is re-evaluated with fresh
        // UIA evidence rather than dispatched against a stale caption.
        if ((oldValue.text != newValue.text || oldValue.note != newValue.note) &&
            (slot.kind == ControlKind::Button || slot.kind == ControlKind::CheckBox ||
             slot.kind == ControlKind::ThreeState ||
             (slot.kind == ControlKind::RadioButton && !slot.captureBitmap)))
            return Fail(error, L"refresh: actionable DirectUI label changed without UIA evidence");
        if (slot.kind == ControlKind::SysLink &&
            oldValue.detail.items != newValue.detail.items)
            return Fail(error, L"refresh: DirectUI link caption changed without UIA evidence");
        if (slot.kind == ControlKind::Toolbar &&
            !SameToolbarCommands(oldValue.detail.toolbarItems, newValue.detail.toolbarItems))
            return Fail(error, L"refresh: DirectUI toolbar command set changed without UIA evidence");
        if (newValue.bounds.right <= newValue.bounds.left ||
            newValue.bounds.bottom <= newValue.bounds.top ||
            !Inside(newValue.bounds, after.directUi.bounds))
            return Fail(error, L"refresh: DirectUI backing geometry escaped the anchor");
    }
    return true;
}

bool DirectUiCaptureFailureIsTopologyChange(std::wstring_view error) noexcept {
    constexpr std::wstring_view prefixes[] = {
        L"generic A/B: root class or descendant count changed",
        L"generic A/B: implementation HWND inventory changed",
        L"generic A/B: implementation HWND class is outside",
        L"generic A/B: visible backing HWND count changed",
        L"generic A/B: backing HWND no longer matches",
        L"generic A/B: semantic backing slot is unavailable",
        L"A/B: descendant count is outside the profile bound",
        L"A/B: DirectUI anchor shape mismatch",
        L"A/B: unexpected visible or implementation HWND class",
        L"A/B: implementation HWND census mismatch",
        L"A/B: visible native backing count does not match",
        L"A/B: native backing class/style/control id is outside",
        L"A/B: profile backing slot has no native window",
        L"U: composite item census changed",
    };
    return std::any_of(std::begin(prefixes), std::end(prefixes),
        [&](std::wstring_view prefix) { return error.starts_with(prefix); });
}

bool MatchDirectUiEvidence(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& native,
    const DirectUiUiaEvidence& uia,
    std::wstring& error) noexcept {
    if (uia.rootHwnd != native.root.hwnd || uia.directUiHwnd != native.directUi.hwnd)
        return Fail(error, L"U: root or DirectUI anchor backing HWND mismatch");

    // Virtual and backing slots each require exactly one UIA semantic with the
    // declared role. Backing slots additionally require geometry/label parity.
    std::unordered_set<HWND> backingSeen;
    for (size_t index = 0; index < profile.slotCount; ++index) {
        const DirectUiSlot& slot = profile.slots[index];
        if (slot.virtualSource) {
            const std::wstring_view automationId = slot.uiaAutomationId.empty()
                ? slot.semanticKey : slot.uiaAutomationId;
            const auto count = static_cast<size_t>(std::count_if(
                uia.semantics.begin(), uia.semantics.end(), [&](const auto& value) {
                    return value.semanticKey == automationId;
                }));
            const auto* semantic = FindSemanticByAutomationId(uia, automationId);
            if (!semantic || count != 1) {
                error = L"U: virtual slot " + std::wstring(slot.semanticKey) +
                    L" requires exactly one semantic (count=" + std::to_wstring(count) +
                    L", nativeCloaked=" + std::to_wstring(native.cloaked) + L")";
                return false;
            }
            const RECT& scope = slot.uiaBoundsScope == DirectUiBoundsScope::Root
                ? native.root.bounds : native.directUi.bounds;
            if ((slot.uiaControlType && semantic->controlType != slot.uiaControlType) ||
                (!slot.uiaClassName.empty() && semantic->className != slot.uiaClassName) ||
                (!slot.uiaFrameworkId.empty() && semantic->frameworkId != slot.uiaFrameworkId) ||
                (slot.project && semantic->backingHwnd != nullptr) ||
                semantic->enabled != slot.uiaEnabled ||
                semantic->focusable != slot.uiaFocusable ||
                semantic->actionable != slot.uiaActionable ||
                semantic->offscreen != slot.uiaOffscreen ||
                (slot.pinUiaPatterns &&
                    (semantic->patternMask != slot.uiaPatternMask ||
                     semantic->toggleState != slot.uiaToggleState)) ||
                (!slot.uiaOffscreen &&
                    (semantic->bounds.right <= semantic->bounds.left ||
                     semantic->bounds.bottom <= semantic->bounds.top ||
                     !Inside(semantic->bounds, scope)))) {
                error = L"U: virtual slot mismatch (slot=" + std::wstring(slot.semanticKey) +
                    L", id=" + semantic->semanticKey + L", class=" + semantic->className +
                    L", framework=" + semantic->frameworkId + L", type=" +
                    std::to_wstring(semantic->controlType) + L", focusable=" +
                    std::to_wstring(semantic->focusable) + L", enabled=" +
                    std::to_wstring(semantic->enabled) + L", actionable=" +
                    std::to_wstring(semantic->actionable) + L")";
                return false;
            }
            continue;
        }
        const DirectUiWindowEvidence& backing = native.slotWindows[index];
        const auto count = static_cast<size_t>(std::count_if(
            uia.semantics.begin(), uia.semantics.end(), [&](const auto& value) {
                return value.backingHwnd == backing.hwnd;
            }));
        const auto* byBacking = FindSemanticByBacking(uia, backing.hwnd);
        if (!byBacking || count != 1) {
            error = L"U: backing slot " + std::wstring(slot.semanticKey) +
                L" requires exactly one semantic (count=" + std::to_wstring(count) + L")";
            return false;
        }
        const bool exactSemanticClass =
            (!slot.uiaControlType || byBacking->controlType == slot.uiaControlType) &&
            (slot.uiaFrameworkId.empty() || byBacking->frameworkId == slot.uiaFrameworkId) &&
            (slot.uiaClassName.empty() ||
                byBacking->className == slot.uiaClassName);
        const bool automationIdMatches = slot.uiaAutomationId.empty() ||
            byBacking->semanticKey == slot.uiaAutomationId;
        const bool expectedBacking = backingSeen.insert(backing.hwnd).second;
        const bool geometryMatches = RectNear(byBacking->bounds, backing.bounds);
        // Kinds whose window text is content rather than a label: the provider's
        // name comes from an associated static or is absent entirely, so holding
        // the two to each other would reject every legitimate surface.
        const bool labelIsWindowText = !(
            slot.kind == ControlKind::ProgressBar ||
            slot.kind == ControlKind::StaticIcon ||
            slot.kind == ControlKind::Edit ||
            slot.kind == ControlKind::Password ||
            slot.kind == ControlKind::ComboBox ||
            slot.kind == ControlKind::ListBox ||
            slot.kind == ControlKind::ListView ||
            slot.kind == ControlKind::TabControl ||
            slot.kind == ControlKind::StatusBar ||
            slot.kind == ControlKind::Toolbar ||
            slot.kind == ControlKind::SysLink ||
            (slot.kind == ControlKind::RadioButton && slot.captureBitmap));
        const bool nativeLabelMatches = !labelIsWindowText ||
            DisplayText(backing.text) == byBacking->name;
        // A status bar and a toolbar have no accessible name of their own; their
        // parts and buttons carry the text and the adapter publishes those.
        const bool nameRequired = slot.kind != ControlKind::StatusBar &&
            slot.kind != ControlKind::Toolbar;
        const bool nativeNoteMatches = backing.note.empty() || backing.note == byBacking->helpText;
        // A BS_3STATE box reaches a value the shared checked facet cannot carry,
        // so its expected toggle comes from the adapter's own reading.
        const int expectedToggleState = slot.kind == ControlKind::CheckBox
            ? (backing.checked ? ToggleState_On : ToggleState_Off)
            : (slot.kind == ControlKind::ThreeState && backing.hasDetail
                ? (backing.detail.checked == 2
                    ? ToggleState_Indeterminate
                    : (backing.detail.checked == 1 ? ToggleState_On : ToggleState_Off))
                : slot.uiaToggleState);
        const bool patternMatches = !slot.pinUiaPatterns ||
            (byBacking->patternMask == slot.uiaPatternMask &&
             byBacking->capabilityMask == slot.uiaCapabilityMask &&
             byBacking->toggleState == expectedToggleState);
        // The adapter's item census pinned at discovery must still hold. This is
        // the equality that catches a collection mutating across the bracket.
        const bool itemCountMatches =
            slot.nativeItemCount == static_cast<size_t>(-1) ||
            (backing.hasDetail &&
             DirectUiNativeItemCount(backing.detail) == slot.nativeItemCount);
        if (!itemCountMatches) {
            // A collection that grew or shrank is a new page state rather than a
            // corrupt one, so it is reported as a topology change: the census the
            // provider corroborated no longer holds, and the surface is
            // re-evaluated from scratch instead of pinned to a stale count.
            error = L"U: composite item census changed (slot=" +
                std::wstring(slot.semanticKey) + L", items=" +
                std::to_wstring(backing.hasDetail
                    ? DirectUiNativeItemCount(backing.detail)
                    : static_cast<size_t>(0)) +
                L", expected=" + std::to_wstring(slot.nativeItemCount) + L")";
            return false;
        }
        if (!expectedBacking || byBacking->focusable != slot.uiaFocusable ||
            byBacking->enabled != slot.uiaEnabled ||
            byBacking->actionable != slot.uiaActionable ||
            byBacking->offscreen != slot.uiaOffscreen ||
            !patternMatches ||
            (slot.project && nameRequired && byBacking->name.empty()) ||
            !exactSemanticClass || !automationIdMatches || !geometryMatches ||
            !nativeLabelMatches || !nativeNoteMatches) {
            error = L"U: backing semantic mismatch (slot=" + std::wstring(slot.semanticKey) +
                L", key=" + byBacking->semanticKey +
                L", name=" + byBacking->name + L", class=" + byBacking->className +
                L", framework=" + byBacking->frameworkId + L", type=" +
                std::to_wstring(byBacking->controlType) + L", focusable=" +
                std::to_wstring(byBacking->focusable) + L", enabled=" +
                std::to_wstring(byBacking->enabled) + L", actionable=" +
                std::to_wstring(byBacking->actionable) + L", hwnd=" +
                std::to_wstring(reinterpret_cast<uintptr_t>(byBacking->backingHwnd)) +
                L", unique=" + std::to_wstring(expectedBacking) +
                L", semanticClass=" + std::to_wstring(exactSemanticClass) +
                L", automationId=" + std::to_wstring(automationIdMatches) +
                L", geometry=" + std::to_wstring(geometryMatches) +
                L", label=" + std::to_wstring(nativeLabelMatches) +
                L", note=" + std::to_wstring(nativeNoteMatches) + L")";
            return false;
        }
    }

    // Nothing actionable/focusable may exist beyond the declared slots. The
    // DirectUI host bounds distinguish page content from root nonclient chrome.
    size_t accounted = 0;
    for (const auto& value : uia.semantics) {
        bool declared = false;
        for (size_t index = 0; index < profile.slotCount; ++index) {
            const DirectUiSlot& slot = profile.slots[index];
            if (slot.virtualSource) {
                const std::wstring_view automationId = slot.uiaAutomationId.empty()
                    ? slot.semanticKey : slot.uiaAutomationId;
                if (value.semanticKey == automationId) { declared = true; break; }
            } else if (value.backingHwnd == native.slotWindows[index].hwnd) {
                declared = true; break;
            }
        }
        if (declared) ++accounted;
        else {
            error = L"U: unexpected actionable semantic (key=" + value.semanticKey +
                L", name=" + value.name + L", class=" + value.className +
                L", framework=" + value.frameworkId + L", type=" +
                std::to_wstring(value.controlType) + L", focusable=" +
                std::to_wstring(value.focusable) + L", actionable=" +
                std::to_wstring(value.actionable) + L", hwnd=" +
                std::to_wstring(reinterpret_cast<uintptr_t>(value.backingHwnd)) + L")";
            return false;
        }
    }
    if (accounted != uia.semantics.size())
        return Fail(error, L"U: semantic accounting mismatch");
    return true;
}

uint64_t DirectUiSemanticNodeId(
    std::wstring_view adapterId,
    std::wstring_view semanticKey) noexcept {
    uint64_t hash = 1469598103934665603ull;
    const auto append = [&](std::wstring_view value) {
        for (wchar_t character : value) {
            hash ^= static_cast<uint16_t>(character);
            hash *= 1099511628211ull;
        }
        hash ^= 0xffu;
        hash *= 1099511628211ull;
    };
    append(adapterId);
    append(semanticKey);
    return hash | (1ull << 63);
}

bool RefreshDirectUiSnapshotFromNative(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& native,
    WindowSnapshot& snapshot,
    std::unordered_map<uint64_t, DirectUiActionBinding>& bindings,
    std::wstring& error) noexcept {
    try {
        if (native.slotWindows.size() != profile.slotCount)
            return Fail(error, L"refresh: DirectUI backing evidence is incomplete");
        const size_t projectedCount = static_cast<size_t>(std::count_if(
            profile.slots, profile.slots + profile.slotCount,
            [](const DirectUiSlot& slot) { return slot.project; }));
        if (snapshot.nodes.size() != projectedCount)
            return Fail(error, L"refresh: projected DirectUI topology changed");

        snapshot.canCancel = std::any_of(profile.slots, profile.slots + profile.slotCount,
            [](const DirectUiSlot& slot) { return slot.cancel; });
        snapshot.nativeHwnd = native.root.hwnd;
        snapshot.ownerHwnd = native.ownerHwnd;
        snapshot.title = native.title;
        snapshot.dpi = native.dpi;
        snapshot.bounds = native.root.bounds;
        snapshot.clientBounds = native.clientBounds;
        snapshot.windowStyle = native.root.style;
        snapshot.windowExStyle = native.root.exStyle;
        snapshot.visible = native.root.visible;
        snapshot.enabled = native.root.enabled;
        snapshot.showInTaskbar = native.ownerHwnd == nullptr ||
            (native.root.exStyle & WS_EX_APPWINDOW) != 0;
        snapshot.rtl = (native.root.exStyle & WS_EX_LAYOUTRTL) != 0;
        bindings.clear();

        size_t nodeIndex = 0;
        for (size_t slotIndex = 0; slotIndex < profile.slotCount; ++slotIndex) {
            const DirectUiSlot& slot = profile.slots[slotIndex];
            if (!slot.project) continue;
            ControlNode& node = snapshot.nodes[nodeIndex++];
            if (node.nodeId != DirectUiSemanticNodeId(profile.adapterId, slot.semanticKey) ||
                node.kind != slot.kind || node.semanticKey != slot.semanticKey)
                return Fail(error, L"refresh: projected DirectUI node identity changed");
            node.isDefault = slot.defaultButton;
            if (slot.virtualSource) {
                node.generation = native.root.generation;
                node.enabled = native.root.enabled && native.directUi.enabled && slot.uiaEnabled;
                node.tabStop = node.enabled && slot.uiaFocusable && slot.tabIndex >= 0;
                node.tabIndex = node.tabStop ? slot.tabIndex : -1;
                SetDirectUiSupportedActions(slot, node);
                if (slot.action != DirectUiAction::None) {
                    bindings.emplace(node.nodeId, DirectUiActionBinding{
                        native.root.hwnd, native.root.generation, slotIndex,
                        slot.kind, slot.action, slot.secondaryAction, slot.cancel,
                        slot.propertySheetButton });
                }
                continue;
            }

            const DirectUiWindowEvidence& backing = native.slotWindows[slotIndex];
            if (!backing.hwnd || !backing.generation)
                return Fail(error, L"refresh: DirectUI backing identity is unavailable");
            node.generation = backing.generation;
            node.hwnd = backing.hwnd;
            node.controlId = backing.controlId;
            node.tabIndex = backing.tabIndex;
            node.rect = ClientRectFor(backing.bounds, native.clientOriginScreen);
            node.style = backing.style;
            node.exStyle = backing.exStyle;
            node.visible = backing.visible;
            node.enabled = native.root.enabled && native.directUi.enabled &&
                backing.enabled;
            node.tabStop = node.enabled && backing.tabIndex >= 0;
            node.dialogCode = backing.dialogCode;
            // Kinds whose window text is their label. The rest either carry
            // content rather than a caption or take their name from an
            // associated static, and a name UIA supplied at admission cannot be
            // re-read while the source is cloaked.
            if (slot.kind == ControlKind::StaticText ||
                slot.kind == ControlKind::Button ||
                slot.kind == ControlKind::CheckBox ||
                slot.kind == ControlKind::ThreeState ||
                slot.kind == ControlKind::GroupBox ||
                (slot.kind == ControlKind::RadioButton && !slot.captureBitmap)) {
                node.text = backing.text;
                node.automationName = DisplayText(backing.text);
            }
            if (slot.kind == ControlKind::StaticText) {
                node.helpText = backing.note;
                node.accessKey.clear();
            } else if (!backing.note.empty()) {
                node.helpText = backing.note;
            }
            if (slot.kind == ControlKind::Button) {
                const DWORD buttonType = static_cast<DWORD>(backing.style) & BS_TYPEMASK;
                node.isDefault = buttonType == BS_DEFPUSHBUTTON ||
                    buttonType == BS_DEFCOMMANDLINK;
            }
            node.checked = backing.checked ? 1 : 0;
            node.groupStart = (backing.style & WS_GROUP) != 0;
            node.minimum = backing.minimum;
            node.maximum = backing.maximum;
            node.position = backing.position;
            node.indeterminate = backing.indeterminate;
            if (slot.captureBitmap) {
                if (backing.imageFormat != L"bgra8-premultiplied" ||
                    backing.imageWidth == 0 || backing.imageHeight == 0 ||
                    backing.imageData.size() !=
                        static_cast<size_t>(backing.imageWidth) * backing.imageHeight * 4)
                    return Fail(error, L"refresh: native bitmap pixels are missing");
                node.imageWidth = backing.imageWidth;
                node.imageHeight = backing.imageHeight;
                node.imageFormat = backing.imageFormat;
                node.imageData = backing.imageData;
            }
            if (!ApplyDirectUiDetailToNode(slot, backing, node, error)) return false;
            if (!node.tabStop) node.tabIndex = -1;
            SetDirectUiSupportedActions(slot, node);
            if (slot.action != DirectUiAction::None) {
                bindings.emplace(node.nodeId, DirectUiActionBinding{
                    backing.hwnd, backing.generation, slotIndex,
                    slot.kind, slot.action, slot.secondaryAction, slot.cancel,
                    slot.propertySheetButton });
            }
        }
        return true;
    } catch (...) {
        return Fail(error, L"refresh: DirectUI native snapshot exception");
    }
}

bool NormalizeDirectUiEvidenceText(
    std::wstring_view input,
    std::wstring& output) noexcept {
    try {
        if (input.size() > 1024 || input.find(L'\0') != std::wstring_view::npos) return false;
        // Localization is evidence. Preserve code units and whitespace exactly;
        // normalization here means only bounded, unambiguous ownership.
        output.assign(input);
        return true;
    } catch (...) {
        return false;
    }
}

bool IsMicrosoftWindowsSignerName(std::wstring_view name) noexcept {
    return FluentShell::EqualsIgnoreCase(name, L"Microsoft Windows") ||
        FluentShell::EqualsIgnoreCase(name, L"Microsoft Corporation");
}

// True when a handoff-declared route is a page navigation: the application
// replaces the page inside the same top-level window, so the surface can keep its
// projection and admit the next page in place instead of handing the whole window
// back to native. Cancel is excluded because it ends the window rather than
// navigating it, and so keeps the terminal handoff route.
bool DirectUiNavigationMayStayProjected(
    const DirectUiActionBinding& binding) noexcept {
    if (binding.cancel) return false;
    switch (binding.action) {
    case DirectUiAction::HandoffClick:
    case DirectUiAction::HandoffLinkClick:
        return binding.propertySheetButton == -1;
    case DirectUiAction::HandoffPropertySheetButton:
        return binding.propertySheetButton == PSBTN_BACK ||
            binding.propertySheetButton == PSBTN_NEXT ||
            binding.propertySheetButton == PSBTN_FINISH;
    default:
        return false;
    }
}

bool DirectUiHandoffMayPost(
    bool pageRevalidated,
    bool bindingGenerationMatches,
    bool proxyIsolated,
    bool nativeVisible,
    bool nativeUncloaked) noexcept {
    return pageRevalidated && bindingGenerationMatches && proxyIsolated &&
        nativeVisible && nativeUncloaked;
}

bool RevalidateAndPostDirectUiPropertySheetAction(
    SourceThreadAgent& agent,
    const DirectUiWindowProfile& profile,
    const DirectUiActionBinding& binding,
    DWORD timeoutMs,
    HANDLE cancelEvent,
    std::wstring& error) noexcept {
    error.clear();
    if (binding.action != DirectUiAction::HandoffPropertySheetButton ||
        binding.slotIndex >= profile.slotCount ||
        !profile.slots[binding.slotIndex].virtualSource ||
        profile.slots[binding.slotIndex].propertySheetButton !=
            binding.propertySheetButton ||
        !IsPropertySheetButton(binding.propertySheetButton)) {
        return Fail(error, L"property-sheet action binding is outside the admitted contract");
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    const auto remainingMs = [&]() -> DWORD {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return 0;
        return static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count());
    };
    struct Result final { bool ok = false; DirectUiUiaEvidence value; std::wstring error; };
    try {
        DirectUiNativeEvidence before;
        DWORD remaining = remainingMs();
        if (!remaining || !agent.CaptureDirectUiNativeEvidence(
                profile, before, error, remaining, cancelEvent)) {
            return Fail(error, L"property-sheet action native A failed: " + error);
        }
        if (before.cloaked || before.root.hwnd != binding.hwnd ||
            before.root.generation != binding.generation ||
            !before.root.visible || !before.root.enabled ||
            !before.propertySheetPageHwnd) {
            return Fail(error, L"property-sheet action root is not current, visible, and uncloaked");
        }

        std::vector<std::wstring> virtualAutomationIds;
        std::vector<HWND> backingWindows;
        for (size_t index = 0; index < profile.slotCount; ++index) {
            const DirectUiSlot& slot = profile.slots[index];
            if (slot.virtualSource) {
                virtualAutomationIds.emplace_back(slot.uiaAutomationId.empty()
                    ? slot.semanticKey : slot.uiaAutomationId);
            } else if (before.slotWindows[index].hwnd) {
                backingWindows.push_back(before.slotWindows[index].hwnd);
            }
        }
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        const HWND root = before.root.hwnd;
        const HWND directUi = before.directUi.hwnd;
        const DWORD processId = GetCurrentProcessId();
        std::thread worker([promise, root, directUi, processId,
                virtualAutomationIds = std::move(virtualAutomationIds),
                backingWindows = std::move(backingWindows)] {
            Result result;
            try {
                result.ok = CaptureUia(root, directUi, processId, true,
                    virtualAutomationIds, backingWindows, false,
                    result.value, result.error);
            } catch (...) {
                result.error = L"property-sheet UIA evidence worker exception";
            }
            try { promise->set_value(std::move(result)); } catch (...) {}
        });
        remaining = (std::min)(remainingMs(), kUiaDeadlineMs);
        auto status = remaining
            ? future.wait_for(std::chrono::milliseconds(remaining))
            : std::future_status::timeout;
        if (cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0)
            status = std::future_status::timeout;
        if (status != std::future_status::ready) {
            agent.PoisonDirectUiUia();
            worker.detach();
            return Fail(error,
                L"property-sheet UIA deadline expired; worker abandoned and agent poisoned");
        }
        worker.join();
        auto uia = future.get();
        if (!uia.ok)
            return Fail(error, L"property-sheet UIA capture failed: " + uia.error);
        if (!MatchDirectUiEvidence(profile, before, uia.value, error))
            return Fail(error, L"property-sheet UIA contract changed: " + error);

        remaining = remainingMs();
        if (!remaining)
            return Fail(error, L"property-sheet action deadline expired before native B");
        return agent.PostDirectUiPropertySheetButton(
            profile, before, binding, error, remaining, cancelEvent);
    } catch (...) {
        return Fail(error, L"property-sheet action revalidation exception");
    }
}

DirectUiAdmissionResult InspectDirectUiSurface(
    SourceThreadAgent& agent,
    const DirectUiWindowProfile& profile,
    DWORD timeoutMs,
    HANDLE cancelEvent,
    std::wstring& diagnostic,
    WindowSnapshot* snapshot,
    std::unordered_map<uint64_t, DirectUiActionBinding>* bindings,
    DirectUiNativeEvidence* nativeEvidence) noexcept {
    diagnostic.clear();
    const auto absoluteDeadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    std::wstring imagePath;
    std::wstring processError;
    if (!ResolveDirectUiWindowProfile(imagePath, processError) ||
        std::chrono::steady_clock::now() >= absoluteDeadline) {
        diagnostic = L"DirectUI adapter rejected at process: " +
            (processError.empty() ? L"profile resolution failed" : processError);
        return DirectUiAdmissionResult::Rejected;
    }
    if (agent.DirectUiUiaPoisoned()) {
        diagnostic = L"DirectUI adapter rejected at U: UIA worker was previously abandoned";
        return DirectUiAdmissionResult::Rejected;
    }
    struct Result final { bool ok = false; DirectUiUiaEvidence value; std::wstring error; };
    try {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto remainingMs = [&]() -> DWORD {
                const auto now = std::chrono::steady_clock::now();
                if (now >= absoluteDeadline) return 0;
                return static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    absoluteDeadline - now).count());
            };
            DirectUiNativeEvidence before;
            DWORD remaining = remainingMs();
            if (!remaining || !agent.CaptureDirectUiNativeEvidence(
                    profile, before, diagnostic, remaining, cancelEvent)) {
                diagnostic = L"DirectUI adapter rejected at A: " + diagnostic;
                return DirectUiAdmissionResult::Rejected;
            }
            auto promise = std::make_shared<std::promise<Result>>();
            auto future = promise->get_future();
            const HWND root = before.root.hwnd;
            const HWND directUi = before.directUi.hwnd;
            const DWORD processId = GetCurrentProcessId();
            const bool allowDeclaredOffscreen = before.cloaked;
            std::vector<std::wstring> virtualAutomationIds;
            std::vector<HWND> backingWindows;
            for (size_t index = 0; index < profile.slotCount; ++index) {
                const DirectUiSlot& slot = profile.slots[index];
                if (slot.virtualSource) {
                    virtualAutomationIds.emplace_back(slot.uiaAutomationId.empty()
                        ? slot.semanticKey : slot.uiaAutomationId);
                } else if (before.slotWindows[index].hwnd) {
                    backingWindows.push_back(before.slotWindows[index].hwnd);
                }
            }
            std::thread worker([promise, root, directUi, processId, allowDeclaredOffscreen,
                    virtualAutomationIds = std::move(virtualAutomationIds),
                    backingWindows = std::move(backingWindows)] {
                Result result;
                try {
                    result.ok = CaptureUia(root, directUi, processId,
                        allowDeclaredOffscreen, virtualAutomationIds, backingWindows, false,
                        result.value, result.error);
                }
                catch (...) { result.error = L"UIA evidence worker exception"; }
                try { promise->set_value(std::move(result)); } catch (...) {}
            });
            remaining = (std::min)(remainingMs(), kUiaDeadlineMs);
            auto status = remaining
                ? future.wait_for(std::chrono::milliseconds(remaining))
                : std::future_status::timeout;
            if (cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0)
                status = std::future_status::timeout;
            if (status != std::future_status::ready) {
                agent.PoisonDirectUiUia();
                worker.detach();
                diagnostic = L"DirectUI adapter rejected at U: bounded UIA deadline expired; worker abandoned and agent poisoned";
                return DirectUiAdmissionResult::Rejected;
            }
            worker.join();
            auto uia = future.get();
            if (!uia.ok || !MatchDirectUiEvidence(profile, before, uia.value, diagnostic)) {
                diagnostic = L"DirectUI adapter rejected at U: " +
                    (diagnostic.empty() ? uia.error : diagnostic);
                return DirectUiAdmissionResult::Rejected;
            }
            DirectUiNativeEvidence after;
            remaining = remainingMs();
            const bool capturedAfter = remaining && agent.CaptureDirectUiNativeEvidence(
                profile, after, diagnostic, remaining, cancelEvent);
            if (capturedAfter &&
                MatchDirectUiMutationBracket(profile, before, after, diagnostic)) {
                if (snapshot && bindings) {
                    WindowSnapshot composite = *snapshot;
                    std::unordered_map<uint64_t, DirectUiActionBinding> capturedBindings;
                    if (!BuildCompositeSnapshot(agent, profile, imagePath, after, uia.value,
                            composite, capturedBindings, diagnostic)) {
                        diagnostic = L"DirectUI adapter rejected at composite: " + diagnostic;
                        return DirectUiAdmissionResult::Rejected;
                    }
                    *snapshot = std::move(composite);
                    *bindings = std::move(capturedBindings);
                }
                if (nativeEvidence) *nativeEvidence = std::move(after);
                diagnostic = L"DirectUI adapter admitted exact signed page";
                return DirectUiAdmissionResult::Admitted;
            }
            if (attempt == 0 &&
                diagnostic == L"B: mutation epoch changed during UIA capture")
                continue;
            diagnostic = L"DirectUI adapter rejected at B: " + diagnostic;
            return DirectUiAdmissionResult::Rejected;
        }
        diagnostic = L"DirectUI adapter rejected at B: mutation bracket retry exhausted";
        return DirectUiAdmissionResult::Rejected;
    } catch (...) {
        diagnostic = L"DirectUI adapter rejected at U: worker setup exception";
        return DirectUiAdmissionResult::Rejected;
    }
}

DirectUiAdmissionResult InspectGenericDirectUiSurface(
    SourceThreadAgent& agent,
    DWORD timeoutMs,
    HANDLE cancelEvent,
    std::wstring& diagnostic,
    std::shared_ptr<DirectUiOwnedProfile>* ownedProfile,
    WindowSnapshot* snapshot,
    std::unordered_map<uint64_t, DirectUiActionBinding>* bindings,
    DirectUiNativeEvidence* nativeEvidence) noexcept {
    diagnostic.clear();
    const auto absoluteDeadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    std::wstring imagePath;
    if (!ResolveGenericDirectUiImage(imagePath, diagnostic)) {
        diagnostic = L"Generic DirectUI adapter rejected at process: " + diagnostic;
        return DirectUiAdmissionResult::Rejected;
    }
    if (agent.DirectUiUiaPoisoned()) {
        diagnostic = L"Generic DirectUI adapter rejected at U: UIA worker was previously abandoned";
        return DirectUiAdmissionResult::Rejected;
    }
    struct Result final { bool ok = false; DirectUiUiaEvidence value; std::wstring error; };
    try {
        constexpr int kMutationBracketAttempts = 2;
        for (int attempt = 0; attempt < kMutationBracketAttempts; ++attempt) {
            const auto remainingMs = [&]() -> DWORD {
                const auto now = std::chrono::steady_clock::now();
                if (now >= absoluteDeadline) return 0;
                return static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    absoluteDeadline - now).count());
            };
            DirectUiBootstrapEvidence before;
            DWORD remaining = remainingMs();
            if (!remaining || !agent.CaptureDirectUiBootstrapEvidence(
                    before, diagnostic, remaining, cancelEvent)) {
                diagnostic = L"Generic DirectUI adapter rejected at A: " + diagnostic;
                return DirectUiAdmissionResult::Rejected;
            }

            auto promise = std::make_shared<std::promise<Result>>();
            auto future = promise->get_future();
            const HWND root = before.native.root.hwnd;
            const HWND directUi = before.native.directUi.hwnd;
            const DWORD processId = GetCurrentProcessId();
            std::thread worker([promise, root, directUi, processId] {
                Result result;
                try {
                    result.ok = CaptureUia(root, directUi, processId, true, {}, {}, true,
                        result.value, result.error);
                } catch (...) {
                    result.error = L"generic UIA evidence worker exception";
                }
                try { promise->set_value(std::move(result)); } catch (...) {}
            });
            remaining = (std::min)(remainingMs(), kUiaDeadlineMs);
            auto status = remaining
                ? future.wait_for(std::chrono::milliseconds(remaining))
                : std::future_status::timeout;
            if (cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0)
                status = std::future_status::timeout;
            if (status != std::future_status::ready) {
                agent.PoisonDirectUiUia();
                worker.detach();
                diagnostic = L"Generic DirectUI adapter rejected at U: bounded UIA deadline expired; worker abandoned and agent poisoned";
                return DirectUiAdmissionResult::Rejected;
            }
            worker.join();
            auto uia = future.get();
            if (!uia.ok) {
                diagnostic = L"Generic DirectUI adapter rejected at U: " + uia.error;
                return DirectUiAdmissionResult::Rejected;
            }

            std::shared_ptr<DirectUiOwnedProfile> discovered;
            if (!BuildGenericSemanticProfile(
                    before, uia.value, imagePath, discovered, diagnostic)) {
                diagnostic = L"Generic DirectUI adapter rejected at contract: " + diagnostic;
                return DirectUiAdmissionResult::Rejected;
            }
            agent.AdoptDirectUiProfile(discovered);
            DirectUiNativeEvidence after;
            remaining = remainingMs();
            const bool capturedAfter = remaining && agent.CaptureDirectUiNativeEvidence(
                discovered->profile, after, diagnostic, remaining, cancelEvent);
            if (capturedAfter && MatchBootstrapToGenerated(before, after, diagnostic) &&
                MatchDirectUiEvidence(discovered->profile, after, uia.value, diagnostic)) {
                if (snapshot && bindings) {
                    WindowSnapshot composite = *snapshot;
                    std::unordered_map<uint64_t, DirectUiActionBinding> capturedBindings;
                    if (!BuildCompositeSnapshot(agent, discovered->profile, imagePath, after,
                            uia.value, composite, capturedBindings, diagnostic)) {
                        diagnostic = L"Generic DirectUI adapter rejected at composite: " + diagnostic;
                        return DirectUiAdmissionResult::Rejected;
                    }
                    *snapshot = std::move(composite);
                    *bindings = std::move(capturedBindings);
                }
                if (nativeEvidence) *nativeEvidence = std::move(after);
                if (ownedProfile) *ownedProfile = discovered;
                diagnostic = L"Generic DirectUI adapter admitted capability-derived semantic page";
                return DirectUiAdmissionResult::Admitted;
            }
            if (attempt + 1 < kMutationBracketAttempts &&
                (diagnostic.starts_with(
                    L"generic B: native evidence changed during semantic discovery") ||
                 diagnostic.starts_with(L"generic A/B:")))
                continue;
            diagnostic = L"Generic DirectUI adapter rejected at B: " + diagnostic;
            return DirectUiAdmissionResult::Rejected;
        }
        diagnostic = L"Generic DirectUI adapter rejected at B: mutation bracket retry exhausted";
        return DirectUiAdmissionResult::Rejected;
    } catch (...) {
        diagnostic = L"Generic DirectUI adapter rejected at U: worker setup exception";
        return DirectUiAdmissionResult::Rejected;
    }
}

} // namespace FluentShell::Bridge::Translation
