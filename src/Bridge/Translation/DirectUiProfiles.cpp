#include "DirectUiEngine.h"

#include <commctrl.h>
#include <oaidl.h>
#include <oleacc.h>
#include <UIAutomation.h>

// UIAutomation.h declares its provider interfaces against the COM base types,
// so the engine header (windows.h), the common-control styles, and the COM
// automation types must precede it.

namespace FluentShell::Bridge::Translation {
namespace {

// UIA control type ids from UIAutomation.h; kept symbolic so the profile
// table stays declarative and readable.
constexpr int kUIImage = UIA_ImageControlTypeId;
constexpr int kUIText = UIA_TextControlTypeId;
constexpr int kUIButton = UIA_ButtonControlTypeId;
constexpr int kUICheckBox = UIA_CheckBoxControlTypeId;
constexpr int kUIPane = UIA_PaneControlTypeId;

// ---------------------------------------------------------------------------
// MdSched 10.0.26100.7309 initial page.
//
// Root #32770 with one visible DirectUIHWND anchor, three visible
// CtrlNotifySink command wrappers, six hidden wrappers, four hidden
// ScrollBars, two hidden SysLinks, and exactly three visible Buttons: two
// command links (sorted top-to-bottom by the engine) and one plain push
// Cancel with control id 0. The dialog tab cycle contains exactly the three
// buttons. The native dialog tab cycle contains only the DirectUI host; the
// slot table pins the provider-owned order of the three buttons. MainIcon is
// the trusted 32x32 RT_GROUP_ICON 5000 resource of the
// admitted signed executable.
// ---------------------------------------------------------------------------
constexpr DirectUiSlot kMdSchedSlots[] = {
    { L"MainIcon", true, ControlKind::StaticIcon, L"mainIcon", -1,
      true, kUIImage, L"MainIcon", L"Element", true, false, false,
      L"", 0, 0, -1, DirectUiAction::None, false, false, 5000, 32, 32 },
    { L"MainInstruction", true, ControlKind::StaticText, L"instruction", -1,
      true, kUIText, L"MainInstruction", L"Element", true, false, false,
      L"", 0, 0, -1, DirectUiAction::None, false, false, 0, 0, 0 },
    { L"ContentText", true, ControlKind::StaticText, L"content", -1,
      true, kUIText, L"ContentText", L"Element", true, false, false,
      L"", 0, 0, -1, DirectUiAction::None, false, false, 0, 0, 0 },
    { L"CommandLink.0", true, ControlKind::Button, L"commandLink", 0,
      false, kUIButton, L"", L"CCCommandLink", true, true, true,
      L"Button", BS_COMMANDLINK, BS_DEFCOMMANDLINK, -1,
      DirectUiAction::HandoffClick, false, false, 0, 0, 0 },
    { L"CommandLink.1", true, ControlKind::Button, L"commandLink", 1,
      false, kUIButton, L"", L"CCCommandLink", true, true, true,
      L"Button", BS_COMMANDLINK, BS_DEFCOMMANDLINK, -1,
      DirectUiAction::HandoffClick, false, false, 0, 0, 0 },
    { L"Cancel", true, ControlKind::Button, L"standard", 2,
      false, kUIButton, L"", L"CCPushButton", true, true, true,
      L"Button", BS_PUSHBUTTON, 0, 0,
      DirectUiAction::HandoffClick, true, false, 0, 0, 0 },
};

// ---------------------------------------------------------------------------
// RecoveryDrive 10.0.26100.33296 first page.
//
// Root NativeHWNDHost with one visible DirectUIHWND anchor. Census: three
// visible CtrlNotifySink wrappers (next, cancel, and the active page host),
// two hidden wrappers (Finish and the inactive page host), zero hidden
// ScrollBars/SysLinks, one hidden Finish
// Button, one hidden #32770 page host, one visible #32770 page host, two
// page-host Static texts (one visible, one hidden), and the id-1000 checkbox.
//
// wizardicon has no trusted pixel source and is deliberately not projected,
// but remains a required semantic. The page host is structural; its visible
// Static child is projected as the canonical explanatory text. backbutton is
// projected disabled with no supported actions.
// ---------------------------------------------------------------------------
constexpr DirectUiSlot kRecoveryDriveSlots[] = {
    { L"backbutton", true, ControlKind::Button, L"standard", -1,
      true, kUIButton, L"backbutton", L"Button", false, true, true,
      L"", 0, 0, -1, DirectUiAction::None, false, false, 0, 0, 0,
      L"DirectUI", DirectUiBoundsScope::Root },
    { L"wizardicon", false, ControlKind::StaticIcon, L"", -1,
      true, kUIImage, L"wizardicon", L"Element", true, false, false,
      L"", 0, 0, -1, DirectUiAction::None, false, false, 0, 0, 0,
      L"DirectUI", DirectUiBoundsScope::Root },
    { L"wizardtitle", true, ControlKind::StaticText, L"wizardTitle", -1,
      true, kUIText, L"wizardtitle", L"Element", true, false, false,
      L"", 0, 0, -1, DirectUiAction::None, false, false, 0, 0, 0,
      L"DirectUI", DirectUiBoundsScope::Root },
    { L"headertitle", true, ControlKind::StaticText, L"header", -1,
      true, kUIText, L"headertitle", L"Element", true, false, false,
      L"", 0, 0, -1, DirectUiAction::None, false, false, 0, 0, 0,
      L"DirectUI", DirectUiBoundsScope::Root },
    { L"pageHost", false, ControlKind::StaticText, L"", -1,
      false, kUIPane, L"",
      L"Win32PropSheetPageHost", true, true, false,
      L"#32770", 0x50010444, 0x50010444, -1,
      DirectUiAction::None, false, false, 0, 0, 0,
      L"DirectUI", DirectUiBoundsScope::Anchor, 0xffffffffull },
    { L"pageText", true, ControlKind::StaticText, L"content", -1,
      false, kUIText, L"", L"Static", true, false, false,
      L"Static", 0x50020000, 0x50020000, -1,
      DirectUiAction::None, false, false, 0, 0, 0,
      L"Win32", DirectUiBoundsScope::Anchor, 0xffffffffull },
    { L"backupSystemFiles", true, ControlKind::CheckBox, L"standard", 0,
      false, kUICheckBox, L"1000", L"Button", true, true, true,
      L"Button", BS_AUTOCHECKBOX, BS_AUTOCHECKBOX, 1000,
      DirectUiAction::ToggleCheck, false, false, 0, 0, 0,
      L"Win32", DirectUiBoundsScope::Anchor, BS_TYPEMASK },
    { L"nextbutton", true, ControlKind::Button, L"standard", 1,
      false, kUIButton, L"nextbutton", L"CCPushButton", true, true, true,
      L"Button", BS_DEFPUSHBUTTON, BS_DEFPUSHBUTTON, 0,
      DirectUiAction::HandoffClick, false, true, 0, 0, 0 },
    { L"cancelbutton", true, ControlKind::Button, L"standard", 2,
      false, kUIButton, L"cancelbutton", L"CCPushButton", true, true, true,
      L"Button", BS_PUSHBUTTON, BS_PUSHBUTTON, 0,
      DirectUiAction::HandoffClick, true, false, 0, 0, 0 },
};

} // namespace

const DirectUiWindowProfile kDirectUiProfiles[] = {
    {
        L"microsoft.mdsched.directui",
        L"initial",
        L"MdSched.exe",
        { 10, 0, 26100, 7309 },
        L"#32770",
        { 3, 6, 4, 2, 0, 0, 0, 0, 0 },
        kMdSchedSlots,
        std::size(kMdSchedSlots),
        true,
        6, 24,
    },
    {
        L"microsoft.recoverydrive.directui",
        L"first",
        L"RecoveryDrive.exe",
        { 10, 0, 26100, 33296 },
        L"NativeHWNDHost",
        { 3, 2, 0, 0, 1, 1, 1, 1, 1 },
        kRecoveryDriveSlots,
        std::size(kRecoveryDriveSlots),
        true,
        8, 40,
    },
};

const size_t kDirectUiProfileCount = std::size(kDirectUiProfiles);

} // namespace FluentShell::Bridge::Translation
