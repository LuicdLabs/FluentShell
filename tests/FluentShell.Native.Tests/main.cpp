#include "../../src/Bridge/Ipc/FrameCodec.h"
#include "../../src/Bridge/Translation/WindowSnapshot.h"
#include "../../src/Bridge/Translation/ControlAdapters.h"
#include "../../src/Bridge/Translation/WindowCapture.h"
#include "../../src/Bridge/Translation/SourceThreadAgent.h"
#include "../../src/Bridge/Translation/GuiThreadDispatch.h"
#include "../../src/Bridge/Translation/ListViewActivation.h"
#include "../../src/Bridge/Translation/UiAutomationGeometry.h"
#include "../../src/Bridge/Translation/DirectUiEngine.h"
#include "../../src/Bridge/Translation/DialogSnapshots.h"
#include "../../src/Bridge/Translation/RollbackEvent.h"
#include "../../src/Bridge/Translation/VirtualTaskDialog.h"
#include "../../src/Bridge/Translation/AccessibleIsland.h"
#include "../../src/Bridge/Translation/MenuBarCapture.h"
#include "../../src/Bridge/Translation/PopupMenuState.h"
#include "../../src/Bridge/Translation/PaintedPixel.h"
#include "../../src/Bridge/Translation/RichText.h"
#include "../../src/Bridge/Translation/PerformanceChart.h"
#include "../../src/Bridge/Translation/RootSuppression.h"
#include "ItemImageCaptureTests.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "MmcHtmlDocumentTests.h"
#include "ListViewModeRegressionTests.h"
#include "OwnerDataListViewTests.h"
#include "OwnerDataListViewActionTests.h"
#include "ToolbarRadioGroupTests.h"
#include "ToolbarImageVariantTests.h"
#include "OwnerDrawComboBoxTests.h"
#include "ListViewGroupTests.h"
#include "DirectUiVirtualImageTests.h"
#include "DirectUiDetailEvidenceTests.h"
#include "NativeDocumentTests.h"
#include "UiAutomationProjectionTests.h"

#include <winrt/base.h>
#include <objbase.h>
#include <oleacc.h>
#include <commctrl.h>
#include <crtdbg.h>
#include <dbghelp.h>
#include <dwmapi.h>
#include <prsht.h>
#include <richedit.h>
#include <UIAutomation.h>

#include <algorithm>
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

// A crashing test otherwise ends the process with no output at all, because no
// test prints on success. Print the faulting stack so the failing test is named.
LONG WINAPI ReportUnhandledException(EXCEPTION_POINTERS* exception) {
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    std::cerr << "CRASH: exception 0x" << std::hex
        << exception->ExceptionRecord->ExceptionCode << std::dec << '\n';
    CONTEXT context = *exception->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    for (int depth = 0; depth < 48 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process,
            GetCurrentThread(), &frame, &context, nullptr, SymFunctionTableAccess64,
            SymGetModuleBase64, nullptr) && frame.AddrPC.Offset != 0; ++depth) {
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line{ sizeof(line) };
        DWORD lineDisplacement = 0;
        std::cerr << "  #" << depth << ' ';
        if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) std::cerr << symbol->Name;
        else std::cerr << "0x" << std::hex << frame.AddrPC.Offset << std::dec;
        if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line))
            std::cerr << " (" << line.FileName << ':' << line.LineNumber << ')';
        std::cerr << '\n';
    }
    std::cerr.flush();
    return EXCEPTION_EXECUTE_HANDLER;
}

