#include "../../src/Bridge/Ipc/FrameCodec.h"
#include "../../src/Bridge/Translation/WindowSnapshot.h"
#include "../../src/Bridge/Translation/ControlAdapters.h"
#include "../../src/Bridge/Translation/WindowCapture.h"
#include "../../src/Bridge/Translation/SourceThreadAgent.h"
#include "../../src/Bridge/Translation/UiAutomationGeometry.h"
#include "../../src/Bridge/Translation/DirectUiEngine.h"
#include "../../src/Bridge/Translation/DialogSnapshots.h"
#include "../../src/Bridge/Translation/AccessibleIsland.h"

#include <winrt/base.h>
#include <objbase.h>
#include <commctrl.h>
#include <prsht.h>
#include <UIAutomation.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using namespace FluentShell::Bridge;

int g_failures = 0;
int g_itemChanging = 0;
int g_itemChanged = 0;
bool g_vetoItemChange = false;
int g_tabChanging = 0;
int g_tabChanged = 0;
bool g_vetoTabChange = false;
std::vector<UINT> g_tabNotifications;
std::vector<WPARAM> g_scrollNotifications;
int g_labelEditsBegun = 0;
int g_labelEditsEnded = 0;
bool g_acceptLabelEdit = true;

LRESULT CALLBACK ListViewNotificationSubclass(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR) {
    if (message == WM_HSCROLL || message == WM_VSCROLL) {
        g_scrollNotifications.push_back(wParam);
    }
    if (message == WM_NOTIFY) {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header && (header->code == TVN_BEGINLABELEDITW ||
                       header->code == LVN_BEGINLABELEDITW)) {
            ++g_labelEditsBegun;
            return FALSE;
        }
        if (header && (header->code == TVN_ENDLABELEDITW ||
                       header->code == LVN_ENDLABELEDITW)) {
            ++g_labelEditsEnded;
            return g_acceptLabelEdit ? TRUE : FALSE;
        }
        if (header && header->code == LVN_ITEMCHANGING) {
            ++g_itemChanging;
            if (g_vetoItemChange) return TRUE;
        } else if (header && header->code == LVN_ITEMCHANGED) {
            ++g_itemChanged;
        } else if (header && header->code == TCN_SELCHANGING) {
            ++g_tabChanging;
            g_tabNotifications.push_back(TCN_SELCHANGING);
            if (g_vetoTabChange) return TRUE;
        } else if (header && header->code == TCN_SELCHANGE) {
            ++g_tabChanged;
            g_tabNotifications.push_back(TCN_SELCHANGE);
        }
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(
        window, ListViewNotificationSubclass, subclassId);
    return result;
}

