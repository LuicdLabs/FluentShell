#pragma once

#include "WindowSnapshot.h"

#include <windows.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FluentShell::Bridge::Translation {

class SourceThreadAgent;

// How a projected slot dispatches semantic actions back to native state.
enum class DirectUiAction {
    None,          // noninteractive presentation only
    HandoffClick,  // restore native visibility first, then post BM_CLICK
    HandoffPropertySheetButton, // restore, then post a standard PSM_PRESSBUTTON
    ToggleCheck,   // walk the native checkbox state machine in place
    SelectRadio,   // post BM_CLICK and require exclusive WS_TABSTOP
    // In-place routes below mutate a native-backed control through its own
    // notification path and accept only that control's delta on recapture.
    SetEditText,     // EM_SETSEL + EM_REPLACESEL so EN_CHANGE still reaches the app
    SelectListItem,  // native list/combo/tab selection plus its own selection notify
    SetItemCheck,    // one ListView LVS_EX_CHECKBOXES item state in place
    ToolbarCommand,  // post the toolbar's own WM_COMMAND to its owner
    HandoffLinkClick, // restore native visibility first, then route NM_CLICK
};

enum class DirectUiBoundsScope {
    Anchor,
    Root,
};

enum DirectUiPattern : uint32_t {
    DirectUiPatternNone = 0,
    DirectUiPatternInvoke = 1u << 0,
    DirectUiPatternToggle = 1u << 1,
    DirectUiPatternExpandCollapse = 1u << 2,
    DirectUiPatternSelectionItem = 1u << 3,
    DirectUiPatternValue = 1u << 4,
};

// Structural provider capabilities. These describe a control's shape rather
// than a mutation route, so they are pinned as evidence and deliberately kept
// out of DirectUiPattern: adding them there would retroactively make every
// already-admitted presentation element look behaviorally actionable.
enum DirectUiCapability : uint32_t {
    DirectUiCapabilityNone = 0,
    DirectUiCapabilitySelection = 1u << 0,
    DirectUiCapabilityText = 1u << 1,
    DirectUiCapabilityRangeValue = 1u << 2,
    DirectUiCapabilityScroll = 1u << 3,
    DirectUiCapabilityGrid = 1u << 4,
    DirectUiCapabilityTable = 1u << 5,
};

// UIA control types a composite owner slot absorbs as its own item descendants.
bool IsDirectUiCompositeItemControlType(int controlType) noexcept;

// One projected element of an admitted DirectUI page. A slot is either a
// UIA virtual element with no backing HWND or a native backing HWND whose
// canonical state the engine reads on the source GUI thread.
struct DirectUiSlot final {
    std::wstring_view semanticKey;
    // Projection contract.
    bool project = true;
    ControlKind kind = ControlKind::StaticText;
    std::wstring_view presentationVariant;
    int tabIndex = -1;
    // Source of truth.
    bool virtualSource = true;
    // UIA requirements (virtual slots and backing corroboration).
    int uiaControlType = 0;
    std::wstring_view uiaAutomationId;
    std::wstring_view uiaClassName;
    bool uiaEnabled = true;
    bool uiaFocusable = false;
    bool uiaActionable = false;
    // Native backing requirements. nativeStyleValue/nativeStyleAlt are matched
    // under BS_TYPEMASK for Button slots; nativeControlId -1 means any.
    std::wstring_view nativeClass;
    uint32_t nativeStyleValue = 0;
    uint32_t nativeStyleAlt = 0;
    int nativeControlId = -1;
    // Action routing. A second route the same slot legitimately offers lives in
    // secondaryAction at the end of this struct, where fields added after the
    // profile table was written go so its positional rows stay valid.
    DirectUiAction action = DirectUiAction::None;
    bool cancel = false;
    bool defaultButton = false;
    // Trusted icon resource inside the admitted executable (0 = none).
    int iconResourceId = 0;
    int iconWidth = 0;
    int iconHeight = 0;
    // Provider identity and geometry are profile data rather than engine
    // assumptions. Empty framework id accepts any provider framework.
    std::wstring_view uiaFrameworkId = L"DirectUI";
    DirectUiBoundsScope uiaBoundsScope = DirectUiBoundsScope::Anchor;
    uint64_t nativeStyleMask = 0x0full;
    bool pinUiaPatterns = false;
    uint32_t uiaPatternMask = DirectUiPatternNone;
    int uiaToggleState = -1;
    int propertySheetButton = -1;
    bool uiaOffscreen = false;
    bool captureBitmap = false;
    // Reads this backing HWND through its own registered Win32 adapter so the
    // DirectUI lane inherits that adapter's proven typed contract instead of
    // re-deriving facets. Only set for slots whose native class is the class the
    // adapter was written against.
    bool captureDetail = false;
    // Structural provider capabilities pinned alongside uiaPatternMask.
    uint32_t uiaCapabilityMask = DirectUiCapabilityNone;
    // Composite item census. A list-like slot owns its UIA item descendants
    // instead of each item becoming its own slot; the count is an equality
    // contract corroborated against the native control's own item count.
    size_t compositeItemCount = 0;
    // Native item count this slot's adapter must keep reporting for the
    // projected composite to stay admitted (SIZE_MAX = not a composite).
    size_t nativeItemCount = static_cast<size_t>(-1);
    // A second route the same slot legitimately offers alongside `action`: an
    // editable ComboBox accepts both a selection and typed text, a checkbox
    // ListView both a selection and one item's check. Never a substitute for the
    // primary route, and only advertised when the revision's own typed state
    // says the control currently accepts it.
    DirectUiAction secondaryAction = DirectUiAction::None;
};