#include "GuiThreadDispatchTests.h"
#include "CrossThreadSurfaceTests.h"
#include "SnapshotDetailFingerprintTests.h"
#include "MdiCaptureRegressionTests.h"
#include "StaticDecorationRegressionTests.h"
#include "SiblingZOrderCaptureTests.h"
#include "MmcMessageViewTests.h"
#include "CaptureVisibilityTests.h"
#include "UpDownAdapterTests.h"
#include "DateTimePickerAdapterTests.h"
#include "ListBoxMultiSelectTests.h"
#include "PerformanceChartTests.h"

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

    // Capability-derived slots exercise the shared native/UIA contract without
    // admitting any executable-specific page or trusting its resource IDs.
    Translation::DirectUiSlot bracketSlots[3];
    bracketSlots[0].semanticKey = L"semantic.instruction";
    bracketSlots[0].uiaControlType = UIA_TextControlTypeId;
    bracketSlots[0].uiaClassName = L"Element";
    bracketSlots[0].pinUiaPatterns = true;
    for (size_t index = 1; index < std::size(bracketSlots); ++index) {
        auto& slot = bracketSlots[index];
        slot.semanticKey = index == 1 ? L"semantic.next" : L"semantic.cancel";
        slot.kind = Translation::ControlKind::Button;
        slot.virtualSource = false;
        slot.nativeClass = L"Button";
        slot.uiaControlType = UIA_ButtonControlTypeId;
        slot.uiaClassName = L"CCPushButton";
        slot.uiaFocusable = true;
        slot.uiaActionable = true;
        slot.pinUiaPatterns = true;
        slot.uiaPatternMask = Translation::DirectUiPatternInvoke;
        slot.action = Translation::DirectUiAction::HandoffClick;
        slot.cancel = index == 2;
    }
    Translation::DirectUiWindowProfile bracketProfile;
    bracketProfile.adapterId = L"microsoft.windows.directui.semantic.v1";
    bracketProfile.pageId = L"semantic-v1";
    bracketProfile.rootClass = L"NativeHWNDHost";
    bracketProfile.slots = bracketSlots;
    bracketProfile.slotCount = std::size(bracketSlots);
    const auto* profile = &bracketProfile;
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
        profile->adapterId, L"semantic.instruction");
    const uint64_t second = Translation::DirectUiSemanticNodeId(
        profile->adapterId, L"semantic.instruction");
    Check(first == second && first != Translation::DirectUiSemanticNodeId(
        profile->adapterId, L"semantic.content"),
        "DirectUI semantic identity is not stable and key-scoped");
    Check(first != Translation::DirectUiSemanticNodeId(
        L"test.other-adapter", L"semantic.instruction"),
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

    Translation::DirectUiUiaEvidence uia;
    uia.rootHwnd = before.root.hwnd;
    uia.directUiHwnd = before.directUi.hwnd;
    for (size_t index = 0; index < profile->slotCount; ++index) {
        const auto& slot = profile->slots[index];
        Translation::DirectUiSemanticEvidence semantic;
        semantic.semanticKey = slot.semanticKey;
        semantic.name = slot.virtualSource ? L"localized instruction" : before.slotWindows[index].text;
        semantic.className = slot.uiaClassName;
        semantic.frameworkId = slot.uiaFrameworkId;
        semantic.controlType = slot.uiaControlType;
        semantic.enabled = slot.uiaEnabled;
        semantic.focusable = slot.uiaFocusable;
        semantic.actionable = slot.uiaActionable;
        semantic.patternMask = slot.uiaPatternMask;
        semantic.bounds = slot.virtualSource ? RECT{ 10, 20, 310, 60 } : before.slotWindows[index].bounds;
        semantic.backingHwnd = slot.virtualSource ? nullptr : before.slotWindows[index].hwnd;
        uia.semantics.push_back(std::move(semantic));
    }
    Check(Translation::MatchDirectUiEvidence(*profile, before, uia, error),
        "capability-derived DirectUI UIA evidence was rejected");
    auto changedUia = uia;
    changedUia.semantics[0].controlType = UIA_PaneControlTypeId;
    Check(!Translation::MatchDirectUiEvidence(*profile, before, changedUia, error),
        "DirectUI accepted changed virtual provider role");
    changedUia = uia;
    changedUia.semantics[0].patternMask = Translation::DirectUiPatternInvoke;
    Check(!Translation::MatchDirectUiEvidence(*profile, before, changedUia, error),
        "DirectUI accepted new actions on a presentation-only virtual slot");
    changedUia = uia;
    changedUia.semantics.push_back(uia.semantics[0]);
    Check(!Translation::MatchDirectUiEvidence(*profile, before, changedUia, error),
        "DirectUI accepted duplicated virtual semantic identity");
    changedUia = uia;
    changedUia.semantics[1].bounds.right += 30;
    Check(!Translation::MatchDirectUiEvidence(*profile, before, changedUia, error),
        "DirectUI accepted native/UIA backing geometry mismatch");
    changedUia = uia;
    changedUia.semantics[1].patternMask = Translation::DirectUiPatternNone;
    Check(!Translation::MatchDirectUiEvidence(*profile, before, changedUia, error),
        "DirectUI accepted removal of a backing control's verified route");
    changedUia = uia;
    auto unexpected = uia.semantics[0];
    unexpected.semanticKey = L"unexpected";
    changedUia.semantics.push_back(std::move(unexpected));
    Check(!Translation::MatchDirectUiEvidence(*profile, before, changedUia, error),
        "DirectUI accepted a semantic outside the sealed capability contract");

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
    listView.items = { L"one", L"two" };
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
        { L"Static", SS_BITMAP, true, ControlKind::StaticBitmap, "bitmap Static" },
        { L"Static", SS_BITMAP | SS_NOTIFY, false, ControlKind::StaticBitmap, "clickable bitmap Static" },
        { L"Button", BS_PUSHBUTTON, true, ControlKind::Button, "push Button" },
        { L"Button", BS_AUTOCHECKBOX, true, ControlKind::CheckBox, "check Button" },
        { L"Button", BS_AUTO3STATE, true, ControlKind::ThreeState, "3-state Button" },
        { L"Button", BS_AUTORADIOBUTTON, true, ControlKind::RadioButton, "radio Button" },
        { L"Button", BS_GROUPBOX, true, ControlKind::GroupBox, "GroupBox" },
        // A group box never takes the focus: a template's WS_TABSTOP on one (MMC's Customize View)
        // is dropped from the traversal at capture, not a reason to keep the dialog native.
        { L"Button", BS_GROUPBOX | WS_TABSTOP, true, ControlKind::GroupBox,
          "tab-stop GroupBox" },
        { L"Button", BS_OWNERDRAW, true, ControlKind::Button, "owner-draw Button" },
        { L"Button", BS_PUSHBUTTON | BS_ICON, true, ControlKind::Button, "icon Button" },
        { L"Button", BS_PUSHBUTTON | BS_BITMAP, true, ControlKind::Button, "bitmap Button" },
        { L"Button", BS_AUTOCHECKBOX | BS_ICON, false, ControlKind::CheckBox, "icon check Button" },
        { L"Edit", 0, true, ControlKind::Edit, "Edit" },
        { L"Edit", ES_PASSWORD, true, ControlKind::Password, "password Edit" },
        { L"ComboBox", CBS_DROPDOWNLIST, true, ControlKind::ComboBox, "dropdown-list ComboBox" },
        { L"ComboBox", CBS_SIMPLE | CBS_HASSTRINGS, false, ControlKind::ComboBox,
          "simple ComboBox" },
        { L"ListBox", 0, true, ControlKind::ListBox, "ListBox" },
        { L"ListBox", LBS_EXTENDEDSEL, true, ControlKind::ListBox, "extended-selection ListBox" },
        { L"ListBox", LBS_MULTIPLESEL, true, ControlKind::ListBox, "multiple-selection ListBox" },
        { L"ListBox", LBS_MULTICOLUMN, false, ControlKind::ListBox, "multi-column ListBox" },
        { L"ListBox", LBS_OWNERDRAWFIXED, true, ControlKind::ListBox, "owner-draw ListBox" },
        { L"ListBox", LBS_NODATA | LBS_OWNERDRAWFIXED, false, ControlKind::ListBox, "virtual ListBox" },
        { PROGRESS_CLASSW, 0, true, ControlKind::ProgressBar, "ProgressBar" },
        { PROGRESS_CLASSW, PBS_MARQUEE, true, ControlKind::ProgressBar, "marquee ProgressBar" },
        { PROGRESS_CLASSW, PBS_VERTICAL, false, ControlKind::ProgressBar, "vertical ProgressBar" },
        { PROGRESS_CLASSW, WS_TABSTOP, true, ControlKind::ProgressBar, "tab-stop ProgressBar" },
        { STATUSCLASSNAMEW, 0, true, ControlKind::StatusBar, "StatusBar" },
        { STATUSCLASSNAMEW, WS_TABSTOP, false, ControlKind::StatusBar, "tab-stop StatusBar" },
        { WC_TREEVIEWW, TVS_HASBUTTONS | TVS_HASLINES, true, ControlKind::TreeView, "TreeView" },
        { WC_TREEVIEWW, TVS_CHECKBOXES, true, ControlKind::TreeView, "checkbox TreeView" },
        { WC_TREEVIEWW, TVS_EDITLABELS, true, ControlKind::TreeView, "label-editing TreeView" },
        { WC_TREEVIEWW, TVS_SINGLEEXPAND, true, ControlKind::TreeView, "single-expand TreeView" },
        { WC_TREEVIEWW, TVS_INFOTIP | TVS_TRACKSELECT, true, ControlKind::TreeView, "info-tip TreeView" },
        { TRACKBAR_CLASSW, TBS_AUTOTICKS, true, ControlKind::Slider, "Trackbar" },
        { TRACKBAR_CLASSW, TBS_VERT | TBS_BOTH, true, ControlKind::Slider, "vertical Trackbar" },
        { TRACKBAR_CLASSW, TBS_ENABLESELRANGE, false, ControlKind::Slider,
          "selection-range Trackbar" },
        { TRACKBAR_CLASSW, TBS_TOOLTIPS, true, ControlKind::Slider, "tooltip Trackbar" },
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

    // A Static that takes the focus and says nothing is a control its dialog subclassed and paints
    // (the palettes of the Color chooser): a text label could not carry its pixels or its clicks.
    {
        struct StaticFocusCase { DWORD style; const wchar_t* text; bool supported; const char* label; };
        const StaticFocusCase focusCases[] = {
            { SS_SIMPLE | WS_TABSTOP, L"", false, "tab-stop Static without text" },
            { SS_LEFT | WS_TABSTOP, L"", false, "tab-stop left Static without text" },
            { SS_SIMPLE | WS_TABSTOP, L"Label", true, "tab-stop Static with text" },
            { SS_SIMPLE, L"", true, "empty Static that is not a tab stop" },
        };
        for (const auto& focusCase : focusCases) {
            HWND child = CreateWindowExW(0, L"Static", focusCase.text, WS_CHILD | focusCase.style,
                0, 0, 80, 24, root, reinterpret_cast<HMENU>(303), GetModuleHandleW(nullptr), nullptr);
            Check(child != nullptr, focusCase.label);
            if (!child) continue;
            ControlKind kind = ControlKind::Count;
            std::wstring reason;
            Check(Translation::ClassifyControl(child, kind, reason) == focusCase.supported,
                focusCase.label);
            if (!focusCase.supported) {
                Check(reason.find(L"custom-drawn") != std::wstring::npos, focusCase.label);
            }
            DestroyWindow(child);
        }
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
    // Without rows a column-less list paints nothing and is carried as an empty list;
    // a virtual list that claims rows it has no columns to show stays refused.
    Check(virtualList && Translation::CaptureWindow(
            virtualWindow, virtualContext, rejected, error),
        "an empty virtual report ListView without columns was refused");
    if (virtualList) SendMessageW(virtualList, LVM_SETITEMCOUNT, 3, 0);
    virtualContext.revision = 2;
    Check(virtualList && !Translation::CaptureWindow(
            virtualWindow, virtualContext, rejected, error) &&
            error.find(L"no native report columns") != std::wstring::npos,
        "virtual report ListView with rows but no columns was accepted by the bounded adapter");
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

    // Clearing the icon sizes a plain SS_ICON to nothing, so it draws nothing and is
    // no longer captured; one that keeps its size with no image is still refused.
    SendMessageW(iconControl, STM_SETICON, 0, 0);
    context.revision = 3;
    Translation::WindowSnapshot missing;
    RECT emptied{};
    GetWindowRect(iconControl, &emptied);
    if (IsRectEmpty(&emptied)) {
        Check(Translation::CaptureWindow(window, context, missing, error) &&
              std::none_of(missing.nodes.begin(), missing.nodes.end(),
                  [&](const auto& node) { return node.hwnd == iconControl; }),
            "an emptied SS_ICON that draws nothing was still captured");
        SetWindowPos(iconControl, nullptr, 0, 0, 32, 32,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
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
    Check(bitmap && Translation::ClassifyControl(bitmap, kind, reason) &&
        kind == Translation::ControlKind::StaticBitmap,
        "SS_BITMAP was not classified as its own staticBitmap kind");
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

// An owner-draw button of any framework but Windows Forms (Performance Monitor's colour swatch) is a push
// button whose whole face its owner paints: the face is captured as painted, all background included.
LRESULT CALLBACK OwnerDrawSwatchParent(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_DRAWITEM && lParam) {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        HBRUSH brush = CreateSolidBrush(RGB(200, 30, 30));
        FillRect(draw->hDC, &draw->rcItem, brush);
        DeleteObject(brush);
        return TRUE;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void TestOwnerDrawButtonFace() {
    HWND root = CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"owner-draw-face",
        WS_OVERLAPPEDWINDOW, 20, 20, 300, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND swatch = root ? CreateWindowExW(0, L"Button", L"Colour", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        10, 10, 90, 24, root, reinterpret_cast<HMENU>(700), GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(root && swatch, "owner-draw swatch controls were not created");
    if (!root || !swatch) {
        if (root) DestroyWindow(root);
        return;
    }
    // A face wider than a button picture may be is the inert picture it shows when no key reaches the
    // button; one a key reaches is a button whose whole face travels under the owner-draw face bound
    // (Internet Options' colour buttons), and past that bound it has no contract.
    HWND wide = CreateWindowExW(0, L"Button", L"Wide", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        10, 44, 150, 24, root, reinterpret_cast<HMENU>(701), GetModuleHandleW(nullptr), nullptr);
    HWND wideFocusable = CreateWindowExW(0, L"Button", L"Wide focusable", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        10, 78, 150, 24, root, reinterpret_cast<HMENU>(702), GetModuleHandleW(nullptr), nullptr);
    HWND tooWide = CreateWindowExW(0, L"Button", L"Too wide", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        10, 112, 600, 24, root, reinterpret_cast<HMENU>(703), GetModuleHandleW(nullptr), nullptr);
    Check(wide && wideFocusable && tooWide, "wide owner-draw buttons were not created");
    if (wide && wideFocusable && tooWide) {
        Translation::ControlKind wideKind{};
        std::wstring wideReason;
        Check(Translation::ClassifyControl(wide, wideKind, wideReason) &&
              wideKind == Translation::ControlKind::StaticBitmap,
            "a wide owner-draw button that no key reaches was not carried as an inert picture");
        wideReason.clear();
        Check(Translation::ClassifyControl(wideFocusable, wideKind, wideReason) &&
              wideKind == Translation::ControlKind::Button,
            "a wide owner-draw button that takes focus was not admitted as a button");
        wideReason.clear();
        Check(!Translation::ClassifyControl(tooWide, wideKind, wideReason) && !wideReason.empty(),
            "an owner-draw button wider than the face bound was admitted");
        DestroyWindow(wideFocusable);
        DestroyWindow(tooWide);
    }
    SetWindowSubclass(root, OwnerDrawSwatchParent, 1, 0);
    ShowWindow(root, SW_SHOWNOACTIVATE);
    UpdateWindow(root);
    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-555555555555";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(root, context, snapshot, error);
    if (!captured) std::wcerr << L"owner-draw button capture: " << error << L'\n';
    Check(captured, "an owner-draw button of a non-WinForms owner was rejected");
    if (captured) {
        RECT client{};
        GetClientRect(swatch, &client);
        const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [swatch](const Translation::ControlNode& candidate) { return candidate.hwnd == swatch; });
        Check(node != snapshot.nodes.end() && node->kind == Translation::ControlKind::Button &&
              node->text == L"Colour" &&
              node->imageWidth == static_cast<uint32_t>(client.right) &&
              node->imageHeight == static_cast<uint32_t>(client.bottom) &&
              node->imageData.size() == static_cast<size_t>(client.right) * client.bottom * 4,
            "an owner-draw button face was not captured at the control's size");
        if (node != snapshot.nodes.end() && node->imageData.size() >= 4) {
            const size_t centre = (static_cast<size_t>(client.bottom / 2) * client.right + client.right / 2) * 4;
            Check(node->imageData[centre] == 30 && node->imageData[centre + 1] == 30 &&
                  node->imageData[centre + 2] == 200 && node->imageData[centre + 3] == 255,
                "an owner-draw button face was keyed out instead of captured as painted");
        }
    }
    DestroyWindow(root);
}

// The native dialog manager numbers a container's tab stops from its first one -- even when the
// container's first child is itself a tab stop -- and a child dialog that is no control parent (the pages of
// an OLE property frame, such as Performance Monitor's chart Properties) is a navigation scope of its own.
void TestTabOrderScopes() {
    const auto find = [](const Translation::WindowSnapshot& snapshot, HWND window) {
        return std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [window](const Translation::ControlNode& node) { return node.hwnd == window; });
    };
    HWND startRoot = CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"tab-order-start",
        WS_OVERLAPPEDWINDOW, 20, 20, 360, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND first = startRoot ? CreateWindowExW(0, L"Edit", L"first", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        10, 10, 120, 24, startRoot, reinterpret_cast<HMENU>(501), GetModuleHandleW(nullptr), nullptr) : nullptr;
    HWND second = startRoot ? CreateWindowExW(0, L"Edit", L"second", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        10, 44, 120, 24, startRoot, reinterpret_cast<HMENU>(502), GetModuleHandleW(nullptr), nullptr) : nullptr;
    HWND third = startRoot ? CreateWindowExW(0, L"Button", L"third", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        10, 78, 120, 24, startRoot, reinterpret_cast<HMENU>(503), GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(startRoot && first && second && third, "tab-order start controls were not created");
    if (startRoot && first && second && third) {
        ShowWindow(startRoot, SW_SHOWNOACTIVATE);
        Translation::CaptureContext context;
        context.surfaceId = L"66666666-6666-6666-6666-555555555555";
        context.generation = 1;
        context.revision = 1;
        Translation::WindowSnapshot snapshot;
        std::wstring error;
        Check(Translation::CaptureWindow(startRoot, context, snapshot, error), "tab-order start window was rejected");
        const auto a = find(snapshot, first), b = find(snapshot, second), c = find(snapshot, third);
        Check(a != snapshot.nodes.end() && b != snapshot.nodes.end() && c != snapshot.nodes.end() &&
              a->tabIndex == 0 && b->tabIndex == 1 && c->tabIndex == 2,
            "a first child that is a tab stop was not numbered first");
    }
    if (startRoot) DestroyWindow(startRoot);

    HWND frame = CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"property-frame",
        WS_OVERLAPPEDWINDOW, 20, 20, 400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND frameButton = frame ? CreateWindowExW(0, L"Button", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        10, 250, 80, 24, frame, reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr) : nullptr;
    // No DS_CONTROL and no WS_EX_CONTROLPARENT: the frame's dialog manager never reaches the page's controls.
    HWND page = frame ? CreateWindowExW(0, L"#32770", L"Page", WS_CHILD | WS_VISIBLE,
        10, 10, 360, 200, frame, reinterpret_cast<HMENU>(600), GetModuleHandleW(nullptr), nullptr) : nullptr;
    HWND pageFirst = page ? CreateWindowExW(0, L"Edit", L"one", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        10, 10, 150, 24, page, reinterpret_cast<HMENU>(601), GetModuleHandleW(nullptr), nullptr) : nullptr;
    HWND pageSecond = page ? CreateWindowExW(0, L"Edit", L"two", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        10, 44, 150, 24, page, reinterpret_cast<HMENU>(602), GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(frame && frameButton && page && pageFirst && pageSecond, "property-page tab-order controls were not created");
    if (frame && frameButton && page && pageFirst && pageSecond) {
        ShowWindow(frame, SW_SHOWNOACTIVATE);
        Translation::CaptureContext context;
        context.surfaceId = L"66666666-6666-6666-6666-666666666666";
        context.generation = 1;
        context.revision = 1;
        Translation::WindowSnapshot snapshot;
        std::wstring error;
        const bool captured = Translation::CaptureWindow(frame, context, snapshot, error);
        if (!captured) std::wcerr << L"property page capture: " << error << L'\n';
        Check(captured, "a child dialog that is no control parent was rejected");
        if (captured) {
            const auto container = find(snapshot, page);
            const auto one = find(snapshot, pageFirst), two = find(snapshot, pageSecond), ok = find(snapshot, frameButton);
            Check(container != snapshot.nodes.end() && one != snapshot.nodes.end() && two != snapshot.nodes.end() &&
                  ok != snapshot.nodes.end() &&
                  container->kind == Translation::ControlKind::DialogContainer && container->tabIndex == -1,
                "a child dialog that is no control parent was not captured as a structural container");
            Check(one != snapshot.nodes.end() && two != snapshot.nodes.end() && ok != snapshot.nodes.end() &&
                  one->tabIndex >= 0 && two->tabIndex >= 0 && ok->tabIndex >= 0 &&
                  one->tabIndex + 1 == two->tabIndex && one->tabIndex != ok->tabIndex && two->tabIndex != ok->tabIndex,
                "the controls of an independent dialog page lost their own tab order");
        }
    }
    if (frame) DestroyWindow(frame);
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

constexpr UINT kTestMenuPopup = WM_APP + 301;
constexpr UINT kTestMenuFlood = WM_APP + 302;
constexpr UINT kTestIslandPopup = WM_APP + 303;
constexpr UINT kTestIslandProbe = WM_APP + 304;
constexpr UINT kTestIslandSentinel = WM_APP + 305;

enum class TestIslandProbe : WPARAM {
    CancelQueued,
    DuplicateQueued,
    DisabledItem,
    DisabledIsland,
    HiddenIsland,
    DisabledRoot,
};

struct TestIslandProbeResult final {
    bool firstQueued = false;
    bool firstRefused = false;
    bool secondQueued = false;
    bool secondRefused = false;
    bool replayPosted = false;
    bool sentinelPosted = false;
    bool directAccepted = false;
    bool directReason = false;
    unsigned directInvocations = 0;
    unsigned providerInvocations = 0;
    bool wrongThread = false;
};

struct TestIslandPopupRuntime final {
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE tracking = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE returned = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE command = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE probeDrained = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<HWND> root{ nullptr };
    std::atomic<HWND> island{ nullptr };
    std::atomic<DWORD> thread{ 0 };
    std::atomic<UINT> flags{ TPM_RETURNCMD };
    std::atomic<bool> disableOpener{ true };
    std::atomic<BOOL> result{ FALSE };
    std::atomic<bool> handled{ false };
    std::atomic<bool> menuAliveAtReturn{ false };
    std::atomic<unsigned> ownerCommands{ 0 };
    std::atomic<unsigned> rootCommands{ 0 };
    std::atomic<UINT> lastCommand{ 0 };
    std::atomic<unsigned> providerInvocations{ 0 };
    std::atomic<bool> openProviderPopup{ true };
    std::atomic<Translation::SourceThreadAgent*> probeAgent{ nullptr };
    Translation::ControlNode probeNode;
    TestIslandProbeResult probeResult;
    // The source thread publishes probeResult only after the queued messages drain.
    std::atomic<bool> probeDone{ false };
    ~TestIslandPopupRuntime() {
        if (ready) CloseHandle(ready);
        if (tracking) CloseHandle(tracking);
        if (returned) CloseHandle(returned);
        if (command) CloseHandle(command);
        if (probeDrained) CloseHandle(probeDrained);
    }
};

// A real toolbar HWND with an application-owned accessible menu provider. Its
// default action posts the popup and destroys the HMENU immediately after the
// intercept, matching MMC's asynchronous tracking/lifetime contract.
class TestMenuAccessible final : public IAccessible, public IEnumVARIANT {
public:
    HWND toolbar = nullptr;
    std::vector<std::wstring> names{ L"System", L"&File", L"&Window" };
    std::atomic<bool>* cancellation = nullptr;
    bool quitOnDrive = false;
    bool flood = false;
    bool checked = false;
    bool escaped = false;
    bool wrongThread = false;
    bool itemEnabled = true;
    bool omitDisabledAction = false;
    bool pageTabs = false;
    bool mixedPageRoles = false;
    bool duplicateTabSelection = false;
    int selectedTab = 1;
    unsigned drives = 0;
    // Menu-bar buttons (one-based) that report STATE_SYSTEM_INVISIBLE, as MMC's hidden
    // Favorites and snap-in placeholders do, and the last one whose default action opens a popup.
    std::vector<long> hiddenChildren;
    long popupChildLimit = 2;
    DWORD thread = GetCurrentThreadId();
    TestIslandPopupRuntime* islandRuntime = nullptr;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IEnumVARIANT) {
            *object = static_cast<IEnumVARIANT*>(this);
            AddRef();
            return S_OK;
        }
        if (iid != IID_IUnknown && iid != IID_IDispatch && iid != IID_IAccessible)
            return E_NOINTERFACE;
        *object = static_cast<IAccessible*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --references_;
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
        if (count) *count = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** value) override { if (value) *value = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override {
        wrongThread |= GetCurrentThreadId() != thread;
        if (!count) return E_POINTER;
        *count = static_cast<long>(names.size());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT, IDispatch** value) override { if (value) *value = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR* name) override {
        wrongThread |= GetCurrentThreadId() != thread;
        if (!name) return E_POINTER;
        *name = nullptr;
        if (child.vt != VT_I4 || child.lVal < 1 ||
            static_cast<size_t>(child.lVal) > names.size()) return E_INVALIDARG;
        *name = SysAllocString(names[static_cast<size_t>(child.lVal - 1)].c_str());
        return *name ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT* role) override {
        if (!role) return E_POINTER;
        role->vt = VT_I4;
        role->lVal = pageTabs ? (child.lVal == CHILDID_SELF ? ROLE_SYSTEM_PAGETABLIST
            : mixedPageRoles && child.lVal == 2 ? ROLE_SYSTEM_PUSHBUTTON : ROLE_SYSTEM_PAGETAB)
            : !islandRuntime ? ROLE_SYSTEM_MENUITEM
            : child.vt == VT_I4 && child.lVal == CHILDID_SELF
                ? ROLE_SYSTEM_CLIENT : ROLE_SYSTEM_BUTTONDROPDOWN;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT* state) override {
        wrongThread |= GetCurrentThreadId() != thread;
        if (!state) return E_POINTER;
        state->vt = VT_I4;
        state->lVal = islandRuntime && !itemEnabled &&
            child.vt == VT_I4 && child.lVal != CHILDID_SELF
            ? STATE_SYSTEM_UNAVAILABLE : 0;
        if (pageTabs && child.lVal != CHILDID_SELF) {
            state->lVal = STATE_SYSTEM_SELECTABLE | STATE_SYSTEM_FOCUSABLE;
            if (child.lVal == selectedTab || duplicateTabSelection) state->lVal |= STATE_SYSTEM_SELECTED;
            if (!itemEnabled) state->lVal |= STATE_SYSTEM_UNAVAILABLE;
        }
        if (child.vt == VT_I4 && std::find(hiddenChildren.begin(), hiddenChildren.end(), child.lVal) !=
                hiddenChildren.end())
            state->lVal |= STATE_SYSTEM_INVISIBLE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR*, VARIANT, long*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT*) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT, BSTR* action) override {
        if (!action) return E_POINTER;
        if (!itemEnabled && omitDisabledAction) { *action = nullptr; return S_FALSE; }
        *action = pageTabs ? SysAllocString(L"Switch") : islandRuntime ? SysAllocString(L"Open") : nullptr;
        return pageTabs || islandRuntime ? (*action ? S_OK : E_OUTOFMEMORY) : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE accSelect(long, VARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accLocation(long* left, long* top, long* width, long* height, VARIANT child) override {
        if (!islandRuntime && !pageTabs) return E_NOTIMPL;
        if (!left || !top || !width || !height) return E_POINTER;
        POINT origin{ 8, 8 };
        if (pageTabs) origin = { (child.lVal - 1) * 65, 0 };
        if (!ClientToScreen(toolbar, &origin)) return E_FAIL;
        *left = origin.x;
        *top = origin.y;
        *width = 120;
        *height = 28;
        if (pageTabs) { *width = child.lVal == 1 ? 73 : 70; *height = 19; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long, VARIANT, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accHitTest(long, long, VARIANT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
        wrongThread |= GetCurrentThreadId() != thread;
        ++drives;
        if (pageTabs) { selectedTab = child.lVal; return S_OK; }
        if (islandRuntime) {
            islandRuntime->providerInvocations.fetch_add(1);
            if (!islandRuntime->openProviderPopup.load()) return S_OK;
            return PostMessageW(toolbar, kTestIslandPopup, 0, 0) ? S_OK : E_FAIL;
        }
        if (quitOnDrive) { PostQuitMessage(73); return S_OK; }
        if (cancellation) {
            PostMessageW(toolbar, kTestMenuPopup, 2, 0);
            cancellation->store(true);
            return S_OK;
        }
        if (flood) {
            PostMessageW(toolbar, kTestMenuFlood, 0, 0);
            return S_OK;
        }
        if (child.vt == VT_I4 && child.lVal <= popupChildLimit)
            PostMessageW(toolbar, kTestMenuPopup, child.lVal, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Next(ULONG count, VARIANT* values, ULONG* fetched) override {
        if (!values) return E_POINTER;
        ULONG copied = 0;
        while (copied < count && enumIndex_ < names.size()) {
            VariantInit(&values[copied]);
            values[copied].vt = VT_I4;
            values[copied].lVal = static_cast<LONG>(++enumIndex_);
            ++copied;
        }
        if (fetched) *fetched = copied;
        return copied == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
        const size_t skipped = std::min<size_t>(count, names.size() - enumIndex_);
        enumIndex_ += skipped;
        return skipped == count ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Reset() override { enumIndex_ = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE Clone(IEnumVARIANT**) override { return E_NOTIMPL; }

private:
    std::atomic<ULONG> references_{ 1 };
    size_t enumIndex_ = 0;
};

LRESULT CALLBACK TestMenuToolbarSubclass(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR data) {
    auto* accessible = reinterpret_cast<TestMenuAccessible*>(data);
    if (message == WM_GETOBJECT && static_cast<LONG>(lParam) == OBJID_CLIENT)
        return LresultFromObject(IID_IAccessible, wParam, static_cast<IAccessible*>(accessible));
    if (message == kTestIslandProbe && accessible->islandRuntime) {
        auto& runtime = *accessible->islandRuntime;
        auto& result = runtime.probeResult;
        auto* agent = runtime.probeAgent.load(std::memory_order_acquire);
        result = {};
        if (agent) {
            // Queue and mutate in one source callback, before the source hook can
            // dispatch the posted action. The node still holds its enabled capture.
            result.firstQueued = agent->PostIslandAction(
                runtime.probeNode, 0, result.firstRefused);
            switch (static_cast<TestIslandProbe>(wParam)) {
            case TestIslandProbe::CancelQueued:
                agent->CancelPopupOnSourceThread();
                break;
            case TestIslandProbe::DuplicateQueued: {
                result.secondQueued = agent->PostIslandAction(
                    runtime.probeNode, 0, result.secondRefused);
                // Copy the real posted message without consuming it. Both copies
                // must travel through the source hook before the sentinel runs.
                MSG queued{};
                if (result.firstQueued && PeekMessageW(&queued, nullptr,
                        agent->MessageId(), agent->MessageId(), PM_NOREMOVE)) {
                    result.replayPosted = PostThreadMessageW(agent->ThreadId(),
                        queued.message, queued.wParam, queued.lParam) != FALSE;
                }
                break;
            }
            case TestIslandProbe::DisabledItem: {
                accessible->itemEnabled = false;
                const auto& item = runtime.probeNode.islandItems[0];
                std::wstring reason;
                result.directAccepted = Translation::InvokeAccessibleIslandItem(
                    window, 0, item.name, item.actionName, reason);
                result.directReason = !reason.empty();
                result.directInvocations = runtime.providerInvocations.load();
                break;
            }
            case TestIslandProbe::DisabledIsland:
                EnableWindow(window, FALSE);
                break;
            case TestIslandProbe::HiddenIsland:
                ShowWindow(window, SW_HIDE);
                break;
            case TestIslandProbe::DisabledRoot:
                EnableWindow(runtime.root.load(), FALSE);
                break;
            }
        }
        result.sentinelPosted = PostMessageW(window, kTestIslandSentinel, wParam, 0) != FALSE;
        return 0;
    }
    if (message == kTestIslandSentinel && accessible->islandRuntime) {
        auto& runtime = *accessible->islandRuntime;
        runtime.probeResult.providerInvocations = runtime.providerInvocations.load();
        runtime.probeResult.wrongThread = accessible->wrongThread;
        if (auto* agent = runtime.probeAgent.load(std::memory_order_acquire))
            agent->CancelPopupOnSourceThread();
        accessible->itemEnabled = true;
        switch (static_cast<TestIslandProbe>(wParam)) {
        case TestIslandProbe::DisabledIsland:
            EnableWindow(window, TRUE);
            break;
        case TestIslandProbe::HiddenIsland:
            ShowWindow(window, SW_SHOWNOACTIVATE);
            break;
        case TestIslandProbe::DisabledRoot:
            EnableWindow(runtime.root.load(), TRUE);
            break;
        default:
            break;
        }
        runtime.probeDone.store(true, std::memory_order_release);
        SetEvent(runtime.probeDrained);
        return 0;
    }
    if (message == kTestIslandPopup && accessible->islandRuntime) {
        auto& runtime = *accessible->islandRuntime;
        // MMC-style providers disable their opener before entering the tracking
        // API. EnableWindow sends WM_CANCELMODE and must not lose this request.
        if (runtime.disableOpener.load()) EnableWindow(window, FALSE);
        const HMENU popup = CreatePopupMenu();
        const HMENU nested = CreatePopupMenu();
        AppendMenuW(popup, MF_STRING, 501, L"Direct command");
        AppendMenuW(nested, MF_STRING, 502, L"Nested command");
        AppendMenuW(popup, MF_POPUP, reinterpret_cast<UINT_PTR>(nested), L"More");
        SetEvent(runtime.tracking);
        BOOL result = FALSE;
        const bool handled = Translation::TryTrackIslandPopup(popup, runtime.flags.load(), window, result);
        runtime.result.store(result);
        runtime.handled.store(handled);
        runtime.menuAliveAtReturn.store(IsMenu(popup) != FALSE);
        DestroyMenu(popup);
        if (runtime.disableOpener.load()) EnableWindow(window, TRUE);
        SetEvent(runtime.returned);
        return 0;
    }
    if (message == WM_COMMAND && accessible->islandRuntime) {
        auto& runtime = *accessible->islandRuntime;
        runtime.ownerCommands.fetch_add(1);
        runtime.lastCommand.store(LOWORD(wParam));
        SetEvent(runtime.command);
        return 0;
    }
    if (message == kTestMenuPopup) {
        const HMENU popup = CreatePopupMenu();
        AppendMenuW(popup, MF_STRING | (accessible->checked ? MF_CHECKED : 0),
            wParam == 1 ? SC_CLOSE : 77, wParam == 1 ? L"Close" : L"Command");
        if (Translation::PopupInterceptionArmed())
            Translation::RecordInterceptedPopup(popup);
        else if (!Translation::ShouldSuppressPopup(window)) accessible->escaped = true;
        DestroyMenu(popup);
        return 0;
    }
    if (message == kTestMenuFlood) {
        if (accessible->flood) PostMessageW(window, kTestMenuFlood, 0, 0);
        return 0;
    }
    if (message == WM_CANCELMODE) {
        accessible->wrongThread |= GetCurrentThreadId() != accessible->thread;
    }
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, TestMenuToolbarSubclass, subclassId);
    return result;
}

void TestMenuBarToolbarLifecycle() {
    Translation::MenuBarRefreshPolicy policy;
    Check(policy.ShouldRead(0), "initial menu refresh was not eligible");
    policy.Complete(100, true);
    Check(!policy.ShouldRead(100000), "quiet menu was reopened without a mutation");
    policy.Invalidate(200);
    Check(!policy.ShouldRead(849) && policy.ShouldRead(850),
        "menu refresh did not debounce and respect its minimum interval");
    policy.Complete(900, false);
    Check(!policy.ShouldRead(1899) && policy.ShouldRead(1900),
        "failed menu refresh did not back off");
    policy.Complete(2000, false);
    policy.Complete(4000, false);
    Check(!policy.ShouldRead(100000), "unreadable menu retried without a bound");
    policy.Invalidate(100000);
    Check(policy.ShouldRead(100250), "new menu mutation did not restart bounded retry");

    const HWND root = CreateWindowExW(0, L"Static", L"menu-bar-lifecycle",
        WS_OVERLAPPEDWINDOW, 0, 0, 360, 220, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND toolbar = root ? CreateWindowExW(0, TOOLBARCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE, 0, 0, 350, 30, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(root && toolbar, "menu-bar lifecycle HWNDs were not created");
    if (!root || !toolbar) { if (root) DestroyWindow(root); return; }
    auto* accessible = new TestMenuAccessible();
    accessible->toolbar = toolbar;
    SetWindowSubclass(toolbar, TestMenuToolbarSubclass, 0xAA11,
        reinterpret_cast<DWORD_PTR>(accessible));
    ShowWindow(root, SW_SHOWNOACTIVATE);
    Translation::CaptureContext context;
    Translation::ObserveMenuBarToolbar(root, context);
    Check(context.menuBarToolbar == toolbar && context.menuBarButtons.size() == 3,
        "menu-bar observation did not discover the accessible toolbar");
    Check(accessible->drives == 0, "capture observation opened native menus");
    std::atomic<bool> cancelled{ false };
    std::wstring error;
    std::vector<Translation::MenuItemSnapshot> menu;
    Check(Translation::CaptureMenuBarToolbar(root, toolbar, context.menuBarButtons,
            250, 3000, cancelled, menu, error),
        "staged menu-bar capture rejected asynchronous popup lifetime");
    Check(menu.size() == 2 && menu[0].itemId == L"0" && menu[0].items.size() == 1 &&
        menu[0].items[0].commandId == 77 && menu[0].items[0].itemId == L"0.0" &&
        menu[1].itemId == L"1" && !menu[1].enabled && menu[1].items.empty(),
        "menu-bar capture lost copied commands, system-menu filtering or empty popup state");
    {
        // The Window button opens nothing. The read remembers that, and the next
        // read waits only briefly for it instead of the full popup interval.
        std::vector<bool> silent;
        auto started = GetTickCount64();
        const bool first = Translation::CaptureMenuBarToolbar(root, toolbar, context.menuBarButtons,
            600, 4000, cancelled, menu, error, nullptr, &silent);
        const auto firstElapsed = GetTickCount64() - started;
        Check(first && silent == std::vector<bool>{ false, false, true },
            "menu-bar read did not remember the button that opened no popup");
        started = GetTickCount64();
        const bool second = Translation::CaptureMenuBarToolbar(root, toolbar, context.menuBarButtons,
            600, 4000, cancelled, menu, error, nullptr, &silent);
        const auto secondElapsed = GetTickCount64() - started;
        Check(second && menu.size() == 2 && !menu[1].enabled && firstElapsed >= 600 &&
            secondElapsed + 300 < firstElapsed,
            "a button that opened no popup still cost the full wait on the next read");
    }
    context.menuBarToolbarMenu = menu;
    Translation::ObserveMenuBarToolbar(root, context);
    Check(!context.menuBarToolbarMenu.empty(), "unchanged toolbar discarded cached menu");
    accessible->checked = true;
    Check(Translation::CaptureMenuBarToolbar(root, toolbar, context.menuBarButtons,
            250, 3000, cancelled, menu, error) && menu.size() == 2 &&
            menu[0].items.size() == 1 && menu[0].items[0].checked,
        "menu-bar refresh did not capture a changed native checked state");
    accessible->names[1] = L"&Console";
    Translation::ObserveMenuBarToolbar(root, context);
    Check(context.menuBarToolbarMenu.empty() && context.menuBarButtons[1].text == L"&Console",
        "relabelled toolbar retained stale menu identities");
    context.menuBarToolbarMenu = menu;
    const auto generation = context.menuBarToolbarGeneration;
    RemovePropW(toolbar, L"FluentShell.Bridge.NodeGeneration");
    Translation::ObserveMenuBarToolbar(root, context);
    Check(context.menuBarToolbarGeneration != generation && context.menuBarToolbarMenu.empty(),
        "same-HWND replacement generation inherited a stale menu");
    context.menuBarToolbarMenu = menu;
    ShowWindow(toolbar, SW_HIDE);
    Translation::ObserveMenuBarToolbar(root, context);
    Check(!context.menuBarToolbar && context.menuBarToolbarMenu.empty(),
        "disappeared toolbar kept its stale projected menu");
    ShowWindow(toolbar, SW_SHOWNOACTIVATE);
    Translation::ObserveMenuBarToolbar(root, context);
    Check(context.menuBarToolbar == toolbar,
        "toolbar appearing after initial absence was permanently ignored");

    BOOL cloaked = TRUE;
    Check(SUCCEEDED(DwmSetWindowAttribute(root, DWMWA_CLOAK, &cloaked, sizeof(cloaked))),
        "test menu owner could not be cloaked");
    accessible->cancellation = &cancelled;
    Check(!Translation::CaptureMenuBarToolbar(root, toolbar, context.menuBarButtons,
            20, 1500, cancelled, menu, error) &&
        !Translation::PopupInterceptionArmed() && !Translation::PopupSuppressionActive() &&
        !Translation::MenuBarReadInProgress(),
        "cancelled menu drive leaked interception or its pump scope");
    MSG latePopup{};
    Check(PeekMessageW(&latePopup, toolbar, kTestMenuPopup, kTestMenuPopup, PM_REMOVE) != FALSE,
        "cancelled test drive did not queue its late popup");
    if (latePopup.message == kTestMenuPopup) DispatchMessageW(&latePopup);
    Check(!accessible->escaped && Translation::ShouldSuppressPopup(toolbar),
        "cancelled menu read let a queued native popup escape over the projection");
    cloaked = FALSE;
    DwmSetWindowAttribute(root, DWMWA_CLOAK, &cloaked, sizeof(cloaked));
    Check(!Translation::ShouldSuppressPopup(toolbar),
        "restored native owner kept suppressing its own menus");
    cancelled.store(false);
    accessible->cancellation = nullptr;
    accessible->quitOnDrive = true;
    Check(!Translation::CaptureMenuBarToolbar(root, toolbar, context.menuBarButtons,
            20, 1500, cancelled, menu, error), "WM_QUIT did not stop menu capture");
    MSG quit{};
    Check(PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) && quit.wParam == 73,
        "menu pump consumed the application's WM_QUIT or lost its exit code");
    Check(!PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE),
        "menu pump reposted WM_QUIT more than once");
    accessible->quitOnDrive = false;
    accessible->flood = true;
    const ULONGLONG started = GetTickCount64();
    Check(!Translation::CaptureMenuBarToolbar(root, toolbar, context.menuBarButtons,
            500, 70, cancelled, menu, error) && GetTickCount64() - started < 1000,
        "continuous native messages extended the menu read's total deadline");
    accessible->flood = false;
    MSG pending{};
    while (PeekMessageW(&pending, toolbar, kTestMenuFlood, kTestMenuFlood, PM_REMOVE)) {}
    Check(!accessible->escaped && !accessible->wrongThread &&
        !Translation::PopupInterceptionArmed() && !Translation::PopupSuppressionActive() &&
        !Translation::MenuBarReadInProgress(),
        "menu lifecycle leaked a popup, read native state off-thread or leaked cleanup state");
    DestroyWindow(root);
    accessible->Release();
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

void TestPopupMenuState() {
    const HMENU menu = CreatePopupMenu();
    const HMENU nested = CreatePopupMenu();
    const HMENU disabled = CreatePopupMenu();
    Check(menu && nested && disabled, "popup lifecycle menus were not created");
    if (!menu || !nested || !disabled) {
        if (menu) DestroyMenu(menu);
        if (nested) DestroyMenu(nested);
        if (disabled) DestroyMenu(disabled);
        return;
    }
    AppendMenuW(menu, MF_STRING, 77, L"&Run");
    AppendMenuW(nested, MF_STRING, 88, L"&Nested");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(nested), L"More");
    AppendMenuW(disabled, MF_STRING, 99, L"Unavailable");
    AppendMenuW(menu, MF_POPUP | MF_DISABLED,
        reinterpret_cast<UINT_PTR>(disabled), L"Disabled branch");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    std::wstring error;
    std::vector<Translation::MenuItemSnapshot> items;
    Check(Translation::CaptureMenuHandle(menu, L"", items, error) && items.size() == 4,
        "native popup snapshot could not be captured for lifecycle tests");
    if (items.size() != 4) { DestroyMenu(menu); return; }
    // Capture normally propagates disabled ancestors. Test the state boundary
    // independently by leaving the disabled branch's child locally enabled.
    items[2].items[0].enabled = true;
    Translation::PopupMenuState state;
    Check(state.Begin(0, 4, items) == 0 && state.Begin(101, -1, items) == 0 &&
        state.Begin(101, 4, {}) == 0 && !state.Current(),
        "popup state admitted an invalid anchor or empty menu");
    const uint64_t first = state.Begin(101, 4, items);
    Check(first != 0 && state.Current() && state.Current()->popupId == first &&
        state.Current()->nodeId == 101 && state.Current()->itemIndex == 4,
        "popup state lost its token or native item anchor");
    Check(state.Begin(202, 0, items) == 0 && state.Current()->popupId == first,
        "a nested tracking attempt replaced the pending native popup");
    Translation::PopupMenuDecision selected;
    Check(!state.Resolve(first + 1, std::wstring(L"0"), selected) && state.Current(),
        "stale popup token consumed the current native popup");
    Check(!state.Resolve(first, std::wstring(L"1"), selected) &&
        !state.Resolve(first, std::wstring(L"3"), selected) &&
        !state.Resolve(first, std::wstring(L"missing"), selected) &&
        !state.Resolve(first, std::wstring(L"2.0"), selected) && state.Current(),
        "popup container, separator, missing path or disabled ancestor was selectable");
    Check(state.Resolve(first, std::wstring(L"1.0"), selected) && !state.Current() &&
        selected.popupId == first && selected.itemId == std::wstring(L"1.0") &&
        selected.commandId == 88 && selected.text == L"&Nested" && !selected.IsDismissal(),
        "nested popup selection did not terminalize with its command identity");
    Check(Translation::PopupMenuState::MatchLiveChoice(items, selected),
        "unchanged live popup rejected the selected command");
    Check(!state.Resolve(first, std::wstring(L"0"), selected) && !state.Cancel(),
        "one native tracking call accepted multiple terminal decisions");

    std::vector<Translation::MenuItemSnapshot> live;
    EnableMenuItem(nested, 88, MF_BYCOMMAND | MF_DISABLED);
    Check(Translation::CaptureMenuHandle(menu, L"", live, error) &&
        !Translation::PopupMenuState::MatchLiveChoice(live, selected),
        "disabled live command retained authority from its old popup snapshot");
    EnableMenuItem(nested, 88, MF_BYCOMMAND | MF_ENABLED);
    ModifyMenuW(nested, 88, MF_BYCOMMAND | MF_STRING, 88, L"Renamed");
    Check(Translation::CaptureMenuHandle(menu, L"", live, error) &&
        !Translation::PopupMenuState::MatchLiveChoice(live, selected),
        "renamed live command retained authority from its old popup snapshot");
    ModifyMenuW(nested, 88, MF_BYCOMMAND | MF_STRING, 89, L"&Nested");
    Check(Translation::CaptureMenuHandle(menu, L"", live, error) &&
        !Translation::PopupMenuState::MatchLiveChoice(live, selected),
        "replacement command ID retained authority at the selected item path");
    ModifyMenuW(nested, 89, MF_BYCOMMAND | MF_STRING, 88, L"&Nested");
    EnableMenuItem(menu, 1, MF_BYPOSITION | MF_DISABLED);
    Check(Translation::CaptureMenuHandle(menu, L"", live, error) &&
        !Translation::PopupMenuState::MatchLiveChoice(live, selected),
        "disabled live ancestor allowed selection of its descendant command");
    EnableMenuItem(menu, 1, MF_BYPOSITION | MF_ENABLED);
    InsertMenuW(nested, 0, MF_BYPOSITION | MF_STRING, 87, L"Inserted");
    Check(Translation::CaptureMenuHandle(menu, L"", live, error) &&
        !Translation::PopupMenuState::MatchLiveChoice(live, selected),
        "reordered live popup selected a different command at the old path");
    RemoveMenu(nested, 0, MF_BYPOSITION);
    CheckMenuItem(nested, 88, MF_BYCOMMAND | MF_CHECKED);
    Check(Translation::CaptureMenuHandle(menu, L"", live, error) &&
        Translation::PopupMenuState::MatchLiveChoice(live, selected),
        "a check-state change incorrectly changed the selected command identity");

    const uint64_t second = state.Begin(101, 4, items);
    Check(second > first && !state.Resolve(first, std::nullopt, selected) && state.Current(),
        "a late dismissal closed a newer popup or reused a token");
    Check(state.Resolve(second, std::wstring(L"0"), selected) &&
        selected.commandId == 77 && !state.Current(),
        "top-level popup command could not complete tracking");
    const uint64_t third = state.Begin(101, 4, items);
    Check(third > second && state.Resolve(third, std::nullopt, selected) &&
        selected.popupId == third && selected.IsDismissal() && selected.commandId == 0 &&
        selected.text.empty() && !state.Current() &&
        Translation::PopupMenuState::MatchLiveChoice({}, selected),
        "null dismissal did not retire its popup without dispatching a command");
    const uint64_t fourth = state.Begin(101, 4, items);
    Check(fourth > third && state.Cancel() && !state.Current() &&
        !state.Resolve(fourth, std::wstring(L"0"), selected),
        "cancelled popup retained a selectable snapshot");
    const uint64_t fifth = state.Begin(101, 4, items);
    Check(fifth > fourth && state.Cancel() && !state.Cancel(),
        "popup cancellation reused a token or terminalized twice");
    Check(!Translation::PopupMenuState::MatchLiveChoice(items, {}),
        "an unissued popup decision was accepted as a live dismissal");
    DestroyMenu(menu);
}

LRESULT CALLBACK TestPopupRootSubclass(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR data) {
    auto* runtime = reinterpret_cast<TestIslandPopupRuntime*>(data);
    if (message == WM_COMMAND) runtime->rootCommands.fetch_add(1);
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, TestPopupRootSubclass, subclassId);
    return result;
}

void TestSourceThreadPopupTracking() {
    TestIslandPopupRuntime runtime;
    Check(runtime.ready && runtime.tracking && runtime.returned && runtime.command && runtime.probeDrained,
        "source popup test events were not created");
    if (!runtime.ready || !runtime.tracking || !runtime.returned || !runtime.command ||
        !runtime.probeDrained) return;
    std::thread gui([&] {
        runtime.thread.store(GetCurrentThreadId());
        HWND root = nullptr;
        TestMenuAccessible* accessible = nullptr;
        ATOM registered = 0;
        bool initialized = false;
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            initialized = true;
            WNDCLASSW islandClass{};
            islandClass.lpfnWndProc = DefWindowProcW;
            islandClass.hInstance = GetModuleHandleW(nullptr);
            islandClass.lpszClassName = L"DirectUIHWND";
            registered = RegisterClassW(&islandClass);
            root = CreateWindowExW(0, L"Static", L"source-popup-test",
                WS_OVERLAPPEDWINDOW, 50, 50, 340, 220, nullptr, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            const HWND island = root ? CreateWindowExW(0, L"DirectUIHWND", L"Actions",
                WS_CHILD | WS_VISIBLE, 0, 0, 300, 150, root,
                reinterpret_cast<HMENU>(991), GetModuleHandleW(nullptr), nullptr) : nullptr;
            if (root && island) {
                accessible = new TestMenuAccessible();
                accessible->toolbar = island;
                accessible->names = { L"Actions" };
                accessible->islandRuntime = &runtime;
                SetWindowSubclass(island, TestMenuToolbarSubclass, 0xAA12,
                    reinterpret_cast<DWORD_PTR>(accessible));
                SetWindowSubclass(root, TestPopupRootSubclass, 0xAA13,
                    reinterpret_cast<DWORD_PTR>(&runtime));
                ShowWindow(root, SW_SHOWNOACTIVATE);
                runtime.island.store(island);
                runtime.root.store(root);
            }
            SetEvent(runtime.ready);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        } catch (...) {
            SetEvent(runtime.ready);
        }
        if (root) DestroyWindow(root);
        if (accessible) accessible->Release();
        if (registered) UnregisterClassW(L"DirectUIHWND", GetModuleHandleW(nullptr));
        if (initialized) winrt::uninit_apartment();
    });

    std::shared_ptr<Translation::SourceThreadAgent> agent;
    const auto cleanup = [&] {
        if (agent) {
            std::wstring ignored;
            agent->Restore(ignored);
            agent->Shutdown();
        }
        if (runtime.thread.load()) PostThreadMessageW(runtime.thread.load(), WM_QUIT, 0, 0);
        gui.join();
        agent.reset();
    };
    Check(WaitForSingleObject(runtime.ready, 5000) == WAIT_OBJECT_0 && runtime.root.load(),
        "source popup GUI thread did not create its island");
    if (!runtime.root.load()) { cleanup(); return; }
    agent = Translation::SourceThreadAgent::Attach(runtime.root.load(), GetModuleHandleW(nullptr));
    Check(agent != nullptr, "source popup agent could not attach to its real GUI thread");
    if (!agent) { cleanup(); return; }
    std::wstring error;
    Check(agent->SetCloaked(true, error), "source popup test could not cloak its native root");
    Translation::WindowSnapshot snapshot;
    snapshot.surfaceId = L"12121212-3434-5656-7878-909090909090";
    snapshot.revision = 1;

    const bool captured = agent->Capture(snapshot, error);
    const auto probeAnchor = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& node) {
            return node.kind == Translation::ControlKind::AccessibleIsland;
        });
    const bool probeReady = captured && probeAnchor != snapshot.nodes.end() &&
        probeAnchor->islandItems.size() == 1 && probeAnchor->islandItems[0].enabled &&
        probeAnchor->islandItems[0].dropDown;
    Check(probeReady, "deferred island probes did not capture an enabled real provider item");
    if (!probeReady) { cleanup(); return; }
    runtime.probeNode = *probeAnchor;
    runtime.probeAgent.store(agent.get(), std::memory_order_release);
    runtime.openProviderPopup.store(false);
    const auto probe = [&](TestIslandProbe scenario) {
        ResetEvent(runtime.probeDrained);
        runtime.probeDone.store(false, std::memory_order_release);
        runtime.providerInvocations.store(0);
        const bool drained = PostMessageW(runtime.island.load(), kTestIslandProbe,
            static_cast<WPARAM>(scenario), 0) &&
            WaitForSingleObject(runtime.probeDrained, 2000) == WAIT_OBJECT_0 &&
            runtime.probeDone.load(std::memory_order_acquire);
        Check(drained, "deferred island probe did not reach its source-thread sentinel");
        if (!drained) return false;
        const auto& result = runtime.probeResult;
        const bool queued = result.firstQueued && !result.firstRefused &&
            result.sentinelPosted && !result.wrongThread;
        Check(queued, "deferred island probe could not queue on its owning source thread");
        return queued;
    };
    if (!probe(TestIslandProbe::CancelQueued)) { cleanup(); return; }
    Check(runtime.probeResult.providerInvocations == 0,
        "cancelled island token invoked its provider after the source queue drained");

    if (!probe(TestIslandProbe::DuplicateQueued)) { cleanup(); return; }
    Check(!runtime.probeResult.secondQueued && runtime.probeResult.secondRefused,
        "a second island action was accepted while the first token was still queued");
    Check(runtime.probeResult.replayPosted && runtime.probeResult.providerInvocations == 1,
        "an island action or replayed token invoked its provider more or less than once");

    if (!probe(TestIslandProbe::DisabledItem)) { cleanup(); return; }
    Check(!runtime.probeResult.directAccepted && runtime.probeResult.directReason &&
        runtime.probeResult.directInvocations == 0,
        "direct island invocation ran a provider whose current MSAA item was disabled");
    Check(runtime.probeResult.providerInvocations == 0,
        "queued island invocation used its old enabled capture after the MSAA item was disabled");

    const auto refusesChangedWindow = [&](TestIslandProbe scenario, const char* message) {
        if (!probe(scenario)) return false;
        Check(runtime.probeResult.providerInvocations == 0, message);
        return true;
    };
    if (!refusesChangedWindow(TestIslandProbe::DisabledIsland,
            "queued island action invoked a provider after its HWND was disabled") ||
        !refusesChangedWindow(TestIslandProbe::HiddenIsland,
            "queued island action invoked a provider after its HWND was hidden") ||
        !refusesChangedWindow(TestIslandProbe::DisabledRoot,
            "queued island action invoked a provider after its root HWND was disabled")) {
        cleanup();
        return;
    }
    runtime.openProviderPopup.store(true);

    const auto openPopup = [&](UINT flags) {
        ResetEvent(runtime.tracking);
        ResetEvent(runtime.returned);
        ResetEvent(runtime.command);
        runtime.flags.store(flags);
        if (!agent->Capture(snapshot, error)) {
            std::wcerr << L"source popup capture: " << error << L'\n';
            return false;
        }
        const auto anchor = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [](const Translation::ControlNode& node) {
                return node.kind == Translation::ControlKind::AccessibleIsland;
            });
        if (anchor == snapshot.nodes.end() || anchor->islandItems.size() != 1 ||
            !anchor->islandItems[0].dropDown) return false;
        Translation::ActionRequest action;
        action.surfaceId = snapshot.surfaceId;
        action.expectedRevision = snapshot.revision;
        action.eventId = 1;
        action.nodeId = anchor->nodeId;
        action.action = L"islandInvoke";
        action.itemIndex = 0;
        Translation::ActionOutcome outcome;
        if (!agent->Invoke(action, outcome) || !outcome.accepted) {
            std::wcerr << L"source popup invoke: " << outcome.error << L'\n';
            return false;
        }
        snapshot = std::move(outcome.snapshot);
        if (WaitForSingleObject(runtime.tracking, 2000) != WAIT_OBJECT_0 ||
            WaitForSingleObject(runtime.returned, 0) != WAIT_TIMEOUT) {
            std::wcerr << L"source popup tracking did not start or returned early\n";
            return false;
        }
        const bool captured = agent->Capture(snapshot, error);
        if (!captured || !snapshot.popupMenu)
            std::wcerr << L"source popup snapshot: " << error << L'\n';
        return captured && snapshot.popupMenu.has_value();
    };
    const auto answer = [&](std::optional<std::wstring> itemId) {
        Translation::ActionRequest action;
        action.surfaceId = snapshot.surfaceId;
        action.expectedRevision = snapshot.revision;
        action.eventId = 2;
        action.action = L"popupCommand";
        action.popupId = snapshot.popupMenu->popupId;
        action.popupItemId = std::move(itemId);
        Translation::ActionOutcome outcome;
        const bool accepted = agent->Invoke(action, outcome) && outcome.accepted;
        if (accepted) snapshot = std::move(outcome.snapshot);
        return accepted && WaitForSingleObject(runtime.returned, 2000) == WAIT_OBJECT_0 &&
            runtime.handled.load() && runtime.menuAliveAtReturn.load();
    };

    bool opened = openPopup(TPM_RETURNCMD);
    Check(opened, "deferred source island action did not publish a tracked native popup");
    if (opened) {
        Check(answer(std::wstring(L"1.0")) && runtime.result.load() == 502 &&
            runtime.ownerCommands.load() == 0 && runtime.rootCommands.load() == 0 &&
            !snapshot.popupMenu,
            "TPM_RETURNCMD selection lost its native result or dispatched an extra command");
    }
    if (opened && WaitForSingleObject(runtime.returned, 0) == WAIT_OBJECT_0) {
        opened = openPopup(0);
        Check(opened, "second native popup did not become available after terminal cleanup");
        if (opened) {
            Check(answer(std::wstring(L"0")) && runtime.result.load() != FALSE &&
                WaitForSingleObject(runtime.command, 2000) == WAIT_OBJECT_0 &&
                runtime.ownerCommands.load() == 1 && runtime.rootCommands.load() == 0 &&
                runtime.lastCommand.load() == 501,
                "popup command was not posted exactly once to its actual child owner");
        }
    }
    if (opened && WaitForSingleObject(runtime.returned, 0) == WAIT_OBJECT_0) {
        opened = openPopup(TPM_RETURNCMD);
        Check(opened, "native popup was not available for dismissal");
        if (opened) {
            Check(answer(std::nullopt) && runtime.result.load() == FALSE &&
                runtime.ownerCommands.load() == 1,
                "popup dismissal returned a command or failed to release native tracking");
        }
    }
    if (opened && WaitForSingleObject(runtime.returned, 0) == WAIT_OBJECT_0) {
        opened = openPopup(TPM_RETURNCMD);
        Check(opened, "native popup was not available for geometry cancellation");
        if (opened) {
            Translation::ActionRequest move;
            move.surfaceId = snapshot.surfaceId;
            move.expectedRevision = snapshot.revision;
            move.eventId = 3;
            move.action = L"move";
            move.rect = snapshot.bounds;
            OffsetRect(&move.rect, 10, 10);
            move.hasRect = true;
            Translation::ActionOutcome outcome;
            Check(agent->Invoke(move, outcome) && outcome.accepted &&
                !outcome.snapshot.popupMenu &&
                WaitForSingleObject(runtime.returned, 2000) == WAIT_OBJECT_0 &&
                runtime.result.load() == FALSE && runtime.ownerCommands.load() == 1,
                "moving the owner left a native popup pending or dispatched a command");
            if (outcome.accepted) snapshot = std::move(outcome.snapshot);
        }
    }
    if (opened && WaitForSingleObject(runtime.returned, 0) == WAIT_OBJECT_0) {
        opened = openPopup(TPM_RETURNCMD);
        Check(opened, "native popup was not available for restore cancellation");
        if (opened) {
            Check(agent->Restore(error) &&
                WaitForSingleObject(runtime.returned, 2000) == WAIT_OBJECT_0 &&
                runtime.result.load() == FALSE && runtime.menuAliveAtReturn.load() &&
                agent->Capture(snapshot, error) && !snapshot.popupMenu &&
                runtime.ownerCommands.load() == 1,
                "whole-surface restore did not cancel tracking before releasing its native menu");
        }
    }
    cleanup();
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
    snapshot.menu[0].enabled = false;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "enabled command under a disabled menu was accepted");
    snapshot.menu[0].enabled = true;
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

void TestPopupActionProtocol() {
    constexpr auto nonce = L"00112233445566778899aabbccddeeff";
    std::wstring error;
    Translation::ActionRequest choice;
    Check(Translation::ParseActionInvoke(ReadFixture(L"action.invoke.popup-command.json"),
        nonce, choice, error) && choice.popupId == UINT64_MAX &&
        choice.popupItemId == std::optional<std::wstring>(L"2.0") && !choice.nodeId,
        "popup command fixture lost its canonical token or path");
    Translation::ActionRequest dismiss;
    Check(Translation::ParseActionInvoke(ReadFixture(L"action.invoke.popup-dismiss.json"),
        nonce, dismiss, error) && dismiss.popupId == UINT64_MAX && !dismiss.popupItemId,
        "popup dismissal fixture was not parsed as a null choice");
    Check(Translation::IsRequestSemanticAction(L"popupCommand"),
        "popup terminal response cannot rebase a harmless owner revision change");

    Translation::MenuItemSnapshot leaf;
    leaf.itemId = L"2.0";
    leaf.text = L"Details";
    leaf.commandId = 78;
    Translation::MenuItemSnapshot branch;
    branch.itemId = L"2";
    branch.kind = Translation::MenuItemKind::Popup;
    branch.text = L"View";
    branch.items.push_back(leaf);
    Translation::WindowSnapshot snapshot;
    snapshot.popupMenu = Translation::PopupMenuSnapshot{UINT64_MAX, 10, 0, {branch}};
    Check(Translation::ValidateActionForSnapshot(choice, snapshot, error) &&
        Translation::ValidateActionForSnapshot(dismiss, snapshot, error),
        "live popup command or dismissal was rejected");
    snapshot.popupMenu->items[0].enabled = false;
    Check(!Translation::ValidateActionForSnapshot(choice, snapshot, error) &&
        Translation::ValidateActionForSnapshot(dismiss, snapshot, error),
        "disabled popup ancestor allowed a command or prevented dismissal");
    Translation::ActionRequest ordinary;
    ordinary.action = L"menuCommand";
    ordinary.menuCommandId = 78;
    snapshot.menu.push_back(branch);
    Check(!Translation::ValidateActionForSnapshot(ordinary, snapshot, error),
        "main-menu action bypassed pending native popup tracking");
    const auto fingerprint = Translation::SnapshotFingerprint(snapshot);
    snapshot.popupMenu->popupId = 1;
    Check(!Translation::ValidateActionForSnapshot(dismiss, snapshot, error) &&
        fingerprint != Translation::SnapshotFingerprint(snapshot),
        "expired popup retained authority or token change was omitted from fingerprint");
    const auto payload = Translation::SerializeWindowOpen(nonce, snapshot);
    Check(payload.find("\"popupMenu\"") != std::string::npos &&
        payload.find("\"popupId\":\"1\"") != std::string::npos,
        "native serializer omitted popup state or encoded its token as a number");
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
        // The renderer refuses a tab index on a control that is not a tab stop.
        if (!node.tabStop && node.tabIndex != -1) ok = false;
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
void TestAccessiblePageTabs() {
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"AMCCustomTab";
    const ATOM registered = RegisterClassW(&wc);
    const HWND root = CreateWindowExW(0, L"Static", L"page-tab-host", WS_OVERLAPPEDWINDOW,
        40, 40, 400, 180, nullptr, nullptr, wc.hInstance, nullptr);
    const HWND tabs = root ? CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
        0, 0, 240, 28, root, reinterpret_cast<HMENU>(721), wc.hInstance, nullptr) : nullptr;
    Check(root && tabs, "MSAA page-tab fixture could not create its windows");
    if (!root || !tabs) {
        if (root) DestroyWindow(root);
        if (registered) UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return;
    }
    auto* provider = new TestMenuAccessible();
    provider->toolbar = tabs;
    provider->pageTabs = true;
    provider->names = { L"Extended", L"Standard" };
    SetWindowSubclass(tabs, TestMenuToolbarSubclass, 0xAA13,
        reinterpret_cast<DWORD_PTR>(provider));
    ShowWindow(root, SW_SHOWNOACTIVATE);
    Translation::ControlKind kind{};
    std::wstring reason;
    Check(Translation::ClassifyControl(tabs, kind, reason) &&
        kind == Translation::ControlKind::AccessibleIsland,
        "AMCCustomTab did not admit its real MSAA page-tab provider");
    Translation::ControlNode node;
    node.kind = kind;
    Check(Translation::CaptureControlDetail(tabs, node, reason) && node.islandItems.size() == 2 &&
        node.islandItems[0].kind == L"pageTab" && node.islandItems[0].selected &&
        !node.islandItems[1].selected && node.islandItems[1].rect.left == 65,
        "MSAA tab labels, geometry, or canonical selection were lost");
    Translation::WindowSnapshot snapshot;
    snapshot.nodes.push_back(node);
    const auto before = Translation::SnapshotFingerprint(snapshot);
    Check(Translation::InvokeAccessibleIslandItem(tabs, 1, L"Standard", L"Switch", reason) &&
        provider->selectedTab == 2 && provider->drives == 1,
        "MSAA tab action did not invoke the provider's own Switch action");
    Check(Translation::CaptureControlDetail(tabs, snapshot.nodes[0], reason) &&
        !snapshot.nodes[0].islandItems[0].selected && snapshot.nodes[0].islandItems[1].selected &&
        Translation::SnapshotFingerprint(snapshot) != before,
        "native page-tab selection did not update the capture fingerprint");
    provider->itemEnabled = false;
    provider->omitDisabledAction = true;
    Check(Translation::CaptureControlDetail(tabs, node, reason) &&
        !node.islandItems[0].enabled && node.islandItems[0].actionName.empty(),
        "disabled MSAA page tab without a default action was dropped");
    Check(!Translation::InvokeAccessibleIslandItem(tabs, 0, L"Extended", L"Switch", reason) &&
        provider->drives == 1, "disabled MSAA page tab was invoked");
    provider->itemEnabled = true;
    Check(!Translation::InvokeAccessibleIslandItem(tabs, 0, L"Stale", L"Switch", reason) &&
        provider->drives == 1, "stale MSAA page-tab identity was invoked");
    provider->duplicateTabSelection = true;
    Check(!Translation::ClassifyControl(tabs, kind, reason),
        "MSAA tab list with multiple selections was admitted");
    provider->duplicateTabSelection = false;
    provider->selectedTab = 0;
    Check(!Translation::ClassifyControl(tabs, kind, reason),
        "MSAA tab list with no selection was admitted");
    provider->selectedTab = 1;
    provider->mixedPageRoles = true;
    Check(!Translation::ClassifyControl(tabs, kind, reason),
        "MSAA tab list with a non-tab item was admitted");
    Check(!provider->wrongThread, "MSAA page-tab provider was read or invoked on a foreign thread");
    RemoveWindowSubclass(tabs, TestMenuToolbarSubclass, 0xAA13);
    DestroyWindow(root);
    provider->Release();
    if (registered) UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

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

    const uint8_t subpixelBlack[] = { 96, 192, 64 };
    const uint8_t subpixelWhite[] = { 255, 240, 224 };
    const uint8_t emptyBlack[] = { 0, 0, 0 };
    const uint8_t emptyWhite[] = { 255, 255, 255 };
    Check(Translation::RecoverPaintAlpha(subpixelBlack, subpixelWhite) >= 192,
        "channel-dependent GDI paint alpha does not accommodate all color channels");
    Check(Translation::RecoverPaintAlpha(emptyBlack, emptyWhite) == 0 &&
        Translation::RecoverPaintAlpha(subpixelBlack, subpixelBlack) == 255,
        "unpainted or opaque GDI pixels lost their original coverage");
    Check(Translation::RecoverPaintAlpha(subpixelWhite, subpixelBlack) == 255,
        "changing paint between passes produced invalid alpha");
    if (node.chromeRegions.size() == 1) {
        const auto& pixels = node.chromeRegions[0].imageData;
        bool premultiplied = true;
        for (size_t offset = 0; offset + 3 < pixels.size(); offset += 4) {
            premultiplied &= pixels[offset] <= pixels[offset + 3] &&
                pixels[offset + 1] <= pixels[offset + 3] &&
                pixels[offset + 2] <= pixels[offset + 3];
        }
        Check(premultiplied, "captured container chrome is not premultiplied BGRA");
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

// A Windows Forms dialog shown without an owner is parented to the desktop and disables the
// thread's other top-level windows instead (Event Viewer's Filter dialog). The desktop is no
// owner; the disabled window is the one the dialog is modal to.
void TestUnownedModalDialogOwnerResolution() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND frame = CreateWindowExW(0, L"Static", L"modal-peer-frame", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        40, 40, 320, 220, nullptr, nullptr, instance, nullptr);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME, L"Static", L"modal-peer-dialog",
        WS_POPUP | WS_CAPTION | WS_VISIBLE, 70, 70, 200, 120, GetDesktopWindow(), nullptr, instance, nullptr);
    HWND plain = CreateWindowExW(0, L"Static", L"modal-peer-plain",
        WS_POPUP | WS_CAPTION | WS_VISIBLE, 90, 90, 200, 120, GetDesktopWindow(), nullptr, instance, nullptr);
    Check(frame && dialog && plain, "unowned modal dialog fixtures were not created");
    if (!frame || !dialog || !plain) {
        for (HWND window : { frame, dialog, plain }) if (window) DestroyWindow(window);
        return;
    }
    Check(Translation::EffectiveTopLevelOwner(dialog) == nullptr && !Translation::IsModalOwnedTopLevel(dialog),
        "a dialog beside an enabled window was given an owner");
    EnableWindow(frame, FALSE);
    Check(Translation::EffectiveTopLevelOwner(dialog) == frame && Translation::IsModalOwnedTopLevel(dialog),
        "an unowned dialog was not modal to the window disabled for it");
    Check(Translation::EffectiveTopLevelOwner(frame) == nullptr && !Translation::IsModalOwnedTopLevel(frame),
        "the disabled window was given an owner");
    Check(Translation::EffectiveTopLevelOwner(plain) == nullptr,
        "a window with no dialog frame was taken for the dialog of a disabled peer");
    EnableWindow(frame, TRUE);
    Check(Translation::EffectiveTopLevelOwner(dialog) == nullptr,
        "a dialog stayed owned by a window that is enabled again");
    DestroyWindow(plain);
    DestroyWindow(dialog);
    DestroyWindow(frame);
}

// A group box a dialog template gave WS_TABSTOP is no stop of the traversal: the box is captured, takes
// neither a tab stop nor a tab index, and the controls around it keep theirs.
void TestTabStopGroupBoxIsNotATabStop() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(WS_EX_CONTROLPARENT, L"#32770", L"group-tab",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 40, 40, 300, 200, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    const HWND first = root ? CreateWindowExW(0, L"Edit", L"a", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        10, 10, 100, 22, root, reinterpret_cast<HMENU>(61), instance, nullptr) : nullptr;
    const HWND group = root ? CreateWindowExW(0, L"Button", L"frame", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        WS_GROUP | BS_GROUPBOX, 10, 40, 200, 100, root, reinterpret_cast<HMENU>(62), instance, nullptr) : nullptr;
    const HWND second = root ? CreateWindowExW(0, L"Edit", L"b", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        20, 60, 100, 22, root, reinterpret_cast<HMENU>(63), instance, nullptr) : nullptr;
    Check(root && first && group && second, "tab-stop group box fixture could not create its windows");
    if (!root || !first || !group || !second) return;
    ShowWindow(root, SW_SHOWNOACTIVATE);
    Translation::CaptureContext context;
    context.surfaceId = L"a1a1a1a1-b2b2-c3c3-d4d4-e5e5e5e5e5e5";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring reason;
    const bool captured = Translation::CaptureWindow(root, context, snapshot, reason);
    if (!captured) std::wcerr << L"tab-stop group box capture: " << reason << L"\n";
    const auto find = [&snapshot](HWND window) {
        return std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [window](const auto& candidate) { return candidate.hwnd == window; });
    };
    const auto groupNode = find(group);
    const auto firstNode = find(first);
    const auto secondNode = find(second);
    Check(captured && groupNode != snapshot.nodes.end() && groupNode->kind == Translation::ControlKind::GroupBox &&
        !groupNode->tabStop && groupNode->tabIndex == -1,
        "a tab-stop group box was captured as a tab stop or refused");
    Check(captured && firstNode != snapshot.nodes.end() && secondNode != snapshot.nodes.end() &&
        firstNode->tabStop && secondNode->tabStop && firstNode->tabIndex >= 0 && secondNode->tabIndex >= 0,
        "the controls around a tab-stop group box lost their tab stops");
}

// An animation control (the flashlight of Find Certificates) takes no input: it travels as the picture it
// shows, and one that is a tab stop is refused.
void TestAnimationControlIsAnInertPicture() {
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_ANIMATE_CLASS };
    InitCommonControlsEx(&controls);
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(WS_EX_CONTROLPARENT, L"#32770", L"animation",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 40, 40, 300, 200, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    const HWND animation = root ? CreateWindowExW(0, ANIMATE_CLASSW, L"", WS_CHILD | WS_VISIBLE | ACS_TRANSPARENT,
        10, 10, 48, 48, root, reinterpret_cast<HMENU>(71), instance, nullptr) : nullptr;
    const HWND focusable = root ? CreateWindowExW(0, ANIMATE_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        80, 10, 48, 48, root, reinterpret_cast<HMENU>(72), instance, nullptr) : nullptr;
    Check(root && animation && focusable, "animation fixture could not create its windows");
    if (!root || !animation || !focusable) return;
    ShowWindow(root, SW_SHOWNOACTIVATE);
    Translation::ControlKind kind{};
    std::wstring reason;
    Check(Translation::ClassifyControl(animation, kind, reason) && kind == Translation::ControlKind::StaticBitmap,
        "an animation control was not classified as an inert picture");
    reason.clear();
    Check(!Translation::ClassifyControl(focusable, kind, reason) && !reason.empty(),
        "an animation control that is a tab stop was accepted");
    DestroyWindow(focusable);
    Translation::CaptureContext context;
    context.surfaceId = L"b1b1b1b1-c2c2-d3d3-e4e4-f5f5f5f5f5f5";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    const bool captured = Translation::CaptureWindow(root, context, snapshot, reason);
    if (!captured) std::wcerr << L"animation capture: " << reason << L"\n";
    const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [animation](const auto& candidate) { return candidate.hwnd == animation; });
    Check(captured && node != snapshot.nodes.end() && node->kind == Translation::ControlKind::StaticBitmap &&
        node->imageWidth > 0 && node->imageHeight > 0 && !node->imageData.empty(),
        "an animation control's frame was not captured as a picture");
}

// A dialog opened from a dialog (a Color chooser from a property sheet from a console) is a
// chain: each window is modal to the one that owns it exactly while that one is disabled.
// The chain is projected one dialog at a time, so every level has to answer for itself.
void TestNestedModalDialogChain() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND frame = CreateWindowExW(0, L"Static", L"chain-frame", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        40, 40, 360, 240, nullptr, nullptr, instance, nullptr);
    HWND sheet = frame ? CreateWindowExW(WS_EX_DLGMODALFRAME, L"Static", L"chain-sheet",
        WS_POPUP | WS_CAPTION | WS_VISIBLE, 70, 70, 260, 160, frame, nullptr, instance, nullptr) : nullptr;
    HWND chooser = sheet ? CreateWindowExW(WS_EX_DLGMODALFRAME, L"Static", L"chain-chooser",
        WS_POPUP | WS_CAPTION | WS_VISIBLE, 100, 100, 180, 110, sheet, nullptr, instance, nullptr) : nullptr;
    Check(frame && sheet && chooser, "nested dialog chain fixtures were not created");
    if (!frame || !sheet || !chooser) {
        for (HWND window : { chooser, sheet, frame }) if (window) DestroyWindow(window);
        return;
    }
    Check(Translation::EffectiveTopLevelOwner(chooser) == sheet &&
          Translation::EffectiveTopLevelOwner(sheet) == frame &&
          Translation::EffectiveTopLevelOwner(frame) == nullptr,
        "the dialog chain did not resolve each window to its own owner");
    // Everything enabled: nothing is modal, whatever the depth.
    Check(!Translation::IsModalOwnedTopLevel(sheet) && !Translation::IsModalOwnedTopLevel(chooser),
        "a dialog of an enabled owner was taken for a modal one");
    // The sheet is open modally over the console.
    EnableWindow(frame, FALSE);
    Check(Translation::IsModalOwnedTopLevel(sheet) && !Translation::IsModalOwnedTopLevel(chooser),
        "the sheet was not modal to the disabled console, or the chooser was modal to the enabled sheet");
    // The chooser is open modally over the sheet: it is modal although its owner is itself owned.
    EnableWindow(sheet, FALSE);
    Check(Translation::IsModalOwnedTopLevel(sheet) && Translation::IsModalOwnedTopLevel(chooser),
        "a dialog modal to an owned dialog was not taken for a modal one");
    Check(!Translation::IsModalOwnedTopLevel(frame), "the console of the chain was taken for a dialog");
    EnableWindow(sheet, TRUE);
    EnableWindow(frame, TRUE);
    DestroyWindow(chooser);
    DestroyWindow(sheet);
    DestroyWindow(frame);
}

// A form page of label/field rows has a strip between each label and its field, and every
// strip has a neighbour on both sides. None of them is a splitter: each is one row tall and
// stops at the next row (Task Scheduler's task Properties page offered a draggable bar).
void TestFormRowStripsAreNotSplits() {
    WNDCLASSEXW containerClass{};
    containerClass.cbSize = sizeof(containerClass);
    containerClass.lpfnWndProc = PaneContainerProc;
    containerClass.hInstance = GetModuleHandleW(nullptr);
    containerClass.lpszClassName = L"FluentShellTestPaneHost";
    RegisterClassExW(&containerClass);

    HWND root = CreateWindowExW(0, L"Static", L"form-rows", WS_OVERLAPPEDWINDOW,
        30, 30, 360, 280, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND container = root ? CreateWindowExW(0, L"FluentShellTestPaneHost", L"",
        WS_CHILD | WS_VISIBLE, 0, 0, 300, 94, root, reinterpret_cast<HMENU>(700),
        GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(root && container, "form-row test container was not created");
    if (!root || !container) {
        if (root) DestroyWindow(root);
        return;
    }
    // Three rows, 26 px tall (a pane's minimum extent is 24), 8 px between a label and its
    // field and between rows: every uncovered piece is a thin strip, so the rows tile.
    for (int row = 0; row < 3; ++row) {
        const int top = row * 34;
        CreateWindowExW(0, L"Static", L"label", WS_CHILD | WS_VISIBLE,
            0, top, 90, 26, container, nullptr, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"Static", L"field", WS_CHILD | WS_VISIBLE,
            98, top, 202, 26, container, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    ShowWindow(root, SW_SHOWNOACTIVATE);
    UpdateWindow(root);

    Translation::ControlKind kind{};
    std::wstring reason;
    Check(Translation::ClassifyControl(container, kind, reason) &&
          kind == Translation::ControlKind::PaneContainer,
        "a container of label/field rows was not admitted");
    Translation::ControlNode node;
    node.kind = Translation::ControlKind::PaneContainer;
    reason.clear();
    Check(Translation::CaptureControlDetail(container, node, reason) && node.splits.empty(),
        "a strip between a label and its field was offered as a draggable split");
    if (!node.splits.empty()) std::wcerr << L"form-row splits=" << node.splits.size() << L'\n';
    DestroyWindow(root);
}

void TestVirtualDialogSnapshots() {
    using Translation::DialogTextMeasure;
    std::unordered_map<uint64_t, int> results;
    const auto messageBox = Translation::BuildMessageBoxSnapshot(
        nullptr, L"Save & exit?", L"Oracle",
        MB_YESNOCANCEL | MB_ICONEXCLAMATION | MB_DEFBUTTON2, results);
    Check(messageBox.surfaceKind == Translation::SurfaceKind::MessageBox &&
          messageBox.icon == L"warning" && messageBox.canCancel &&
          messageBox.nodes.size() == 4 && results.size() == 3,
        "MessageBox translation did not describe its buttons");
    CheckVirtualDialogSnapshot(messageBox, "translated MessageBox broke a protocol invariant");

    // The dialog is as large as its text: a short message is a small dialog, its text
    // beside the icon, and the buttons right-aligned in the band below it.
    const DialogTextMeasure measure(messageBox.dpi);
    const RECT& client = messageBox.clientBounds;
    const auto& text = messageBox.nodes.front();
    Check(client.right < measure.Px(480) && client.bottom < measure.Px(200),
        "a short MessageBox was not sized to its text");
    Check(text.kind == Translation::ControlKind::StaticText && text.text == L"Save && exit?" &&
          text.rect.left >= measure.Px(20 + 32) && text.rect.right <= client.right,
        "MessageBox text was not escaped or not placed beside its icon");
    LONG previousRight = 0;
    for (size_t index = 1; index < messageBox.nodes.size(); ++index) {
        const auto& button = messageBox.nodes[index];
        Check(button.kind == Translation::ControlKind::Button && button.rect.top >= text.rect.bottom &&
              button.rect.bottom <= client.bottom && button.rect.left >= previousRight &&
              button.text == Translation::DialogButtonLabel(results.at(button.nodeId)),
            "MessageBox buttons are not a band of labelled buttons under the text");
        previousRight = button.rect.right;
    }
    Check(client.right - previousRight == measure.Px(20),
        "MessageBox buttons are not right-aligned at the margin");
    Check(messageBox.nodes[2].isDefault && !messageBox.nodes[1].isDefault,
        "MB_DEFBUTTON2 did not make the second button the default");
    const LONG frameWidth = messageBox.bounds.right - messageBox.bounds.left;
    Check(frameWidth > client.right, "MessageBox bounds do not include the window frame");

    std::unordered_map<uint64_t, int> okResults;
    const auto okBox = Translation::BuildMessageBoxSnapshot(nullptr, L"Done.", L"", MB_OK, okResults);
    Check(okBox.canCancel && okBox.icon == L"none" && okBox.nodes.front().rect.left == measure.Px(20),
        "a lone OK message box cannot be dismissed with Escape or reserves icon space");

    // Links: <a href> markup becomes the visible label, reported by its href.
    const auto parsed = Translation::ParseTaskDialogLinks(
        L"Visit <a href=\"https://example.test/\">the site</a> or <A HREF='help'>help</A>.");
    Check(parsed.text == L"Visit the site or help." && parsed.labels.size() == 2 &&
          parsed.hrefs[0] == L"https://example.test/" && parsed.hrefs[1] == L"help",
        "task dialog link markup was not parsed");

    // A full task dialog: every element in one snapshot with unique IDs and z-order.
    Translation::TaskDialogModel model;
    model.title = L"Oracle";
    model.instruction = L"Main instruction";
    model.content = L"Body content with <a href=\"x\">a link</a>.";
    model.expandedInformation = L"Expanded details";
    model.footer = L"Footer";
    model.footerIcon.standard = L"info";
    model.verification = L"Do not ask again";
    model.mainIcon.standard = L"warning";
    model.hyperlinks = true;
    model.canCancel = true;
    model.progress = Translation::TaskDialogProgress::Normal;
    model.progressPosition = 40;
    model.radios = { { 10, L"&First", true }, { 11, L"&Second", true } };
    model.selectedRadio = 11;
    Translation::TaskDialogButtonModel link;
    link.id = 100;
    link.text = L"&Continue";
    link.note = L"Keeps going";
    link.commandLink = true;
    model.buttons.push_back(link);
    model.buttons.push_back({ IDOK, L"OK" });
    model.buttons.push_back({ IDCANCEL, L"Cancel" });
    Translation::TaskDialogElements elements;
    const auto taskDialog = Translation::BuildTaskDialogSnapshot(
        model, nullptr, nullptr, 96, std::nullopt, elements);
    CheckVirtualDialogSnapshot(taskDialog, "translated TaskDialog broke a protocol invariant");
    size_t radios = 0, buttons = 0, texts = 0, verification = 0, expando = 0;
    for (const auto& [_, element] : elements) {
        switch (element.kind) {
        case Translation::TaskDialogElementKind::Radio: ++radios; break;
        case Translation::TaskDialogElementKind::Button: ++buttons; break;
        case Translation::TaskDialogElementKind::Text: ++texts; break;
        case Translation::TaskDialogElementKind::Verification: ++verification; break;
        case Translation::TaskDialogElementKind::Expando: ++expando; break;
        }
    }
    Check(taskDialog.surfaceKind == Translation::SurfaceKind::TaskDialog && taskDialog.icon == L"warning" &&
          radios == 2 && buttons == 3 && texts == 1 && verification == 1 && expando == 1,
        "TaskDialog translation did not describe every element");
    const auto nodeById = [](const Translation::WindowSnapshot& snapshot, uint64_t id) {
        return std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [&](const auto& node) { return node.nodeId == id; });
    };
    const auto commandLink = std::find_if(taskDialog.nodes.begin(), taskDialog.nodes.end(),
        [](const auto& node) { return node.presentationVariant == L"commandLink"; });
    Check(commandLink != taskDialog.nodes.end() && commandLink->helpText == L"Keeps going" &&
          commandLink->controlId == 100,
        "a command link lost its note");
    const auto selected = std::count_if(taskDialog.nodes.begin(), taskDialog.nodes.end(),
        [](const auto& node) { return node.kind == Translation::ControlKind::RadioButton && node.checked == 1; });
    Check(selected == 1, "TaskDialog radio selection is not exactly the default radio");
    Check(nodeById(taskDialog, 4) == taskDialog.nodes.end(), "collapsed expanded information was projected");

    // Expanding keeps every element's identity and grows the dialog downward.
    model.expanded = true;
    Translation::TaskDialogElements expandedElements;
    const auto expanded = Translation::BuildTaskDialogSnapshot(
        model, nullptr, nullptr, 96, POINT{ 0, 0 }, expandedElements);
    Check(nodeById(expanded, 4) != expanded.nodes.end() &&
          expanded.clientBounds.bottom > taskDialog.clientBounds.bottom &&
          expanded.nodes.size() == taskDialog.nodes.size() + 1,
        "expanding a TaskDialog did not add its details in place");
    for (const auto& node : taskDialog.nodes) {
        Check(nodeById(expanded, node.nodeId) != expanded.nodes.end(),
            "a TaskDialog rebuild changed an element's node ID");
    }

    // An empty text block is skipped rather than projected as a blank node.
    Translation::TaskDialogModel sparse;
    sparse.instruction = L"Only an instruction";
    sparse.buttons.push_back({ IDOK, L"OK" });
    Translation::TaskDialogElements sparseElements;
    const auto minimal = Translation::BuildTaskDialogSnapshot(
        sparse, nullptr, nullptr, 96, std::nullopt, sparseElements);
    Check(minimal.nodes.size() == 2 && minimal.icon == L"none" && !minimal.title.empty(),
        "a sparse TaskDialog was not reduced to its real content");
    CheckVirtualDialogSnapshot(minimal, "sparse TaskDialog broke a protocol invariant");
}

// The task dialog's callback contract: notifications reach the callback with the
// host HWND on the calling thread, TDM_* messages change the published dialog, and
// S_FALSE from TDN_BUTTON_CLICKED keeps it open.
struct TaskDialogProbe final {
    std::vector<UINT> notifications;
    int refusals = 1;
    HWND host = nullptr;
    std::wstring href;
};

HRESULT CALLBACK ProbeTaskDialogCallback(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LONG_PTR data) {
    auto* probe = reinterpret_cast<TaskDialogProbe*>(data);
    probe->notifications.push_back(message);
    probe->host = hwnd;
    if (message == TDN_DIALOG_CONSTRUCTED) SendMessageW(hwnd, TDM_ENABLE_BUTTON, IDOK, FALSE);
    if (message == TDN_HYPERLINK_CLICKED && lParam) probe->href = reinterpret_cast<PCWSTR>(lParam);
    if (message == TDN_BUTTON_CLICKED && wParam == IDCANCEL && probe->refusals > 0) {
        --probe->refusals;
        return S_FALSE;
    }
    return S_OK;
}

void PumpTaskDialogHost() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
}

void TestVirtualTaskDialogCallbacks() {
    TaskDialogProbe probe;
    const TASKDIALOG_BUTTON radios[] = { { 20, L"One" }, { 21, L"Two" } };
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_SHOW_PROGRESS_BAR;
    config.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
    config.pszMainInstruction = L"Probe";
    config.pszContent = L"See <a href=\"probe-link\">the link</a>.";
    config.cRadioButtons = 2;
    config.pRadioButtons = radios;
    config.pfCallback = ProbeTaskDialogCallback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&probe);
    std::wstring reason;
    auto dialog = Translation::VirtualTaskDialog::Create(config, nullptr, reason);
    Check(dialog != nullptr, "a callback task dialog was refused");
    if (!dialog) return;
    const auto initial = dialog->InitialSnapshot();
    Check(!initial.nodes.empty(), "the callback task dialog has no content");
    std::vector<Translation::WindowSnapshot> published;
    bool completed = false;
    Translation::VirtualTaskDialog::Sink sink;
    std::vector<uint64_t> publishedEvents;
    sink.publish = [&](Translation::WindowSnapshot snapshot, uint64_t eventId) {
        published.push_back(std::move(snapshot));
        publishedEvents.push_back(eventId);
    };
    sink.complete = [&] { completed = true; };
    Check(dialog->Start(std::move(sink)), "the task dialog host window was not created");
    dialog->Announce();
    Check(probe.notifications.size() >= 2 && probe.notifications[0] == TDN_DIALOG_CONSTRUCTED &&
          probe.notifications[1] == TDN_CREATED && probe.host == dialog->Host(),
        "the callback did not see the dialog created on its host window");
    Check(!published.empty() && std::any_of(published.back().nodes.begin(), published.back().nodes.end(),
        [](const auto& node) { return node.controlId == IDOK && !node.enabled; }),
        "TDM_ENABLE_BUTTON from TDN_DIALOG_CONSTRUCTED did not disable the button");
    if (published.empty()) return;

    // A radio click and a link click arrive as renderer actions.
    const auto current = published.back();
    const auto radio = std::find_if(current.nodes.begin(), current.nodes.end(),
        [](const auto& node) { return node.controlId == 21; });
    const auto text = std::find_if(current.nodes.begin(), current.nodes.end(),
        [](const auto& node) { return node.kind == Translation::ControlKind::SysLink; });
    Check(radio != current.nodes.end() && text != current.nodes.end(), "the probe dialog lost a radio or its link");
    if (radio == current.nodes.end() || text == current.nodes.end()) return;
    Translation::ActionRequest select;
    select.action = L"setCheck";
    select.nodeId = radio->nodeId;
    select.integerValue = 1;
    select.eventId = 7;
    dialog->PostAction(select);
    Translation::ActionRequest follow;
    follow.action = L"invoke";
    follow.nodeId = text->nodeId;
    follow.eventId = 9;
    dialog->PostAction(follow);
    PumpTaskDialogHost();
    Check(dialog->Result().radioButton == 21 && probe.href == L"probe-link",
        "a radio or link action did not reach the callback");
    // Every accepted action is answered by a patch naming its event, or the renderer keeps
    // the click pending for good.
    Check(std::find(publishedEvents.begin(), publishedEvents.end(), 7) != publishedEvents.end() &&
          std::find(publishedEvents.begin(), publishedEvents.end(), 9) != publishedEvents.end(),
        "a radio or link action was not answered with a patch naming its event");

    SendMessageW(dialog->Host(), TDM_SET_PROGRESS_BAR_POS, 60, 0);
    Check(std::any_of(published.back().nodes.begin(), published.back().nodes.end(),
        [](const auto& node) { return node.kind == Translation::ControlKind::ProgressBar && node.position == 60; }),
        "TDM_SET_PROGRESS_BAR_POS did not move the progress bar");

    // The first Cancel is refused by the callback, the second closes the dialog.
    Translation::ActionRequest close;
    close.action = L"close";
    dialog->PostAction(close);
    PumpTaskDialogHost();
    Check(!completed && !dialog->Completed(), "S_FALSE from TDN_BUTTON_CLICKED did not keep the dialog open");
    SendMessageW(dialog->Host(), TDM_CLICK_BUTTON, IDCANCEL, 0);
    Check(completed && dialog->Result().button == IDCANCEL, "TDM_CLICK_BUTTON did not close the dialog");
    dialog->Finish();
    Check(probe.notifications.back() == TDN_DESTROYED && dialog->Host() == nullptr,
        "the task dialog was not destroyed with TDN_DESTROYED");
}

// A container's painted bands can flicker between two renderings from one capture to the next;
// reconcile publishes such a change only once two captures agree, which needs to tell "the pixels
// moved" from "anything else moved".
void TestChromePixelsOnlyChange() {
    Translation::WindowSnapshot canonical;
    canonical.generation = 1;
    Translation::ControlNode node;
    node.nodeId = 1;
    node.generation = 1;
    node.kind = Translation::ControlKind::PaneContainer;
    node.rect = { 0, 0, 100, 40 };
    Translation::ChromeRegionSnapshot band;
    band.rect = { 0, 0, 4, 2 };
    band.imageWidth = 4;
    band.imageHeight = 2;
    band.imageFormat = L"bgra8-premultiplied";
    band.imageData.assign(32, 0x10);
    node.chromeRegions.push_back(band);
    canonical.nodes.push_back(node);
    const uint64_t canonicalFingerprint = Translation::SnapshotFingerprint(canonical);

    Translation::WindowSnapshot next = canonical;
    Check(!Translation::OnlyChromePixelsChanged(canonical, next, canonicalFingerprint),
        "an unchanged capture was reported as a pixels-only change");

    next.nodes[0].chromeRegions[0].imageData.assign(32, 0x80);
    Check(Translation::SnapshotFingerprint(next) != canonicalFingerprint,
        "the fingerprint ignores the pixels of a chrome region");
    Check(Translation::OnlyChromePixelsChanged(canonical, next, canonicalFingerprint),
        "a capture that only repainted a chrome region was not recognised");
    Check(canonical.nodes[0].chromeRegions[0].imageData.front() == 0x10 &&
          next.nodes[0].chromeRegions[0].imageData.front() == 0x80 &&
          Translation::SnapshotFingerprint(canonical) == canonicalFingerprint,
        "the comparison left a snapshot's pixels swapped");

    Translation::WindowSnapshot moved = next;
    moved.nodes[0].rect = { 0, 0, 120, 40 };
    Check(!Translation::OnlyChromePixelsChanged(canonical, moved, canonicalFingerprint),
        "a geometry change was taken for a pixels-only change");

    Translation::WindowSnapshot retitled = next;
    retitled.title = L"renamed";
    Check(!Translation::OnlyChromePixelsChanged(canonical, retitled, canonicalFingerprint),
        "a window change was taken for a pixels-only change");

    Translation::WindowSnapshot resized = next;
    resized.nodes[0].chromeRegions[0].rect = { 0, 0, 8, 2 };
    Check(!Translation::OnlyChromePixelsChanged(canonical, resized, canonicalFingerprint),
        "a chrome region that moved was taken for a pixels-only change");
}

// A task dialog's own labels come from comctl32 (or English without it), and the application's
// expando text takes their place -- for both states when it names only one.
void TestTaskDialogLabels() {
    for (const int id : { IDOK, IDCANCEL, IDYES, IDNO, IDRETRY, IDCLOSE }) {
        const std::wstring label = Translation::TaskDialogButtonLabel(id);
        Check(!label.empty() && label.find(L'%') == std::wstring::npos,
            "a task dialog common button has no usable label");
    }
    const std::wstring collapsed = Translation::TaskDialogExpandoLabel(false);
    const std::wstring expanded = Translation::TaskDialogExpandoLabel(true);
    Check(!collapsed.empty() && !expanded.empty() && collapsed != expanded,
        "the expando button has the same label in both states");

    const auto expandoText = [](bool expand, const wchar_t* collapsedText, const wchar_t* expandedText) {
        Translation::TaskDialogModel model;
        model.instruction = L"Instruction";
        model.expandedInformation = L"Details";
        model.collapsedControlText = collapsedText;
        model.expandedControlText = expandedText;
        model.expanded = expand;
        model.buttons.push_back({ IDOK, L"OK" });
        Translation::TaskDialogElements elements;
        const auto snapshot = Translation::BuildTaskDialogSnapshot(model, nullptr, nullptr, 96, std::nullopt, elements);
        for (const auto& node : snapshot.nodes) {
            if (node.presentationVariant == L"expandoCollapsed" || node.presentationVariant == L"expandoExpanded") {
                return node.text;
            }
        }
        return std::wstring();
    };
    Check(expandoText(false, L"", L"") == collapsed && expandoText(true, L"", L"") == expanded,
        "the default expando labels were not used");
    Check(expandoText(false, L"More", L"Less") == L"More" && expandoText(true, L"More", L"Less") == L"Less",
        "the application's expando labels were not used per state");
    Check(expandoText(false, L"Toggle", L"") == L"Toggle" && expandoText(true, L"Toggle", L"") == L"Toggle",
        "a lone expando label was not used for both states");
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
        Check(node->stateImageList.empty() &&
              node->itemStateImages == std::vector<int>{ -1, -1, -1, -1 },
            "TreeView invented state images for items that draw none");
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
    // Overlay images are painted on the item icon, so the projected icon is the
    // composited glyph rather than a reason to restore the native window.
    if (icons) {
        Check(ImageList_SetOverlayImage(icons, 1, 1) != FALSE,
            "TreeView overlay image was not registered");
        TVITEMW overlayItem{};
        overlayItem.mask = TVIF_HANDLE | TVIF_STATE;
        overlayItem.hItem = events;
        overlayItem.stateMask = TVIS_OVERLAYMASK;
        overlayItem.state = INDEXTOOVERLAYMASK(1);
        Check(SendMessageW(tree, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&overlayItem)) != FALSE,
            "TreeView overlay state was not stored");
        Translation::ControlNode overlaid;
        overlaid.kind = Translation::ControlKind::TreeView;
        overlaid.style = treeStyle;
        reason.clear();
        Check(Translation::CaptureControlDetail(tree, overlaid, reason),
            "TreeView with an overlay image was rejected");
        if (!reason.empty()) std::wcerr << L"overlay rejection: " << reason << L'\n';
        Check(overlaid.itemImages.size() == 4 && overlaid.itemImages[3] != overlaid.itemImages[0],
            "TreeView overlay did not produce a distinct item icon");
        HICON composited = ImageList_GetIcon(icons, 0, ILD_NORMAL | INDEXTOOVERLAYMASK(1));
        std::vector<uint8_t> compositedPixels;
        uint32_t overlayWidth = 0;
        uint32_t overlayHeight = 0;
        std::wstring overlayFormat;
        std::wstring overlayReason;
        const bool overlayCopied = composited && Translation::CaptureOwnedIconPixels(
            composited, overlayWidth, overlayHeight, overlayFormat, compositedPixels, overlayReason);
        if (composited) DestroyIcon(composited);
        Check(overlayCopied && overlaid.itemImages[3] >= 0 &&
              static_cast<size_t>(overlaid.itemImages[3]) < overlaid.imageList.size() &&
              overlaid.imageList[static_cast<size_t>(overlaid.itemImages[3])].imageData == compositedPixels,
            "TreeView overlay icon was not the control's composited glyph");
        overlayItem.state = 0;
        SendMessageW(tree, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&overlayItem));
    }

    // State image index 0 means "draw nothing", so a referenced glyph is index 1.
    // The same bits without a list are TVIS_USERMASK and must not refuse the tree.
    const auto addColor = [](HIMAGELIST list, uint8_t red, uint8_t green, uint8_t blue) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 16;
        info.bmiHeader.biHeight = -16;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        const HDC screen = GetDC(nullptr);
        const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (screen) ReleaseDC(nullptr, screen);
        if (!bitmap || !bits) return -1;
        auto* pixels = static_cast<uint32_t*>(bits);
        const uint32_t pixel = static_cast<uint32_t>(blue) |
            (static_cast<uint32_t>(green) << 8) |
            (static_cast<uint32_t>(red) << 16) |
            0xFF000000u;
        for (int i = 0; i < 16 * 16; ++i) pixels[i] = pixel;
        const int index = ImageList_Add(list, bitmap, nullptr);
        DeleteObject(bitmap);
        return index;
    };
    HIMAGELIST states = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 3, 1);
    Check(states && addColor(states, 0, 0, 0) == 0 && addColor(states, 220, 20, 20) == 1 &&
          addColor(states, 20, 20, 220) == 2,
        "TreeView state image glyphs were not created");
    SendMessageW(tree, TVM_SETIMAGELIST, TVSIL_STATE, reinterpret_cast<LPARAM>(states));
    reason.clear();
    Check(Translation::ClassifyControl(tree, kind, reason) &&
          kind == Translation::ControlKind::TreeView,
        "a TreeView state image list was rejected");
    TVITEMW stateItem{};
    stateItem.mask = TVIF_HANDLE | TVIF_STATE;
    stateItem.hItem = root;
    stateItem.stateMask = TVIS_STATEIMAGEMASK;
    stateItem.state = INDEXTOSTATEIMAGEMASK(1);
    Check(SendMessageW(tree, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&stateItem)) != FALSE,
        "TreeView state image was not stored");
    stateItem.hItem = services;
    stateItem.state = INDEXTOSTATEIMAGEMASK(2);
    SendMessageW(tree, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&stateItem));
    Translation::ControlNode stated;
    stated.kind = Translation::ControlKind::TreeView;
    stated.style = treeStyle;
    reason.clear();
    Check(Translation::CaptureControlDetail(tree, stated, reason),
        "TreeView with state images was rejected");
    if (!reason.empty()) std::wcerr << L"state-image rejection: " << reason << L'\n';
    Check(stated.itemStateImages == std::vector<int>{ 0, 1, -1, -1 } &&
          stated.stateImageList.size() == 2 &&
          stated.stateImageList[0].imageData != stated.stateImageList[1].imageData,
        "TreeView state images were not captured as a separate list");
    HICON stateGlyph = states ? ImageList_GetIcon(states, 1, ILD_NORMAL) : nullptr;
    std::vector<uint8_t> statePixels;
    uint32_t stateWidth = 0;
    uint32_t stateHeight = 0;
    std::wstring stateFormat;
    std::wstring stateReason;
    const bool stateCopied = stateGlyph && Translation::CaptureOwnedIconPixels(
        stateGlyph, stateWidth, stateHeight, stateFormat, statePixels, stateReason);
    if (stateGlyph) DestroyIcon(stateGlyph);
    Check(stateCopied && !stated.stateImageList.empty() &&
          stated.stateImageList[0].imageData == statePixels,
        "TreeView state image pixels were not image-list index 1");
    if (!stated.itemStateImages.empty()) {
        Translation::WindowSnapshot owned;
        owned.nodes.push_back(stated);
        const auto stateFingerprint = Translation::SnapshotFingerprint(owned);
        owned.nodes[0].itemStateImages[0] = 1;
        Check(stateFingerprint != Translation::SnapshotFingerprint(owned),
            "TreeView state image index was omitted from the fingerprint");
    }
    SendMessageW(tree, TVM_SETIMAGELIST, TVSIL_STATE, 0);
    if (states) ImageList_Destroy(states);
    stateItem.hItem = root;
    stateItem.state = INDEXTOSTATEIMAGEMASK(4);
    SendMessageW(tree, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&stateItem));
    stateItem.hItem = services;
    stateItem.state = 0;
    SendMessageW(tree, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&stateItem));
    reason.clear();
    Translation::ControlNode userMask;
    userMask.kind = Translation::ControlKind::TreeView;
    userMask.style = treeStyle;
    Check(Translation::CaptureControlDetail(tree, userMask, reason) &&
          userMask.stateImageList.empty() &&
          userMask.itemStateImages == std::vector<int>{ -1, -1, -1, -1 },
        "TreeView user-mask bits without a state image list were refused");
    stateItem.hItem = root;
    stateItem.state = 0;
    SendMessageW(tree, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&stateItem));
    reason.clear();
    Check(Translation::ClassifyControl(tree, kind, reason) &&
          kind == Translation::ControlKind::TreeView,
        "an icon-bearing TreeView was rejected");

    SetWindowLongPtrW(tree, GWL_STYLE, treeStyle | TVS_RTLREADING);
    reason.clear();
    Check(!Translation::ClassifyControl(tree, kind, reason) &&
          reason.find(L"TVS_RTLREADING") != std::wstring::npos &&
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
    // A tree with nothing inserted is a real console state, not an unsupported
    // control. The select that lands in that moment must not roll the window back.
    SendMessageW(tree, TVM_DELETEITEM, 0, reinterpret_cast<LPARAM>(TVI_ROOT));
    reason.clear();
    Translation::ControlNode empty;
    empty.kind = Translation::ControlKind::TreeView;
    empty.style = treeStyle;
    Check(Translation::CaptureControlDetail(tree, empty, reason) &&
          empty.items.empty() && empty.selectedIndex == -1 &&
          empty.itemDepths.empty() && empty.itemStateImages.empty() &&
          empty.imageList.empty() && empty.stateImageList.empty(),
        "an empty TreeView was rejected");
    if (!reason.empty()) std::wcerr << L"empty-tree rejection: " << reason << L'\n';
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

#include "MenuBarIdentityRegressionTests.h"
#include "SourceThreadStabilityRegressionTests.h"
#include "ListViewActivationTests.h"
#include "NativeListViewDoubleClickTests.h"
#include "ListViewActivationFallbackTests.h"
#include "ItemContextMenuTests.h"
#include "WinFormsOwnerDrawTests.h"
#include "ScrollingContainerTests.h"
#include "EventViewerSurfaceTests.h"
#include "ActionsPaneIslandTests.h"
#include "LabelSessionTests.h"
#include "ComboBoxExTests.h"
#include "MstscPresentationTests.h"
#include "ObjectPickerTests.h"
#include "PassivePictureTests.h"
#include "TileListViewTests.h"
#include "SplitButtonTests.h"
#include "TreeViewCheckBoxTests.h"
#include "OwnerDrawListBoxTests.h"

} // namespace

