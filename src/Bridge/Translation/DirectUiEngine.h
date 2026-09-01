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
    ToggleCheck,   // walk the native checkbox state machine in place
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
    // Action routing.
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
};

// Native A/B evidence captured on the source GUI thread. slotWindows is
// parallel to profile->slots and only filled for backing (non-virtual) slots.
struct DirectUiNativeEvidence final {
    DirectUiWindowEvidence root;
    DirectUiWindowEvidence directUi;
    std::vector<DirectUiWindowEvidence> slotWindows;
    std::vector<HWND> pageHosts;
    std::vector<HWND> pageStatics;
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
};

struct DirectUiBootstrapEvidence final {
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
    DirectUiAction action = DirectUiAction::None;
    bool cancel = false;
};

bool MatchDirectUiMutationBracket(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& before,
    const DirectUiNativeEvidence& after,
    std::wstring& error,
    bool requireStableEpoch = true) noexcept;
bool MatchDirectUiMoveTransition(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& before,
    const DirectUiNativeEvidence& after,
    std::wstring& error) noexcept;
bool MatchDirectUiEvidence(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& native,
    const DirectUiUiaEvidence& uia,
    std::wstring& error) noexcept;
uint64_t DirectUiSemanticNodeId(
    std::wstring_view adapterId,
    std::wstring_view semanticKey) noexcept;
bool NormalizeDirectUiEvidenceText(
    std::wstring_view input,
    std::wstring& output) noexcept;
bool IsMicrosoftWindowsSignerName(std::wstring_view name) noexcept;
bool DirectUiHandoffMayPost(
    bool pageRevalidated,
    bool bindingGenerationMatches,
    bool proxyIsolated,
    bool nativeVisible,
    bool nativeUncloaked) noexcept;

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
