#include "DialogSnapshots.h"

#include <dwrite.h>
#include <psapi.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "../Ipc/Protocol.h"
#include "ControlAdapters.h"

#pragma comment(lib, "dwrite.lib")

namespace FluentShell::Bridge::Translation {
namespace {

// The renderer's sizes for dialog text, in DIPs: ControlFactory.NativeFontSize for
// body text and labels, and the main instruction's heading size.
constexpr int kBodyFontDip = 12;
constexpr int kInstructionFontDip = 15;

constexpr int kMarginDip = 20;
constexpr int kIconDip = 32;
constexpr int kIconGapDip = 12;
constexpr int kButtonHeightDip = 28;
constexpr int kButtonMinWidthDip = 88;
constexpr int kButtonGapDip = 8;
constexpr int kFooterIconDip = 16;

// Stable node IDs of a task dialog's elements.  Radio buttons and buttons are
// numbered from their own bases by declaration order.
constexpr uint64_t kMainIconNode = 1;
constexpr uint64_t kInstructionNode = 2;
constexpr uint64_t kContentNode = 3;
constexpr uint64_t kExpandedNode = 4;
constexpr uint64_t kProgressNode = 5;
constexpr uint64_t kExpandoNode = 6;
constexpr uint64_t kVerificationNode = 7;
constexpr uint64_t kFooterIconNode = 8;
constexpr uint64_t kFooterNode = 9;
constexpr uint64_t kRadioNodeBase = 100;
constexpr uint64_t kButtonNodeBase = 300;

RECT WorkAreaFor(HWND reference) {
    const HMONITOR monitor = reference
        ? MonitorFromWindow(reference, MONITOR_DEFAULTTOPRIMARY)
        : MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{ sizeof(info) };
    if (!GetMonitorInfoW(monitor, &info)) return { 0, 0, 1024, 768 };
    return info.rcWork;
}

UINT DialogDpi(HWND owner) {
    const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    return dpi ? dpi : 96;
}

ControlNode NewNode(const WindowSnapshot& snapshot, uint64_t nodeId, ControlKind kind, const RECT& rect) {
    ControlNode node;
    node.nodeId = nodeId;
    node.generation = snapshot.generation;
    node.kind = kind;
    // Paint order doubles as the z-index, which the protocol requires to be unique
    // per node: nodes.size() is the count already appended, so each node lands one
    // step above the previous one.
    node.zIndex = static_cast<int>(snapshot.nodes.size());
    node.rect = rect;
    node.visible = true;
    node.enabled = true;
    return node;
}

void AddIconNode(WindowSnapshot& snapshot, uint64_t nodeId, const DialogIconImage& image,
    LONG left, LONG top, LONG size) {
    if (image.Empty()) return;
    auto node = NewNode(snapshot, nodeId, ControlKind::StaticIcon,
        { left, top, left + size, top + size });
    node.imageWidth = image.width;
    node.imageHeight = image.height;
    node.imageFormat = L"bgra8-premultiplied";
    node.imageData = image.pixels;
    snapshot.nodes.push_back(std::move(node));
}

void InitializeDialogSnapshot(WindowSnapshot& snapshot, SurfaceKind kind, HWND owner,
    std::wstring_view title, std::wstring_view fallbackTitle, UINT dpi) {
    snapshot.surfaceId = Ipc::NewGuidString();
    snapshot.surfaceKind = kind;
    snapshot.modal = true;
    snapshot.generation = GetTickCount64();
    snapshot.revision = 1;
    snapshot.ownerHwnd = owner;
    snapshot.title = title.empty() ? std::wstring(fallbackTitle) : std::wstring(title);
    snapshot.dpi = dpi;
    snapshot.windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    snapshot.windowExStyle = WS_EX_DLGMODALFRAME;
    snapshot.showInTaskbar = owner == nullptr;
}

} // namespace

namespace {

IDWriteFactory* WriteFactory() noexcept {
    static IDWriteFactory* const factory = [] {
        IDWriteFactory* created = nullptr;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&created)))) {
            return static_cast<IDWriteFactory*>(nullptr);
        }
        return created;
    }();
    return factory;
}

} // namespace

// XAML lays text out with DirectWrite in Segoe UI (what its TextBlocks report through
// UIA) and the system font fallback for the user's language, so the same layout gives the
// renderer's own line breaks and line heights (CJK text included, which GDI's
// font linking draws smaller).
DialogTextMeasure::DialogTextMeasure(UINT dpi) : dpi_(dpi ? dpi : 96) {
    IDWriteFactory* factory = WriteFactory();
    if (!factory) return;
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (!GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH)) wcscpy_s(locale, L"en-us");
    auto make = [&](float size, DWRITE_FONT_WEIGHT weight) {
        IDWriteTextFormat* format = nullptr;
        if (FAILED(factory->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, locale, &format))) {
            return static_cast<IDWriteTextFormat*>(nullptr);
        }
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        return format;
    };
    formats_[0] = make(static_cast<float>(kBodyFontDip), DWRITE_FONT_WEIGHT_NORMAL);
    formats_[1] = make(static_cast<float>(kInstructionFontDip), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    formats_[2] = make(static_cast<float>(kBodyFontDip), DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

DialogTextMeasure::~DialogTextMeasure() {
    for (IDWriteTextFormat* format : formats_) {
        if (format) format->Release();
    }
}

LONG DialogTextMeasure::Px(int dip) const noexcept {
    return MulDiv(dip, static_cast<int>(dpi_), 96);
}

IDWriteTextFormat* DialogTextMeasure::FormatFor(Style style) const noexcept {
    switch (style) {
    case Style::Instruction: return formats_[1];
    case Style::Strong: return formats_[2];
    default: return formats_[0];
    }
}

// The layout's extent in DIPs, wrapped at `maxWidth` DIPs (or unwrapped).
bool DialogTextMeasure::Layout(std::wstring_view text, float maxWidth, Style style, bool wrap,
    float& width, float& height) const {
    IDWriteTextFormat* format = FormatFor(style);
    IDWriteFactory* factory = WriteFactory();
    if (!format || !factory) return false;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format,
            wrap ? maxWidth : 100000.0f, 100000.0f, &layout)) || !layout) {
        return false;
    }
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    const bool measured = SUCCEEDED(layout->GetMetrics(&metrics));
    layout->Release();
    if (!measured) return false;
    width = metrics.widthIncludingTrailingWhitespace;
    height = metrics.height;
    return true;
}