// A rollback suppression holds only while its window carries the stamp it was made with, so a
// destroyed window, or one created at a reused HWND, must never inherit the rollback.
struct FakeWindowStore final {
    struct State final {
        std::unordered_set<int> alive;
        std::unordered_map<int, uintptr_t> stamps;
        bool refuseStamps = false;
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    bool Attach(int window, uintptr_t stamp) const {
        if (state->refuseStamps) return false;
        state->stamps[window] = stamp;
        return true;
    }
    uintptr_t Stamp(int window) const {
        const auto found = state->stamps.find(window);
        return found == state->stamps.end() ? 0 : found->second;
    }
    void Detach(int window) const { state->stamps.erase(window); }
    bool Alive(int window) const { return state->alive.contains(window); }
};

void TestRootSuppressionLedger() {
    using Ledger = FluentShell::Bridge::Translation::RootSuppressionLedger<int, FakeWindowStore>;
    FakeWindowStore store;
    const auto& state = store.state;
    state->alive = { 1, 2, 3 };
    Ledger ledger(store);

    Check(ledger.Suppress(1), "a live root could not be suppressed");
    Check(ledger.IsSuppressed(1), "a suppressed root is not suppressed");
    Check(!ledger.IsSuppressed(2), "an unrelated window is suppressed");
    Check(!ledger.Suppress(9), "a window that does not exist was suppressed");

    // Destroying the window drops its property list; a new window at the same handle has no stamp.
    state->alive.erase(1);
    state->stamps.erase(1);
    state->alive.insert(1);
    Check(!ledger.IsSuppressed(1), "a window at a reused handle inherited a rollback");
    Check(ledger.Count() == 0, "a stale suppression was not dropped");

    Check(ledger.Suppress(2), "a second root could not be suppressed");
    ledger.Allow(2);
    Check(!ledger.IsSuppressed(2), "an allowed root is still suppressed");
    Check(state->stamps.count(2) == 0, "allowing a root left its stamp on the window");

    // A window that cannot carry the stamp is not recorded as suppressed.
    state->refuseStamps = true;
    Check(!ledger.Suppress(3), "a root without a stamp was recorded as suppressed");
    Check(!ledger.IsSuppressed(3), "an unstamped root reads as suppressed");
    state->refuseStamps = false;

    // A suppressed window that is destroyed ends its suppression.
    Check(ledger.Suppress(3), "a live root could not be suppressed again");
    state->alive.erase(3);
    Check(!ledger.IsSuppressed(3), "a destroyed window stayed suppressed");
    Check(ledger.Count() == 0, "a destroyed window's entry was kept");
}

// The GUI finds a root's rollback event by this name, so the format is part of the contract.
void TestRollbackEventName() {
    using FluentShell::Bridge::Translation::RollbackEventName;
    Check(RollbackEventName(1234, reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1a2b))) ==
            L"Local\\FluentShell.Rollback.1234.1a2b",
        "a rollback event name does not match the GUI's contract");
    Check(RollbackEventName(1234, reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1a2b))) !=
            RollbackEventName(1235, reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1a2b))),
        "rollback event names do not separate processes");
    Check(RollbackEventName(1234, reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1a2b))) !=
            RollbackEventName(1234, reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1a2c))),
        "rollback event names do not separate windows");
}