void Check(bool condition, const char* message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string ReadFixture(const wchar_t* name) {
    const auto path = std::filesystem::current_path() / L"tests" / L"ProtocolFixtures" / name;
    std::ifstream stream(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

std::string ReplaceOnce(std::string value, std::string_view from, std::string_view to) {
    const auto position = value.find(from);
    if (position != std::string::npos) value.replace(position, from.size(), to);
    return value;
}

void TestVisibleUiaBoundsClipping() {
    Check(Translation::ScreenHitTestMatchesExposure(true, true),
        "an exposed proxy resolved by UIA was rejected");
    Check(!Translation::ScreenHitTestMatchesExposure(true, false),
        "an exposed proxy missed by UIA was accepted");
    Check(Translation::ScreenHitTestMatchesExposure(false, false),
        "a fully occluded proxy required an impossible desktop UIA hit");

    const POINT contentOrigin{ 185, 216 };
    const RECT rootViewport{ 185, 216, 517, 559 };
    const RECT canonicalClient{ 0, 0, 353, 353 };
    const RECT exactCanonicalViewport{ 185, 216, 538, 569 };
    Check(Translation::ContentViewportFitsCanonicalSize(
              exactCanonicalViewport, canonicalClient),
        "renderer viewport matching canonical client size was rejected");
    Check(Translation::ContentViewportFitsCanonicalSize(rootViewport, canonicalClient),
        "live host-clipped renderer viewport was rejected");
    const RECT hostClippedViewport{ 185, 216, 500, 540 };
    Check(Translation::ContentViewportFitsCanonicalSize(hostClippedViewport, canonicalClient),
        "host-clipped renderer viewport was rejected");
    const RECT oversizedViewport{ 185, 216, 557, 588 };
    Check(!Translation::ContentViewportFitsCanonicalSize(oversizedViewport, canonicalClient),
        "renderer viewport exceeding canonical size and rounding tolerance was accepted");

    const RECT unclippedNode{ 10, 12, 90, 36 };
    const RECT expectedUnclipped{ 195, 228, 275, 252 };
    RECT visible{};
    Check(Translation::ComputeVisibleUiaBounds(
              unclippedNode, contentOrigin, rootViewport, nullptr, visible) &&
          EqualRect(&visible, &expectedUnclipped),
        "unclipped UIA bounds lost exact physical-pixel geometry");

    const RECT negativeContainer{ -11, 0, 342, 353 };
    RECT containerVisible{};
    Check(Translation::ComputeVisibleUiaBounds(
              negativeContainer, contentOrigin, rootViewport, nullptr, containerVisible) &&
          EqualRect(&containerVisible, &rootViewport),
        "negative dialog container did not clip to the root client viewport");

    const RECT child{ -20, 10, 350, 360 };
    const RECT expectedChild{ 185, 226, 517, 559 };
    RECT childVisible{};
    Check(Translation::ComputeVisibleUiaBounds(
              child, contentOrigin, rootViewport, &containerVisible, childVisible) &&
          EqualRect(&childVisible, &expectedChild),
        "nested dialog descendant did not inherit root and parent clipping");

    const RECT nestedContainer{ 20, 20, 300, 300 };
    const RECT expectedNested{ 205, 236, 485, 516 };
    RECT nestedVisible{};
    Check(Translation::ComputeVisibleUiaBounds(
              nestedContainer, contentOrigin, rootViewport, &childVisible, nestedVisible) &&
          EqualRect(&nestedVisible, &expectedNested),
        "nested dialog container bounds did not retain ancestor clipping");

    const RECT nestedChild{ 0, 0, 400, 400 };
    RECT nestedChildVisible{};
    Check(Translation::ComputeVisibleUiaBounds(
              nestedChild, contentOrigin, rootViewport, &nestedVisible, nestedChildVisible) &&
          EqualRect(&nestedChildVisible, &expectedNested),
        "nested descendant did not clip to every structural ancestor");

    const RECT outsideContainer{ 400, 0, 450, 50 };
    Check(!Translation::ComputeVisibleUiaBounds(
              outsideContainer, contentOrigin, rootViewport, nullptr, visible),
        "fully out-of-client visible dialog container was accepted");
}

Translation::DirectUiWindowEvidence DirectUiWindow(
    uintptr_t hwnd,
    uint64_t generation,
    LONG top,
    DWORD style = WS_VISIBLE | WS_CHILD) {
    Translation::DirectUiWindowEvidence value;
    value.hwnd = reinterpret_cast<HWND>(hwnd);
    value.generation = generation;
    value.bounds = { 10, top, 310, top + 40 };
    value.style = style;
    value.visible = true;
    value.enabled = true;
    return value;
}

const Translation::DirectUiWindowProfile* TestProfileById(std::wstring_view adapterId) {
    for (size_t index = 0; index < Translation::kDirectUiProfileCount; ++index) {
        if (Translation::kDirectUiProfiles[index].adapterId == adapterId)
            return &Translation::kDirectUiProfiles[index];
    }
    return nullptr;
}

// Builds profile-shaped native evidence. All slots get backing windows for
// non-virtual slots; virtual slots keep null HWNDs.
Translation::DirectUiNativeEvidence DirectUiNativeFixture(
    const Translation::DirectUiWindowProfile& profile,
    std::vector<HWND>& backingHandles) {
    Translation::DirectUiNativeEvidence evidence;
    evidence.root = DirectUiWindow(0x100, 1, 0, WS_VISIBLE | WS_POPUP);
    evidence.directUi = DirectUiWindow(0x101, 2, 10);
    evidence.root.bounds = { 0, 0, 400, 400 };
    evidence.directUi.bounds = { 0, 0, 400, 400 };
    evidence.title = L"localized window title";
    evidence.dpi = 144;
    evidence.mutationEpoch = 42;
    evidence.slotWindows.assign(profile.slotCount, Translation::DirectUiWindowEvidence{});
    backingHandles.clear();
    HWND next = reinterpret_cast<HWND>(0x201);
    for (size_t index = 0; index < profile.slotCount; ++index) {
        const auto& slot = profile.slots[index];
        if (slot.virtualSource) continue;
        auto value = DirectUiWindow(reinterpret_cast<uintptr_t>(next), 3 + index, 100 + static_cast<LONG>(index * 50));
        value.text = L"localized action";
        evidence.slotWindows[index] = value;
        backingHandles.push_back(next);
        next = reinterpret_cast<HWND>(reinterpret_cast<uintptr_t>(next) + 0x10);
    }
    return evidence;
}

void TestDirectUiEvidenceContracts() {
    std::wstring normalized;
    Check(Translation::NormalizeDirectUiEvidenceText(L"Localized label", normalized) &&
          normalized == L"Localized label",
        "DirectUI evidence normalization changed localized text");
    const std::wstring embeddedNull{ L'a', L'\0', L'b' };
    Check(!Translation::NormalizeDirectUiEvidenceText(embeddedNull, normalized),
        "ambiguous embedded-NUL DirectUI evidence was accepted");

    // The mutation bracket is profile-independent: it compares root, anchor,
    // and every backing slot window between A and B.
    const auto* profile = TestProfileById(L"microsoft.mdsched.directui");
    Check(profile != nullptr, "MdSched profile is missing from the registry");
    if (!profile) return;
    std::vector<HWND> backing;
    auto before = DirectUiNativeFixture(*profile, backing);
    auto after = before;
    std::wstring error;
    Check(Translation::MatchDirectUiMutationBracket(*profile, before, after, error),
        "unchanged DirectUI A/U/B mutation bracket was rejected");
    after.mutationEpoch++;
    Check(!Translation::MatchDirectUiMutationBracket(*profile, before, after, error),
        "DirectUI mutation epoch race was accepted");
    Check(Translation::MatchDirectUiMutationBracket(*profile, before, after, error, false),
        "equal native evidence was rejected by the post-admission barrier");
    after = before;
    for (auto& slot : after.slotWindows) {
        if (slot.hwnd) { slot.generation++; break; }
    }
    Check(!Translation::MatchDirectUiMutationBracket(*profile, before, after, error),
        "recreated DirectUI backing Button was accepted");
    after = before;
    after.title = L"changed";
    Check(!Translation::MatchDirectUiMutationBracket(*profile, before, after, error),
        "DirectUI root title change inside the bracket was accepted");

    auto moved = before;
    constexpr LONG moveX = 137;
    constexpr LONG moveY = -42;
    OffsetRect(&moved.root.bounds, moveX, moveY);
    OffsetRect(&moved.directUi.bounds, moveX, moveY);
    moved.clientOriginScreen.x += moveX;
    moved.clientOriginScreen.y += moveY;
    for (size_t index = 0; index < profile->slotCount; ++index) {
        if (!profile->slots[index].virtualSource)
            OffsetRect(&moved.slotWindows[index].bounds, moveX, moveY);
    }
    ++moved.mutationEpoch;
    Check(Translation::MatchDirectUiMoveTransition(*profile, before, moved, error),
        "pure DirectUI whole-window translation was rejected");
    auto resized = moved;
    ++resized.root.bounds.right;
    Check(!Translation::MatchDirectUiMoveTransition(*profile, before, resized, error),
        "DirectUI move transition accepted a native resize");
    auto splitGeometry = moved;
    for (size_t index = 0; index < profile->slotCount; ++index) {
        if (!profile->slots[index].virtualSource) {
            --splitGeometry.slotWindows[index].bounds.left;
            break;
        }
    }
    Check(!Translation::MatchDirectUiMoveTransition(
              *profile, before, splitGeometry, error),
        "DirectUI move transition accepted a backing control that did not move atomically");

    // Semantic identity is adapter-scoped, so the same key under two profiles is
    // two different nodes and a RuntimeId never leaks into identity.
    const uint64_t first = Translation::DirectUiSemanticNodeId(
        profile->adapterId, L"MainInstruction");
    const uint64_t second = Translation::DirectUiSemanticNodeId(
        profile->adapterId, L"MainInstruction");
    Check(first == second && first != Translation::DirectUiSemanticNodeId(
        profile->adapterId, L"ContentText"),
        "DirectUI semantic identity is not stable and key-scoped");
    Check(first != Translation::DirectUiSemanticNodeId(
        L"microsoft.recoverydrive.directui", L"MainInstruction"),
        "DirectUI semantic identity is not adapter-scoped");

    Check(Translation::IsMicrosoftWindowsSignerName(L"Microsoft Windows") &&
          Translation::IsMicrosoftWindowsSignerName(L"microsoft corporation") &&
          !Translation::IsMicrosoftWindowsSignerName(L"Microsoft Windows Hardware Compatibility Publisher"),
        "DirectUI signer-name predicate accepted the wrong signer boundary");
    Check(Translation::DirectUiHandoffMayPost(true, true, true, true, true) &&
          !Translation::DirectUiHandoffMayPost(true, true, false, true, true) &&
          !Translation::DirectUiHandoffMayPost(true, false, true, true, true) &&
          !Translation::DirectUiHandoffMayPost(true, true, true, true, false),
        "DirectUI handoff ordering allowed a pre-restore or stale click");

    Translation::DirectUiSlot generatedSlots[2];
    generatedSlots[0].semanticKey = L"semantic.back";
    generatedSlots[0].kind = Translation::ControlKind::Button;
    generatedSlots[0].virtualSource = true;
    generatedSlots[0].uiaEnabled = true;
    generatedSlots[0].uiaFocusable = true;
    generatedSlots[0].tabIndex = 0;
    generatedSlots[0].action = Translation::DirectUiAction::HandoffPropertySheetButton;
    generatedSlots[0].propertySheetButton = PSBTN_BACK;
    generatedSlots[1].semanticKey = L"semantic.progress";
    generatedSlots[1].kind = Translation::ControlKind::ProgressBar;
    generatedSlots[1].virtualSource = false;
    generatedSlots[1].nativeClass = PROGRESS_CLASSW;
    Translation::DirectUiWindowProfile generatedProfile;
    generatedProfile.adapterId = L"microsoft.windows.directui.semantic.v1";
    generatedProfile.pageId = L"semantic-v1";
    generatedProfile.rootClass = L"NativeHWNDHost";
    generatedProfile.slots = generatedSlots;
    generatedProfile.slotCount = std::size(generatedSlots);
    generatedProfile.genericSemantic = true;
    Translation::DirectUiNativeEvidence generatedNative;
    generatedNative.root = DirectUiWindow(0x500, 50, 0, WS_VISIBLE | WS_POPUP);
    generatedNative.directUi = DirectUiWindow(0x501, 51, 0);
    generatedNative.root.bounds = { 10, 20, 410, 320 };
    generatedNative.directUi.bounds = { 20, 50, 400, 300 };
    generatedNative.title = L"semantic page";
    generatedNative.root.text = generatedNative.title;
    generatedNative.dpi = 120;
    generatedNative.clientBounds = { 0, 0, 400, 300 };
    generatedNative.clientOriginScreen = { 10, 20 };
    generatedNative.pageHosts = { reinterpret_cast<HWND>(0x503) };
    generatedNative.propertySheetPageHwnd = generatedNative.pageHosts[0];
    generatedNative.implementationWindows = {
        { generatedNative.directUi.hwnd, generatedNative.directUi.generation,
          generatedNative.root.hwnd, L"DirectUIHWND", true },
        { generatedNative.pageHosts[0], 53, generatedNative.directUi.hwnd,
          L"#32770", true },
        { reinterpret_cast<HWND>(0x502), 52, generatedNative.directUi.hwnd,
          PROGRESS_CLASSW, true },
    };
    generatedNative.slotWindows.resize(std::size(generatedSlots));
    generatedNative.slotWindows[1] = DirectUiWindow(0x502, 52, 100);
    generatedNative.slotWindows[1].bounds = { 30, 100, 200, 120 };
    generatedNative.slotWindows[1].minimum = 10;
    generatedNative.slotWindows[1].maximum = 90;
    generatedNative.slotWindows[1].position = 40;
    generatedNative.slotWindows[1].indeterminate = true;
    Translation::WindowSnapshot generatedSnapshot;
    generatedSnapshot.adapterId = generatedProfile.adapterId;
    generatedSnapshot.pageId = generatedProfile.pageId;
    for (const auto& slot : generatedSlots) {
        Translation::ControlNode node;
        node.nodeId = Translation::DirectUiSemanticNodeId(
            generatedProfile.adapterId, slot.semanticKey);
        node.kind = slot.kind;
        node.semanticKey = slot.semanticKey;
        generatedSnapshot.nodes.push_back(std::move(node));
    }
    std::unordered_map<uint64_t, Translation::DirectUiActionBinding> generatedBindings;
    auto refreshedNative = generatedNative;
    refreshedNative.cloaked = true;
    refreshedNative.slotWindows[1].position = 45;
    Check(Translation::MatchDirectUiRefreshTransition(
              generatedProfile, generatedNative, refreshedNative, error),
        "generated DirectUI refresh rejected a canonical progress-only delta");
    auto recreatedNative = refreshedNative;
    recreatedNative.slotWindows[1].generation++;
    recreatedNative.implementationWindows[2].generation++;
    Check(!Translation::MatchDirectUiRefreshTransition(
              generatedProfile, generatedNative, recreatedNative, error),
        "generated DirectUI refresh accepted a recreated backing HWND");
    auto reparentedNative = refreshedNative;
    reparentedNative.implementationWindows[1].parent = generatedNative.root.hwnd;
    Check(!Translation::MatchDirectUiRefreshTransition(
              generatedProfile, generatedNative, reparentedNative, error),
        "generated DirectUI refresh accepted changed implementation parenting");
    auto ownerChangedNative = refreshedNative;
    ownerChangedNative.ownerHwnd = reinterpret_cast<HWND>(0x504);
    Check(!Translation::MatchDirectUiRefreshTransition(
              generatedProfile, generatedNative, ownerChangedNative, error),
        "generated DirectUI refresh accepted a changed owner");
    auto uncloakedNative = refreshedNative;
    uncloakedNative.cloaked = false;
    Check(!Translation::MatchDirectUiRefreshTransition(
              generatedProfile, generatedNative, uncloakedNative, error),
        "generated DirectUI refresh accepted cloak loss");
    Check(Translation::DirectUiCaptureFailureIsTopologyChange(
              L"generic A/B: root class or descendant count changed") &&
          !Translation::DirectUiCaptureFailureIsTopologyChange(
              L"source UI thread did not acknowledge DirectUI native evidence capture"),
        "DirectUI topology-change classification is not fail-closed");
    Check(Translation::RefreshDirectUiSnapshotFromNative(generatedProfile,
              refreshedNative, generatedSnapshot, generatedBindings, error),
        "generated DirectUI snapshot did not refresh from canonical native evidence");
    const auto backBinding = generatedBindings.find(generatedSnapshot.nodes[0].nodeId);
    Check(backBinding != generatedBindings.end() &&
          backBinding->second.hwnd == generatedNative.root.hwnd &&
          backBinding->second.generation == generatedNative.root.generation &&
          backBinding->second.action ==
              Translation::DirectUiAction::HandoffPropertySheetButton &&
          backBinding->second.propertySheetButton == PSBTN_BACK,
        "virtual property-sheet action was not rebound to the canonical root");
    Check(generatedSnapshot.nodes[1].minimum == 10 &&
          generatedSnapshot.nodes[1].maximum == 90 &&
          generatedSnapshot.nodes[1].position == 45 &&
          generatedSnapshot.nodes[1].indeterminate,
        "native-backed DirectUI progress state was not refreshed");
    generatedSnapshot.nodes[0].nodeId++;
    Check(!Translation::RefreshDirectUiSnapshotFromNative(generatedProfile,
              generatedNative, generatedSnapshot, generatedBindings, error),
        "generated DirectUI refresh accepted a changed semantic identity");

    // The admitted profile set is exactly the two known application pages.
    Check(Translation::kDirectUiProfileCount == 2 &&
          TestProfileById(L"microsoft.mdsched.directui") != nullptr &&
          TestProfileById(L"microsoft.recoverydrive.directui") != nullptr,
        "DirectUI profile registry does not contain exactly the two admitted pages");
    const auto* recovery = TestProfileById(L"microsoft.recoverydrive.directui");
    Check(recovery && recovery->rootClass == L"NativeHWNDHost" &&
          recovery->fileVersion[3] == 33296,
        "RecoveryDrive profile identity is not the exact admitted contract");
    bool foundToggle = false;
    bool foundDisabledBack = false;
    bool foundPageText = false;
    for (size_t index = 0; recovery && index < recovery->slotCount; ++index) {
        const auto& slot = recovery->slots[index];
        if (slot.action == Translation::DirectUiAction::ToggleCheck) foundToggle = true;
        if (slot.semanticKey == L"backbutton" && !slot.project) {}
        if (slot.semanticKey == L"backbutton" && slot.action == Translation::DirectUiAction::None &&
            !slot.uiaEnabled)
            foundDisabledBack = true;
        if (slot.semanticKey == L"pageText" && slot.project && !slot.virtualSource &&
            slot.nativeClass == L"Static")
            foundPageText = true;
    }
    Check(foundToggle, "RecoveryDrive profile does not route the checkbox through native toggle");
    Check(foundDisabledBack, "RecoveryDrive profile does not declare the back button inert");
    Check(foundPageText && recovery->directUiOwnsTabOrder &&
          recovery->census.visibleNotifyWrappers == 3 &&
          recovery->census.hiddenNotifyWrappers == 2,
        "RecoveryDrive profile does not pin its observed page text, tab owner, and census");
}

// The in-place routes are the ones that keep a capability-derived page
// projected: a request only resolves to a route the binding actually
// advertised, only the in-place routes are classified as in place, and the
// recapture accepts nothing but the acting control's own mutable delta.
void TestDirectUiInPlaceRoutes() {
    Translation::DirectUiActionBinding combo;
    combo.kind = Translation::ControlKind::ComboBox;
    combo.action = Translation::DirectUiAction::SelectListItem;
    combo.secondaryAction = Translation::DirectUiAction::SetEditText;
    Check(Translation::DirectUiActionForRequest(combo, L"select") ==
              Translation::DirectUiAction::SelectListItem &&
          Translation::DirectUiActionForRequest(combo, L"setText") ==
              Translation::DirectUiAction::SetEditText,
        "editable combo binding did not resolve both advertised routes");
    Check(Translation::DirectUiActionForRequest(combo, L"setSelection") ==
              Translation::DirectUiAction::None &&
          Translation::DirectUiActionForRequest(combo, L"invoke") ==
              Translation::DirectUiAction::None &&
          Translation::DirectUiActionForRequest(combo, L"") ==
              Translation::DirectUiAction::None,
        "combo binding accepted a route it never advertised");

    Translation::DirectUiActionBinding list;
    list.kind = Translation::ControlKind::ListView;
    list.action = Translation::DirectUiAction::SelectListItem;
    list.secondaryAction = Translation::DirectUiAction::SetItemCheck;
    Check(Translation::DirectUiActionForRequest(list, L"setSelection") ==
              Translation::DirectUiAction::SelectListItem &&
          Translation::DirectUiActionForRequest(list, L"setItemCheck") ==
              Translation::DirectUiAction::SetItemCheck &&
          Translation::DirectUiActionForRequest(list, L"select") ==
              Translation::DirectUiAction::None,
        "ListView binding did not name its own selection route");

    Translation::DirectUiActionBinding link;
    link.kind = Translation::ControlKind::SysLink;
    link.action = Translation::DirectUiAction::HandoffLinkClick;
    Check(Translation::DirectUiActionForRequest(link, L"invoke") ==
              Translation::DirectUiAction::HandoffLinkClick &&
          !Translation::IsDirectUiInPlaceAction(link.action),
        "a SysLink handoff was classified as an in-place mutation");
    Check(Translation::IsDirectUiInPlaceAction(Translation::DirectUiAction::ToggleCheck) &&
          Translation::IsDirectUiInPlaceAction(Translation::DirectUiAction::SelectRadio) &&
          Translation::IsDirectUiInPlaceAction(Translation::DirectUiAction::SetEditText) &&
          Translation::IsDirectUiInPlaceAction(Translation::DirectUiAction::SelectListItem) &&
          Translation::IsDirectUiInPlaceAction(Translation::DirectUiAction::SetItemCheck) &&
          Translation::IsDirectUiInPlaceAction(Translation::DirectUiAction::ToolbarCommand),
        "an in-place DirectUI route was classified as a handoff");
    Check(!Translation::IsDirectUiInPlaceAction(Translation::DirectUiAction::None) &&
          !Translation::IsDirectUiInPlaceAction(Translation::DirectUiAction::HandoffClick) &&
          !Translation::IsDirectUiInPlaceAction(
              Translation::DirectUiAction::HandoffPropertySheetButton),
        "a handoff DirectUI route was classified in place");

    // Only a handoff that replaces the page inside the same top-level window may
    // keep its projection. A route that ends the window must stay terminal.
    Translation::DirectUiActionBinding sheet;
    sheet.action = Translation::DirectUiAction::HandoffPropertySheetButton;
    for (const int button : {PSBTN_BACK, PSBTN_NEXT, PSBTN_FINISH}) {
        sheet.propertySheetButton = button;
        Check(Translation::DirectUiNavigationMayStayProjected(sheet),
            "a property-sheet page navigation was refused the in-place route");
    }
    sheet.propertySheetButton = PSBTN_CANCEL;
    Check(!Translation::DirectUiNavigationMayStayProjected(sheet),
        "Cancel was allowed to keep a projection it ends");
    sheet.propertySheetButton = PSBTN_NEXT;
    sheet.cancel = true;
    Check(!Translation::DirectUiNavigationMayStayProjected(sheet),
        "a cancel-declared property-sheet slot kept its projection");

    Translation::DirectUiActionBinding click;
    click.action = Translation::DirectUiAction::HandoffClick;
    Check(Translation::DirectUiNavigationMayStayProjected(click),
        "a plain handoff click was refused the in-place navigation route");
    click.action = Translation::DirectUiAction::HandoffLinkClick;
    Check(Translation::DirectUiNavigationMayStayProjected(click),
        "a SysLink handoff was refused the in-place navigation route");
    click.cancel = true;
    Check(!Translation::DirectUiNavigationMayStayProjected(click),
        "a cancel-declared link kept its projection");
    click.cancel = false;
    click.propertySheetButton = PSBTN_CANCEL;
    Check(!Translation::DirectUiNavigationMayStayProjected(click),
        "a click carrying a property-sheet button took the wrong navigation route");

    Translation::DirectUiActionBinding inPlace;
    inPlace.action = Translation::DirectUiAction::ToggleCheck;
    Check(!Translation::DirectUiNavigationMayStayProjected(inPlace) &&
          !Translation::DirectUiNavigationMayStayProjected(
              Translation::DirectUiActionBinding{}),
        "a non-handoff route claimed the in-place page navigation lane");

    // Two native-backed slots on a capability-derived page: the acting edit box
    // and a bystander ListView whose own state may not move alongside it.
    constexpr uint32_t childStyle = static_cast<uint32_t>(WS_VISIBLE | WS_CHILD);
    Translation::DirectUiSlot slots[2];
    slots[0].semanticKey = L"summary";
    slots[0].kind = Translation::ControlKind::Edit;
    slots[0].virtualSource = false;
    slots[0].nativeClass = L"Edit";
    slots[0].action = Translation::DirectUiAction::SetEditText;
    slots[0].nativeStyleValue = childStyle;
    slots[0].nativeStyleAlt = childStyle;
    slots[0].nativeStyleMask = UINT64_MAX;
    slots[0].captureDetail = true;
    slots[1].semanticKey = L"volumes";
    slots[1].kind = Translation::ControlKind::ListView;
    slots[1].virtualSource = false;
    slots[1].nativeClass = WC_LISTVIEWW;
    slots[1].action = Translation::DirectUiAction::SelectListItem;
    slots[1].secondaryAction = Translation::DirectUiAction::SetItemCheck;
    slots[1].nativeStyleValue = childStyle;
    slots[1].nativeStyleAlt = childStyle;
    slots[1].nativeStyleMask = UINT64_MAX;
    slots[1].captureDetail = true;
    Translation::DirectUiWindowProfile profile;
    profile.adapterId = L"microsoft.windows.directui.semantic.v1";
    profile.pageId = L"semantic-v1";
    profile.rootClass = L"NativeHWNDHost";
    profile.slots = slots;
    profile.slotCount = std::size(slots);
    profile.genericSemantic = true;

    Translation::DirectUiNativeEvidence before;
    before.root = DirectUiWindow(0x600, 60, 0, WS_VISIBLE | WS_POPUP);
    before.directUi = DirectUiWindow(0x601, 61, 0);
    before.root.bounds = { 0, 0, 500, 400 };
    before.directUi.bounds = { 0, 0, 500, 400 };
    before.title = L"in-place page";
    before.root.text = before.title;
    before.dpi = 96;
    before.mutationEpoch = 7;
    before.clientBounds = { 0, 0, 500, 400 };
    before.slotWindows.resize(std::size(slots));
    before.slotWindows[0] = DirectUiWindow(0x602, 62, 40);
    before.slotWindows[0].text = L"before text";
    before.slotWindows[0].hasDetail = true;
    before.slotWindows[0].detail.kind = Translation::ControlKind::Edit;
    before.slotWindows[0].detail.text = L"before text";
    before.slotWindows[1] = DirectUiWindow(0x603, 63, 120);
    before.slotWindows[1].hasDetail = true;
    before.slotWindows[1].detail.kind = Translation::ControlKind::ListView;
    before.slotWindows[1].detail.checkBoxes = true;
    before.slotWindows[1].detail.items = { L"Volume 1", L"Volume 2" };
    before.slotWindows[1].detail.selectedIndices = { 0 };
    before.slotWindows[1].detail.checkedIndices = { 0 };

    std::wstring error;
    auto accepted = before;
    accepted.mutationEpoch = before.mutationEpoch + 1;
    accepted.slotWindows[0].text = L"after text";
    accepted.slotWindows[0].detail.text = L"after text";
    accepted.slotWindows[0].detail.selectionStart = 10;
    Check(Translation::MatchDirectUiInPlaceMutation(profile, before, accepted, 0, error),
        "in-place mutation rejected the acting edit slot's own text delta");
    auto bystander = accepted;
    bystander.slotWindows[1].detail.selectedIndices = { 1 };
    Check(!Translation::MatchDirectUiInPlaceMutation(profile, before, bystander, 0, error),
        "in-place mutation accepted an unrelated slot's selection change");
    auto reshaped = accepted;
    reshaped.slotWindows[0].detail.readOnly = true;
    Check(!Translation::MatchDirectUiInPlaceMutation(profile, before, reshaped, 0, error),
        "in-place mutation accepted a shape change on the acting slot");
    // The pinned style mask is what keeps ES_READONLY and CBS_DROPDOWN from
    // flipping under a projected page, so the derived route set stays true.
    auto restyled = accepted;
    restyled.slotWindows[0].style |= ES_READONLY;
    Check(!Translation::MatchDirectUiInPlaceMutation(profile, before, restyled, 0, error),
        "in-place mutation accepted a pinned style flip on the acting slot");
    auto disabled = accepted;
    disabled.slotWindows[0].enabled = false;
    Check(!Translation::MatchDirectUiInPlaceMutation(profile, before, disabled, 0, error),
        "in-place mutation accepted the acting slot disabling itself");
    auto retitled = accepted;
    retitled.title = L"another page";
    Check(!Translation::MatchDirectUiInPlaceMutation(profile, before, retitled, 0, error),
        "in-place mutation accepted a page-level change alongside the delta");
    Check(!Translation::MatchDirectUiInPlaceMutation(profile, before, accepted, 2, error),
        "in-place mutation accepted an acting slot outside the profile");

    // The same delta is canonical when it is credited to the slot that produced
    // it, and only to that slot.
    auto listDelta = before;
    listDelta.slotWindows[1].detail.selectedIndices = { 1 };
    listDelta.slotWindows[1].detail.checkedIndices = { 0, 1 };
    Check(Translation::MatchDirectUiInPlaceMutation(profile, before, listDelta, 1, error),
        "in-place mutation rejected the ListView's own selection and check delta");
    Check(!Translation::MatchDirectUiInPlaceMutation(profile, before, listDelta, 0, error),
        "in-place mutation credited the ListView delta to the edit slot");
}

HICON CreateColorIcon(
    int width,
    int height,
    std::array<uint8_t, 4> bgra,
    bool transparentMask = false) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* rawPixels = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &rawPixels, nullptr, 0);
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (color && rawPixels) {
        auto* pixels = static_cast<uint8_t*>(rawPixels);
        for (size_t index = 0; index < pixelCount; ++index) {
            std::copy(bgra.begin(), bgra.end(), pixels + index * 4);
        }
    }
    const size_t maskStride = (static_cast<size_t>(width) + 15u) / 16u * 2u;
    std::vector<uint8_t> maskBits(
        maskStride * static_cast<size_t>(height), transparentMask ? 0xff : 0);
    HBITMAP mask = CreateBitmap(width, height, 1, 1, maskBits.data());
    ICONINFO iconInfo{};
    iconInfo.fIcon = TRUE;
    iconInfo.hbmColor = color;
    iconInfo.hbmMask = mask;
    HICON icon = color && mask ? CreateIconIndirect(&iconInfo) : nullptr;
    if (color) DeleteObject(color);
    if (mask) DeleteObject(mask);
    return icon;
}

void TestHeaderValidation() {
    Check(sizeof(Ipc::FrameHeader) == 32, "FLSH header must remain 32 bytes");
    Ipc::FrameHeader header{};
    header.type = static_cast<uint16_t>(Ipc::MessageType::Heartbeat);
    header.sequence = 1;
    std::wstring error;
    Check(Ipc::ValidateHeader(header, 0, error), "valid header was rejected");

    auto invalid = header;
    invalid.major = 2;
    Check(!Ipc::ValidateHeader(invalid, 0, error), "major mismatch was accepted");
    invalid = header;
    invalid.flags = 1;
    Check(!Ipc::ValidateHeader(invalid, 0, error), "unknown flags were accepted");
    invalid = header;
    invalid.type = 99;
    Check(!Ipc::ValidateHeader(invalid, 0, error), "unknown message type was accepted");
    invalid = header;
    invalid.payloadLength = Ipc::kMaxPayloadBytes + 1;
    Check(!Ipc::ValidateHeader(invalid, 0, error), "oversized payload was accepted");
    invalid = header;
    invalid.minor = static_cast<uint16_t>(Ipc::kProtocolMinor + 1);
    Check(Ipc::ValidateHeader(invalid, 0, error),
        "a newer same-major protocol minor was rejected");
    Check(!Ipc::ValidateHeader(header, 1, error), "duplicate sequence was accepted");
}

void TestPipeRoundTrip() {
    const std::wstring name = L"\\\\.\\pipe\\FluentShell.NativeTests." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    HANDLE server = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);
    Check(server != INVALID_HANDLE_VALUE, "test named pipe server creation failed");
    if (server == INVALID_HANDLE_VALUE) return;
    HANDLE client = CreateFileW(
        name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(client != INVALID_HANDLE_VALUE, "test named pipe client creation failed");
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server);
        return;
    }
    const BOOL connected = ConnectNamedPipe(server, nullptr);
    Check(connected || GetLastError() == ERROR_PIPE_CONNECTED, "test pipe did not connect");

    std::wstring error;
    const std::string payload = "{\"ok\":true}";
    Check(Ipc::WriteFrame(server, Ipc::MessageType::Heartbeat, 1, 7, payload, error),
        "native WriteFrame failed");
    std::vector<unsigned char> wire(sizeof(Ipc::FrameHeader) + payload.size());
    DWORD read = 0;
    Check(ReadFile(client, wire.data(), static_cast<DWORD>(wire.size()), &read, nullptr) &&
        read == wire.size(), "could not read encoded frame bytes");
    Check(wire[0] == 'F' && wire[1] == 'L' && wire[2] == 'S' && wire[3] == 'H',
        "frame magic is not little-endian FLSH");

    auto* sequence = reinterpret_cast<uint64_t*>(wire.data() + 16);
    *sequence = 2;
    DWORD written = 0;
    Check(WriteFile(client, wire.data(), static_cast<DWORD>(wire.size()), &written, nullptr) &&
        written == wire.size(), "could not write frame bytes back to server");
    Ipc::Frame decoded;
    Check(Ipc::ReadFrame(server, 1, decoded, error, 1000), "native ReadFrame failed");
    Check(decoded.header.sequence == 2 && decoded.header.revision == 7 && decoded.payload == payload,
        "native frame round-trip changed data");
    CloseHandle(client);
    DisconnectNamedPipe(server);
    CloseHandle(server);
}

void TestReadFrameUsesOneDeadline() {
    const std::wstring name = L"\\\\.\\pipe\\FluentShell.NativeDeadlineTests." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    HANDLE server = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);
    Check(server != INVALID_HANDLE_VALUE, "deadline test pipe server creation failed");
    if (server == INVALID_HANDLE_VALUE) return;
    HANDLE client = CreateFileW(
        name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(client != INVALID_HANDLE_VALUE, "deadline test pipe client creation failed");
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server);
        return;
    }
    const BOOL connected = ConnectNamedPipe(server, nullptr);
    Check(connected || GetLastError() == ERROR_PIPE_CONNECTED,
        "deadline test pipe did not connect");

    Ipc::FrameHeader header{};
    header.type = static_cast<uint16_t>(Ipc::MessageType::Heartbeat);
    header.payloadLength = 2;
    header.sequence = 1;
    const std::string payload = "{}";
    std::thread writer([client, header, payload] {
        Sleep(80);
        DWORD written = 0;
        WriteFile(client, &header, sizeof(header), &written, nullptr);
        Sleep(80);
        WriteFile(client, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    });

    const ULONGLONG started = GetTickCount64();
    Ipc::Frame decoded;
    std::wstring error;
    const bool read = Ipc::ReadFrame(server, 0, decoded, error, 120);
    const ULONGLONG elapsed = GetTickCount64() - started;
    Check(!read, "frame header and payload each received a fresh timeout");
    Check(elapsed < 220, "frame read exceeded its total deadline by too much");
    writer.join();
    CloseHandle(client);
    DisconnectNamedPipe(server);
    CloseHandle(server);
}