// XAML positions the same glyphs on whole pixels and comes out a few percent wider
// than this layout, so the text is wrapped at 95% of its slot and reported 5%
// wider: the renderer then never moves a word to a line the slot does not have.
constexpr float kRendererWidthFactor = 1.05f;

SIZE DialogTextMeasure::Measure(std::wstring_view text, LONG maxWidth, Style style) const {
    if (text.empty() || maxWidth <= 0) return { 0, 0 };
    const float scale = static_cast<float>(dpi_) / 96.0f;
    float width = 0;
    float height = 0;
    if (!Layout(text, (static_cast<float>(maxWidth) / scale - 4.0f) / kRendererWidthFactor,
            style, true, width, height)) {
        // Without DirectWrite, assume one em per character on 1.4 em lines.
        const LONG em = Px(style == Style::Instruction ? kInstructionFontDip : kBodyFontDip);
        const LONG perLine = (std::max)(1L, maxWidth / em);
        const LONG lines = (static_cast<LONG>(text.size()) + perLine - 1) / perLine;
        return { (std::min)(maxWidth, static_cast<LONG>(text.size()) * em), lines * em * 14 / 10 };
    }
    return { (std::min)(maxWidth, static_cast<LONG>(std::ceil((width * kRendererWidthFactor + 4.0f) * scale))),
        static_cast<LONG>(std::ceil((height + 2.0f) * scale)) };
}

LONG DialogTextMeasure::SingleLineWidth(std::wstring_view text, Style style) const {
    if (text.empty()) return 0;
    float width = 0;
    float height = 0;
    if (!Layout(text, 0, style, false, width, height)) {
        return static_cast<LONG>(text.size()) * Px(style == Style::Instruction ? kInstructionFontDip : kBodyFontDip);
    }
    return static_cast<LONG>(std::ceil((width * kRendererWidthFactor + 4.0f) * static_cast<float>(dpi_) / 96.0f));
}

RECT CenteredBounds(HWND owner, LONG width, LONG height) {
    RECT reference{};
    if (!owner || !IsWindowVisible(owner) || !GetWindowRect(owner, &reference)) reference = WorkAreaFor(owner);
    // The Win32 min/max macros are in scope here, so the algorithm calls are
    // parenthesized to keep them from being expanded.
    LONG x = reference.left + (reference.right - reference.left - width) / 2;
    LONG y = reference.top + (reference.bottom - reference.top - height) / 2;
    // A dialog larger than a small owner still opens on the owner's monitor.
    const RECT work = WorkAreaFor(owner);
    x = (std::max)(work.left, (std::min)(x, work.right - width));
    y = (std::max)(work.top, (std::min)(y, work.bottom - height));
    return { x, y, x + width, y + height };
}

RECT DialogWindowBounds(HWND reference, std::optional<POINT> origin, LONG clientWidth,
    LONG clientHeight, UINT dpi) {
    RECT frame{ 0, 0, clientWidth, clientHeight };
    // The renderer's proxy keeps WinUI's own frame, which is the standard frame of
    // an overlapped window at the dialog's DPI; the bounds are its window rectangle.
    if (!AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi)) {
        frame = { -8, -31, clientWidth + 8, clientHeight + 8 };
    }
    const LONG width = frame.right - frame.left;
    const LONG height = frame.bottom - frame.top;
    if (origin) return { origin->x, origin->y, origin->x + width, origin->y + height };
    return CenteredBounds(reference, width, height);
}

std::wstring MessageBoxIcon(UINT type) {
    switch (type & MB_ICONMASK) {
    case MB_ICONHAND: return L"error";
    case MB_ICONQUESTION: return L"question";
    case MB_ICONEXCLAMATION: return L"warning";
    case MB_ICONASTERISK: return L"info";
    default: return L"none";
    }
}

std::wstring TaskDialogIcon(PCWSTR icon) {
    if (!icon) return L"none";
    if (icon == TD_WARNING_ICON) return L"warning";
    if (icon == TD_ERROR_ICON) return L"error";
    if (icon == TD_INFORMATION_ICON) return L"info";
    if (icon == TD_SHIELD_ICON) return L"shield";
    // The shield variants comctl32 draws with a coloured banner (-5 to -9).
    const auto value = static_cast<SHORT>(reinterpret_cast<ULONG_PTR>(icon) & 0xffff);
    if (IS_INTRESOURCE(icon) && value >= -9 && value <= -5) return L"shield";
    return L"none";
}