int wmain() {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    SetUnhandledExceptionFilter(ReportUnhandledException);
    // Snapshot serialization uses WinRT JSON, so the apartment must exist before
    // the first test that serializes a frame.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    // Capture always runs inside a per-monitor-aware scope, so every rectangle it
    // reads is in physical pixels.  A DPI-unaware test process would have its own
    // windows virtualized by DWM, leaving window rectangles and the geometry a
    // control reports about itself in two different coordinate spaces -- which is
    // precisely what MessageGeometryScale corrects for a real target.  The tests
    // declare the same awareness so their fixtures live in one space.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    TestVisibleUiaBoundsClipping();
    TestGuiThreadDispatch();
    TestCrossThreadSurface();
    TestSnapshotDetailFingerprint();
    FluentShell::Tests::TestDirectUiVirtualImages(Check);
    FluentShell::Tests::TestDirectUiDetailEvidence(Check);
    FluentShell::Tests::TestNativeDocumentAdmission(Check);
    FluentShell::Tests::TestOwnerDrawComboBoxes(Check);
    {
        ListActivationCommonControls groups(ICC_LISTVIEW_CLASSES);
        Check(static_cast<bool>(groups), "grouped ListView common-controls v6 context failed");
        if (groups) {
            FluentShell::Tests::TestListViewGroups(Check);
            FluentShell::Tests::TestListViewGroupsWithoutText(Check);
        }
    }
    TestDirectUiEvidenceContracts();
    TestDirectUiInPlaceRoutes();
    INITCOMMONCONTROLSEX controls{ sizeof(controls),
        ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS };
    Check(InitCommonControlsEx(&controls) != FALSE,
        "common-control classes were not initialized");
    FluentShell::Tests::TestReferencedItemImagery(Check);
    TestHeaderValidation();
    TestPipeRoundTrip();
    TestRootSuppressionLedger();
    TestRollbackEventName();
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
    TestStaticDecorationCaptureAndRejection();
    TestAncestorEnabledTabOrderCapture();
    TestOwnerDrawButtonFace();
    TestTabOrderScopes();
    TestTabOrderRejectionSpecificity();
    TestStructuredCommonControlCapture();
    TestListViewCheckNotificationSemantics();
    TestTabControlCaptureAndSelection();
    TestTreeViewCaptureAndExpansion();
    TestTrackbarCaptureAndValue();
    TestMdiFrameCaptureAndCommands();
    TestMdiCapturePreservesNavigationAcrossModalDisable();
    TestStandardMenuCapture();
    TestMenuBarToolbarLifecycle();
    TestMenuBarCommandIdentityAndNativeRouting();
    TestSourceThreadCallbackLifetime();
    TestMenuActionBindingGeneration();
    TestDeferredListViewActivation();
    TestNativeListViewDoubleClickDispatch();
    TestListViewNativeActivationFallback();
    TestPopupMenuState();
    TestSourceThreadPopupTracking();
    TestPopupActionProtocol();
    TestItemContextMenuProtocol();
    TestItemContextMenuTracking();
    TestWinFormsOwnerDrawControls();
    TestWindowsFormsSpinButtons();
    TestScrollingContainer();
    TestSurfaceFilledContainers();
    TestReadOnlyRichEditAdapter();
    TestObscuredHostIsNotCaptured();
    TestListViewHeaderPlacementFlag();
    TestVirtualListWindow();
    TestEmptyReportListWithoutColumns();
    TestMmcBusyShieldIdentity();
    TestMenuActionValidationAndSerialization();
    TestToolbarCaptureBoundary();
    TestVirtualDialogSnapshots();
    TestVirtualTaskDialogCallbacks();
    TestChromePixelsOnlyChange();
    TestTaskDialogLabels();
    TestPaneContainerCaptureAndSplit();
    TestFormRowStripsAreNotSplits();
    TestUnownedModalDialogOwnerResolution();
    TestNestedModalDialogChain();
    TestTabStopGroupBoxIsNotATabStop();
    TestAnimationControlIsAnInertPicture();
    TestOverlappingSiblingZOrderCapture();
    TestAccessibleIslandBoundary();
    TestAccessiblePageTabs();
    TestMmcMessageViewAdapter();
    TestMmcDescriptionBarIdentity();
    TestTabStopProgressBarLeavesTraversal();
    TestFullyClippedChildIsNotCaptured();
    TestStaticBitmapCapture();
    TestUpDownAdapter();
    TestDateTimePickerDayNumbers();
    TestDateTimePickerAdapter();
    TestWizardSheetTabStripIsNotCaptured();
    TestIconButtonCapture();
    TestMultiSelectListBox();
    TestPerformanceChartLayout();
    TestPerformanceChartText();
    TestPerformanceChartSerialization();
    TestPerformanceChartActions();
    TestActionsPaneSections();
    TestTitleBarCloseCapture();
    TestApplicationLabelSessions();
    TestFocusAction();
    TestComboBoxExCapture();
    TestToolbarLabelBeside();
    TestCueBanners();
    TestDialogPaintedBands();
    TestObjectPickerControls();
    TestPassivePictureControls();
    TestTileListViewCapture();
    TestSplitButtons();
    TestTreeViewCheckBoxes();
    TestOwnerDrawListBoxes();
    {
        // A command link is drawn by the common-controls v6 button.
        ListActivationCommonControls buttonControls(ICC_STANDARD_CLASSES);
        Check(static_cast<bool>(buttonControls), "button v6 activation failed");
        if (buttonControls) TestCommandLinkButtons();
    }
    FluentShell::Tests::TestUiAutomationProjectionScope(Check);
    FluentShell::Tests::TestMmcHtmlDocumentAdmission(Check);
    FluentShell::Tests::TestListViewModes(Check);
    FluentShell::Tests::TestOwnerDataListViews(Check);
    FluentShell::Tests::TestOwnerDataListViewActions(Check);
    FluentShell::Tests::TestToolbarRadioGroups(Check);
    {
        // Pressed toolbar images are a v6 contract; keep other fixtures on the
        // legacy common-controls path they are intended to exercise.
        ListActivationCommonControls toolbarControls(ICC_BAR_CLASSES);
        Check(static_cast<bool>(toolbarControls), "toolbar v6 activation failed");
        if (toolbarControls) FluentShell::Tests::TestToolbarImageVariants(Check);
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " native protocol test(s) failed.\n";
        return 1;
    }
    std::cout << "Native protocol tests passed.\n";
    return 0;
}