void TestWriteFrameUsesOneDeadline() {
    const std::wstring name = L"\\\\.\\pipe\\FluentShell.NativeWriteDeadlineTests." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    HANDLE server = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 1024, 1024, 0, nullptr);
    Check(server != INVALID_HANDLE_VALUE, "write deadline test pipe server creation failed");
    if (server == INVALID_HANDLE_VALUE) return;
    HANDLE client = CreateFileW(
        name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(client != INVALID_HANDLE_VALUE, "write deadline test pipe client creation failed");
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server);
        return;
    }
    const BOOL connected = ConnectNamedPipe(server, nullptr);
    Check(connected || GetLastError() == ERROR_PIPE_CONNECTED,
        "write deadline test pipe did not connect");

    // Keep the client from draining the server's output so WriteFile enters
    // overlapped pending state. The payload remains within the protocol cap.
    const std::string payload(1024 * 1024, 'x');
    std::wstring error;
    const ULONGLONG started = GetTickCount64();
    const bool written = Ipc::WriteFrame(
        server, Ipc::MessageType::Heartbeat, 1, 0, payload, error, 120);
    const ULONGLONG elapsed = GetTickCount64() - started;
    Check(!written, "backpressured frame write unexpectedly completed");
    Check(elapsed < 500, "frame write exceeded its total deadline by too much");
    CloseHandle(client);
    DisconnectNamedPipe(server);
    CloseHandle(server);
}

void TestScalarContracts() {
    uint64_t value = 0;
    Check(Ipc::TryParseUInt64(L"0", value) && value == 0, "canonical zero u64 rejected");
    Check(Ipc::TryParseUInt64(L"18446744073709551615", value), "maximum u64 rejected");
    Check(!Ipc::TryParseUInt64(L"01", value), "non-canonical leading-zero u64 accepted");
    Check(!Ipc::TryParseUInt64(L"18446744073709551616", value), "overflow u64 accepted");
    const auto nonce = Ipc::NewNonceHex();
    Check(nonce.size() == 32 && nonce.find_first_not_of(L"0123456789abcdef") == std::wstring::npos,
        "nonce is not 128-bit hexadecimal");
    const auto guid = Ipc::NewGuidString();
    GUID parsed{};
    const auto bracedGuid = L"{" + guid + L"}";
    Check(guid.size() == 36 && guid.front() != L'{' && guid.back() != L'}' &&
        guid.find_first_of(L"ABCDEF") == std::wstring::npos &&
        SUCCEEDED(CLSIDFromString(bracedGuid.c_str(), &parsed)),
        "GUID is not canonical lowercase D format");
    Check(Ipc::ProcessCreationTime(GetCurrentProcess()) != 0, "process creation identity is zero");

    const auto gatedCommit = Translation::SerializeSurfaceCommit(
        L"00112233445566778899aabbccddeeff",
        L"11111111-2222-3333-4444-555555555555", 7, true, false);
    Check(gatedCommit.find("\"show\":true") != std::string::npos &&
          gatedCommit.find("\"interactive\":false") != std::string::npos,
        "provisional surface commit did not serialize its interaction gate");
}

void TestSharedFixtures() {
    std::wstring error;
    Translation::HelloMessage hello;
    const auto helloPayload = ReadFixture(L"hello.renderer.json");
    Check(!helloPayload.empty() && Translation::ParseHello(helloPayload, hello, error),
        "renderer hello fixture did not parse");
    Check(hello.role == L"renderer" && hello.processId == 4242 &&
        hello.protocolMajor == Ipc::kProtocolMajor &&
        hello.protocolMinor == Ipc::kProtocolMinor,
        "renderer hello fixture changed meaning");

    Translation::HelloMessage futureHello;
    const auto futureHelloPayload = ReadFixture(L"hello.renderer.future-minor.json");
    Check(!futureHelloPayload.empty() && Translation::ParseHello(
        futureHelloPayload, futureHello, error),
        "future-minor hello fixture did not parse");
    Check(futureHello.protocolMajor == Ipc::kProtocolMajor &&
        futureHello.protocolMinor == Ipc::kProtocolMinor + 1,
        "future-minor hello fixture changed meaning");

    Translation::ActionRequest action;
    const auto actionPayload = ReadFixture(L"action.invoke.json");
    Check(!actionPayload.empty() && Translation::ParseActionInvoke(
        actionPayload, L"00112233445566778899aabbccddeeff", action, error),
        "action.invoke fixture did not parse");
    Check(action.action == L"invoke" && action.nodeId == 1 &&
        action.eventId == 18 && action.expectedRevision == 7,
        "action.invoke fixture changed meaning");

    Translation::ActionRequest unicodeAction;
    const auto unicodePayload = ReadFixture(L"action.invoke.unicode.json");
    Check(!unicodePayload.empty() && Translation::ParseActionInvoke(
        unicodePayload, L"00112233445566778899aabbccddeeff", unicodeAction, error),
        "Unicode action fixture did not parse");
    Check(unicodeAction.action == L"setText" && unicodeAction.nodeId == 9 &&
        unicodeAction.eventId == 19 && unicodeAction.expectedRevision == 8 &&
        unicodeAction.text == L"\u7E41\u9AD4\u4E2D\u6587\u8207 English \u53EF\u4EE5\u5171\u5B58\u3002",
        "Unicode action fixture changed meaning");

    const std::string malformed =
        "{\"messageType\":\"action.invoke\",\"sessionNonce\":\"00112233445566778899aabbccddeeff\","
        "\"surfaceId\":\"4f17d4bb-b2bf-42b8-a334-2f9ad8d54d42\",\"eventId\":\"1\","
        "\"expectedRevision\":\"7\",\"action\":\"resize\",\"value\":{\"width\":10}}";
    Translation::ActionRequest rejected;
    Check(!Translation::ParseActionInvoke(
        malformed, L"00112233445566778899aabbccddeeff", rejected, error),
        "partial resize bounds were accepted");

    Translation::WindowSnapshot snapshot;
    snapshot.surfaceId = L"4f17d4bb-b2bf-42b8-a334-2f9ad8d54d42";
    snapshot.revision = 8;
    snapshot.dpi = 96;
    const auto patch = Translation::SerializeWindowPatch(
        L"00112233445566778899AABBCCDDEEFF", 7, snapshot, 18);
    Check(patch.find("\"operations\":[]") != std::string::npos,
        "full snapshot patch did not keep operations empty");
    Check(patch.find("\"property\":\"snapshot\"") == std::string::npos,
        "full snapshot patch still contains the rejected marker operation");
    Check(patch.find("\"eventId\":\"18\"") != std::string::npos,
        "full snapshot patch lost its canonical eventId");
}

void TestStrictMessageValidation() {
    constexpr std::wstring_view nonce = L"00112233445566778899aabbccddeeff";
    std::wstring error;
    Translation::HelloMessage hello;
    const auto validHello = ReadFixture(L"hello.renderer.json");

    Check(!Translation::ParseHello(
        ReplaceOnce(validHello, "\"processId\": 4242", "\"processId\": 1.5"),
        hello, error), "fractional hello processId was accepted");
    Check(!Translation::ParseHello(
        ReplaceOnce(validHello, "\"processId\": 4242", "\"processId\": 4294967296"),
        hello, error), "overflowing hello processId was accepted");
    Check(!Translation::ParseHello(
        ReplaceOnce(validHello, "\"protocolMajor\": 1", "\"protocolMajor\": 1.5"),
        hello, error), "fractional hello major was accepted");
    // Derived from the constant so a minor bump cannot quietly turn this into a
    // no-op: ReplaceOnce leaves the payload untouched when its needle is gone.
    const std::string currentMinor =
        "\"protocolMinor\": " + std::to_string(Ipc::kProtocolMinor);
    Check(validHello.find(currentMinor) != std::string::npos,
        "hello fixture no longer declares the current protocol minor");
    Check(Translation::ParseHello(
        ReplaceOnce(validHello, currentMinor,
            "\"protocolMinor\": " + std::to_string(Ipc::kProtocolMinor + 1)),
        hello, error), "future same-major hello minor was rejected");

    const std::string actionPrefix =
        "{\"messageType\":\"action.invoke\","
        "\"sessionNonce\":\"00112233445566778899aabbccddeeff\","
        "\"surfaceId\":\"4f17d4bb-b2bf-42b8-a334-2f9ad8d54d42\","
        "\"nodeId\":\"1\",\"eventId\":\"1\",\"expectedRevision\":\"7\",";
    Translation::ActionRequest action;
    Check(!Translation::ParseActionInvoke(
        actionPrefix + "\"action\":\"setCheck\",\"value\":1.5}", nonce, action, error),
        "fractional setCheck value was accepted");
    const auto itemCheckPrefix = actionPrefix + "\"action\":\"setItemCheck\",\"value\":";
    Check(Translation::ParseActionInvoke(
        itemCheckPrefix + "{\"index\":1,\"checked\":true}}", nonce, action, error) &&
        action.itemIndex == 1 && action.booleanValue,
        "canonical setItemCheck value was rejected");
    Check(!Translation::ParseActionInvoke(
        itemCheckPrefix + "{\"index\":1.5,\"checked\":true}}", nonce, action, error),
        "fractional setItemCheck index was accepted");
    Check(!Translation::ParseActionInvoke(
        itemCheckPrefix + "{\"index\":4096,\"checked\":true}}", nonce, action, error),
        "oversized setItemCheck index was accepted");
    Check(!Translation::ParseActionInvoke(
        itemCheckPrefix + "{\"index\":1,\"checked\":1}}", nonce, action, error),
        "non-boolean setItemCheck state was accepted");
    Check(!Translation::ParseActionInvoke(
        itemCheckPrefix + "{\"index\":1,\"checked\":true,\"extra\":0}}", nonce, action, error),
        "setItemCheck value with an extra field was accepted");
    Check(!Translation::ParseActionInvoke(
        ReplaceOnce(actionPrefix + "\"action\":\"invoke\",\"value\":null}",
            "\"eventId\":\"1\"", "\"eventId\":\"0\""),
        nonce, action, error), "zero action eventId was accepted");

    const std::string maximumText(Ipc::kMaxStringChars, 'a');
    const auto maximumAction = actionPrefix +
        "\"action\":\"setText\",\"value\":\"" + maximumText + "\"}";
    Check(Translation::ParseActionInvoke(maximumAction, nonce, action, error),
        "maximum-size action string was rejected");
    const auto oversizedAction = actionPrefix +
        "\"action\":\"setText\",\"value\":\"" + maximumText + "a\"}";
    Check(!Translation::ParseActionInvoke(oversizedAction, nonce, action, error),
        "oversized action string was accepted");

    const std::string validError =
        "{\"messageType\":\"error\","
        "\"sessionNonce\":\"00112233445566778899aabbccddeeff\","
        "\"surfaceId\":null,\"code\":\"protocol_fault\","
        "\"detail\":\"bad frame\",\"fatal\":true}";
    Check(Translation::ParseErrorMessage(validError, nonce, error),
        "valid authenticated error message was rejected");
    Check(!Translation::ParseErrorMessage(
        ReplaceOnce(validError, "00112233445566778899aabbccddeeff",
            "10112233445566778899aabbccddeeff"), nonce, error),
        "error message with wrong nonce was accepted");
    Check(!Translation::ParseErrorMessage(
        ReplaceOnce(validError, ",\"fatal\":true", ""), nonce, error),
        "error message without fatal was accepted");

    const std::string validShutdown =
        "{\"messageType\":\"shutdown\","
        "\"sessionNonce\":\"00112233445566778899aabbccddeeff\","
        "\"reason\":\"renderer shutdown\"}";
    Check(Translation::ParseShutdownMessage(validShutdown, nonce, error),
        "valid authenticated shutdown message was rejected");
    Check(!Translation::ParseShutdownMessage(
        ReplaceOnce(validShutdown, "\"reason\":\"renderer shutdown\"", "\"reason\":\"\""),
        nonce, error), "shutdown message with empty reason was accepted");
}

void TestActionSemanticValidation() {
    Translation::WindowSnapshot snapshot;
    Translation::ControlNode button;
    button.nodeId = 1;
    button.kind = Translation::ControlKind::Button;
    snapshot.nodes.push_back(button);
    Translation::ControlNode check;
    check.nodeId = 2;
    check.kind = Translation::ControlKind::CheckBox;
    snapshot.nodes.push_back(check);
    Translation::ControlNode threeState;
    threeState.nodeId = 3;
    threeState.kind = Translation::ControlKind::ThreeState;
    snapshot.nodes.push_back(threeState);
    Translation::ControlNode radio;
    radio.nodeId = 4;
    radio.kind = Translation::ControlKind::RadioButton;
    snapshot.nodes.push_back(radio);
    Translation::ControlNode edit;
    edit.nodeId = 5;
    edit.kind = Translation::ControlKind::Edit;
    edit.readOnly = true;
    snapshot.nodes.push_back(edit);
    Translation::ControlNode combo;
    combo.nodeId = 6;
    combo.kind = Translation::ControlKind::ComboBox;
    combo.items = { L"one", L"two" };
    snapshot.nodes.push_back(combo);
    Translation::ControlNode editableCombo = combo;
    editableCombo.nodeId = 7;
    editableCombo.editable = true;
    snapshot.nodes.push_back(editableCombo);
    Translation::ControlNode listView;
    listView.nodeId = 8;
    listView.kind = Translation::ControlKind::ListView;
    listView.rows = { { L"one" }, { L"two" } };
    listView.checkBoxes = true;
    snapshot.nodes.push_back(listView);
    Translation::ControlNode tabControl;
    tabControl.nodeId = 9;
    tabControl.kind = Translation::ControlKind::TabControl;
    tabControl.items = { L"one", L"two" };
    tabControl.selectedIndex = 0;
    snapshot.nodes.push_back(tabControl);
    Translation::ControlNode treeView;
    treeView.nodeId = 10;
    treeView.kind = Translation::ControlKind::TreeView;
    treeView.items = { L"root", L"child" };
    treeView.itemDepths = { 0, 1 };
    treeView.itemExpanded = { true, false };
    treeView.itemHasChildren = { true, false };
    treeView.selectedIndex = 0;
    snapshot.nodes.push_back(treeView);
    Translation::ControlNode slider;
    slider.nodeId = 11;
    slider.kind = Translation::ControlKind::Slider;
    slider.minimum = 0;
    slider.maximum = 20;
    slider.position = 7;
    snapshot.nodes.push_back(slider);

    std::wstring error;
    Translation::ActionRequest action;
    action.nodeId = 1;
    action.action = L"invoke";
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "button invoke semantic action was rejected");
    action.nodeId = 2;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "invoke on a checkbox was accepted");
    action.action = L"setCheck";
    action.integerValue = 2;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "two-state checkbox accepted indeterminate value");
    action.nodeId = 3;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "three-state checkbox rejected indeterminate value");
    action.nodeId = 4;
    action.integerValue = 0;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "radio button accepted semantic uncheck");
    action.nodeId = 5;
    action.action = L"setText";
    action.text = L"updated";
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "read-only edit accepted setText");
    action.nodeId = 6;
    action.action = L"select";
    action.integerValue = 2;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "out-of-range selection was accepted");
    action.integerValue = -1;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "selection clear was rejected");
    action.action = L"setText";
    action.text = L"typed";
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "non-editable ComboBox accepted setText");
    action.nodeId = 7;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "editable ComboBox rejected setText");
    action.nodeId = 8;
    action.action = L"setItemCheck";
    action.itemIndex = 1;
    action.booleanValue = true;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "bounded ListView item check was rejected");
    action.itemIndex = 2;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "out-of-range ListView item check was accepted");
    action.itemIndex = 0;
    // Address the ListView by its identity rather than by position, so appending
    // another node to this snapshot cannot silently retarget the rule.
    const auto listViewNode = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& candidate) { return candidate.nodeId == 8; });
    Check(listViewNode != snapshot.nodes.end(), "ListView semantic node is missing");
    if (listViewNode != snapshot.nodes.end()) listViewNode->checkBoxes = false;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "item check on a checkbox-disabled ListView was accepted");
    action.nodeId = 9;
    action.action = L"select";
    action.integerValue = 1;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "bounded TabControl selection was rejected");
    action.integerValue = -1;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "TabControl accepted an empty selection");

    action.nodeId = 10;
    action.action = L"select";
    action.integerValue = 1;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "bounded TreeView selection was rejected");
    action.integerValue = -1;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "TreeView accepted an empty selection");
    action.action = L"setExpand";
    action.itemIndex = 0;
    action.booleanValue = true;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "TreeView parent expansion was rejected");
    action.itemIndex = 1;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "TreeView leaf expansion was accepted");
    action.itemIndex = 2;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "out-of-range TreeView expansion was accepted");
    action.nodeId = 9;
    action.itemIndex = 0;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "setExpand on a TabControl was accepted");

    action.nodeId = 11;
    action.action = L"setValue";
    action.integerValue = 20;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "in-range Trackbar value was rejected");
    action.integerValue = 21;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "out-of-range Trackbar value was accepted");
    action.nodeId = 10;
    action.integerValue = 1;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "setValue on a TreeView was accepted");

    snapshot.adapterId = L"microsoft.windows.directui.semantic.v1";
    action.nodeId = 1;
    action.action = L"invoke";
    snapshot.nodes[0].supportedActions.clear();
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "application-adapter node accepted an undeclared action");
    snapshot.nodes[0].supportedActions = { L"invoke" };
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "application-adapter node rejected its declared action");
}

void TestActionRevisionPolicy() {
    Check(Translation::IsRequestSemanticAction(L"invoke"),
        "button invoke must use the latest canonical revision");
    Check(Translation::IsRequestSemanticAction(L"close"),
        "close must remain request semantic");
    Check(Translation::IsRequestSemanticAction(L"move") &&
          Translation::IsRequestSemanticAction(L"resize"),
        "geometry is latest-wins and must not reject a stale revision");
    Check(Translation::IsRequestSemanticAction(L"setValue"),
        "a Trackbar drag is latest-wins and must not reject a stale revision");
    Check(!Translation::IsRequestSemanticAction(L"activate") &&
           !Translation::IsRequestSemanticAction(L"setText") &&
           !Translation::IsRequestSemanticAction(L"setCheck") &&
           !Translation::IsRequestSemanticAction(L"setItemCheck") &&
           !Translation::IsRequestSemanticAction(L"setExpand") &&
          !Translation::IsRequestSemanticAction(L"select") &&
          !Translation::IsRequestSemanticAction(L"menuCommand") &&
          !Translation::IsRequestSemanticAction(L"minimize") &&
          !Translation::IsRequestSemanticAction(L"maximize") &&
          !Translation::IsRequestSemanticAction(L"restore"),
        "property actions must retain stale-revision rejection");
}

