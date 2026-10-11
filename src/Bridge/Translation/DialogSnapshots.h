// Snapshot builders for the translated dialog lane.
//
// A MessageBox or TaskDialog call is answered by the Bridge itself: no native
// dialog window is ever created, so these snapshots describe fully virtual
// surfaces whose nodes carry no HWND.  They live outside RendererSession so the
// native test suite can hold them to the same cross-node invariants the renderer
// enforces on admission -- unique nonzero node IDs, unique z-index, and nodes that
// claim no native window.
//
// The layout is the dialog's own: a dialog is as large as its text, measured with
// the font the renderer draws it in (12 DIP body, 15 DIP semibold main
// instruction) at the owner's DPI, and laid out the way the native dialogs are --
// icon and text in the content area, the buttons right-aligned in a command band
// below it.  The renderer draws the band from `kDialogBandPaddingDip` above its
// first row, so both sides agree on where the band starts.
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "WindowSnapshot.h"

struct IDWriteTextFormat;

namespace FluentShell::Bridge::Translation {

// The space between the top of a dialog's command band and its first row, in DIPs.
// The renderer derives the band's top edge from the first band control with it.
inline constexpr int kDialogBandPaddingDip = 12;

// Measures dialog text the way the renderer lays it out: DirectWrite, Segoe UI
// with the system fallback, at the renderer's sizes, word-wrapped.
// All results are physical pixels at the requested DPI.
class DialogTextMeasure final {
public:
    enum class Style { Body, Instruction, Strong };

    explicit DialogTextMeasure(UINT dpi);
    ~DialogTextMeasure();
    DialogTextMeasure(const DialogTextMeasure&) = delete;
    DialogTextMeasure& operator=(const DialogTextMeasure&) = delete;

    UINT Dpi() const noexcept { return dpi_; }
    LONG Px(int dip) const noexcept;
    // The wrapped extent of `text` within `maxWidth` pixels.  An empty text has
    // a zero size.  `&` is drawn literally: callers pass display text.
    SIZE Measure(std::wstring_view text, LONG maxWidth, Style style = Style::Body) const;
    // The width of the longest unwrapped line of `text`.
    LONG SingleLineWidth(std::wstring_view text, Style style = Style::Body) const;

private:
    IDWriteTextFormat* FormatFor(Style style) const noexcept;
    bool Layout(std::wstring_view text, float maxWidth, Style style, bool wrap,
        float& width, float& height) const;