// Exact census of implementation HWND classes below the DirectUI host. Every
// count is an equality contract; any drift rejects the whole surface.
struct DirectUiCensus final {
    size_t visibleNotifyWrappers = 0;
    size_t hiddenNotifyWrappers = 0;
    size_t hiddenScrollBars = 0;
    size_t hiddenSysLinks = 0;
    size_t hiddenButtons = 0;
    size_t hiddenPageHosts = 0;
    size_t hiddenPageStaticTexts = 0;
    size_t visiblePageHosts = 0;
    size_t pageHostStaticTexts = 0;
};

// A declarative application contract. The engine contains no per-application
// branches; every supported page is one row of profile data.
struct DirectUiWindowProfile final {
    std::wstring_view adapterId;
    std::wstring_view pageId;
    std::wstring_view executableBasename;
    uint16_t fileVersion[4] = {};
    std::wstring_view rootClass;
    DirectUiCensus census;
    const DirectUiSlot* slots = nullptr;
    size_t slotCount = 0;
    // The native dialog manager exposes the DirectUI host as its only tab
    // stop; focus traversal within that host is pinned by slot tabIndex.
    bool directUiOwnsTabOrder = false;
    size_t minDescendants = 0;
    size_t maxDescendants = 0;
    struct ImplementationClassContract final {
        std::wstring_view className;
        bool visible = false;
        size_t count = 0;
    };
    const ImplementationClassContract* implementationClasses = nullptr;
    size_t implementationClassCount = 0;
    bool genericSemantic = false;
};

struct DirectUiOwnedProfile final {
    std::deque<std::wstring> strings;
    std::vector<DirectUiSlot> slots;
    std::vector<DirectUiWindowProfile::ImplementationClassContract> implementationClasses;
    DirectUiWindowProfile profile;
};

extern const DirectUiWindowProfile kDirectUiProfiles[];
extern const size_t kDirectUiProfileCount;

// Resolves the profile matching the current process image (canonical System32
// path, exact fixed file version, Microsoft signature). Returns nullptr when
// no profile applies; `error` is only set for a basename match that failed a
// stricter identity check.
const DirectUiWindowProfile* ResolveDirectUiWindowProfile(
    std::wstring& imagePath,
    std::wstring& error);
bool ResolveGenericDirectUiImage(std::wstring& imagePath, std::wstring& error);

struct DirectUiWindowEvidence final {
    HWND hwnd = nullptr;
    uint64_t generation = 0;
    RECT bounds{};
    uint64_t style = 0;
    uint64_t exStyle = 0;
    bool visible = false;
    bool enabled = false;
    int tabIndex = -1;
    std::wstring text;
    std::wstring note;
    int controlId = 0;
    uint32_t dialogCode = 0;
    bool checked = false;
    int minimum = 0;
    int maximum = 100;
    int position = 0;
    bool indeterminate = false;
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    std::wstring imageFormat;
    std::vector<uint8_t> imageData;
    // Typed state read by this backing control's own registered Win32 adapter.
    // Reusing the adapter registry is what lets the DirectUI lane inherit every
    // proven control contract instead of re-reading facets per application.
    bool hasDetail = false;
    ControlNode detail;
};