void TestErrorScopeParsing() {
    const wchar_t* nonce = L"00112233445566778899aabbccddeeff";
    const std::string prefix =
        R"({"messageType":"error","sessionNonce":"00112233445566778899aabbccddeeff",)";
    std::wstring error;

    bool fatal = true;
    std::wstring surfaceId = L"stale";
    const std::string scoped = prefix +
        R"("surfaceId":"11111111-2222-3333-4444-555555555555",)"
        R"("code":"surface_protocol_fault","detail":"bad patch","fatal":false})";
    Check(Translation::ParseErrorMessage(scoped, nonce, error, &fatal, &surfaceId),
        "surface-scoped error payload did not parse");
    Check(!fatal, "non-fatal error was reported as fatal");
    Check(surfaceId == L"11111111-2222-3333-4444-555555555555",
        "surface-scoped error lost its surfaceId");

    // No surfaceId means the fault cannot be attributed to one window.
    fatal = false;
    surfaceId = L"stale";
    const std::string unscoped = prefix +
        R"("code":"protocol_fault","detail":"bad frame","fatal":true})";
    Check(Translation::ParseErrorMessage(unscoped, nonce, error, &fatal, &surfaceId),
        "session error payload did not parse");
    Check(fatal && surfaceId.empty(),
        "session error must stay fatal and unscoped");

    // An unparseable payload must never be downgraded to a recoverable fault.
    fatal = false;
    surfaceId = L"11111111-2222-3333-4444-555555555555";
    Check(!Translation::ParseErrorMessage(prefix + R"("code":"","detail":"x","fatal":false})",
        nonce, error, &fatal, &surfaceId),
        "empty error code was accepted");
    Check(fatal && surfaceId.empty(),
        "rejected error payload must default to session scope");
}

void TestExpandedControlSerialization() {
    Translation::WindowSnapshot snapshot;
    snapshot.surfaceId = L"11111111-2222-3333-4444-555555555555";
    Translation::ControlNode group;
    group.nodeId = 1;
    group.generation = 1;
    group.kind = Translation::ControlKind::GroupBox;
    group.text = L"Status";
    snapshot.nodes.push_back(group);
    Translation::ControlNode progress;
    progress.nodeId = 2;
    progress.generation = 1;
    progress.kind = Translation::ControlKind::ProgressBar;
    progress.minimum = -10;
    progress.maximum = 30;
    progress.position = 12;
    progress.indeterminate = true;
    snapshot.nodes.push_back(progress);
    Translation::ControlNode combo;
    combo.nodeId = 3;
    combo.generation = 1;
    combo.kind = Translation::ControlKind::ComboBox;
    combo.editable = true;
    combo.text = L"custom";
    combo.selectedIndex = 1;
    combo.items = { L"one", L"two" };
    snapshot.nodes.push_back(combo);
    Translation::ControlNode link;
    link.nodeId = 4;
    link.generation = 1;
    link.kind = Translation::ControlKind::SysLink;
    link.text = L"Read the license terms";
    link.automationName = link.text;
    link.items = { L"license terms" };
    snapshot.nodes.push_back(link);
    Translation::ControlNode list;
    list.nodeId = 5;
    list.generation = 1;
    list.kind = Translation::ControlKind::ListView;
    list.columns = { L"Drive", L"Status" };
    list.columnWidths = { 120, 180 };
    list.rows = { { L"C:", L"OK" }, { L"D:", L"Unavailable" } };
    list.items = { L"C:", L"D:" };
    list.columnHeadersVisible = true;
    list.checkBoxes = true;
    list.checkedIndices = { 1 };
    list.selectedIndices = { 0 };
    list.focusedIndex = 0;
    list.multiSelect = true;
    snapshot.nodes.push_back(list);
    Translation::ControlNode status;
    status.nodeId = 6;
    status.generation = 1;
    status.kind = Translation::ControlKind::StatusBar;
    status.items = { L"Ln 1, Col 1", L"100%" };
    status.columnWidths = { 140, 80 };
    snapshot.nodes.push_back(status);
    Translation::ControlNode dialogContainer;
    dialogContainer.nodeId = 7;
    dialogContainer.generation = 1;
    dialogContainer.kind = Translation::ControlKind::DialogContainer;
    dialogContainer.style = WS_CHILD | DS_CONTROL;
    dialogContainer.exStyle = WS_EX_CONTROLPARENT;
    snapshot.nodes.push_back(dialogContainer);

    const auto json = Translation::SerializeWindowOpen(
        L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"kind\":\"groupBox\"") != std::string::npos,
        "GroupBox kind was not serialized");
    Check(json.find("\"kind\":\"progressBar\"") != std::string::npos &&
          json.find("\"minimum\":-10") != std::string::npos &&
          json.find("\"maximum\":30") != std::string::npos &&
          json.find("\"position\":12") != std::string::npos &&
          json.find("\"indeterminate\":true") != std::string::npos,
        "ProgressBar state was not serialized");
    Check(json.find("\"editable\":true") != std::string::npos &&
          json.find("\"text\":\"custom\"") != std::string::npos &&
          json.find("\"selectedIndex\":1") != std::string::npos,
        "editable ComboBox state was not serialized");
    Check(json.find("\"kind\":\"sysLink\"") != std::string::npos &&
           json.find("\"kind\":\"listView\"") != std::string::npos &&
           json.find("\"columns\":[\"Drive\",\"Status\"]") != std::string::npos &&
           json.find("\"columnHeadersVisible\":true") != std::string::npos &&
           json.find("\"checkBoxes\":true") != std::string::npos &&
           json.find("\"checkedIndices\":[1]") != std::string::npos &&
           json.find("\"selectedIndices\":[0]") != std::string::npos &&
           json.find("\"kind\":\"statusBar\"") != std::string::npos &&
           json.find("\"kind\":\"dialogContainer\"") != std::string::npos,
        "expanded common-control state was not serialized");

    const auto editableFingerprint = Translation::SnapshotFingerprint(snapshot);
    snapshot.nodes[2].editable = false;
    Check(editableFingerprint != Translation::SnapshotFingerprint(snapshot),
        "editable state was omitted from the snapshot fingerprint");

    const auto progressFingerprint = Translation::SnapshotFingerprint(snapshot);
    snapshot.nodes[1].indeterminate = false;
    Check(progressFingerprint != Translation::SnapshotFingerprint(snapshot),
        "ProgressBar indeterminate state was omitted from the snapshot fingerprint");

    const auto listFingerprint = Translation::SnapshotFingerprint(snapshot);
    snapshot.nodes[4].selectedIndices = { 1 };
    Check(listFingerprint != Translation::SnapshotFingerprint(snapshot),
        "ListView selection was omitted from the snapshot fingerprint");
    const auto listHeaderFingerprint = Translation::SnapshotFingerprint(snapshot);
    snapshot.nodes[4].columnHeadersVisible = false;
    Check(listHeaderFingerprint != Translation::SnapshotFingerprint(snapshot),
        "ListView column-header visibility was omitted from the snapshot fingerprint");
    const auto listCheckFingerprint = Translation::SnapshotFingerprint(snapshot);
    snapshot.nodes[4].checkedIndices = { 0 };
    Check(listCheckFingerprint != Translation::SnapshotFingerprint(snapshot),
        "ListView checked rows were omitted from the snapshot fingerprint");

    Translation::ActionRequest invokeLink;
    invokeLink.action = L"invoke";
    invokeLink.nodeId = 4;
    std::wstring error;
    Check(Translation::ValidateActionForSnapshot(invokeLink, snapshot, error),
        "bounded SysLink invoke was rejected");
    Translation::ActionRequest selectRows;
    selectRows.action = L"setSelection";
    selectRows.nodeId = 5;
    selectRows.integerValues = { 1 };
    Check(Translation::ValidateActionForSnapshot(selectRows, snapshot, error),
        "bounded ListView selection was rejected");
    selectRows.integerValues = { 2 };
    Check(!Translation::ValidateActionForSnapshot(selectRows, snapshot, error),
        "out-of-range ListView selection was accepted");
}

bool CaptureSingleCombo(DWORD comboStyle, Translation::WindowSnapshot& snapshot) {
    HWND window = CreateWindowExW(0, L"Static", L"combo-capture", WS_OVERLAPPEDWINDOW,
        0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return false;
    HWND combo = CreateWindowExW(0, L"ComboBox", L"", WS_CHILD | WS_VISIBLE | comboStyle,
        10, 10, 200, 120, window, reinterpret_cast<HMENU>(100), GetModuleHandleW(nullptr), nullptr);
    if (combo) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"one"));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"two"));
        SendMessageW(combo, CB_SETCURSEL, 1, 0);
        if ((comboStyle & 0x0003u) == CBS_DROPDOWN) SetWindowTextW(combo, L"custom");
    }
    ShowWindow(window, SW_SHOWNOACTIVATE);
    Translation::CaptureContext context;
    context.surfaceId = L"11111111-2222-3333-4444-555555555555";
    context.generation = 1;
    context.revision = 1;
    std::wstring error;
    const bool captured = combo && Translation::CaptureWindow(window, context, snapshot, error);
    DestroyWindow(window);
    return captured;
}

// The adapter registry is the single place that decides which native class and
// style combinations are inside the bounded translation boundary.  These cases
// pin both directions of that decision so a registry edit cannot silently widen
// or narrow the boundary.
struct AdapterCase final {
    const wchar_t* className;
    DWORD style;
    bool supported;
    Translation::ControlKind kind;
    const char* label;
};

void TestControlAdapterRegistry() {
    HWND root = CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"adapter-registry",
        WS_OVERLAPPEDWINDOW, 20, 20, 480, 320, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    Check(root != nullptr, "adapter registry root was not created");
    if (!root) return;

    using Translation::ControlKind;
    const AdapterCase cases[] = {
        { L"Static", SS_LEFT, true, ControlKind::StaticText, "left Static" },
        { L"Static", SS_ETCHEDHORZ, true, ControlKind::Separator, "etched Static" },
        { L"Static", SS_BITMAP, false, ControlKind::StaticText, "bitmap Static" },
        { L"Button", BS_PUSHBUTTON, true, ControlKind::Button, "push Button" },
        { L"Button", BS_AUTOCHECKBOX, true, ControlKind::CheckBox, "check Button" },
        { L"Button", BS_AUTO3STATE, true, ControlKind::ThreeState, "3-state Button" },
        { L"Button", BS_AUTORADIOBUTTON, true, ControlKind::RadioButton, "radio Button" },
        { L"Button", BS_GROUPBOX, true, ControlKind::GroupBox, "GroupBox" },
        { L"Button", BS_GROUPBOX | WS_TABSTOP, false, ControlKind::GroupBox,
          "tab-stop GroupBox" },
        { L"Button", BS_OWNERDRAW, false, ControlKind::Button, "owner-draw Button" },
        { L"Button", BS_PUSHBUTTON | BS_ICON, false, ControlKind::Button, "icon Button" },
        { L"Edit", 0, true, ControlKind::Edit, "Edit" },
        { L"Edit", ES_PASSWORD, true, ControlKind::Password, "password Edit" },
        { L"ComboBox", CBS_DROPDOWNLIST, true, ControlKind::ComboBox, "dropdown-list ComboBox" },
        { L"ComboBox", CBS_SIMPLE | CBS_HASSTRINGS, false, ControlKind::ComboBox,
          "simple ComboBox" },
        { L"ListBox", 0, true, ControlKind::ListBox, "ListBox" },
        { L"ListBox", LBS_EXTENDEDSEL, false, ControlKind::ListBox, "multi-select ListBox" },
        { PROGRESS_CLASSW, 0, true, ControlKind::ProgressBar, "ProgressBar" },
        { PROGRESS_CLASSW, PBS_MARQUEE, true, ControlKind::ProgressBar, "marquee ProgressBar" },
        { PROGRESS_CLASSW, PBS_VERTICAL, false, ControlKind::ProgressBar, "vertical ProgressBar" },
        { PROGRESS_CLASSW, WS_TABSTOP, false, ControlKind::ProgressBar, "tab-stop ProgressBar" },
        { STATUSCLASSNAMEW, 0, true, ControlKind::StatusBar, "StatusBar" },
        { STATUSCLASSNAMEW, WS_TABSTOP, false, ControlKind::StatusBar, "tab-stop StatusBar" },
        { WC_TREEVIEWW, TVS_HASBUTTONS | TVS_HASLINES, true, ControlKind::TreeView, "TreeView" },
        { WC_TREEVIEWW, TVS_CHECKBOXES, false, ControlKind::TreeView, "checkbox TreeView" },
        { WC_TREEVIEWW, TVS_EDITLABELS, true, ControlKind::TreeView, "label-editing TreeView" },
        { WC_TREEVIEWW, TVS_SINGLEEXPAND, false, ControlKind::TreeView, "single-expand TreeView" },
        { TRACKBAR_CLASSW, TBS_AUTOTICKS, true, ControlKind::Slider, "Trackbar" },
        { TRACKBAR_CLASSW, TBS_VERT | TBS_BOTH, true, ControlKind::Slider, "vertical Trackbar" },
        { TRACKBAR_CLASSW, TBS_ENABLESELRANGE, false, ControlKind::Slider,
          "selection-range Trackbar" },
        { TRACKBAR_CLASSW, TBS_TOOLTIPS, false, ControlKind::Slider, "tooltip Trackbar" },
        { TRACKBAR_CLASSW, TBS_NOTHUMB, false, ControlKind::Slider, "thumbless Trackbar" },
        { TRACKBAR_CLASSW, TBS_NOTIFYBEFOREMOVE, false, ControlKind::Slider,
          "veto-snapping Trackbar" },
        // SysLink is deliberately absent: its class is only registered by
        // comctl32 v6, which this unmanifested test host does not activate.
        // TestStructuredCommonControlCapture covers the SysLink capture path.
    };

    for (const auto& adapter : cases) {
        HWND child = CreateWindowExW(0, adapter.className, L"probe",
            WS_CHILD | adapter.style, 0, 0, 80, 24, root,
            reinterpret_cast<HMENU>(300), GetModuleHandleW(nullptr), nullptr);
        Check(child != nullptr, adapter.label);
        if (!child) continue;
        ControlKind kind = ControlKind::Count;
        std::wstring reason;
        const bool classified = Translation::ClassifyControl(child, kind, reason);
        Check(classified == adapter.supported, adapter.label);
        if (adapter.supported) {
            Check(kind == adapter.kind, adapter.label);
            Check(reason.empty(), adapter.label);
        } else {
            Check(!reason.empty(), adapter.label);
        }
        DestroyWindow(child);
    }

    HWND styledList = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT,
        0, 0, 80, 24, root, reinterpret_cast<HMENU>(302), GetModuleHandleW(nullptr), nullptr);
    if (styledList) {
        Translation::ControlKind kind{};
        std::wstring reason;
        ListView_SetExtendedListViewStyleEx(
            styledList, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
        Check(Translation::ClassifyControl(styledList, kind, reason) &&
              kind == ControlKind::ListView,
            "LVS_EX_CHECKBOXES was rejected by the ListView probe");
        struct RejectedStyle { DWORD value; const wchar_t* name; };
        const RejectedStyle rejectedStyles[] = {
            { LVS_EX_TRACKSELECT, L"LVS_EX_TRACKSELECT" },
            { LVS_EX_ONECLICKACTIVATE, L"LVS_EX_ONECLICKACTIVATE" },
            { LVS_EX_TWOCLICKACTIVATE, L"LVS_EX_TWOCLICKACTIVATE" },
        };
        for (const auto& rejectedStyle : rejectedStyles) {
            ListView_SetExtendedListViewStyleEx(styledList, 0xffffffff,
                LVS_EX_CHECKBOXES | rejectedStyle.value);
            reason.clear();
            Check(!Translation::ClassifyControl(styledList, kind, reason) &&
                  reason.find(rejectedStyle.name) != std::wstring::npos &&
                  reason.find(L"0x") != std::wstring::npos,
                "unsupported ListView extended style lacked exact numeric diagnostics");
        }
        DestroyWindow(styledList);
    }

    // A reorderable header is projected, not refused: the display order travels as a
    // permutation of the logical columns so the projection can present them in
    // header order while every index on the wire keeps the application's meaning.
    {
        HWND orderRoot = CreateWindowExW(0, L"Static", L"list-order", WS_OVERLAPPEDWINDOW,
            10, 10, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND orderList = orderRoot ? CreateWindowExW(0, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT, 0, 0, 300, 160, orderRoot,
            reinterpret_cast<HMENU>(431), GetModuleHandleW(nullptr), nullptr) : nullptr;
        Check(orderRoot && orderList, "column order test controls were not created");
        if (orderRoot && orderList) {
            ListView_SetExtendedListViewStyleEx(orderList,
                LVS_EX_HEADERDRAGDROP, LVS_EX_HEADERDRAGDROP);
            for (int index = 0; index < 3; ++index) {
                LVCOLUMNW column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH;
                wchar_t label[8]{};
                swprintf_s(label, L"C%d", index);
                column.pszText = label;
                column.cx = 60 + index;
                SendMessageW(orderList, LVM_INSERTCOLUMNW, index,
                    reinterpret_cast<LPARAM>(&column));
            }
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.pszText = const_cast<LPWSTR>(L"row");
            SendMessageW(orderList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            int order[3] = { 2, 0, 1 };
            SendMessageW(orderList, LVM_SETCOLUMNORDERARRAY, 3,
                reinterpret_cast<LPARAM>(order));
            ShowWindow(orderRoot, SW_SHOWNOACTIVATE);
            Translation::ControlKind orderKind{};
            std::wstring orderReason;
            Check(Translation::ClassifyControl(orderList, orderKind, orderReason) &&
                  orderKind == Translation::ControlKind::ListView,
                "a reorderable ListView header was refused");
            Translation::ControlNode detail;
            detail.kind = Translation::ControlKind::ListView;
            orderReason.clear();
            Check(Translation::CaptureControlDetail(orderList, detail, orderReason) &&
                  detail.columnOrder == std::vector<int>{ 2, 0, 1 } &&
                  detail.columns.size() == 3 && detail.columns[0] == L"C0",
                "ListView column display order was not captured in header order");
            // The action route sets the order the same way the native header does.
            Check(Translation::SetListViewColumnOrder(orderList, { 1, 2, 0 }),
                "a projected column reorder was refused by the list");
            detail.columnOrder.clear();
            orderReason.clear();
            Check(Translation::CaptureControlDetail(orderList, detail, orderReason) &&
                  detail.columnOrder == std::vector<int>{ 1, 2, 0 },
                "a projected column reorder did not reach the native header");
        }
        if (orderRoot) DestroyWindow(orderRoot);
    }

    // An unregistered class is rejected by name so the log identifies it.
    HWND custom = CreateWindowExW(0, L"ScrollBar", L"probe", WS_CHILD,
        0, 0, 40, 40, root, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (custom) {
        ControlKind kind = ControlKind::Count;
        std::wstring reason;
        Check(!Translation::ClassifyControl(custom, kind, reason),
            "unregistered class was accepted");
        Check(reason.find(L"unsupported visible control class") != std::wstring::npos,
            "unregistered class rejection does not name the class");
        // Win32 reports the registered casing, so compare the way the registry does.
        wchar_t buffer[Translation::kMaxClassNameChars]{};
        const std::wstring reported(Translation::ClassNameOf(custom, buffer));
        Check(_wcsicmp(reported.c_str(), L"ScrollBar") == 0,
            "ClassNameOf did not report the native class");
        DestroyWindow(custom);
    }

    // A ComboBox owns its edit and dropdown list, so they are part of its adapter
    // contract rather than separate projected nodes.
    HWND combo = CreateWindowExW(0, L"ComboBox", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_HASSTRINGS,
        0, 0, 160, 24, root, reinterpret_cast<HMENU>(301),
        GetModuleHandleW(nullptr), nullptr);
    if (combo) {
        COMBOBOXINFO info{ sizeof(info) };
        Check(GetComboBoxInfo(combo, &info) != FALSE, "ComboBox info was unavailable");
        Check(Translation::IsCompositeImplementationChild(info.hwndItem),
            "ComboBox edit child was not treated as an implementation child");
        Check(!Translation::IsCompositeImplementationChild(combo),
            "ComboBox itself was treated as an implementation child");
        DestroyWindow(combo);
    }
    DestroyWindow(root);
}

void TestStructuredCommonControlCapture() {
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"common-control-capture",
        WS_OVERLAPPEDWINDOW, 20, 20, 640, 420, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "common-control capture root was not created");
    if (!window) return;

    constexpr DWORD visibleHeaderStyle = 0x5021000D;
    constexpr DWORD headerlessStyle = 0x5021400D;
    constexpr DWORD targetExStyle = 0x00000204;
    HWND list = CreateWindowExW(targetExStyle, WC_LISTVIEWW, L"",
        visibleHeaderStyle,
        10, 10, 600, 260, window, reinterpret_cast<HMENU>(200),
        GetModuleHandleW(nullptr), nullptr);
    HWND status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE, 0, 340, 620, 24, window,
        reinterpret_cast<HMENU>(201), GetModuleHandleW(nullptr), nullptr);
    Check(list != nullptr && status != nullptr,
        "structured common controls were not created");
    if (!list || !status) {
        DestroyWindow(window);
        return;
    }

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = 180;
    column.pszText = const_cast<LPWSTR>(L"Drive");
    SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
    column.cx = 260;
    column.pszText = const_cast<LPWSTR>(L"Status");
    SendMessageW(list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = 0;
    item.pszText = const_cast<LPWSTR>(L"C:");
    SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    item.iItem = 1;
    item.pszText = const_cast<LPWSTR>(L"D:");
    SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    LVITEMW subItem{};
    subItem.iSubItem = 1;
    subItem.pszText = const_cast<LPWSTR>(L"OK");
    SendMessageW(list, LVM_SETITEMTEXTW, 0, reinterpret_cast<LPARAM>(&subItem));
    subItem.pszText = const_cast<LPWSTR>(L"Unavailable");
    SendMessageW(list, LVM_SETITEMTEXTW, 1, reinterpret_cast<LPARAM>(&subItem));
    LVITEMW selection{};
    selection.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    selection.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(list, LVM_SETITEMSTATE, 1, reinterpret_cast<LPARAM>(&selection));
    ListView_SetExtendedListViewStyleEx(list, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
    Check(Translation::SetListViewItemCheck(list, 1, true),
        "native ListView checkbox state could not be initialized");

    int parts[] = { 260, -1 };
    SendMessageW(status, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
    SendMessageW(status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"Ln 1, Col 1"));
    SendMessageW(status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(L"100%"));
    // A status bar's text lives in its own parts.  An application that parks an empty
    // child window in one -- MMC keeps a simple Static and a hidden progress bar there
    // -- contributes nothing the projection can lose, so the child is absorbed rather
    // than refusing the whole window.
    HWND statusPlaceholder = CreateWindowExW(0, L"Static", L"",
        WS_CHILD | WS_VISIBLE | SS_SIMPLE, 4, 2, 120, 18, status,
        reinterpret_cast<HMENU>(4097), GetModuleHandleW(nullptr), nullptr);
    Check(statusPlaceholder != nullptr, "StatusBar placeholder child was not created");
    ShowWindow(window, SW_SHOWNOACTIVATE);

    Translation::CaptureContext context;
    context.surfaceId = L"11111111-2222-3333-4444-555555555555";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    Check(Translation::CaptureWindow(window, context, snapshot, error),
        "bounded ListView/StatusBar window did not capture");
    const auto listNode = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& node) {
            return node.kind == Translation::ControlKind::ListView;
        });
    Check(listNode != snapshot.nodes.end() &&
          listNode->columns == std::vector<std::wstring>{ L"Drive", L"Status" } &&
           listNode->rows.size() == 2 && listNode->rows[1][1] == L"Unavailable" &&
           listNode->selectedIndices == std::vector<int>{ 1 } &&
           listNode->focusedIndex == 1 && !listNode->multiSelect &&
           listNode->columnHeadersVisible && listNode->checkBoxes &&
           listNode->checkedIndices == std::vector<int>{ 1 },
        "bounded report ListView state was not captured canonically");
    const auto statusNode = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& node) {
            return node.kind == Translation::ControlKind::StatusBar;
        });
    Check(statusNode != snapshot.nodes.end() &&
          statusNode->items == std::vector<std::wstring>{ L"Ln 1, Col 1", L"100%" } &&
          statusNode->columnWidths.size() == 2,
        "bounded StatusBar state was not captured canonically");
    // The absorbed placeholder must not appear as a node of its own: the status bar's
    // parts already carry every piece of text the window shows there.
    Check(std::none_of(snapshot.nodes.begin(), snapshot.nodes.end(),
            [&](const Translation::ControlNode& node) {
                return node.hwnd == statusPlaceholder;
            }),
        "StatusBar placeholder child was projected as a node of its own");
    // One carrying text is content, and content inside a status bar is not something
    // the parts can describe, so it still refuses the window.
    SetWindowTextW(statusPlaceholder, L"Ready");
    Translation::WindowSnapshot textualChild;
    context.revision = 7;
    Check(!Translation::CaptureWindow(window, context, textualChild, error),
        "StatusBar child carrying text was absorbed instead of refused");
    SetWindowTextW(statusPlaceholder, L"");
    context.revision = 1;

    SetWindowLongPtrW(list, GWL_STYLE, headerlessStyle);
    SetWindowPos(list, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    Translation::WindowSnapshot headerlessSnapshot;
    context.revision = 2;
    const auto actualHeaderlessStyle =
        static_cast<DWORD>(GetWindowLongPtrW(list, GWL_STYLE));
    // The control owns WS_VSCROLL and clears it when the current rows fit.
    const bool exactStyle =
        (actualHeaderlessStyle & ~WS_VSCROLL) == (headerlessStyle & ~WS_VSCROLL) &&
        static_cast<DWORD>(GetWindowLongPtrW(list, GWL_EXSTYLE)) == targetExStyle;
    const bool headerlessCaptured =
        Translation::CaptureWindow(window, context, headerlessSnapshot, error);
    if (!headerlessCaptured) std::wcerr << L"headerless capture rejection: " << error << L'\n';
    Check(exactStyle && headerlessCaptured,
        "exact headerless report ListView style did not capture");
    const auto headerlessNode = std::find_if(
        headerlessSnapshot.nodes.begin(), headerlessSnapshot.nodes.end(),
        [](const Translation::ControlNode& node) {
            return node.kind == Translation::ControlKind::ListView;
        });
    Check(headerlessNode != headerlessSnapshot.nodes.end() &&
          !headerlessNode->columnHeadersVisible &&
          headerlessNode->columns == std::vector<std::wstring>{ L"Drive", L"Status" } &&
          headerlessNode->columnWidths == std::vector<int>{ 180, 260 } &&
          headerlessNode->rows.size() == 2 &&
          headerlessNode->rows[1][1] == L"Unavailable",
        "headerless report ListView lost its canonical columns or row cells");
    const auto headerlessJson = Translation::SerializeWindowOpen(
        L"00112233445566778899aabbccddeeff", headerlessSnapshot);
    Check(headerlessJson.find("\"columnHeadersVisible\":false") != std::string::npos,
        "headerless report ListView visibility was not serialized");

    SendMessageW(status, SB_SETTEXTW, SBT_OWNERDRAW,
        reinterpret_cast<LPARAM>(reinterpret_cast<void*>(1)));
    Translation::WindowSnapshot rejected;
    context.revision = 2;
    Check(!Translation::CaptureWindow(window, context, rejected, error),
        "owner-draw StatusBar part was accepted");
    DestroyWindow(window);

    HWND virtualWindow = CreateWindowExW(0, L"Static", L"virtual-list",
        WS_OVERLAPPEDWINDOW, 20, 20, 320, 200, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    HWND virtualList = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA,
        10, 10, 280, 120, virtualWindow, reinterpret_cast<HMENU>(202),
        GetModuleHandleW(nullptr), nullptr);
    ShowWindow(virtualWindow, SW_SHOWNOACTIVATE);
    Translation::CaptureContext virtualContext;
    virtualContext.surfaceId = L"22222222-2222-3333-4444-555555555555";
    virtualContext.generation = 1;
    virtualContext.revision = 1;
    Check(virtualList && !Translation::CaptureWindow(
            virtualWindow, virtualContext, rejected, error),
        "virtual ListView was accepted by the bounded adapter");
    DestroyWindow(virtualWindow);
}