std::wstring DialogButtonLabel(int id) {
    using MbGetString = LPCWSTR(WINAPI*)(UINT);
    static const auto getString = [] {
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<MbGetString>(GetProcAddress(user32, "MB_GetString")) : nullptr;
    }();
    if (getString && id >= IDOK && id <= IDCONTINUE) {
        // MB_GetString is indexed by the result code minus one.
        const LPCWSTR label = getString(static_cast<UINT>(id - 1));
        if (label && *label) return label;
    }
    switch (id) {
    case IDOK: return L"OK";
    case IDCANCEL: return L"Cancel";
    case IDABORT: return L"&Abort";
    case IDRETRY: return L"&Retry";
    case IDIGNORE: return L"&Ignore";
    case IDYES: return L"&Yes";
    case IDNO: return L"&No";
    case IDCLOSE: return L"&Close";
    case IDHELP: return L"Help";
    case IDTRYAGAIN: return L"&Try Again";
    case IDCONTINUE: return L"&Continue";
    default: return L"OK";
    }
}

namespace {

// The comctl32 that draws task dialogs: version 6, the module that exports TaskDialogIndirect.  A
// process whose manifest asks for version 5 has none, and every label falls back to English.
HMODULE TaskDialogResourceModule() noexcept {
    static const HMODULE module = []() noexcept -> HMODULE {
        HMODULE modules[1024]{};
        DWORD needed = 0;
        if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return nullptr;
        const DWORD count = (std::min)(needed / static_cast<DWORD>(sizeof(HMODULE)),
            static_cast<DWORD>(std::size(modules)));
        for (DWORD index = 0; index < count; ++index) {
            wchar_t name[MAX_PATH]{};
            if (!GetModuleBaseNameW(GetCurrentProcess(), modules[index], name, MAX_PATH) ||
                _wcsicmp(name, L"comctl32.dll") != 0) {
                continue;
            }
            if (GetProcAddress(modules[index], "TaskDialogIndirect")) return modules[index];
        }
        return nullptr;
    }();
    return module;
}

// One of that module's own strings.  The ids are comctl32's (4240..4244 the common buttons, 4160
// Close, 4245/4246 the expando labels); a label is used only if it reads like one -- short, and not a
// format string -- so a build that renumbered them falls back to English instead of showing
// something unrelated.
std::wstring TaskDialogString(UINT id, std::wstring_view fallback) {
    if (const HMODULE module = TaskDialogResourceModule()) {
        wchar_t buffer[96]{};
        const int length = LoadStringW(module, id, buffer, static_cast<int>(std::size(buffer)));
        if (length > 0 && length < static_cast<int>(std::size(buffer)) - 1 &&
            std::wstring_view(buffer, static_cast<size_t>(length)).find(L'%') == std::wstring_view::npos) {
            return std::wstring(buffer, static_cast<size_t>(length));
        }
    }
    return std::wstring(fallback);
}

} // namespace

std::wstring TaskDialogButtonLabel(int id) {
    switch (id) {
    case IDOK: return TaskDialogString(4240, L"OK");
    case IDCANCEL: return TaskDialogString(4241, L"Cancel");
    case IDYES: return TaskDialogString(4242, L"&Yes");
    case IDNO: return TaskDialogString(4243, L"&No");
    case IDRETRY: return TaskDialogString(4244, L"&Retry");
    case IDCLOSE: return TaskDialogString(4160, L"Close");
    default: return DialogButtonLabel(id);
    }
}

std::wstring TaskDialogExpandoLabel(bool expanded) {
    return expanded ? TaskDialogString(4245, L"Hide &details") : TaskDialogString(4246, L"See &details");
}

std::wstring EscapeMnemonics(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (const wchar_t character : text) {
        result.push_back(character);
        if (character == L'&') result.push_back(L'&');
    }
    return result;
}

std::wstring StripMnemonics(std::wstring_view text) {
    std::wstring result;
    result.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        if (text[index] == L'&' && index + 1 < text.size()) {
            if (text[index + 1] != L'&') continue;
            ++index;
        }
        result.push_back(text[index]);
    }
    return result;
}

bool CaptureDialogIcon(HICON icon, DialogIconImage& image) {
    image = {};
    if (!icon) return false;
    std::wstring format;
    std::wstring reason;
    if (!CaptureOwnedIconPixels(icon, image.width, image.height, format, image.pixels, reason) ||
        format != L"bgra8-premultiplied") {
        image = {};
        return false;
    }
    return true;
}

bool LoadDialogIcon(HINSTANCE instance, PCWSTR name, int size, DialogIconImage& image) {
    image = {};
    if (!name) return false;
    // LoadIconWithScaleDown exists only in comctl32 version 6, which a process
    // without a manifest asking for it never loads; it is looked up, not imported.
    using LoadIconWithScaleDownFunction = HRESULT(WINAPI*)(HINSTANCE, PCWSTR, int, int, HICON*);
    static const auto loadScaled = [] {
        const HMODULE comctl = GetModuleHandleW(L"comctl32.dll");
        return comctl ? reinterpret_cast<LoadIconWithScaleDownFunction>(
            GetProcAddress(comctl, "LoadIconWithScaleDown")) : nullptr;
    }();
    HICON icon = nullptr;
    if (!loadScaled || FAILED(loadScaled(instance, name, size, size, &icon)) || !icon) {
        icon = nullptr;
        icon = static_cast<HICON>(LoadImageW(instance, name, IMAGE_ICON, size, size,
            instance ? LR_DEFAULTCOLOR : LR_SHARED));
        if (!icon) return false;
        const bool captured = CaptureDialogIcon(icon, image);
        if (instance) DestroyIcon(icon);
        return captured;
    }
    const bool captured = CaptureDialogIcon(icon, image);
    DestroyIcon(icon);
    return captured;
}