// Facets of `detail` a projected DirectUI composite may legitimately change
// while it stays the same control (selection, content, scroll state). Stable
// shape facets are compared separately and any drift rejects the surface.
bool SameDirectUiDetailShape(
    const DirectUiWindowEvidence& left,
    const DirectUiWindowEvidence& right) noexcept;
bool SameDirectUiDetailContent(
    const DirectUiWindowEvidence& left,
    const DirectUiWindowEvidence& right) noexcept;

struct DirectUiImplementationEvidence final {
    HWND hwnd = nullptr;
    uint64_t generation = 0;
    HWND parent = nullptr;
    std::wstring className;
    bool visible = false;
};

// Native A/B evidence captured on the source GUI thread. slotWindows is
// parallel to profile->slots and only filled for backing (non-virtual) slots.
struct DirectUiNativeEvidence final {
    DirectUiWindowEvidence root;
    DirectUiWindowEvidence directUi;
    std::vector<DirectUiWindowEvidence> slotWindows;
    std::vector<DirectUiImplementationEvidence> implementationWindows;
    std::vector<HWND> pageHosts;
    std::vector<HWND> pageStatics;
    HWND propertySheetPageHwnd = nullptr;
    uint32_t dpi = 0;
    uint64_t mutationEpoch = 0;
    HWND lastMutationHwnd = nullptr;
    uint32_t lastMutationMessage = 0;
    bool cloaked = false;
    HWND ownerHwnd = nullptr;
    RECT clientBounds{};
    POINT clientOriginScreen{};
    std::wstring title;
};

struct DirectUiBootstrapWindowEvidence final {
    DirectUiWindowEvidence window;
    HWND parent = nullptr;
    std::wstring className;
    bool controlSupported = false;
    ControlKind controlKind = ControlKind::StaticText;
    // A composite control's own implementation child (ComboBox edit/list,
    // ListView header, MonitorPalette entry). Detected on the source GUI thread
    // because the probe sends messages; the owner slot absorbs it.
    bool compositeImplementationChild = false;
    // Item count this control's own adapter reports (SIZE_MAX = not composite).
    size_t nativeItemCount = static_cast<size_t>(-1);
};

struct DirectUiBootstrapEvidence final {
    std::wstring rootClass;
    DirectUiNativeEvidence native;
    std::vector<DirectUiBootstrapWindowEvidence> descendants;
};

struct DirectUiSemanticEvidence final {
    std::wstring semanticKey;
    std::wstring name;
    std::wstring helpText;
    std::wstring accessKey;
    std::wstring className;
    std::wstring frameworkId;
    RECT bounds{};
    HWND backingHwnd = nullptr;
    int controlType = 0;
    bool focusable = false;
    bool enabled = false;
    bool actionable = false;
    bool offscreen = false;
    uint32_t patternMask = DirectUiPatternNone;
    int toggleState = -1;
    bool valueReadOnly = true;
    uint32_t capabilityMask = DirectUiCapabilityNone;
    std::vector<int> runtimeId;
};

struct DirectUiUiaEvidence final {
    HWND rootHwnd = nullptr;
    HWND directUiHwnd = nullptr;
    std::vector<DirectUiSemanticEvidence> semantics;
};

enum class DirectUiAdmissionResult {
    NotApplicable,
    Rejected,
    Admitted,
};

struct DirectUiActionBinding final {
    HWND hwnd = nullptr;
    uint64_t generation = 0;
    size_t slotIndex = 0;
    // Projected kind of the bound slot. In-place routes dispatch through the
    // backing control's own registered adapter, which needs the kind to pick the
    // right notification path.
    ControlKind kind = ControlKind::StaticText;
    DirectUiAction action = DirectUiAction::None;
    DirectUiAction secondaryAction = DirectUiAction::None;
    bool cancel = false;
    int propertySheetButton = -1;
};

// Resolves the requested protocol action string against a binding's declared
// routes. Returns DirectUiAction::None when the binding does not offer it, which
// is what refuses an action the projected slot never advertised.
DirectUiAction DirectUiActionForRequest(
    const DirectUiActionBinding& binding,
    std::wstring_view action) noexcept;

// True for the in-place routes that mutate a native-backed control through its
// own registered adapter and accept only that control's delta on recapture.
bool IsDirectUiInPlaceAction(DirectUiAction action) noexcept;

