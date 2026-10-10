#include "RichText.h"

#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace FluentShell::Bridge::Translation {
namespace {

using Microsoft::WRL::ComPtr;

constexpr size_t kMaxRichParagraphs = 512;
constexpr size_t kMaxRichRunsPerParagraph = 128;
constexpr size_t kMaxRichRuns = 1024;
constexpr size_t kMaxRichCharacters = 65536;

bool Reject(std::wstring& reason, const wchar_t* text) {
    reason = text;
    return false;
}

bool Off(long value) noexcept { return value == tomFalse || value == tomNone; }

// A float the control reports for a paragraph or run format: finite and inside a sane range.
bool Points(float value, float low, float high) noexcept {
    return std::isfinite(value) && value >= low && value <= high;
}

// A COLORREF, or the control's automatic colour (-1 on the wire).
bool ReadColor(long raw, int64_t& color) noexcept {
    if (raw == tomAutoColor) {
        color = -1;
        return true;
    }
    // Anything above the 24 colour bits is a palette index or a flag, not a colour.
    if (raw < 0 || (static_cast<unsigned long>(raw) & 0xFF000000ul) != 0) return false;
    const auto value = static_cast<unsigned long>(raw);
    color = (static_cast<int64_t>(GetRValue(value)) << 16) | (static_cast<int64_t>(GetGValue(value)) << 8) |
        static_cast<int64_t>(GetBValue(value));
    return true;
}

// A range's text as the projection draws it: the paragraph mark is the paragraph boundary
// and a vertical tab is a line break.
std::wstring RunText(ITextRange* range) {
    BSTR raw = nullptr;
    std::wstring text;
    if (SUCCEEDED(range->GetText(&raw)) && raw) text.assign(raw, SysStringLen(raw));
    if (raw) SysFreeString(raw);
    std::wstring result;
    result.reserve(text.size());
    for (const wchar_t character : text) {
        if (character == L'\r') continue;
        result.push_back(character == L'\v' ? L'\n' : character);
    }
    return result;
}

} // namespace