WindowSnapshot BuildMessageBoxSnapshot(
    HWND owner,
    std::wstring_view text,
    std::wstring_view caption,
    UINT type,
    std::unordered_map<uint64_t, int>& results,
    const DialogIconImage* customIcon) {
    WindowSnapshot snapshot;
    const UINT dpi = DialogDpi(owner);
    InitializeDialogSnapshot(snapshot, SurfaceKind::MessageBox, owner, caption, L"Error", dpi);
    const bool userIcon = customIcon && !customIcon->Empty();
    snapshot.icon = userIcon ? L"none" : MessageBoxIcon(type);
    snapshot.rtl = (type & MB_RTLREADING) != 0;

    std::vector<int> ids;
    switch (type & MB_TYPEMASK) {
    case MB_OKCANCEL: ids = { IDOK, IDCANCEL }; break;
    case MB_ABORTRETRYIGNORE: ids = { IDABORT, IDRETRY, IDIGNORE }; break;
    case MB_YESNOCANCEL: ids = { IDYES, IDNO, IDCANCEL }; break;
    case MB_YESNO: ids = { IDYES, IDNO }; break;
    case MB_RETRYCANCEL: ids = { IDRETRY, IDCANCEL }; break;
    case MB_CANCELTRYCONTINUE: ids = { IDCANCEL, IDTRYAGAIN, IDCONTINUE }; break;
    default: ids = { IDOK }; break;
    }
    // A lone OK closes on Escape too, as the native message box does.
    snapshot.canCancel = ids.size() == 1 ||
        std::find(ids.begin(), ids.end(), IDCANCEL) != ids.end();
    // user32 greys the close button of a box it cannot cancel (Yes/No, Abort/Retry/Ignore).
    snapshot.canClose = snapshot.canCancel;

    const DialogTextMeasure measure(dpi);
    const RECT work = WorkAreaFor(owner);
    const LONG workWidth = work.right - work.left;
    const LONG workHeight = work.bottom - work.top;
    const LONG margin = measure.Px(kMarginDip);
    const LONG iconSize = measure.Px(kIconDip);
    const bool hasIcon = userIcon || snapshot.icon != L"none";
    const LONG textLeft = margin + (hasIcon ? iconSize + measure.Px(kIconGapDip) : 0);

    // A message wraps at a comfortable reading width first and is widened toward
    // five eighths of the screen only when that would make it too tall to read.
    const LONG widest = (std::max)(measure.Px(160), workWidth * 5 / 8 - textLeft - margin);
    LONG wrap = (std::min)(measure.Px(420), widest);
    SIZE textSize = measure.Measure(text, wrap);
    if (textSize.cy > workHeight / 3 && wrap < widest) {
        wrap = widest;
        textSize = measure.Measure(text, wrap);
    }

    std::vector<std::wstring> labels;
    std::vector<LONG> widths;
    LONG buttonsWidth = 0;
    for (const int id : ids) {
        labels.push_back(DialogButtonLabel(id));
        const LONG width = (std::max)(measure.Px(kButtonMinWidthDip),
            measure.SingleLineWidth(StripMnemonics(labels.back())) + measure.Px(24));
        widths.push_back(width);
        buttonsWidth += width;
    }
    buttonsWidth += static_cast<LONG>(ids.size() - 1) * measure.Px(kButtonGapDip);
    // The caption needs room beside the caption buttons, but never more than the
    // widest the text itself may grow.
    const LONG captionWidth = (std::min)(widest + textLeft + margin,
        measure.SingleLineWidth(snapshot.title) + measure.Px(150));
    const LONG clientWidth = (std::max)({ textLeft + textSize.cx + margin,
        buttonsWidth + 2 * margin, captionWidth, measure.Px(260) });

    LONG textTop = margin;
    if (hasIcon && textSize.cy < iconSize) textTop += (iconSize - textSize.cy) / 2;
    const LONG contentBottom = (std::max)(textTop + textSize.cy, hasIcon ? margin + iconSize : 0L) + margin;
    const LONG buttonTop = contentBottom + measure.Px(kDialogBandPaddingDip);
    const LONG buttonHeight = measure.Px(kButtonHeightDip);
    const LONG clientHeight = buttonTop + buttonHeight + measure.Px(kDialogBandPaddingDip);

    snapshot.clientBounds = { 0, 0, clientWidth, clientHeight };
    snapshot.bounds = DialogWindowBounds(owner, std::nullopt, clientWidth, clientHeight, dpi);

    uint64_t nodeId = 1;
    if (userIcon) AddIconNode(snapshot, nodeId++, *customIcon, margin, margin, iconSize);
    auto content = NewNode(snapshot, nodeId++, ControlKind::StaticText,
        { textLeft, textTop, clientWidth - margin, textTop + (std::max)(textSize.cy, measure.Px(16)) });
    // The native message box draws its text literally, `&` included.
    content.text = EscapeMnemonics(text);
    content.automationName = content.text;
    if ((type & MB_RIGHT) != 0) content.style = SS_RIGHT;
    snapshot.nodes.push_back(std::move(content));

    int defaultIndex = static_cast<int>((type & MB_DEFMASK) >> 8);
    defaultIndex = (std::clamp)(defaultIndex, 0, static_cast<int>(ids.size()) - 1);
    LONG left = clientWidth - margin - buttonsWidth;
    for (size_t index = 0; index < ids.size(); ++index) {
        auto button = NewNode(snapshot, nodeId++, ControlKind::Button,
            { left, buttonTop, left + widths[index], buttonTop + buttonHeight });
        button.controlId = ids[index];
        button.tabStop = true;
        button.tabIndex = static_cast<int>(index);
        button.isDefault = static_cast<int>(index) == defaultIndex;
        button.text = labels[index];
        button.automationName = button.text;
        results.emplace(button.nodeId, ids[index]);
        snapshot.nodes.push_back(std::move(button));
        left += widths[index] + measure.Px(kButtonGapDip);
    }
    return snapshot;
}