void TestListViewCheckNotificationSemantics() {
    HWND window = CreateWindowExW(0, L"Static", L"list-check-action", WS_OVERLAPPEDWINDOW,
        20, 20, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND list = window ? CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT, 10, 10, 280, 120, window,
        reinterpret_cast<HMENU>(440), GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(window && list, "ListView notification test controls were not created");
    if (!window || !list) {
        if (window) DestroyWindow(window);
        return;
    }
    SetWindowSubclass(window, ListViewNotificationSubclass, 1, 0);
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = 100;
    column.pszText = const_cast<LPWSTR>(L"Name");
    ListView_InsertColumn(list, 0, &column);
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.pszText = const_cast<LPWSTR>(L"row");
    ListView_InsertItem(list, &item);
    ListView_SetExtendedListViewStyleEx(list, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);

    g_itemChanging = g_itemChanged = 0;
    g_vetoItemChange = false;
    Check(Translation::SetListViewItemCheck(list, 0, true) &&
          g_itemChanging == 1 && g_itemChanged == 1,
        "setItemCheck did not preserve one native changing/changed notification pair");
    g_vetoItemChange = true;
    Check(!Translation::SetListViewItemCheck(list, 0, false) &&
          ListView_GetCheckState(list, 0) && g_itemChanging == 2 && g_itemChanged == 1,
        "setItemCheck ignored LVN_ITEMCHANGING veto or double-fired notifications");
    g_vetoItemChange = false;
    DestroyWindow(window);
}

void TestTabControlCaptureAndSelection() {
    HWND window = CreateWindowExW(0, L"Static", L"tab-control", WS_OVERLAPPEDWINDOW,
        20, 20, 440, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    constexpr DWORD targetStyle = 0x54030240;
    constexpr DWORD targetExStyle = 0x00000004;
    HWND tab = window ? CreateWindowExW(targetExStyle, WC_TABCONTROLW, L"", targetStyle,
        10, 10, 260, 180, window, reinterpret_cast<HMENU>(450),
        GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(window && tab, "TabControl capture controls were not created");
    if (!window || !tab) {
        if (window) DestroyWindow(window);
        return;
    }
    SetWindowSubclass(window, ListViewNotificationSubclass, 2, 0);
    const wchar_t* labels[] = { L"General", L"Advanced options", L"Diagnostics", L"About" };
    const std::array<LPARAM, 4> itemData{
        static_cast<LPARAM>(0x1111222233334444ll),
        static_cast<LPARAM>(0x2222333344445555ll),
        static_cast<LPARAM>(0x3333444455556666ll),
        static_cast<LPARAM>(0x4444555566667777ll),
    };
    for (int index = 0; index < static_cast<int>(std::size(labels)); ++index) {
        TCITEMW item{};
        item.mask = TCIF_TEXT | TCIF_PARAM;
        item.pszText = const_cast<LPWSTR>(labels[index]);
        item.lParam = itemData[static_cast<size_t>(index)];
        Check(SendMessageW(tab, TCM_INSERTITEMW, index,
            reinterpret_cast<LPARAM>(&item)) == index, "TabControl item insertion failed");
    }
    SendMessageW(tab, TCM_SETCURSEL, 2, 0);
    ShowWindow(window, SW_SHOWNOACTIVATE);

    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-555555555555";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(window, context, snapshot, error);
    if (!captured) std::wcerr << L"TabControl capture rejection: " << error << L'\n';
    Check(captured, "bounded multiline/hot-track TabControl was rejected");
    const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& candidate) {
            return candidate.kind == Translation::ControlKind::TabControl;
        });
    Check(node != snapshot.nodes.end() && node->items.size() == std::size(labels) &&
          node->itemRects.size() == node->items.size() && node->selectedIndex == 2,
        "TabControl labels, local rectangles, or selection were not captured");
    if (node != snapshot.nodes.end()) {
        std::vector<std::pair<LONG, LONG>> rows;
        bool strictRows = true;
        for (const RECT& rect : node->itemRects) {
            const auto row = std::find_if(rows.begin(), rows.end(),
                [&rect](const auto& candidate) { return candidate.first == rect.top; });
            if (row == rows.end()) rows.emplace_back(rect.top, rect.bottom - rect.top);
            else if (row->second != rect.bottom - rect.top) strictRows = false;
        }
        std::sort(rows.begin(), rows.end());
        for (size_t index = 1; index < rows.size(); ++index) {
            if (rows[index - 1].first + rows[index - 1].second > rows[index].first)
                strictRows = false;
        }
        Check(strictRows && rows.size() > 1,
            "TCS_MULTILINE did not expose distinct ordered header-row geometry");
        const auto json = Translation::SerializeWindowOpen(
            L"00112233445566778899aabbccddeeff", snapshot);
        Check(json.find("\"kind\":\"tabControl\"") != std::string::npos &&
              json.find("\"itemRects\":[") != std::string::npos &&
              json.find("\"lParam\"") == std::string::npos &&
              json.find("\"itemData\"") == std::string::npos,
            "TabControl state was not serialized or leaked opaque item data");
        const auto fingerprint = Translation::SnapshotFingerprint(snapshot);
        snapshot.nodes[static_cast<size_t>(node - snapshot.nodes.begin())].selectedIndex = 1;
        Check(fingerprint != Translation::SnapshotFingerprint(snapshot),
            "TabControl selection was omitted from the fingerprint");
    }

    const auto itemDataUnchanged = [&] {
        for (int index = 0; index < static_cast<int>(itemData.size()); ++index) {
            TCITEMW item{};
            item.mask = TCIF_PARAM;
            if (!SendMessageW(tab, TCM_GETITEMW, index, reinterpret_cast<LPARAM>(&item)) ||
                item.lParam != itemData[static_cast<size_t>(index)]) return false;
        }
        return true;
    };

    g_tabChanging = g_tabChanged = 0;
    g_tabNotifications.clear();
    g_vetoTabChange = false;
    Check(Translation::SelectTabControl(window, tab, 450, 1, 4) &&
          TabCtrl_GetCurSel(tab) == 1 && g_tabChanging == 1 && g_tabChanged == 1 &&
          g_tabNotifications == std::vector<UINT>{ TCN_SELCHANGING, TCN_SELCHANGE } &&
          itemDataUnchanged(),
        "TabControl selection changed item data or notification ordering");
    g_tabNotifications.clear();
    g_vetoTabChange = true;
    Check(!Translation::SelectTabControl(window, tab, 450, 3, 4) &&
          TabCtrl_GetCurSel(tab) == 1 && g_tabChanging == 2 && g_tabChanged == 1 &&
          g_tabNotifications == std::vector<UINT>{ TCN_SELCHANGING } &&
          itemDataUnchanged(),
        "TabControl veto changed item data or notification ordering");
    g_vetoTabChange = false;

    Translation::ControlKind kind{};
    std::wstring reason;
    struct RejectedStyle { DWORD value; const wchar_t* diagnostic; };
    const RejectedStyle rejected[] = {
        { TCS_OWNERDRAWFIXED, L"TCS_OWNERDRAWFIXED" },
        { TCS_BUTTONS, L"button" },
        { TCS_FLATBUTTONS, L"button" },
        { TCS_VERTICAL, L"vertical" },
        { TCS_BOTTOM, L"bottom" },
        { TCS_FIXEDWIDTH, L"TCS_FIXEDWIDTH" },
        { TCS_TOOLTIPS, L"tooltips" },
    };
    for (const auto& entry : rejected) {
        SetWindowLongPtrW(tab, GWL_STYLE, targetStyle | entry.value);
        reason.clear();
        Check(!Translation::ClassifyControl(tab, kind, reason) &&
              reason.find(entry.diagnostic) != std::wstring::npos,
            "unsupported TabControl style lacked precise diagnostics");
    }
    SetWindowLongPtrW(tab, GWL_STYLE, targetStyle);
    HIMAGELIST images = ImageList_Create(16, 16, ILC_COLOR32, 1, 1);
    TabCtrl_SetImageList(tab, images);
    reason.clear();
    Check(images && !Translation::ClassifyControl(tab, kind, reason) &&
          reason.find(L"image-backed") != std::wstring::npos,
        "image-backed TabControl was accepted");
    TabCtrl_SetImageList(tab, nullptr);
    if (images) ImageList_Destroy(images);

    Translation::ControlNode detail;
    detail.kind = Translation::ControlKind::TabControl;
    detail.style = targetStyle;
    reason.clear();
    Check(Translation::CaptureControlDetail(tab, detail, reason) && itemDataUnchanged(),
        "opaque nonzero TabControl item data was rejected or mutated");
    TCITEMW extra{};
    extra.mask = TCIF_TEXT;
    extra.pszText = const_cast<LPWSTR>(L"extra");
    while (TabCtrl_GetItemCount(tab) <= static_cast<int>(Ipc::kMaxTabItems)) {
        const int index = TabCtrl_GetItemCount(tab);
        SendMessageW(tab, TCM_INSERTITEMW, index, reinterpret_cast<LPARAM>(&extra));
    }
    reason.clear();
    Check(!Translation::CaptureControlDetail(tab, detail, reason) &&
          reason.find(L"excessive") != std::wstring::npos,
        "excessive TabControl item count was accepted");
    DestroyWindow(window);
}

void TestEditableComboCaptureBoundary() {
    Translation::WindowSnapshot snapshot;
    Check(CaptureSingleCombo(CBS_DROPDOWNLIST, snapshot) &&
          snapshot.nodes.size() == 1 && !snapshot.nodes[0].editable,
        "standard CBS_DROPDOWNLIST was rejected");
    Check(CaptureSingleCombo(CBS_DROPDOWN | CBS_HASSTRINGS, snapshot) &&
          snapshot.nodes.size() == 1 && snapshot.nodes[0].editable &&
          snapshot.nodes[0].text == L"custom" && snapshot.nodes[0].selectedIndex == 1 &&
          snapshot.nodes[0].items.size() == 2,
        "string-backed editable ComboBox state was not captured canonically");
    Check(!CaptureSingleCombo(CBS_SIMPLE | CBS_HASSTRINGS, snapshot),
        "CBS_SIMPLE was accepted");
    Check(CaptureSingleCombo(CBS_DROPDOWN, snapshot) &&
          snapshot.nodes.size() == 1 && snapshot.nodes[0].editable,
        "Win32-normalized string-backed CBS_DROPDOWN was rejected");
    Check(!CaptureSingleCombo(CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, snapshot),
        "owner-draw ComboBox was accepted");
}

void TestStaticIconCaptureBoundary() {
    HWND window = CreateWindowExW(0, L"Static", L"icon-capture", WS_OVERLAPPEDWINDOW,
        20, 20, 240, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND iconControl = window ? CreateWindowExW(0, L"Static", nullptr,
        WS_CHILD | WS_VISIBLE | SS_ICON, 10, 10, 32, 32, window,
        reinterpret_cast<HMENU>(430), GetModuleHandleW(nullptr), nullptr) : nullptr;
    HICON icon = CreateColorIcon(2, 2, { 100, 50, 200, 128 });
    Check(window && iconControl && icon, "Static icon capture controls were not created");
    if (!window || !iconControl || !icon) {
        if (icon) DestroyIcon(icon);
        if (window) DestroyWindow(window);
        return;
    }
    SendMessageW(iconControl, STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    Translation::CaptureContext context;
    context.surfaceId = L"66666666-6666-6666-6666-555555555555";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    Check(Translation::CaptureWindow(window, context, snapshot, error),
        "bounded SS_ICON was rejected");
    const auto captured = snapshot.nodes.empty() ? nullptr : &snapshot.nodes[0];
    Check(captured && captured->kind == Translation::ControlKind::StaticIcon &&
          captured->imageWidth == 2 && captured->imageHeight == 2 &&
          captured->imageFormat == L"bgra8-premultiplied" &&
          captured->imageData.size() == 16 &&
          captured->imageData[0] == 50 && captured->imageData[1] == 25 &&
          captured->imageData[2] == 100 && captured->imageData[3] == 128,
        "SS_ICON pixels were not copied as premultiplied BGRA");
    const auto serialized = Translation::SerializeWindowOpen(
        L"00112233445566778899aabbccddeeff", snapshot);
    Check(serialized.find("\"kind\":\"staticIcon\"") != std::string::npos &&
          serialized.find("\"imageWidth\":2") != std::string::npos &&
          serialized.find("\"imageFormat\":\"bgra8-premultiplied\"") != std::string::npos &&
          serialized.find("\"imageData\":") != std::string::npos,
        "SS_ICON owned pixels were not serialized onto the wire");
    const uint64_t firstFingerprint = Translation::SnapshotFingerprint(snapshot);
    const auto copiedPixels = captured ? captured->imageData : std::vector<uint8_t>{};
    DestroyIcon(icon);
    icon = nullptr;
    Check(captured && captured->imageData == copiedPixels,
        "captured Static icon retained the source HICON lifetime");

    HICON replacement = CreateColorIcon(1, 1, { 0, 0, 255, 0 }, true);
    SendMessageW(iconControl, STM_SETICON, reinterpret_cast<WPARAM>(replacement), 0);
    context.revision = 3;
    Translation::WindowSnapshot replaced;
    Check(replacement && Translation::CaptureWindow(window, context, replaced, error) &&
          Translation::SnapshotFingerprint(replaced) != firstFingerprint &&
          replaced.nodes[0].imageData == std::vector<uint8_t>{ 0, 0, 0, 0 },
        "mask-based STM_SETICON replacement was not captured transparently");
    if (replacement) DestroyIcon(replacement);

    SendMessageW(iconControl, STM_SETICON, 0, 0);
    context.revision = 3;
    Translation::WindowSnapshot missing;
    Check(!Translation::CaptureWindow(window, context, missing, error) &&
          error.find(L"no current HICON") != std::wstring::npos,
        "SS_ICON without an image was accepted");

    HICON oversized = CreateColorIcon(
        static_cast<int>(Ipc::kMaxImageDimension) + 1, 1, { 0, 0, 0, 255 });
    SendMessageW(iconControl, STM_SETICON, reinterpret_cast<WPARAM>(oversized), 0);
    context.revision = 4;
    Check(oversized && !Translation::CaptureWindow(window, context, missing, error) &&
          error.find(L"dimensions exceed") != std::wstring::npos,
        "oversized SS_ICON payload was accepted");
    if (oversized) DestroyIcon(oversized);
    DestroyWindow(window);

    Translation::ControlKind kind{};
    std::wstring reason;
    HWND bitmap = CreateWindowExW(0, L"Static", nullptr, WS_CHILD | SS_BITMAP,
        0, 0, 10, 10, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(bitmap && !Translation::ClassifyControl(bitmap, kind, reason),
        "SS_BITMAP was accepted by the SS_ICON adapter");
    if (bitmap) DestroyWindow(bitmap);
}

void TestAncestorEnabledTabOrderCapture() {
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"tab-order-capture",
        WS_OVERLAPPEDWINDOW, 20, 20, 360, 240, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    HWND panel = window ? CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"",
        WS_CHILD | WS_VISIBLE | WS_DISABLED, 10, 10, 320, 160, window,
        reinterpret_cast<HMENU>(400), GetModuleHandleW(nullptr), nullptr) : nullptr;
    HWND edit = panel ? CreateWindowExW(0, L"Edit", L"nested",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 10, 10, 180, 24, panel,
        reinterpret_cast<HMENU>(401), GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(window && panel && edit, "ancestor-enabled capture controls were not created");
    if (!window || !panel || !edit) {
        if (window) DestroyWindow(window);
        return;
    }
    ShowWindow(window, SW_SHOWNOACTIVATE);

    Translation::CaptureContext context;
    context.surfaceId = L"33333333-3333-3333-3333-555555555555";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot disabledSnapshot;
    std::wstring error;
    Check(Translation::CaptureWindow(window, context, disabledSnapshot, error),
        "tab stop below a disabled ancestor was rejected");
    const auto disabledEdit = std::find_if(disabledSnapshot.nodes.begin(), disabledSnapshot.nodes.end(),
        [edit](const Translation::ControlNode& node) { return node.hwnd == edit; });
    Check(disabledEdit != disabledSnapshot.nodes.end() && !disabledEdit->enabled &&
          disabledEdit->tabStop && disabledEdit->tabIndex == -1,
        "disabled ancestor was not reflected in child enabledness and tab order");

    EnableWindow(panel, TRUE);
    context.revision = 2;
    Translation::WindowSnapshot enabledSnapshot;
    Check(Translation::CaptureWindow(window, context, enabledSnapshot, error),
        "tab stop below an enabled control parent was rejected");
    const auto enabledEdit = std::find_if(enabledSnapshot.nodes.begin(), enabledSnapshot.nodes.end(),
        [edit](const Translation::ControlNode& node) { return node.hwnd == edit; });
    Check(enabledEdit != enabledSnapshot.nodes.end() && enabledEdit->enabled &&
          enabledEdit->tabStop && enabledEdit->tabIndex >= 0,
        "enabled ancestor did not restore child enabledness and tab order");
    Check(Translation::SnapshotFingerprint(disabledSnapshot) !=
          Translation::SnapshotFingerprint(enabledSnapshot),
        "ancestor enabledness change was omitted from the snapshot fingerprint");
    DestroyWindow(window);
}

void TestTabOrderRejectionSpecificity() {
    HWND dialogRoot = CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"dialog-container",
        WS_OVERLAPPEDWINDOW, 20, 20, 360, 240, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    HWND childDialog = dialogRoot ? CreateWindowExW(WS_EX_CONTROLPARENT, L"#32770", L"Page name",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | DS_CONTROL, 10, 10, 320, 160, dialogRoot,
        reinterpret_cast<HMENU>(410), GetModuleHandleW(nullptr), nullptr) : nullptr;
    HWND nestedEdit = childDialog ? CreateWindowExW(0, L"Edit", L"nested",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 10, 10, 180, 24, childDialog,
        reinterpret_cast<HMENU>(411), GetModuleHandleW(nullptr), nullptr) : nullptr;
    HWND nestedRadio = childDialog ? CreateWindowExW(0, L"Button", L"choice",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
        10, 45, 180, 24, childDialog, reinterpret_cast<HMENU>(413),
        GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(dialogRoot && childDialog && nestedEdit && nestedRadio,
        "child dialog focus test controls were not created");
    if (dialogRoot && childDialog && nestedEdit && nestedRadio) {
        ShowWindow(dialogRoot, SW_SHOWNOACTIVATE);
        Translation::CaptureContext context;
        context.surfaceId = L"44444444-4444-4444-4444-555555555555";
        context.generation = 1;
        context.revision = 1;
        Translation::WindowSnapshot snapshot;
        std::wstring error;
        Check(Translation::CaptureWindow(dialogRoot, context, snapshot, error),
            "bounded DS_CONTROL container was rejected");
        const auto container = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [childDialog](const Translation::ControlNode& node) { return node.hwnd == childDialog; });
        const auto editNode = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [nestedEdit](const Translation::ControlNode& node) { return node.hwnd == nestedEdit; });
        const auto radioNode = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [nestedRadio](const Translation::ControlNode& node) { return node.hwnd == nestedRadio; });
        Check(container != snapshot.nodes.end() &&
              container->kind == Translation::ControlKind::DialogContainer &&
              !container->tabStop && container->tabIndex == -1,
            "DS_CONTROL container was not captured as a non-focusable structural node");
        Check(container != snapshot.nodes.end() && editNode != snapshot.nodes.end() &&
              editNode->parentNodeId == container->nodeId && editNode->tabIndex >= 0,
            "nested Edit lost parent identity or dialog-manager tab order");
        Check(container != snapshot.nodes.end() && container->automationName == L"Page name",
            "DS_CONTROL accessibility name was not retained");
        Check(container != snapshot.nodes.end() && radioNode != snapshot.nodes.end() &&
              radioNode->parentNodeId == container->nodeId && radioNode->groupStart,
            "nested radio control lost its container scope or group boundary");
        Check(Translation::SyntheticNotificationTarget(dialogRoot, nestedEdit) == childDialog &&
              Translation::SyntheticNotificationTarget(dialogRoot, childDialog) == dialogRoot,
            "synthetic control notification would bypass the immediate dialog procedure");
    }
    if (dialogRoot) DestroyWindow(dialogRoot);

    HWND unsupportedRoot = CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"unsupported-dialog",
        WS_OVERLAPPEDWINDOW, 20, 20, 360, 240, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    HWND unsupportedDialog = unsupportedRoot ? CreateWindowExW(
        WS_EX_CONTROLPARENT | WS_EX_CLIENTEDGE, L"#32770", L"caption",
        WS_CHILD | WS_VISIBLE | DS_CONTROL | WS_BORDER, 10, 10, 200, 100,
        unsupportedRoot, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    if (unsupportedRoot && unsupportedDialog) {
        ShowWindow(unsupportedRoot, SW_SHOWNOACTIVATE);
        Translation::CaptureContext context;
        context.surfaceId = L"44444444-4444-3333-4444-555555555555";
        context.generation = 1;
        context.revision = 1;
        Translation::WindowSnapshot snapshot;
        std::wstring error;
        Check(!Translation::CaptureWindow(unsupportedRoot, context, snapshot, error) &&
              error.find(L"unsupported visible chrome") != std::wstring::npos,
            "chromed DS_CONTROL child dialog was accepted");
    }
    if (unsupportedRoot) DestroyWindow(unsupportedRoot);

    HWND listRoot = CreateWindowExW(0, L"Static", L"label-edit-list",
        WS_OVERLAPPEDWINDOW, 20, 20, 360, 240, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    HWND list = listRoot ? CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_EDITLABELS,
        10, 10, 320, 160, listRoot, reinterpret_cast<HMENU>(412),
        GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(listRoot && list, "label-edit ListView test controls were not created");
    if (listRoot && list) {
        ShowWindow(listRoot, SW_SHOWNOACTIVATE);
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(L"Name");
        column.cx = 200;
        SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.pszText = const_cast<LPWSTR>(L"Volume");
        SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        Translation::CaptureContext context;
        context.surfaceId = L"55555555-5555-5555-5555-555555555555";
        context.generation = 1;
        context.revision = 1;
        Translation::WindowSnapshot snapshot;
        std::wstring error;
        // A label-editable report list is projected, and the capture says so, so
        // the projection can offer the rename instead of hiding the capability.
        const bool captured = Translation::CaptureWindow(listRoot, context, snapshot, error);
        if (!captured) std::wcerr << L"label-edit ListView rejection: " << error << L'\n';
        const auto listNode = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [](const Translation::ControlNode& candidate) {
                return candidate.kind == Translation::ControlKind::ListView;
            });
        Check(captured && listNode != snapshot.nodes.end() && listNode->editableLabels,
            "a label-editable report ListView was not projected as renamable");
    }
    if (listRoot) DestroyWindow(listRoot);

    HWND privateRoot = CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"private-host",
        WS_OVERLAPPEDWINDOW, 20, 20, 360, 240, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    HWND privateHost = privateRoot ? CreateWindowExW(0, L"Static", L"",
        WS_CHILD | WS_VISIBLE, 10, 10, 320, 160, privateRoot,
        reinterpret_cast<HMENU>(420), GetModuleHandleW(nullptr), nullptr) : nullptr;
    HWND privateControl = privateHost ? CreateWindowExW(0, L"ScrollBar", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | SBS_HORZ, 10, 10, 180, 24, privateHost,
        reinterpret_cast<HMENU>(421), GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(privateRoot && privateHost && privateControl,
        "nested unsupported focus test controls were not created");
    if (privateRoot && privateHost && privateControl) {
        ShowWindow(privateRoot, SW_SHOWNOACTIVATE);
        Translation::CaptureContext context;
        context.surfaceId = L"66666666-6666-6666-6666-555555555555";
        context.generation = 1;
        context.revision = 1;
        Translation::WindowSnapshot snapshot;
        std::wstring error;
        Check(!Translation::CaptureWindow(privateRoot, context, snapshot, error) &&
              error.find(L"unsupported visible control class") != std::wstring::npos &&
              error.find(L"ScrollBar") != std::wstring::npos,
            "unsupported nested class was masked by dialog tab-order diagnostics");
    }
    if (privateRoot) DestroyWindow(privateRoot);
}

void TestStandardMenuCapture() {
    HWND window = CreateWindowExW(0, L"Static", L"menu-test", WS_OVERLAPPED,
        0, 0, 300, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "menu test window creation failed");
    if (!window) return;
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING | MF_CHECKED, 100, L"&Open\tCtrl+O");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING | MF_DISABLED, 101, L"E&xit");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    SetMenu(window, bar);

    std::vector<Translation::MenuItemSnapshot> menu;
    std::wstring error;
    Check(Translation::CaptureTopLevelMenu(window, menu, error),
        "standard textual menu was rejected");
    Check(menu.size() == 1 && menu[0].itemId == L"0" && menu[0].text == L"&File" &&
        menu[0].items.size() == 3 && menu[0].items[0].commandId == 100 &&
        menu[0].items[0].checked && !menu[0].items[2].enabled,
        "captured menu lost labels, identities, or state");

    AppendMenuW(file, MF_STRING, 100, L"Duplicate");
    Check(!Translation::CaptureTopLevelMenu(window, menu, error),
        "duplicate executable command ID was accepted");
    RemoveMenu(file, 3, MF_BYPOSITION);
    AppendMenuW(file, MF_OWNERDRAW, 102, reinterpret_cast<LPCWSTR>(1));
    Check(!Translation::CaptureTopLevelMenu(window, menu, error),
        "owner-draw menu item was accepted");
    RemoveMenu(file, 3, MF_BYPOSITION);
    MENUINFO callbackInfo{sizeof(callbackInfo)};
    callbackInfo.fMask = MIM_STYLE;
    callbackInfo.dwStyle = MNS_NOTIFYBYPOS;
    SetMenuInfo(file, &callbackInfo);
    Check(!Translation::CaptureTopLevelMenu(window, menu, error),
        "position-callback menu was accepted");
    callbackInfo.dwStyle = 0;
    SetMenuInfo(file, &callbackInfo);
    for (UINT id = 1000; id < 1257; ++id) AppendMenuW(file, MF_STRING, id, L"Item");
    Check(!Translation::CaptureTopLevelMenu(window, menu, error),
        "excessive menu item tree was accepted");

    SetMenu(window, nullptr);
    DestroyMenu(bar);

    HMENU deepBar = CreateMenu();
    HMENU current = CreatePopupMenu();
    AppendMenuW(deepBar, MF_POPUP, reinterpret_cast<UINT_PTR>(current), L"Root");
    for (int depth = 0; depth < 8; ++depth) {
        HMENU next = CreatePopupMenu();
        AppendMenuW(current, MF_POPUP, reinterpret_cast<UINT_PTR>(next), L"Nested");
        current = next;
    }
    AppendMenuW(current, MF_STRING, 200, L"Command");
    SetMenu(window, deepBar);
    Check(!Translation::CaptureTopLevelMenu(window, menu, error),
        "deep menu tree was accepted");
    SetMenu(window, nullptr);
    DestroyMenu(deepBar);
    DestroyWindow(window);
}

void TestMenuActionValidationAndSerialization() {
    Translation::WindowSnapshot snapshot;
    Translation::MenuItemSnapshot root;
    root.itemId = L"0";
    root.kind = Translation::MenuItemKind::Popup;
    root.text = L"&File";
    Translation::MenuItemSnapshot command;
    command.itemId = L"0.0";
    command.kind = Translation::MenuItemKind::Command;
    command.text = L"&Open";
    command.commandId = 77;
    root.items.push_back(command);
    snapshot.menu.push_back(root);
    Translation::ActionRequest action;
    action.action = L"menuCommand";
    action.menuCommandId = 77;
    std::wstring error;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "known enabled menu command was rejected");
    action.menuCommandId = 78;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "unknown menu command was accepted");
    snapshot.surfaceId = L"11111111-2222-3333-4444-555555555555";
    const auto json = Translation::SerializeWindowOpen(
        L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"menu\":[") != std::string::npos &&
        json.find("\"commandId\":77") != std::string::npos &&
        json.find("\"itemId\":\"0.0\"") != std::string::npos,
        "typed menu snapshot was not serialized");
}