    UINT dpi_ = 96;
    IDWriteTextFormat* formats_[3]{};
};

// Centers a dialog of the requested size over its owner, or over the primary
// monitor's work area when the call has no owner window.
RECT CenteredBounds(HWND owner, LONG width, LONG height);

// The outer window rectangle of a dialog frame around a client area of the given
// size, at `origin` (top-left), or centered over `reference` when there is none.
RECT DialogWindowBounds(HWND reference, std::optional<POINT> origin, LONG clientWidth,
    LONG clientHeight, UINT dpi);

std::wstring MessageBoxIcon(UINT type);
std::wstring TaskDialogIcon(PCWSTR icon);
// The label user32 gives a MessageBox button in the user's language (`是(&Y)`,
// `確定`), falling back to the English label when user32 does not provide one.
std::wstring DialogButtonLabel(int id);
// The labels a task dialog gives its common buttons and the expando button, taken from the
// comctl32 that draws task dialogs (a MessageBox's come from user32), in the user's language;
// English when that module or string is not there.  `expanded` names the state the dialog is in:
// the label offers the opposite ("Hide details" while expanded).
std::wstring TaskDialogButtonLabel(int id);
std::wstring TaskDialogExpandoLabel(bool expanded);
// Doubles every `&` so text the native dialog draws literally survives the
// renderer's mnemonic processing.
std::wstring EscapeMnemonics(std::wstring_view text);
// Strips mnemonic markup the way a button draws its label.
std::wstring StripMnemonics(std::wstring_view text);

// Pixels of a dialog icon the application supplied (an HICON or an icon
// resource), copied when the dialog is built so the application may destroy it.
struct DialogIconImage final {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
    bool Empty() const noexcept { return pixels.empty(); }
};
bool CaptureDialogIcon(HICON icon, DialogIconImage& image);
// Loads an icon by resource name at `size` pixels: from `instance`, or a system
// icon (IDI_*) when `instance` is null.
bool LoadDialogIcon(HINSTANCE instance, PCWSTR name, int size, DialogIconImage& image);

// `results` receives the node ID to dialog result code mapping the surface needs
// in order to answer the original API call.  `customIcon` is an MB_USERICON
// picture; a standard icon travels as the surface icon instead.
WindowSnapshot BuildMessageBoxSnapshot(
    HWND owner,
    std::wstring_view text,
    std::wstring_view caption,
    UINT type,
    std::unordered_map<uint64_t, int>& results,
    const DialogIconImage* customIcon = nullptr);

// Everything a task dialog shows, resolved from its TASKDIALOGCONFIG (strings
// loaded, icons copied) and updated by the TDM_* messages the application sends.
struct TaskDialogButtonModel final {
    int id = 0;
    std::wstring text;
    // A command link's second line: the text after the first newline.
    std::wstring note;
    bool enabled = true;
    bool elevated = false;
    bool commandLink = false;
};

struct TaskDialogRadioModel final {
    int id = 0;
    std::wstring text;
    bool enabled = true;
};

struct TaskDialogIconModel final {
    // warning, error, info, shield or none; ignored when `image` carries pixels.
    std::wstring standard = L"none";
    DialogIconImage image;
    bool Present() const noexcept { return standard != L"none" || !image.Empty(); }
};

enum class TaskDialogProgress { None, Normal, Marquee };

struct TaskDialogModel final {
    std::wstring title;
    std::wstring instruction;
    std::wstring content;
    std::wstring expandedInformation;
    std::wstring footer;
    std::wstring verification;
    std::wstring expandedControlText;
    std::wstring collapsedControlText;
    TaskDialogIconModel mainIcon;
    TaskDialogIconModel footerIcon;
    // Band buttons and command links, in the order the application declared them.
    std::vector<TaskDialogButtonModel> buttons;
    std::vector<TaskDialogRadioModel> radios;
    int defaultButton = 0;
    // The selected radio button's ID, or 0 when none is selected.
    int selectedRadio = 0;
    bool verificationChecked = false;
    bool expanded = false;
    bool expandFooterArea = false;
    bool hyperlinks = false;
    bool canCancel = false;
    bool rtl = false;
    bool minimizable = false;
    bool sizeToContent = false;
    bool commandLinkIcons = true;
    // TASKDIALOGCONFIG::cxWidth: the client width in dialog units, or 0 to size.
    UINT widthDlu = 0;
    TaskDialogProgress progress = TaskDialogProgress::None;
    int progressMinimum = 0;
    int progressMaximum = 100;
    int progressPosition = 0;
    bool marqueeRunning = true;
};

// What a node of a built task dialog stands for, so an action on it can be
// answered: a button or radio button by ID, the verification check box, the
// expando button, or a text whose links are listed by index.
enum class TaskDialogElementKind { Button, Radio, Verification, Expando, Text };
struct TaskDialogElement final {
    TaskDialogElementKind kind = TaskDialogElementKind::Text;
    int id = 0;
    std::vector<std::wstring> hrefs;
};
using TaskDialogElements = std::unordered_map<uint64_t, TaskDialogElement>;

// The visible text and link targets of a TDF_ENABLE_HYPERLINKS string:
// `<a href="x">label</a>` becomes `label`, and `x` is reported on click.
struct TaskDialogLinkText final {
    std::wstring text;
    std::vector<std::wstring> labels;
    std::vector<std::wstring> hrefs;
};
TaskDialogLinkText ParseTaskDialogLinks(std::wstring_view markup);

// Builds the dialog at `dpi`.  `origin` keeps the window's top-left corner when an
// existing dialog is rebuilt (an expanded section, a navigated page); otherwise
// the dialog is centered over `reference`.  Node IDs are stable per element, so a
// rebuild keeps the identity of everything that is still shown.
WindowSnapshot BuildTaskDialogSnapshot(
    const TaskDialogModel& model,
    HWND owner,
    HWND reference,
    UINT dpi,
    std::optional<POINT> origin,
    TaskDialogElements& elements);

}  // namespace FluentShell::Bridge::Translation