TaskDialogLinkText ParseTaskDialogLinks(std::wstring_view markup) {
    TaskDialogLinkText result;
    auto lower = [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); };
    auto startsWith = [&](size_t at, std::wstring_view prefix) {
        if (at + prefix.size() > markup.size()) return false;
        for (size_t index = 0; index < prefix.size(); ++index) {
            if (lower(markup[at + index]) != prefix[index]) return false;
        }
        return true;
    };
    size_t index = 0;
    while (index < markup.size()) {
        const bool opening = startsWith(index, L"<a") &&
            index + 2 < markup.size() && (markup[index + 2] == L'>' || std::iswspace(markup[index + 2]));
        if (!opening) {
            result.text.push_back(markup[index++]);
            continue;
        }
        const size_t tagEnd = markup.find(L'>', index);
        size_t close = std::wstring_view::npos;
        for (size_t at = tagEnd == std::wstring_view::npos ? markup.size() : tagEnd; at < markup.size(); ++at) {
            if (startsWith(at, L"</a>")) {
                close = at;
                break;
            }
        }
        if (tagEnd == std::wstring_view::npos || close == std::wstring_view::npos) {
            result.text.push_back(markup[index++]);
            continue;
        }
        // href="value", href='value' or href=value; a link without one reports "".
        std::wstring href;
        const std::wstring_view attributes = markup.substr(index + 2, tagEnd - index - 2);
        for (size_t at = 0; at + 4 < attributes.size(); ++at) {
            if (lower(attributes[at]) != L'h' || lower(attributes[at + 1]) != L'r' ||
                lower(attributes[at + 2]) != L'e' || lower(attributes[at + 3]) != L'f') continue;
            size_t value = at + 4;
            while (value < attributes.size() && std::iswspace(attributes[value])) ++value;
            if (value >= attributes.size() || attributes[value] != L'=') continue;
            ++value;
            while (value < attributes.size() && std::iswspace(attributes[value])) ++value;
            if (value < attributes.size() && (attributes[value] == L'"' || attributes[value] == L'\'')) {
                const wchar_t quote = attributes[value++];
                const size_t end = attributes.find(quote, value);
                href.assign(attributes.substr(value, end == std::wstring_view::npos ? std::wstring_view::npos : end - value));
            } else {
                size_t end = value;
                while (end < attributes.size() && !std::iswspace(attributes[end])) ++end;
                href.assign(attributes.substr(value, end - value));
            }
            break;
        }
        const std::wstring label(markup.substr(tagEnd + 1, close - tagEnd - 1));
        result.text += label;
        if (!label.empty()) {
            result.labels.push_back(label);
            result.hrefs.push_back(std::move(href));
        }
        index = close + 4;
    }
    return result;
}

namespace {

// A text element: a Static, or a SysLink when the dialog enables hyperlinks and
// the text carries at least one whose label occurs exactly once (the renderer
// places each link by finding its label in the text).
void AddTaskDialogText(WindowSnapshot& snapshot, TaskDialogElements& elements, uint64_t nodeId,
    const TaskDialogLinkText& parsed, const RECT& rect, bool hyperlinks, int& tabIndex,
    std::wstring_view variant = {}) {
    if (parsed.text.empty()) return;
    bool links = hyperlinks && !parsed.labels.empty();
    // Labels are located left to right; a label that cannot be found after the
    // previous one leaves the whole text plain.
    // A single label must occur exactly once, as a native SysLink's does.
    size_t cursor = 0;
    for (const auto& label : parsed.labels) {
        const size_t at = parsed.text.find(label, cursor);
        if (at == std::wstring::npos) {
            links = false;
            break;
        }
        cursor = at + label.size();
    }
    if (links && parsed.labels.size() == 1 &&
        parsed.text.find(parsed.labels.front(), cursor) != std::wstring::npos) {
        links = false;
    }
    if (parsed.labels.size() > 32) links = false;
    auto node = NewNode(snapshot, nodeId, links ? ControlKind::SysLink : ControlKind::StaticText, rect);
    if (links) {
        node.text = parsed.text;
        node.items = parsed.labels;
        node.automationName = EscapeMnemonics(parsed.text);
        node.tabStop = true;
        node.tabIndex = tabIndex++;
        TaskDialogElement element;
        element.kind = TaskDialogElementKind::Text;
        element.hrefs = parsed.hrefs;
        elements.emplace(nodeId, std::move(element));
    } else {
        node.text = EscapeMnemonics(parsed.text);
        node.automationName = node.text;
        node.presentationVariant = std::wstring(variant);
    }
    snapshot.nodes.push_back(std::move(node));
}

TaskDialogLinkText PlainText(std::wstring_view text) {
    TaskDialogLinkText result;
    result.text.assign(text);
    return result;
}

} // namespace