bool MatchDirectUiMutationBracket(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& before,
    const DirectUiNativeEvidence& after,
    std::wstring& error,
    bool requireStableEpoch = true) noexcept;
// Accepts only the acting slot's own mutable delta after an in-place route, so a
// projected page can publish the result of a mutation without ever accepting an
// unverified page change alongside it.
bool MatchDirectUiInPlaceMutation(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& before,
    const DirectUiNativeEvidence& after,
    size_t slotIndex,
    std::wstring& error) noexcept;
bool MatchDirectUiMoveTransition(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& before,
    const DirectUiNativeEvidence& after,
    std::wstring& error) noexcept;
bool MatchDirectUiRefreshTransition(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& before,
    const DirectUiNativeEvidence& after,
    std::wstring& error) noexcept;
bool DirectUiCaptureFailureIsTopologyChange(std::wstring_view error) noexcept;
bool MatchDirectUiEvidence(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& native,
    const DirectUiUiaEvidence& uia,
    std::wstring& error) noexcept;
uint64_t DirectUiSemanticNodeId(
    std::wstring_view adapterId,
    std::wstring_view semanticKey) noexcept;
bool RefreshDirectUiSnapshotFromNative(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& native,
    WindowSnapshot& snapshot,
    std::unordered_map<uint64_t, DirectUiActionBinding>& bindings,
    std::wstring& error) noexcept;
bool NormalizeDirectUiEvidenceText(
    std::wstring_view input,
    std::wstring& output) noexcept;
bool IsMicrosoftWindowsSignerName(std::wstring_view name) noexcept;
// True when a handoff-declared route is a page navigation: the application replaces
// the page inside the same top-level window, so the surface keeps its projection and
// admits the next page in place. Cancel ends the window instead, so it is excluded
// and keeps the terminal handoff route.
bool DirectUiNavigationMayStayProjected(
    const DirectUiActionBinding& binding) noexcept;
bool DirectUiHandoffMayPost(
    bool pageRevalidated,
    bool bindingGenerationMatches,
    bool proxyIsolated,
    bool nativeVisible,
    bool nativeUncloaked) noexcept;
bool RevalidateAndPostDirectUiPropertySheetAction(
    SourceThreadAgent& agent,
    const DirectUiWindowProfile& profile,
    const DirectUiActionBinding& binding,
    DWORD timeoutMs,
    HANDLE cancelEvent,
    std::wstring& error) noexcept;

DirectUiAdmissionResult InspectDirectUiSurface(
    SourceThreadAgent& agent,
    const DirectUiWindowProfile& profile,
    DWORD timeoutMs,
    HANDLE cancelEvent,
    std::wstring& diagnostic,
    WindowSnapshot* snapshot = nullptr,
    std::unordered_map<uint64_t, DirectUiActionBinding>* bindings = nullptr,
    DirectUiNativeEvidence* nativeEvidence = nullptr) noexcept;

DirectUiAdmissionResult InspectGenericDirectUiSurface(
    SourceThreadAgent& agent,
    DWORD timeoutMs,
    HANDLE cancelEvent,
    std::wstring& diagnostic,
    std::shared_ptr<DirectUiOwnedProfile>* ownedProfile = nullptr,
    WindowSnapshot* snapshot = nullptr,
    std::unordered_map<uint64_t, DirectUiActionBinding>* bindings = nullptr,
    DirectUiNativeEvidence* nativeEvidence = nullptr) noexcept;

// Reads one backing HWND's common facets plus its registered Win32 adapter's
// typed state. Runs only on that HWND's GUI thread because adapters interrogate
// their control with messages. Shared by generated-profile capture and by the
// in-place node action route so both observe the same canonical shape.
bool CaptureDirectUiSlotNode(
    HWND window,
    ControlKind kind,
    ControlNode& node,
    std::wstring& error) noexcept;

// Runs only on the source HWND's GUI thread.
bool CaptureDirectUiNativeEvidenceOnSourceThread(
    SourceThreadAgent& agent,
    const DirectUiWindowProfile& profile,
    DirectUiNativeEvidence& evidence,
    std::wstring& error) noexcept;
bool CaptureDirectUiBootstrapEvidenceOnSourceThread(
    SourceThreadAgent& agent,
    DirectUiBootstrapEvidence& evidence,
    std::wstring& error) noexcept;

} // namespace FluentShell::Bridge::Translation