// The translated dialog lane answers the native call itself, so its snapshots are
// built without any HWND tree.  They still travel over the same protocol, so they
// have to satisfy the same cross-node invariants the renderer enforces on
// admission: unique nonzero node IDs, unique z-index per node, and no node
// claiming a native window.  A missing z-index here is invisible in the Bridge and
// faults the surface only once a dialog actually opens.
void CheckVirtualDialogSnapshot(
    const Translation::WindowSnapshot& snapshot,
    const char* label) {
    std::unordered_map<uint64_t, int> seenIds;
    std::unordered_map<int, int> seenZ;
    bool ok = !snapshot.nodes.empty();
    for (const auto& node : snapshot.nodes) {
        if (node.nodeId == 0 || node.generation == 0) ok = false;
        if (node.hwnd != nullptr) ok = false;
        if (++seenIds[node.nodeId] > 1) ok = false;
        if (++seenZ[node.zIndex] > 1) ok = false;
    }
    Check(ok, label);
}

// A private container is admitted geometrically, so the suite builds a real one: a
// class the registry does not know, a caption band it paints itself, and two panes
// separated by a thin strip.  The band has to arrive as chrome pixels and the strip
// as a draggable split that moves exactly those two panes.
LRESULT CALLBACK PaneContainerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_ERASEBKGND) {
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH brush = CreateSolidBrush(RGB(0x20, 0x40, 0x60));
        if (brush) {
            FillRect(reinterpret_cast<HDC>(wParam), &client, brush);
            DeleteObject(brush);
        }
        return 1;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// An accessible island: a host window whose content owns no HWND at all.  The suite
// builds a real one out of an ordinary Static, because a Static answers accessibility
// for itself and exposes no accessible children, which is exactly the shape that has
// to be refused.  MMC's DirectUI Actions pane is the admitted case; the rules that
// decide between them are what this covers.
void TestAccessibleIslandBoundary() {
    HWND root = CreateWindowExW(0, L"Static", L"island-host", WS_OVERLAPPEDWINDOW,
        40, 40, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND leaf = root ? CreateWindowExW(0, L"Static", L"", WS_CHILD | WS_VISIBLE,
        0, 0, 200, 40, root, reinterpret_cast<HMENU>(720),
        GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(root && leaf, "accessible island test controls were not created");
    if (!root || !leaf) {
        if (root) DestroyWindow(root);
        return;
    }
    ShowWindow(root, SW_SHOWNOACTIVATE);

    // The host class list is closed: DirectUI's window class belongs to Windows, so it
    // is named, and an application class is not.
    Check(Translation::IsAccessibleIslandClass(L"DirectUIHWND"),
        "the DirectUI host class is not recognized as an island host");
    Check(Translation::IsAccessibleIslandClass(L"directuihwnd"),
        "island host class matching is case sensitive");
    Check(!Translation::IsAccessibleIslandClass(L"AfxWnd42u") &&
          !Translation::IsAccessibleIslandClass(L"Static"),
        "an application class was treated as an island host");

    // A window whose accessible object exposes no children is not an island: there is
    // nothing to project, and admitting it would produce an empty surface.
    std::vector<Translation::AccessibleIslandItem> items;
    std::wstring reason;
    const bool admitted = Translation::ReadAccessibleIslandItems(leaf, items, reason);
    if (admitted) std::wcerr << L"childless island produced " << items.size() << L" items\n";
    Check(!admitted && items.empty() &&
          reason.find(L"not a container") != std::wstring::npos,
        "a window that is not an element container was admitted as an accessible island");

    // A destroyed window is refused rather than crashing the capture.
    HWND gone = CreateWindowExW(0, L"Static", L"", WS_CHILD, 0, 0, 10, 10, root,
        nullptr, GetModuleHandleW(nullptr), nullptr);
    if (gone) DestroyWindow(gone);
    reason.clear();
    Check(!Translation::ReadAccessibleIslandItems(gone, items, reason),
        "a destroyed island window was admitted");
    reason.clear();
    Check(!Translation::InvokeAccessibleIslandItem(leaf, 0, L"Anything", L"Act", reason),
        "an island action ran against a window with no elements");

    // The accessible name of an internal child is what lets an icon-only toolbar
    // button project; a window that publishes none reports an empty string rather than
    // inventing one.
    Check(Translation::AccessibleChildName(leaf, 0).empty() &&
          Translation::AccessibleChildName(nullptr, 1).empty(),
        "an accessible child name was invented for a window that has none");
    DestroyWindow(root);
}

void TestPaneContainerCaptureAndSplit() {
    WNDCLASSEXW containerClass{};
    containerClass.cbSize = sizeof(containerClass);
    containerClass.lpfnWndProc = PaneContainerProc;
    containerClass.hInstance = GetModuleHandleW(nullptr);
    containerClass.lpszClassName = L"FluentShellTestPaneHost";
    RegisterClassExW(&containerClass);

    HWND root = CreateWindowExW(0, L"Static", L"pane-host", WS_OVERLAPPEDWINDOW,
        30, 30, 360, 280, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND container = root ? CreateWindowExW(0, L"FluentShellTestPaneHost", L"",
        WS_CHILD | WS_VISIBLE, 0, 0, 300, 200, root, reinterpret_cast<HMENU>(700),
        GetModuleHandleW(nullptr), nullptr) : nullptr;
    // A 30 px caption band at the top is chrome; the panes tile everything below it.
    HWND left = container ? CreateWindowExW(0, L"Static", L"left", WS_CHILD | WS_VISIBLE,
        0, 30, 148, 170, container, reinterpret_cast<HMENU>(701),
        GetModuleHandleW(nullptr), nullptr) : nullptr;
    HWND right = container ? CreateWindowExW(0, L"Static", L"right", WS_CHILD | WS_VISIBLE,
        151, 30, 149, 170, container, reinterpret_cast<HMENU>(702),
        GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(root && container && left && right, "pane container test controls were not created");
    if (!root || !container || !left || !right) {
        if (root) DestroyWindow(root);
        return;
    }
    ShowWindow(root, SW_SHOWNOACTIVATE);
    UpdateWindow(root);

    Translation::ControlKind kind{};
    std::wstring reason;
    Check(Translation::ClassifyControl(container, kind, reason) &&
          kind == Translation::ControlKind::PaneContainer,
        "a container whose children tile it was not admitted");
    if (kind != Translation::ControlKind::PaneContainer) {
        std::wcerr << L"pane container rejection: " << reason << L'\n';
    }

    Translation::ControlNode node;
    node.kind = Translation::ControlKind::PaneContainer;
    reason.clear();
    Check(Translation::CaptureControlDetail(container, node, reason),
        "container pane state was not captured");
    Check(node.splits.size() == 1 && node.splits[0].vertical &&
          node.splits[0].position == 148 && node.splits[0].thickness == 3 &&
          node.splits[0].minimum < node.splits[0].maximum,
        "the strip between two panes was not captured as one draggable split");
    Check(node.chromeRegions.size() == 1 &&
          node.chromeRegions[0].rect.top == 0 && node.chromeRegions[0].rect.bottom == 30 &&
          node.chromeRegions[0].imageWidth == 300 && node.chromeRegions[0].imageHeight == 30 &&
          node.chromeRegions[0].imageFormat == L"bgra8-premultiplied" &&
          node.chromeRegions[0].imageData.size() == 300 * 30 * 4,
        "the band the container paints itself was not captured as bounded pixels");
    if (node.splits.size() != 1 || node.chromeRegions.size() != 1) {
        std::wcerr << L"splits=" << node.splits.size()
            << L" chrome=" << node.chromeRegions.size() << L'\n';
    }

    // Moving the split resizes exactly the two panes it divides.
    Check(Translation::SetPaneSplit(container, 0, 100),
        "a projected split move was refused");
    RECT leftRect{};
    RECT rightRect{};
    GetWindowRect(left, &leftRect);
    GetWindowRect(right, &rightRect);
    MapWindowPoints(nullptr, container, reinterpret_cast<POINT*>(&leftRect), 2);
    MapWindowPoints(nullptr, container, reinterpret_cast<POINT*>(&rightRect), 2);
    Check(leftRect.right == 100 && rightRect.left == 103 &&
          leftRect.left == 0 && rightRect.right == 300,
        "a split move did not land on exactly the two panes it divides");

    // The range is bounded by the two panes, and a position outside it is refused
    // rather than collapsing a pane.
    Check(!Translation::SetPaneSplit(container, 0, 0) &&
          !Translation::SetPaneSplit(container, 0, 299) &&
          !Translation::SetPaneSplit(container, 3, 120),
        "an out-of-range split move was accepted");

    // A container keeps its own proportion in private data that no message writes, so a
    // split the user asked for is expressed again against the new extent after the
    // application re-lays the container out.  The proportion is what carries, and the
    // range the container currently offers is what bounds it.
    Check(Translation::ProportionalSplitTarget(100, 300, 600, 24, 573) == 200 &&
          Translation::ProportionalSplitTarget(100, 300, 300, 24, 273) == 100 &&
          Translation::ProportionalSplitTarget(290, 300, 60, 24, 33) == 33 &&
          Translation::ProportionalSplitTarget(10, 300, 600, 24, 573) == 24,
        "a remembered split did not scale with the container or stay inside its range");
    // A degenerate measurement carries no proportion, so nothing is asserted from it.
    Check(Translation::ProportionalSplitTarget(100, 0, 600, 24, 573) == 100 &&
          Translation::ProportionalSplitTarget(100, 300, 0, 24, 573) == 100 &&
          Translation::ProportionalSplitTarget(100, 300, 600, 573, 24) == 100,
        "a split target was invented from a degenerate measurement");

    // A container that paints more than the caps allow is refused, with the offending
    // rectangle as evidence: that is the line between a frame and a custom control.
    // Widening the container leaves a band wider than the protocol cap.
    MoveWindow(container, 0, 0, 1100, 200, TRUE);
    reason.clear();
    Check(!Translation::ClassifyControl(container, kind, reason) &&
          reason.find(L"paints more than") != std::wstring::npos,
        "a container with an oversized painted band was accepted");
    if (reason.find(L"paints more than") == std::wstring::npos) {
        std::wcerr << L"oversized container reason: " << reason << L'\n';
    }
    DestroyWindow(root);
}

void TestVirtualDialogSnapshots() {
    std::unordered_map<uint64_t, int> results;
    const auto messageBox = Translation::BuildMessageBoxSnapshot(
        nullptr, L"The call was answered by the projection.", L"Oracle",
        MB_YESNOCANCEL | MB_ICONEXCLAMATION, results);
    Check(messageBox.surfaceKind == Translation::SurfaceKind::MessageBox &&
          messageBox.icon == L"warning" && messageBox.canCancel &&
          messageBox.nodes.size() == 4 && results.size() == 3,
        "MessageBox translation did not describe its buttons");
    CheckVirtualDialogSnapshot(messageBox, "translated MessageBox broke a protocol invariant");

    // Every text block, the verification checkbox, and every button share one
    // snapshot, which is where duplicate z-indexes came from.
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_VERIFICATION_FLAG_CHECKED | TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.nDefaultButton = IDOK;
    const std::vector<std::pair<int, std::wstring>> buttons{
        { IDOK, L"OK" }, { IDCANCEL, L"Cancel" }
    };
    std::unordered_map<uint64_t, int> dialogResults;
    std::optional<uint64_t> verificationNode;
    const auto taskDialog = Translation::BuildTaskDialogSnapshot(
        config, buttons, L"Oracle", L"Main instruction", L"Body content",
        L"Footer", L"Do not ask again", dialogResults, verificationNode);
    Check(taskDialog.surfaceKind == Translation::SurfaceKind::TaskDialog &&
          taskDialog.icon == L"info" && taskDialog.canCancel &&
          taskDialog.nodes.size() == 6 && dialogResults.size() == 2 &&
          verificationNode.has_value(),
        "TaskDialog translation did not describe its content");
    CheckVirtualDialogSnapshot(taskDialog, "translated TaskDialog broke a protocol invariant");

    // An empty text block is skipped rather than projected as a blank node, and the
    // remaining nodes still form one contiguous, unique z-order.
    TASKDIALOGCONFIG spare{};
    spare.cbSize = sizeof(spare);
    std::unordered_map<uint64_t, int> spareResults;
    std::optional<uint64_t> spareVerification;
    const auto minimal = Translation::BuildTaskDialogSnapshot(
        spare, { { IDOK, L"OK" } }, L"", L"Only an instruction", L"", L"", L"",
        spareResults, spareVerification);
    Check(minimal.nodes.size() == 2 && !spareVerification.has_value() &&
          minimal.title == L"Dialog" && minimal.icon == L"none",
        "a sparse TaskDialog was not reduced to its real content");
    CheckVirtualDialogSnapshot(minimal, "sparse TaskDialog broke a protocol invariant");
}

void TestToolbarCaptureBoundary() {
    HWND window = CreateWindowExW(0, L"Static", L"toolbar-capture", WS_OVERLAPPEDWINDOW,
        20, 20, 480, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    constexpr DWORD toolbarStyle = WS_CHILD | WS_VISIBLE | CCS_TOP | TBSTYLE_TOOLTIPS |
        TBSTYLE_WRAPABLE | TBSTYLE_FLAT | TBSTYLE_TRANSPARENT;
    HWND toolbar = window ? CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr, toolbarStyle,
        0, 0, 440, 40, window, reinterpret_cast<HMENU>(500), GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(window != nullptr && toolbar != nullptr, "ToolbarWindow32 capture controls were not created");
    if (!window || !toolbar) {
        if (window) DestroyWindow(window);
        return;
    }
    SendMessageW(toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    HIMAGELIST images = ImageList_Create(32, 32, ILC_COLOR32 | ILC_MASK, 1, 1);
    HICON sourceIcon = LoadIconW(nullptr, IDI_APPLICATION);
    Check(images != nullptr && sourceIcon != nullptr && ImageList_AddIcon(images, sourceIcon) == 0,
        "ToolbarWindow32 normal image list was not created");
    SendMessageW(toolbar, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images));
    wchar_t openLabel[] = L"Open";
    wchar_t saveLabel[] = L"Save";
    wchar_t hiddenLabel[] = L"Hidden";
    wchar_t latchLabel[] = L"Latch";
    TBBUTTON buttons[5]{};
    buttons[0].iBitmap = 0;
    buttons[0].idCommand = 100;
    // TBSTATE_PRESSED is the transient look of a button under the pointer and
    // TBSTATE_MARKED an application highlight the control paints; neither changes what
    // the button does, so neither may refuse the window.
    buttons[0].fsState = TBSTATE_ENABLED | TBSTATE_PRESSED | TBSTATE_MARKED;
    buttons[0].fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
    buttons[0].iString = reinterpret_cast<INT_PTR>(openLabel);
    buttons[1].iBitmap = 8;
    buttons[1].fsState = TBSTATE_ENABLED;
    buttons[1].fsStyle = BTNS_SEP;
    buttons[2].iBitmap = 0;
    buttons[2].idCommand = 101;
    buttons[2].fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
    buttons[2].iString = reinterpret_cast<INT_PTR>(saveLabel);
    buttons[3].iBitmap = 0;
    buttons[3].idCommand = 102;
    buttons[3].fsState = TBSTATE_ENABLED | TBSTATE_HIDDEN;
    buttons[3].fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
    buttons[3].iString = reinterpret_cast<INT_PTR>(hiddenLabel);
    // A latched button whose image index the list does not own: the control draws that
    // face itself, so the projection reproduces the pixels it drew.
    buttons[4].iBitmap = 9;
    buttons[4].idCommand = 103;
    buttons[4].fsState = TBSTATE_ENABLED | TBSTATE_CHECKED;
    buttons[4].fsStyle = BTNS_CHECK | BTNS_AUTOSIZE;
    buttons[4].iString = reinterpret_cast<INT_PTR>(latchLabel);
    Check(SendMessageW(toolbar, TB_ADDBUTTONSW, 5, reinterpret_cast<LPARAM>(buttons)) != FALSE,
        "ToolbarWindow32 buttons were not added");
    SendMessageW(toolbar, TB_AUTOSIZE, 0, 0);
    ShowWindow(window, SW_SHOWNOACTIVATE);

    Translation::CaptureContext context;
    context.surfaceId = L"11111111-2222-3333-4444-555555555555";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(window, context, snapshot, error);
    if (!captured) std::wcerr << L"toolbar capture rejection: " << error << L'\n';
    Check(captured, "bounded ToolbarWindow32 was rejected");
    const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& node) { return node.kind == Translation::ControlKind::Toolbar; });
    Check(found != snapshot.nodes.end(), "ToolbarWindow32 node was not captured");
    if (found != snapshot.nodes.end()) {
        Check(found->toolbarItems.size() == 5 &&
              found->toolbarItems[0].text == L"Open" &&
              !found->toolbarItems[2].enabled && found->toolbarItems[3].hidden &&
              found->toolbarItems[0].imageFormat == L"bgra8-premultiplied" &&
              !found->toolbarItems[0].imageData.empty(),
            "ToolbarWindow32 typed items were not captured");
        // The latched button keeps the state the control owns.  Its image index is not
        // in the list, but it carries its own label, so the label is its face and no
        // cropped bitmap of that text travels with it.
        Check(found->toolbarItems[4].kind == Translation::ToolbarItemKind::ToggleButton &&
              found->toolbarItems[4].checked &&
              found->toolbarItems[4].text == L"Latch" &&
              !found->toolbarItems[4].paintedFace &&
              found->toolbarItems[4].imageData.empty(),
            "ToolbarWindow32 latched button state or its label-only face was not captured");
        Translation::ActionRequest action;
        action.action = L"toolbarCommand";
        action.nodeId = found->nodeId;
        action.menuCommandId = 100;
        Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
            "enabled ToolbarWindow32 command was rejected");
        action.menuCommandId = 101;
        Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
            "disabled ToolbarWindow32 command was accepted");
        const auto json = Translation::SerializeWindowOpen(
            L"00112233445566778899aabbccddeeff", snapshot);
        Check(json.find("\"kind\":\"toolbar\"") != std::string::npos &&
              json.find("\"toolbarItems\":[") != std::string::npos &&
              json.find("\"commandId\":100") != std::string::npos,
            "ToolbarWindow32 typed items were not serialized");
        const auto fingerprint = Translation::SnapshotFingerprint(snapshot);
        snapshot.nodes[static_cast<size_t>(found - snapshot.nodes.begin())].toolbarItems[0].enabled = false;
        Check(fingerprint != Translation::SnapshotFingerprint(snapshot),
            "ToolbarWindow32 item state was omitted from the fingerprint");
    }
    DestroyWindow(window);
    if (images) ImageList_Destroy(images);
}

HTREEITEM InsertTreeItem(HWND tree, HTREEITEM parent, const wchar_t* text) {
    TVINSERTSTRUCTW insert{};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT;
    insert.item.pszText = const_cast<LPWSTR>(text);
    return reinterpret_cast<HTREEITEM>(
        SendMessageW(tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&insert)));
}

void TestTreeViewCaptureAndExpansion() {
    HWND window = CreateWindowExW(0, L"Static", L"tree-view", WS_OVERLAPPEDWINDOW,
        20, 20, 420, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    constexpr DWORD treeStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_EDITLABELS;
    HWND tree = window ? CreateWindowExW(0, WC_TREEVIEWW, L"", treeStyle,
        10, 10, 380, 260, window, reinterpret_cast<HMENU>(460),
        GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(window && tree, "TreeView capture controls were not created");
    if (!window || !tree) {
        if (window) DestroyWindow(window);
        return;
    }
    SetWindowSubclass(window, ListViewNotificationSubclass, 4, 0);
    // A two-icon image list proves per-item imagery survives the boundary and that
    // the selected-state index is captured separately.
    HIMAGELIST icons = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 2, 0);
    if (icons) {
        if (HICON app = LoadIconW(nullptr, IDI_APPLICATION)) ImageList_AddIcon(icons, app);
        if (HICON info = LoadIconW(nullptr, IDI_INFORMATION)) ImageList_AddIcon(icons, info);
        SendMessageW(tree, TVM_SETIMAGELIST, TVSIL_NORMAL, reinterpret_cast<LPARAM>(icons));
    }

    // Console Root > (Services > Local, Event Viewer): one collapsed parent, one
    // expanded parent, and two leaves, so depth, expansion, and child evidence
    // are all exercised by one flattened order.
    const HTREEITEM root = InsertTreeItem(tree, TVI_ROOT, L"Console Root");
    const HTREEITEM services = InsertTreeItem(tree, root, L"Services");
    const HTREEITEM local = InsertTreeItem(tree, services, L"Local");
    const HTREEITEM events = InsertTreeItem(tree, root, L"Event Viewer");
    Check(root && services && local && events, "TreeView items were not inserted");
    // Assign the selected-state icon after insertion too: that is the common
    // application pattern, and the capture must read the stored value rather than
    // whatever was passed to TVM_INSERTITEM.
    for (const HTREEITEM item : { root, services, local, events }) {
        if (!item) continue;
        TVITEMW selectedImage{};
        selectedImage.mask = TVIF_HANDLE | TVIF_SELECTEDIMAGE;
        selectedImage.hItem = item;
        selectedImage.iSelectedImage = 1;
        SendMessageW(tree, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&selectedImage));
    }
    SendMessageW(tree, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(root));
    SendMessageW(tree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(services));
    ShowWindow(window, SW_SHOWNOACTIVATE);

    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-666666666666";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(window, context, snapshot, error);
    if (!captured) std::wcerr << L"TreeView capture rejection: " << error << L'\n';
    Check(captured, "bounded textual TreeView was rejected");
    const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& candidate) {
            return candidate.kind == Translation::ControlKind::TreeView;
        });
    Check(node != snapshot.nodes.end(), "TreeView node was not captured");
    if (node != snapshot.nodes.end()) {
        const std::vector<std::wstring> expectedItems{
            L"Console Root", L"Services", L"Local", L"Event Viewer" };
        Check(node->items == expectedItems,
            "TreeView items were not flattened in native depth-first order");
        Check(node->itemDepths == std::vector<int>{ 0, 1, 2, 1 },
            "TreeView item depths do not describe the native hierarchy");
        Check(node->itemExpanded == std::vector<bool>{ true, false, false, false },
            "TreeView expansion state was not captured");
        Check(node->itemHasChildren == std::vector<bool>{ true, true, false, false },
            "TreeView child evidence was not captured");
        Check(node->imageList.size() == 2 &&
              node->imageList[0].imageWidth == 16 && node->imageList[0].imageHeight == 16 &&
              node->imageList[0].imageFormat == L"bgra8-premultiplied" &&
              node->imageList[0].imageData.size() == 16 * 16 * 4,
            "TreeView image list was not captured as bounded owned pixels");
        Check(node->itemImages == std::vector<int>{ 0, 0, 0, 0 } &&
              node->itemSelectedImages == std::vector<int>{ 1, 1, 1, 1 },
            "TreeView per-item image indexes were not captured");
        if (node->itemImages != std::vector<int>{ 0, 0, 0, 0 } ||
            node->itemSelectedImages != std::vector<int>{ 1, 1, 1, 1 }) {
            std::wcerr << L"actual tree item images:";
            for (size_t index = 0; index < node->itemImages.size(); ++index) {
                std::wcerr << L' ' << node->itemImages[index] << L'/'
                    << (index < node->itemSelectedImages.size()
                        ? node->itemSelectedImages[index] : -99);
            }
            std::wcerr << L" imageList=" << node->imageList.size() << L'\n';
        }
        Check(node->editableLabels && node->editingIndex == -1,
            "TreeView label editing state was not captured");
        Check(node->selectedIndex == 1 && !node->multiSelect,
            "TreeView selection was not captured as a single flattened index");
        Check(node->text.empty() && node->automationName.empty(),
            "TreeView duplicated its items in the window text");
        const auto json = Translation::SerializeWindowOpen(
            L"00112233445566778899aabbccddeeff", snapshot);
        Check(json.find("\"kind\":\"treeView\"") != std::string::npos &&
              json.find("\"itemDepths\":[0,1,2,1]") != std::string::npos &&
              json.find("\"itemHasChildren\":[") != std::string::npos,
            "TreeView hierarchy was not serialized");
        const auto fingerprint = Translation::SnapshotFingerprint(snapshot);
        const auto index = static_cast<size_t>(node - snapshot.nodes.begin());
        snapshot.nodes[index].itemHasChildren[2] = true;
        Check(fingerprint != Translation::SnapshotFingerprint(snapshot),
            "TreeView child evidence was omitted from the fingerprint");
    }

    // Every index-addressed action resolves through the same flattened order.
    Check(Translation::SelectTreeViewItem(tree, 3) &&
          reinterpret_cast<HTREEITEM>(
              SendMessageW(tree, TVM_GETNEXTITEM, TVGN_CARET, 0)) == events,
        "TreeView selection did not follow the flattened index");
    Check(!Translation::SelectTreeViewItem(tree, 4),
        "TreeView selection accepted an index outside its items");
    Check(Translation::SetTreeViewItemExpanded(tree, 1, true),
        "TreeView parent expansion was rejected");
    Check(Translation::SetTreeViewItemExpanded(tree, 1, false),
        "TreeView parent collapse was rejected");
    Check(!Translation::SetTreeViewItemExpanded(tree, 2, true),
        "TreeView leaf reported a successful expansion");

    // A rename runs the control's own label session, so the application decides
    // whether the new text is kept.
    g_labelEditsBegun = g_labelEditsEnded = 0;
    g_acceptLabelEdit = true;
    Check(Translation::RenameTreeViewItem(tree, 3, L"Renamed leaf") &&
          g_labelEditsBegun == 1 && g_labelEditsEnded == 1,
        "an accepted TreeView rename did not run the native label session");
    g_acceptLabelEdit = false;
    Check(!Translation::RenameTreeViewItem(tree, 3, L"Refused leaf") &&
          g_labelEditsBegun == 2 && g_labelEditsEnded == 2,
        "a refused TreeView rename was reported as applied");
    g_acceptLabelEdit = true;
    Check(!Translation::RenameTreeViewItem(tree, 9, L"Out of range"),
        "a rename outside the item range was accepted");

    Translation::ControlKind kind{};
    std::wstring reason;
    HIMAGELIST states = ImageList_Create(16, 16, ILC_COLOR32, 1, 1);
    SendMessageW(tree, TVM_SETIMAGELIST, TVSIL_STATE,
        reinterpret_cast<LPARAM>(states));
    reason.clear();
    Check(states && !Translation::ClassifyControl(tree, kind, reason) &&
          reason.find(L"state image list") != std::wstring::npos,
        "a TreeView state image list was accepted");
    SendMessageW(tree, TVM_SETIMAGELIST, TVSIL_STATE, 0);
    if (states) ImageList_Destroy(states);
    reason.clear();
    Check(Translation::ClassifyControl(tree, kind, reason) &&
          kind == Translation::ControlKind::TreeView,
        "an icon-bearing TreeView was rejected");

    SetWindowLongPtrW(tree, GWL_STYLE, treeStyle | TVS_TRACKSELECT);
    reason.clear();
    Check(!Translation::ClassifyControl(tree, kind, reason) &&
          reason.find(L"TVS_TRACKSELECT") != std::wstring::npos &&
          reason.find(L"0x") != std::wstring::npos,
        "unsupported TreeView style lacked exact named diagnostics");
    SetWindowLongPtrW(tree, GWL_STYLE, treeStyle);

    Translation::ControlNode detail;
    detail.kind = Translation::ControlKind::TreeView;
    detail.style = treeStyle;
    reason.clear();
    Check(Translation::CaptureControlDetail(tree, detail, reason),
        "TreeView detail capture was rejected");
    TVITEMW blank{};
    blank.mask = TVIF_HANDLE | TVIF_TEXT;
    blank.hItem = local;
    blank.pszText = const_cast<LPWSTR>(L"");
    SendMessageW(tree, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&blank));
    reason.clear();
    Check(!Translation::CaptureControlDetail(tree, detail, reason) &&
          reason.find(L"nonempty") != std::wstring::npos,
        "TreeView accepted an item with no label");
    DestroyWindow(window);
    if (icons) ImageList_Destroy(icons);
}