WindowSnapshot BuildTaskDialogSnapshot(
    const TaskDialogModel& model,
    HWND owner,
    HWND reference,
    UINT dpi,
    std::optional<POINT> origin,
    TaskDialogElements& elements) {
    elements.clear();
    WindowSnapshot snapshot;
    if (!dpi) dpi = DialogDpi(owner);
    std::wstring fallbackTitle = L"Dialog";
    {
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
            const std::wstring_view full(path);
            const size_t slash = full.find_last_of(L"\\/");
            fallbackTitle = std::wstring(slash == std::wstring_view::npos ? full : full.substr(slash + 1));
        }
    }
    InitializeDialogSnapshot(snapshot, SurfaceKind::TaskDialog, owner, model.title, fallbackTitle, dpi);
    snapshot.canCancel = model.canCancel;
    snapshot.canClose = model.canCancel;
    snapshot.rtl = model.rtl;
    if (model.minimizable) snapshot.windowStyle |= WS_MINIMIZEBOX;
    snapshot.icon = model.mainIcon.image.Empty() ? model.mainIcon.standard : L"none";

    const DialogTextMeasure measure(dpi);
    const RECT work = WorkAreaFor(reference ? reference : owner);
    const LONG workWidth = work.right - work.left;
    const LONG margin = measure.Px(kMarginDip);
    const LONG iconSize = measure.Px(kIconDip);
    const LONG gap = measure.Px(8);
    const bool hasIcon = model.mainIcon.Present();
    const LONG textLeft = margin + (hasIcon ? iconSize + measure.Px(kIconGapDip) : 0);
    const bool links = model.hyperlinks;
    auto parse = [&](const std::wstring& text) { return links ? ParseTaskDialogLinks(text) : PlainText(text); };
    const auto content = parse(model.content);
    const auto expanded = parse(model.expandedInformation);
    const auto footer = parse(model.footer);
    const bool showExpanded = model.expanded && !expanded.text.empty();
    const bool hasExpando = !model.expandedInformation.empty();

    // The band row: band buttons right, the expando button left, and the
    // verification check box beside it when the row has room.
    std::vector<size_t> bandButtons;
    std::vector<size_t> commandLinks;
    for (size_t index = 0; index < model.buttons.size(); ++index) {
        (model.buttons[index].commandLink ? commandLinks : bandButtons).push_back(index);
    }
    std::vector<LONG> bandWidths;
    LONG bandButtonsWidth = 0;
    for (const size_t index : bandButtons) {
        const auto& button = model.buttons[index];
        LONG width = (std::max)(measure.Px(kButtonMinWidthDip),
            measure.SingleLineWidth(StripMnemonics(button.text)) + measure.Px(24) +
                (button.elevated ? measure.Px(20) : 0));
        width = (std::min)(width, workWidth / 2);
        bandWidths.push_back(width);
        bandButtonsWidth += width;
    }
    if (!bandButtons.empty()) bandButtonsWidth += static_cast<LONG>(bandButtons.size() - 1) * measure.Px(kButtonGapDip);
    // The label of the button that expands (collapsed) or collapses (expanded) the details: the
    // application's text for that state, else its text for the other one, else comctl32's own.
    const std::wstring& stateText = model.expanded ? model.expandedControlText : model.collapsedControlText;
    const std::wstring& otherText = model.expanded ? model.collapsedControlText : model.expandedControlText;
    const std::wstring expandoLabel = !stateText.empty() ? stateText
        : !otherText.empty() ? otherText
        : TaskDialogExpandoLabel(model.expanded);
    const LONG expandoWidth = hasExpando
        ? measure.SingleLineWidth(StripMnemonics(expandoLabel)) + measure.Px(48) : 0;
    const LONG verificationWidth = model.verification.empty() ? 0
        : measure.SingleLineWidth(StripMnemonics(model.verification)) + measure.Px(32);

    // Width: the application's own (cxWidth, in dialog units of the 9 pt message
    // font, about 7/4 px each at 96 DPI), or a reading width widened for long
    // content, a size-to-content instruction, and a band that would not fit.
    LONG clientWidth = 0;
    const LONG widest = (std::max)(measure.Px(320), workWidth * 3 / 4);
    if (model.widthDlu != 0) {
        clientWidth = MulDiv(static_cast<int>(model.widthDlu), measure.Px(7), 4);
    } else {
        clientWidth = measure.Px(440);
        if (model.sizeToContent) {
            clientWidth = (std::max)(clientWidth,
                textLeft + measure.SingleLineWidth(model.instruction, DialogTextMeasure::Style::Instruction) + margin);
        }
        const SIZE probe = measure.Measure(content.text, clientWidth - textLeft - margin);
        if (probe.cy > measure.Px(260)) clientWidth = (std::max)(clientWidth, measure.Px(600));
    }
    const LONG bandRow = bandButtonsWidth + 2 * margin +
        (hasExpando ? expandoWidth + measure.Px(16) : 0);
    clientWidth = (std::clamp)((std::max)(clientWidth, bandRow), measure.Px(260), widest);
    const LONG column = clientWidth - textLeft - margin;

    int tabIndex = 0;
    LONG y = margin;
    if (!model.mainIcon.image.Empty()) {
        AddIconNode(snapshot, kMainIconNode, model.mainIcon.image, margin, margin, iconSize);
    }
    auto advance = [&](LONG height) { y += height + gap; };
    bool anyText = false;
    if (!model.instruction.empty()) {
        const SIZE size = measure.Measure(model.instruction, column, DialogTextMeasure::Style::Instruction);
        LONG top = y;
        if (hasIcon && size.cy < iconSize && content.text.empty()) top += (iconSize - size.cy) / 2;
        AddTaskDialogText(snapshot, elements, kInstructionNode, PlainText(model.instruction),
            { textLeft, top, textLeft + column, top + size.cy }, false, tabIndex, L"mainInstruction");
        y = top;
        advance(size.cy);
        anyText = true;
    }
    if (!content.text.empty()) {
        const SIZE size = measure.Measure(content.text, column);
        LONG top = y;
        if (hasIcon && !anyText && size.cy < iconSize) top += (iconSize - size.cy) / 2;
        AddTaskDialogText(snapshot, elements, kContentNode, content,
            { textLeft, top, textLeft + column, top + size.cy }, links, tabIndex);
        y = top;
        advance(size.cy);
        anyText = true;
    }
    if (showExpanded && !model.expandFooterArea) {
        const SIZE size = measure.Measure(expanded.text, column);
        AddTaskDialogText(snapshot, elements, kExpandedNode, expanded,
            { textLeft, y, textLeft + column, y + size.cy }, links, tabIndex);
        advance(size.cy);
    }
    if (model.progress != TaskDialogProgress::None) {
        y += measure.Px(4);
        auto progress = NewNode(snapshot, kProgressNode, ControlKind::ProgressBar,
            { textLeft, y, textLeft + column, y + measure.Px(16) });
        progress.indeterminate = model.progress == TaskDialogProgress::Marquee;
        progress.minimum = model.progressMinimum;
        progress.maximum = (std::max)(model.progressMaximum, model.progressMinimum + 1);
        progress.position = (std::clamp)(model.progressPosition, progress.minimum, progress.maximum);
        progress.automationName = model.instruction;
        snapshot.nodes.push_back(std::move(progress));
        advance(measure.Px(16) + measure.Px(4));
    }
    if (!model.radios.empty()) {
        y += measure.Px(4);
        const bool anySelected = std::any_of(model.radios.begin(), model.radios.end(),
            [&](const auto& radio) { return radio.id == model.selectedRadio; });
        for (size_t index = 0; index < model.radios.size(); ++index) {
            const auto& radio = model.radios[index];
            const SIZE size = measure.Measure(StripMnemonics(radio.text), column - measure.Px(28));
            const LONG height = (std::max)(measure.Px(24), size.cy + measure.Px(4));
            const uint64_t nodeId = kRadioNodeBase + index;
            auto node = NewNode(snapshot, nodeId, ControlKind::RadioButton,
                { textLeft, y, textLeft + column, y + height });
            node.controlId = radio.id;
            node.text = radio.text;
            node.automationName = radio.text;
            node.enabled = radio.enabled;
            node.checked = radio.id == model.selectedRadio ? 1 : 0;
            node.groupStart = index == 0;
            // One stop for the group: the selected member, or the first.
            node.tabStop = anySelected ? node.checked == 1 : index == 0;
            node.tabIndex = node.tabStop ? tabIndex++ : -1;
            elements.emplace(nodeId, TaskDialogElement{ TaskDialogElementKind::Radio, radio.id, {} });
            snapshot.nodes.push_back(std::move(node));
            y += height + measure.Px(2);
        }
        y += gap;
    }
    const auto isDefault = [&](size_t index) {
        if (model.defaultButton != 0) return model.buttons[index].id == model.defaultButton;
        return index == 0;
    };
    if (!commandLinks.empty()) {
        y += measure.Px(4);
        const LONG linkLeft = textLeft;
        const LONG linkWidth = column;
        const LONG labelWidth = linkWidth - measure.Px(20) - (model.commandLinkIcons ? measure.Px(28) : 0);
        for (const size_t index : commandLinks) {
            const auto& button = model.buttons[index];
            const SIZE title = measure.Measure(StripMnemonics(button.text), labelWidth, DialogTextMeasure::Style::Strong);
            const SIZE note = measure.Measure(button.note, labelWidth);
            const LONG height = (std::max)(measure.Px(40),
                measure.Px(16) + title.cy + (note.cy ? measure.Px(2) + note.cy : 0));
            const uint64_t nodeId = kButtonNodeBase + index;
            auto node = NewNode(snapshot, nodeId, ControlKind::Button,
                { linkLeft, y, linkLeft + linkWidth, y + height });
            node.controlId = button.id;
            node.text = button.text;
            node.automationName = button.text;
            node.helpText = button.note;
            node.presentationVariant = button.elevated ? L"commandLinkElevated"
                : model.commandLinkIcons ? L"commandLink" : L"commandLinkNoIcon";
            node.enabled = button.enabled;
            node.tabStop = true;
            node.tabIndex = tabIndex++;
            node.isDefault = isDefault(index);
            elements.emplace(nodeId, TaskDialogElement{ TaskDialogElementKind::Button, button.id, {} });
            snapshot.nodes.push_back(std::move(node));
            y += height + measure.Px(4);
        }
        y += gap;
    }
    // y is one gap past the last element; the content area ends one margin below it.
    LONG contentBottom = (std::max)(y - gap + margin, hasIcon ? margin + iconSize + margin : margin);

    // Command band.
    const bool hasBand = !bandButtons.empty() || hasExpando || !model.verification.empty();
    LONG bandBottom = contentBottom;
    if (hasBand) {
        const LONG buttonHeight = measure.Px(kButtonHeightDip);
        LONG row = contentBottom + measure.Px(kDialogBandPaddingDip);
        LONG rowBottom = row + buttonHeight;
        LONG leftEdge = margin;
        if (hasExpando) {
            auto node = NewNode(snapshot, kExpandoNode, ControlKind::Button,
                { margin, row, margin + expandoWidth, row + buttonHeight });
            node.text = expandoLabel;
            node.automationName = expandoLabel;
            node.presentationVariant = model.expanded ? L"expandoExpanded" : L"expandoCollapsed";
            node.tabStop = true;
            node.tabIndex = tabIndex++;
            elements.emplace(kExpandoNode, TaskDialogElement{ TaskDialogElementKind::Expando, 0, {} });
            snapshot.nodes.push_back(std::move(node));
            leftEdge = margin + expandoWidth + measure.Px(16);
        }
        LONG verificationTop = row + (buttonHeight - measure.Px(24)) / 2;
        const LONG buttonsLeft = clientWidth - margin - bandButtonsWidth;
        const bool verificationFits = !hasExpando &&
            leftEdge + verificationWidth + measure.Px(16) <= buttonsLeft;
        if (!model.verification.empty() && !verificationFits) {
            verificationTop = rowBottom + measure.Px(6);
            rowBottom = verificationTop + measure.Px(24);
        }
        if (!model.verification.empty()) {
            const LONG right = verificationFits ? buttonsLeft - measure.Px(16) : clientWidth - margin;
            auto node = NewNode(snapshot, kVerificationNode, ControlKind::CheckBox,
                { margin, verificationTop, right, verificationTop + measure.Px(24) });
            node.text = model.verification;
            node.automationName = model.verification;
            node.checked = model.verificationChecked ? 1 : 0;
            node.tabStop = true;
            elements.emplace(kVerificationNode, TaskDialogElement{ TaskDialogElementKind::Verification, 0, {} });
            // The check box is tabbed to after the band buttons when it sits below
            // them; its index is assigned once they are.
            snapshot.nodes.push_back(std::move(node));
        }
        LONG left = buttonsLeft;
        for (size_t slot = 0; slot < bandButtons.size(); ++slot) {
            const size_t index = bandButtons[slot];
            const auto& button = model.buttons[index];
            const uint64_t nodeId = kButtonNodeBase + index;
            auto node = NewNode(snapshot, nodeId, ControlKind::Button,
                { left, row, left + bandWidths[slot], row + buttonHeight });
            node.controlId = button.id;
            node.text = button.text;
            node.automationName = button.text;
            if (button.elevated) node.presentationVariant = L"elevated";
            node.enabled = button.enabled;
            node.tabStop = true;
            node.tabIndex = tabIndex++;
            node.isDefault = isDefault(index);
            elements.emplace(nodeId, TaskDialogElement{ TaskDialogElementKind::Button, button.id, {} });
            snapshot.nodes.push_back(std::move(node));
            left += bandWidths[slot] + measure.Px(kButtonGapDip);
        }
        for (auto& node : snapshot.nodes) {
            if (node.nodeId == kVerificationNode) node.tabIndex = tabIndex++;
        }
        bandBottom = rowBottom + measure.Px(kDialogBandPaddingDip);
    }

    // Footer: below the band, its icon at the left margin.
    LONG clientHeight = bandBottom;
    const bool expandedInFooter = showExpanded && model.expandFooterArea;
    if (!footer.text.empty() || expandedInFooter) {
        LONG fy = bandBottom + measure.Px(10);
        const LONG footerIcon = measure.Px(kFooterIconDip);
        const bool hasFooterIcon = model.footerIcon.Present() && !footer.text.empty();
        const LONG footerLeft = margin + (hasFooterIcon ? footerIcon + measure.Px(8) : 0);
        const LONG footerColumn = clientWidth - footerLeft - margin;
        if (!footer.text.empty()) {
            if (hasFooterIcon) {
                DialogIconImage image = model.footerIcon.image;
                if (image.Empty()) {
                    static const std::pair<std::wstring_view, PCWSTR> kSystemIcons[] = {
                        { L"warning", IDI_WARNING }, { L"error", IDI_ERROR },
                        { L"info", IDI_INFORMATION }, { L"shield", IDI_SHIELD },
                    };
                    for (const auto& [name, resource] : kSystemIcons) {
                        if (model.footerIcon.standard == name) LoadDialogIcon(nullptr, resource, footerIcon, image);
                    }
                }
                AddIconNode(snapshot, kFooterIconNode, image, margin, fy, footerIcon);
            }
            const SIZE size = measure.Measure(footer.text, footerColumn);
            AddTaskDialogText(snapshot, elements, kFooterNode, footer,
                { footerLeft, fy, footerLeft + footerColumn, fy + (std::max)(size.cy, footerIcon) }, links, tabIndex);
            fy += (std::max)(size.cy, footerIcon) + gap;
        }
        if (expandedInFooter) {
            const SIZE size = measure.Measure(expanded.text, clientWidth - 2 * margin);
            AddTaskDialogText(snapshot, elements, kExpandedNode, expanded,
                { margin, fy, clientWidth - margin, fy + size.cy }, links, tabIndex);
            fy += size.cy + gap;
        }
        clientHeight = fy - gap + measure.Px(10);
    }

    snapshot.clientBounds = { 0, 0, clientWidth, clientHeight };
    snapshot.bounds = DialogWindowBounds(reference, origin, clientWidth, clientHeight, dpi);
    if (!reference && !origin) {
        // Without TDF_POSITION_RELATIVE_TO_WINDOW a task dialog is centered on the
        // monitor its owner is on.
        const LONG width = snapshot.bounds.right - snapshot.bounds.left;
        const LONG height = snapshot.bounds.bottom - snapshot.bounds.top;
        const LONG x = work.left + (std::max)(0L, (work.right - work.left - width) / 2);
        const LONG y = work.top + (std::max)(0L, (work.bottom - work.top - height) / 2);
        snapshot.bounds = { x, y, x + width, y + height };
    }
    return snapshot;
}

}  // namespace FluentShell::Bridge::Translation