bool ReadRichText(HWND richEdit, std::vector<RichParagraphSnapshot>& paragraphs, std::wstring& reason) noexcept {
    try {
        paragraphs.clear();
        IRichEditOle* raw = nullptr;
        if (!SendMessageW(richEdit, EM_GETOLEINTERFACE, 0, reinterpret_cast<LPARAM>(&raw)) || !raw)
            return Reject(reason, L"RichEdit publishes no text object model");
        ComPtr<IRichEditOle> ole;
        ole.Attach(raw);
        if (ole->GetObjectCount() != 0)
            return Reject(reason, L"RichEdit embeds objects the projection cannot draw");
        ComPtr<ITextDocument> document;
        ComPtr<ITextRange> story;
        long storyLength = 0;
        if (FAILED(ole.As(&document)) || FAILED(document->Range(0, 0, story.GetAddressOf())) || !story ||
            FAILED(story->GetStoryLength(&storyLength)))
            return Reject(reason, L"RichEdit character formats could not be read");
        // The story's final paragraph mark keeps a format of its own and draws nothing.
        const long end = storyLength - 1;
        size_t runs = 0;
        size_t characters = 0;
        long position = 0;
        while (position < end) {
            if (paragraphs.size() >= kMaxRichParagraphs)
                return Reject(reason, L"RichEdit has more paragraphs than the projection carries");
            ComPtr<ITextRange> paragraph;
            long moved = 0;
            long paragraphStart = 0;
            long paragraphEnd = 0;
            if (FAILED(document->Range(position, position + 1, paragraph.GetAddressOf())) || !paragraph ||
                FAILED(paragraph->Expand(tomParagraph, &moved)) || FAILED(paragraph->GetStart(&paragraphStart)) ||
                FAILED(paragraph->GetEnd(&paragraphEnd)) || paragraphEnd <= position)
                return Reject(reason, L"RichEdit paragraph formats could not be read");
            paragraphStart = std::max(paragraphStart, position);

            RichParagraphSnapshot snapshot;
            ComPtr<ITextPara> format;
            long alignment = tomUndefined;
            long listType = tomUndefined;
            if (FAILED(paragraph->GetPara(format.GetAddressOf())) || !format ||
                FAILED(format->GetAlignment(&alignment)) || FAILED(format->GetListType(&listType)) ||
                FAILED(format->GetLeftIndent(&snapshot.leftIndent)) ||
                FAILED(format->GetFirstLineIndent(&snapshot.firstLineIndent)) ||
                FAILED(format->GetSpaceBefore(&snapshot.spaceBefore)) ||
                FAILED(format->GetSpaceAfter(&snapshot.spaceAfter)))
                return Reject(reason, L"RichEdit paragraph formats could not be read");
            switch (alignment) {
            case tomAlignLeft: snapshot.alignment = L"left"; break;
            case tomAlignCenter: snapshot.alignment = L"center"; break;
            case tomAlignRight: snapshot.alignment = L"right"; break;
            case tomAlignJustify: snapshot.alignment = L"justify"; break;
            default: return Reject(reason, L"RichEdit paragraph alignment is not one the projection draws");
            }
            if (listType != tomListNone && listType != tomListBullet)
                return Reject(reason, L"RichEdit numbered lists are not drawn by the projection");
            snapshot.bullet = listType == tomListBullet;
            if (!Points(snapshot.leftIndent, -2000.0f, 2000.0f) || !Points(snapshot.firstLineIndent, -2000.0f, 2000.0f) ||
                !Points(snapshot.spaceBefore, 0.0f, 2000.0f) || !Points(snapshot.spaceAfter, 0.0f, 2000.0f))
                return Reject(reason, L"RichEdit paragraph spacing is outside the range the projection carries");

            // Runs of one character format inside this paragraph; its mark ends the last one.
            long runStart = paragraphStart;
            while (runStart < paragraphEnd) {
                if (++runs > kMaxRichRuns || snapshot.runs.size() >= kMaxRichRunsPerParagraph)
                    return Reject(reason, L"RichEdit has more formatted runs than the projection carries");
                ComPtr<ITextRange> run;
                long runBegin = 0;
                long runEnd = 0;
                if (FAILED(document->Range(runStart, runStart + 1, run.GetAddressOf())) || !run ||
                    FAILED(run->Expand(tomCharFormat, &moved)) || FAILED(run->GetStart(&runBegin)) ||
                    FAILED(run->GetEnd(&runEnd)))
                    return Reject(reason, L"RichEdit character formats could not be read");
                runBegin = std::max(runBegin, runStart);
                runEnd = std::min(runEnd, paragraphEnd);
                if (runEnd <= runBegin) runEnd = std::min(runStart + 1, paragraphEnd);
                if (FAILED(document->Range(runBegin, runEnd, run.ReleaseAndGetAddressOf())) || !run)
                    return Reject(reason, L"RichEdit character formats could not be read");

                ComPtr<ITextFont> font;
                long bold = tomUndefined, italic = tomUndefined, underline = tomUndefined, strike = tomUndefined;
                long hidden = tomUndefined, superscript = tomUndefined, subscript = tomUndefined;
                long foreground = tomUndefined, background = tomUndefined;
                float size = static_cast<float>(tomUndefined);
                if (FAILED(run->GetFont(font.GetAddressOf())) || !font ||
                    FAILED(font->GetBold(&bold)) || FAILED(font->GetItalic(&italic)) ||
                    FAILED(font->GetUnderline(&underline)) || FAILED(font->GetStrikeThrough(&strike)) ||
                    FAILED(font->GetHidden(&hidden)) || FAILED(font->GetSuperscript(&superscript)) ||
                    FAILED(font->GetSubscript(&subscript)) || FAILED(font->GetForeColor(&foreground)) ||
                    FAILED(font->GetBackColor(&background)) || FAILED(font->GetSize(&size)))
                    return Reject(reason, L"RichEdit character formats could not be read");
                if (!Off(hidden) || !Off(superscript) || !Off(subscript))
                    return Reject(reason, L"RichEdit hidden, raised or lowered text is not drawn by the projection");
                if (background != tomAutoColor)
                    return Reject(reason, L"RichEdit text with a background colour is not drawn by the projection");
                if (!Points(size, 1.0f, 400.0f))
                    return Reject(reason, L"RichEdit text size is not one format");
                // A link draws as a link and acts on a click; only the TOM 2 font reports one.
                ComPtr<ITextRange2> run2;
                ComPtr<ITextFont2> font2;
                long link = tomUndefined;
                if (FAILED(run.As(&run2)) || FAILED(run2->GetFont2(font2.GetAddressOf())) || !font2 ||
                    FAILED(font2->GetLinkType(&link)))
                    return Reject(reason, L"RichEdit cannot report whether its text holds links");
                if (link != tomNoLink)
                    return Reject(reason, L"RichEdit text holds links the projection does not carry");

                RichRunSnapshot value;
                value.bold = !Off(bold) && bold != tomUndefined;
                value.italic = !Off(italic) && italic != tomUndefined;
                value.underline = !Off(underline) && underline != tomUndefined;
                value.strike = !Off(strike) && strike != tomUndefined;
                value.size = size;
                if (bold == tomUndefined || italic == tomUndefined || underline == tomUndefined ||
                    strike == tomUndefined || !ReadColor(foreground, value.color))
                    return Reject(reason, L"RichEdit character format varies inside one run");
                value.text = RunText(run.Get());
                characters += value.text.size();
                if (characters > kMaxRichCharacters)
                    return Reject(reason, L"RichEdit text is longer than the projection carries");
                if (!value.text.empty()) snapshot.runs.push_back(std::move(value));
                runStart = runEnd;
            }
            paragraphs.push_back(std::move(snapshot));
            position = paragraphEnd;
        }
        return true;
    } catch (...) {
        paragraphs.clear();
        try { reason = L"exception while reading rich text"; } catch (...) {}
        return false;
    }
}

} // namespace FluentShell::Bridge::Translation