void TestTrackbarCaptureAndValue() {
    HWND window = CreateWindowExW(0, L"Static", L"trackbar", WS_OVERLAPPEDWINDOW,
        20, 20, 360, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    constexpr DWORD trackbarStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_AUTOTICKS;
    HWND trackbar = window ? CreateWindowExW(0, TRACKBAR_CLASSW, L"", trackbarStyle,
        10, 10, 300, 40, window, reinterpret_cast<HMENU>(470),
        GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(window && trackbar, "Trackbar capture controls were not created");
    if (!window || !trackbar) {
        if (window) DestroyWindow(window);
        return;
    }
    SetWindowSubclass(window, ListViewNotificationSubclass, 3, 0);
    SendMessageW(trackbar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 20));
    SendMessageW(trackbar, TBM_SETLINESIZE, 0, 2);
    SendMessageW(trackbar, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(trackbar, TBM_SETPOS, TRUE, 7);
    ShowWindow(window, SW_SHOWNOACTIVATE);

    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-777777777777";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(window, context, snapshot, error);
    if (!captured) std::wcerr << L"Trackbar capture rejection: " << error << L'\n';
    Check(captured, "bounded Trackbar was rejected");
    const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& candidate) {
            return candidate.kind == Translation::ControlKind::Slider;
        });
    Check(node != snapshot.nodes.end(), "Trackbar node was not captured");
    if (node != snapshot.nodes.end()) {
        Check(node->minimum == 0 && node->maximum == 20 && node->position == 7,
            "Trackbar range or position was not captured");
        Check(node->smallChange == 2 && node->largeChange == 5,
            "Trackbar line or page size was not captured");
        Check(!node->vertical && !node->reversed,
            "horizontal Trackbar reported vertical or reversed geometry");
        const auto json = Translation::SerializeWindowOpen(
            L"00112233445566778899aabbccddeeff", snapshot);
        Check(json.find("\"kind\":\"slider\"") != std::string::npos &&
              json.find("\"smallChange\":2") != std::string::npos &&
              json.find("\"largeChange\":5") != std::string::npos,
            "Trackbar state was not serialized");
        const auto fingerprint = Translation::SnapshotFingerprint(snapshot);
        snapshot.nodes[static_cast<size_t>(node - snapshot.nodes.begin())].position = 8;
        Check(fingerprint != Translation::SnapshotFingerprint(snapshot),
            "Trackbar position was omitted from the fingerprint");
    }

    // The application observes a projected move exactly as it observes a drag:
    // through the trackbar's own WM_HSCROLL to its parent.
    g_scrollNotifications.clear();
    Check(Translation::SetTrackbarPosition(window, trackbar, false, 12) &&
          static_cast<int>(SendMessageW(trackbar, TBM_GETPOS, 0, 0)) == 12 &&
          !g_scrollNotifications.empty(),
        "Trackbar move did not reach the parent as a scroll notification");
    g_scrollNotifications.clear();
    Check(Translation::SetTrackbarPosition(window, trackbar, false, 12) &&
          g_scrollNotifications.empty(),
        "Trackbar re-notified the parent for an unchanged position");

    Translation::ControlKind kind{};
    std::wstring reason;
    SetWindowLongPtrW(trackbar, GWL_STYLE, trackbarStyle | TBS_ENABLESELRANGE);
    reason.clear();
    Check(!Translation::ClassifyControl(trackbar, kind, reason) &&
          reason.find(L"TBS_ENABLESELRANGE") != std::wstring::npos &&
          reason.find(L"0x") != std::wstring::npos,
        "unsupported Trackbar style lacked exact named diagnostics");
    SetWindowLongPtrW(trackbar, GWL_STYLE, trackbarStyle);

    Translation::ControlNode detail;
    detail.kind = Translation::ControlKind::Slider;
    detail.style = trackbarStyle;
    SendMessageW(trackbar, TBM_SETRANGE, TRUE, MAKELPARAM(5, 5));
    reason.clear();
    Check(!Translation::CaptureControlDetail(trackbar, detail, reason) &&
          reason.find(L"invalid native range") != std::wstring::npos,
        "Trackbar with an empty range was accepted");
    DestroyWindow(window);
}

