#pragma once

#include "../../src/Bridge/Translation/DirectUiEngine.h"
#include "../../src/Bridge/Ipc/Protocol.h"
#include <UIAutomation.h>
#include <thread>

namespace FluentShell::Tests {
namespace DirectUiVirtualImages {

using namespace Bridge::Translation;

struct Fixture final {
    HWND host = nullptr;
    bool draw = true;
    bool resizeOnPrint = false;
    COLORREF color = RGB(23, 45, 67);
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        }
        auto* self = reinterpret_cast<Fixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_PRINTCLIENT && self) {
            if (self->resizeOnPrint) {
                self->resizeOnPrint = false;
                SetWindowPos(window, nullptr, 0, 0, 129, 128,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (self->draw) {
                const RECT image{16, 16, 48, 48};
                HBRUSH brush = CreateSolidBrush(self->color);
                FillRect(reinterpret_cast<HDC>(wParam), &image, brush);
                DeleteObject(brush);
            }
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    Fixture() {
        WNDCLASSW cls{};
        cls.lpfnWndProc = Proc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"FluentShell.DirectUiVirtualImageFixture";
        RegisterClassW(&cls);
        host = CreateWindowExW(0, cls.lpszClassName, L"", WS_POPUP,
            100, 100, 128, 128, nullptr, nullptr, cls.hInstance, this);
    }
    ~Fixture() { if (host) DestroyWindow(host); }
};

inline void CheckContract(void (*check)(bool, const char*)) {
    DirectUiBootstrapEvidence native;
    native.native.clientOriginScreen = {100, 100};
    native.native.directUi.hwnd = reinterpret_cast<HWND>(0x100);
    native.native.directUi.bounds = {100, 100, 400, 400};
    DirectUiBootstrapWindowEvidence host;
    host.window.hwnd = native.native.directUi.hwnd;
    host.window.generation = 42;
    host.window.bounds = native.native.directUi.bounds;
    host.window.visible = true;
    host.clientBounds = {0, 0, 300, 300};
    host.clientOriginScreen = {100, 100};
    native.descendants.push_back(host);
    DirectUiSemanticEvidence semantic;
    semantic.controlType = UIA_ImageControlTypeId;
    semantic.imageHostHwnd = host.window.hwnd;
    semantic.frameworkId = L"DirectUI";
    semantic.semanticKey = L"arbitrary-resource-independent-identifier";
    semantic.name = L"An arbitrary provider image";
    semantic.bounds = {116, 116, 148, 148};
    semantic.patternMask = DirectUiPatternValue;
    semantic.valueReadOnly = true;
    DirectUiSlot slot;
    std::wstring error;
    check(ConfigureDirectUiVirtualImage(native, semantic, slot, error) &&
        slot.virtualSource && slot.captureBitmap && slot.kind == ControlKind::StaticIcon &&
        slot.presentationVariant == L"standard" && slot.imageHostGeneration == 42 &&
        slot.imageClientBounds.left == 16 && slot.imageClientBounds.right == 48,
        "DirectUI image was not derived from its native host and provider rectangle");
    semantic.name.clear();
    check(ConfigureDirectUiVirtualImage(native, semantic, slot, error),
        "DirectUI image pixels were incorrectly selected by accessible name");
    const auto valid = semantic;
    semantic.actionable = true;
    check(!ConfigureDirectUiVirtualImage(native, semantic, slot, error),
        "actionable virtual image lost its action in a pixel projection");
    semantic = valid;
    semantic.valueReadOnly = false;
    check(!ConfigureDirectUiVirtualImage(native, semantic, slot, error),
        "writable virtual image was admitted as inert");
    semantic = valid;
    semantic.bounds.right = semantic.bounds.left + 97;
    check(!ConfigureDirectUiVirtualImage(native, semantic, slot, error),
        "virtual image exceeded the owned icon dimension cap");
    semantic = valid;
    semantic.bounds.left = 60;
    check(!ConfigureDirectUiVirtualImage(native, semantic, slot, error),
        "virtual image escaped its native host client");
    // An AeroWizard's page icon reaches a few pixels past its host's client edge:
    // the part the host paints is the image.
    semantic = valid;
    semantic.bounds.top = 95;
    check(ConfigureDirectUiVirtualImage(native, semantic, slot, error) &&
        slot.imageClientBounds.top == 0 && slot.imageClientBounds.bottom == 48,
        "a virtual image a few pixels past its host was refused instead of clipped");
    semantic = valid;
    semantic.imageHostHwnd = reinterpret_cast<HWND>(0x200);
    check(!ConfigureDirectUiVirtualImage(native, semantic, slot, error),
        "virtual image accepted a UIA host outside the native inventory");
    semantic = valid;
    auto overlapping = host;
    overlapping.window.hwnd = reinterpret_cast<HWND>(0x200);
    overlapping.window.bounds = semantic.bounds;
    native.descendants.push_back(overlapping);
    check(!ConfigureDirectUiVirtualImage(native, semantic, slot, error),
        "virtual image absorbed a separate native control");
}

inline void CheckPaintAndEvidence(void (*check)(bool, const char*)) {
    Fixture fixture;
    check(fixture.host != nullptr, "DirectUI image paint fixture failed to initialize");
    if (!fixture.host) return;
    std::wstring error;
    DirectUiWindowEvidence image;
    check(CaptureDirectUiVirtualImagePixels(fixture.host, {16, 16, 48, 48}, image, error),
        "virtual image did not capture the native host print surface");
    check(image.imageWidth == 32 && image.imageHeight == 32 &&
        image.imageFormat == L"bgra8-premultiplied" && image.imageData.size() == 4096 &&
        image.imageData[0] == 67 && image.imageData[1] == 45 &&
        image.imageData[2] == 23 && image.imageData[3] == 255,
        "virtual image crop did not preserve the host's owned pixels");
    if (image.imageData.size() != 4096) return;
    const auto copied = image;
    DirectUiWindowEvidence padded;
    check(CaptureDirectUiVirtualImagePixels(fixture.host, {8, 8, 56, 56}, padded, error),
        "virtual image with an unpainted border was refused");
    bool borderPreserved = padded.imageWidth == 48 && padded.imageHeight == 48 &&
        padded.imageData.size() == 48u * 48u * 4u;
    for (size_t y = 0; borderPreserved && y < 48; ++y) {
        for (size_t x = 0; borderPreserved && x < 48; ++x) {
            const bool painted = x >= 8 && x < 40 && y >= 8 && y < 40;
            const size_t offset = (y * 48 + x) * 4;
            borderPreserved = padded.imageData[offset] == (painted ? 67 : 0) &&
                padded.imageData[offset + 1] == (painted ? 45 : 0) &&
                padded.imageData[offset + 2] == (painted ? 23 : 0) &&
                padded.imageData[offset + 3] == (painted ? 255 : 0);
        }
    }
    check(borderPreserved, "virtual image made unpainted border pixels opaque");
    fixture.color = RGB(0, 0, 0);
    DirectUiWindowEvidence black;
    check(CaptureDirectUiVirtualImagePixels(fixture.host, {16, 16, 48, 48}, black, error) &&
        black.imageData.size() == 4096 && black.imageData[0] == 0 &&
        black.imageData[1] == 0 && black.imageData[2] == 0 && black.imageData[3] == 255,
        "virtual image confused owned black pixels with unpainted pixels");
    fixture.color = RGB(23, 45, 67);
    bool foreignThreadAccepted = true;
    std::thread foreign([&] {
        DirectUiWindowEvidence rejected;
        std::wstring rejectedError;
        foreignThreadAccepted = CaptureDirectUiVirtualImagePixels(
            fixture.host, {16, 16, 48, 48}, rejected, rejectedError);
    });
    foreign.join();
    check(!foreignThreadAccepted, "DirectUI image capture ran outside its native GUI thread");
    check(!CaptureDirectUiVirtualImagePixels(fixture.host, {-1, 16, 48, 48}, image, error),
        "DirectUI image capture accepted pixels outside the native client");
    fixture.draw = false;
    check(!CaptureDirectUiVirtualImagePixels(fixture.host, {16, 16, 48, 48}, image, error) &&
        image.imageData.empty(), "unpainted image silently fell back to unrelated pixels");
    fixture.draw = true;
    fixture.resizeOnPrint = true;
    check(!CaptureDirectUiVirtualImagePixels(fixture.host, {16, 16, 48, 48}, image, error),
        "DirectUI image capture accepted a host resized during print rendering");

    DirectUiSlot slot;
    slot.kind = ControlKind::StaticIcon;
    slot.virtualSource = true;
    slot.captureBitmap = true;
    DirectUiWindowProfile profile;
    profile.slots = &slot;
    profile.slotCount = 1;
    DirectUiNativeEvidence before;
    before.slotWindows = { copied };
    auto after = before;
    check(MatchDirectUiMutationBracket(profile, before, after, error),
        "unchanged DirectUI image evidence was refused");
    after.slotWindows[0].imageData[0] ^= 1;
    check(!MatchDirectUiMutationBracket(profile, before, after, error),
        "DirectUI image pixels changed across the semantic gate without rejection");
    after = before;
    after.slotWindows[0].generation += 1;
    check(!MatchDirectUiMutationBracket(profile, before, after, error),
        "DirectUI image host recreation escaped the native evidence bracket");
    DestroyWindow(fixture.host);
    fixture.host = nullptr;
    check(copied.imageData[0] == 67 && copied.imageData[3] == 255,
        "DirectUI image retained borrowed source storage");
}
} // namespace DirectUiVirtualImages

inline void TestDirectUiVirtualImages(void (*check)(bool, const char*)) {
    DirectUiVirtualImages::CheckContract(check);
    DirectUiVirtualImages::CheckPaintAndEvidence(check);
}
} // namespace FluentShell::Tests