LRESULT CALLBACK TestMdiChildProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefMDIChildProc(window, message, wParam, lParam);
}

HWND g_testMdiClient = nullptr;

LRESULT CALLBACK TestMdiFrameProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_SIZE && g_testMdiClient) {
        RECT client{};
        GetClientRect(window, &client);
        MoveWindow(g_testMdiClient, 0, 0, client.right, client.bottom, TRUE);
        return 0;
    }
    return DefFrameProcW(window, g_testMdiClient, message, wParam, lParam);
}

void TestMdiFrameCaptureAndCommands() {
    WNDCLASSEXW childClass{ sizeof(childClass) };
    childClass.lpfnWndProc = TestMdiChildProc;
    childClass.hInstance = GetModuleHandleW(nullptr);
    childClass.lpszClassName = L"FluentShell.Test.MdiChild";
    RegisterClassExW(&childClass);
    WNDCLASSEXW frameClass{ sizeof(frameClass) };
    frameClass.lpfnWndProc = TestMdiFrameProc;
    frameClass.hInstance = GetModuleHandleW(nullptr);
    frameClass.lpszClassName = L"FluentShell.Test.MdiFrame";
    RegisterClassExW(&frameClass);

    HWND frame = CreateWindowExW(0, frameClass.lpszClassName, L"mdi-frame",
        WS_OVERLAPPEDWINDOW, 20, 20, 640, 480, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    Check(frame != nullptr, "MDI frame was not created");
    if (!frame) return;
    CLIENTCREATESTRUCT clientCreate{};
    clientCreate.idFirstChild = 40000;
    g_testMdiClient = CreateWindowExW(0, L"MDIClient", nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 620, 440, frame,
        reinterpret_cast<HMENU>(0xC0C), GetModuleHandleW(nullptr), &clientCreate);
    Check(g_testMdiClient != nullptr, "MDI client was not created");
    if (!g_testMdiClient) {
        DestroyWindow(frame);
        return;
    }
    const auto createChild = [](const wchar_t* title, int offset) {
        MDICREATESTRUCTW create{};
        create.szClass = L"FluentShell.Test.MdiChild";
        create.szTitle = title;
        create.hOwner = GetModuleHandleW(nullptr);
        create.x = 10 + offset;
        create.y = 10 + offset;
        create.cx = 280;
        create.cy = 160;
        return reinterpret_cast<HWND>(SendMessageW(
            g_testMdiClient, WM_MDICREATE, 0, reinterpret_cast<LPARAM>(&create)));
    };
    const HWND first = createChild(L"Document 1", 0);
    const HWND second = createChild(L"Document 2", 30);
    Check(first && second, "MDI children were not created");
    ShowWindow(frame, SW_SHOWNOACTIVATE);

    using Translation::ControlKind;
    ControlKind kind{};
    std::wstring reason;
    Check(Translation::ClassifyControl(g_testMdiClient, kind, reason) &&
          kind == ControlKind::MdiClient,
        "MDIClient was not classified as a projected container");
    kind = ControlKind::Count;
    reason.clear();
    // The MDI child's class is the application's; its role is the frame's.
    Check(second && Translation::ClassifyControl(second, kind, reason) &&
          kind == ControlKind::MdiChild,
        "WS_EX_MDICHILD frame was not classified by its window-manager role");

    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-888888888888";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(frame, context, snapshot, error);
    if (!captured) std::wcerr << L"MDI capture rejection: " << error << L'\n';
    Check(captured, "MDI frame was rejected");
    const auto clientNode = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& node) {
            return node.kind == ControlKind::MdiClient;
        });
    Check(clientNode != snapshot.nodes.end() && !clientNode->parentNodeId,
        "MDI client node is missing or nested");
    std::vector<const Translation::ControlNode*> children;
    for (const auto& node : snapshot.nodes) {
        if (node.kind == ControlKind::MdiChild) children.push_back(&node);
    }
    Check(children.size() == 2, "both MDI child frames were not captured");
    if (clientNode != snapshot.nodes.end() && children.size() == 2) {
        for (const auto* child : children) {
            Check(child->parentNodeId && *child->parentNodeId == clientNode->nodeId,
                "MDI child is not owned by the MDI client");
            Check(child->windowState == L"normal",
                "MDI child state was not captured as normal");
            Check(child->clientRect.left >= 0 && child->clientRect.top > 0 &&
                  child->clientRect.right - child->clientRect.left <=
                      child->rect.right - child->rect.left &&
                  child->clientRect.bottom - child->clientRect.top <=
                      child->rect.bottom - child->rect.top,
                "MDI child client band is not inside its frame");
        }
        const auto activeCount = std::count_if(children.begin(), children.end(),
            [](const Translation::ControlNode* child) { return child->active; });
        Check(activeCount == 1, "exactly one MDI child must report activation");
        const auto json = Translation::SerializeWindowOpen(
            L"00112233445566778899aabbccddeeff", snapshot);
        Check(json.find("\"kind\":\"mdiClient\"") != std::string::npos &&
              json.find("\"kind\":\"mdiChild\"") != std::string::npos &&
              json.find("\"windowState\":\"normal\"") != std::string::npos &&
              json.find("\"clientRect\"") != std::string::npos,
            "MDI frame state was not serialized");
        const auto fingerprint = Translation::SnapshotFingerprint(snapshot);
        const auto index = static_cast<size_t>(children[0] - snapshot.nodes.data());
        snapshot.nodes[index].active = !snapshot.nodes[index].active;
        Check(fingerprint != Translation::SnapshotFingerprint(snapshot),
            "MDI activation was omitted from the fingerprint");
        snapshot.nodes[index].active = !snapshot.nodes[index].active;

        // Every caption verb is admitted only against the child's own style and
        // state, so the projection can never offer one the frame would refuse.
        Translation::ActionRequest action;
        action.action = L"mdiCommand";
        action.nodeId = children[0]->nodeId;
        action.text = L"restore";
        Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
            "restore was accepted for an already restored MDI child");
        action.text = L"maximize";
        Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
            "maximize was rejected for a maximizable MDI child");
        action.text = L"activate";
        const bool firstIsActive = children[0]->active;
        Check(Translation::ValidateActionForSnapshot(action, snapshot, error) != firstIsActive,
            "activate must be refused only for the already active MDI child");
        action.text = L"teleport";
        Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
            "an unknown MDI verb was accepted");
        action.nodeId = clientNode->nodeId;
        action.text = L"close";
        Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
            "mdiCommand was accepted for the MDI client");
    }
    Check(Translation::IsRequestSemanticAction(L"mdiCommand"),
        "an MDI caption command must be rebased like other pointer requests");
    DestroyWindow(frame);
    g_testMdiClient = nullptr;
}

} // namespace

int wmain() {
    TestVisibleUiaBoundsClipping();
    TestDirectUiEvidenceContracts();
    TestDirectUiInPlaceRoutes();
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    INITCOMMONCONTROLSEX controls{ sizeof(controls),
        ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS };
    Check(InitCommonControlsEx(&controls) != FALSE,
        "common-control classes were not initialized");
    TestHeaderValidation();
    TestPipeRoundTrip();
    TestReadFrameUsesOneDeadline();
    TestWriteFrameUsesOneDeadline();
    TestScalarContracts();
    TestSharedFixtures();
    TestStrictMessageValidation();
    TestActionSemanticValidation();
    TestActionRevisionPolicy();
    TestErrorScopeParsing();
    TestExpandedControlSerialization();
    TestControlAdapterRegistry();
    TestEditableComboCaptureBoundary();
    TestStaticIconCaptureBoundary();
    TestAncestorEnabledTabOrderCapture();
    TestTabOrderRejectionSpecificity();
    TestStructuredCommonControlCapture();
    TestListViewCheckNotificationSemantics();
    TestTabControlCaptureAndSelection();
    TestTreeViewCaptureAndExpansion();
    TestTrackbarCaptureAndValue();
    TestMdiFrameCaptureAndCommands();
    TestStandardMenuCapture();
    TestMenuActionValidationAndSerialization();
    TestToolbarCaptureBoundary();
    TestVirtualDialogSnapshots();
    TestPaneContainerCaptureAndSplit();
    TestAccessibleIslandBoundary();
    if (g_failures != 0) {
        std::cerr << g_failures << " native protocol test(s) failed.\n";
        return 1;
    }
    std::cout << "Native protocol tests passed.\n";
    return 0;
}
