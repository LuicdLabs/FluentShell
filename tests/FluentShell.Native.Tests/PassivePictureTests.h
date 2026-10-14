#pragma once

// Include after main.cpp's Check helper, inside its test namespace.

// A control the application only paints -- the Date and Time clock, the mouse drawn in Mouse
// Properties, the Volume Mixer's level meter -- is projected as the picture it paints.  One
// that could take input (a tab stop, scroll bars) or is an MFC document surface is not.
LRESULT CALLBACK PassivePictureProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        const HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wParam);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        const HBRUSH accent = CreateSolidBrush(RGB(0, 120, 215));
        const HGDIOBJ previous = SelectObject(dc, accent);
        Ellipse(dc, 4, 4, client.right - 4, client.bottom - 4);
        SelectObject(dc, previous);
        DeleteObject(accent);
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void TestPassivePictureControls() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    for (const wchar_t* name : { L"FluentShellTestClock", L"AfxTestView" }) {
        WNDCLASSW picture{};
        picture.lpfnWndProc = PassivePictureProc;
        picture.hInstance = instance;
        picture.lpszClassName = name;
        picture.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&picture);
    }
    const auto capture = [&](const wchar_t* className, DWORD style, Translation::WindowSnapshot& snapshot,
                             std::wstring& error) {
        struct WindowOwner final {
            HWND window;
            ~WindowOwner() { if (window) DestroyWindow(window); }
        } host{ CreateWindowExW(0, WC_DIALOG, L"passive-picture", WS_POPUP | WS_CAPTION | WS_SYSMENU,
            60, 60, 320, 240, nullptr, nullptr, instance, nullptr) };
        const HWND clock = host.window ? CreateWindowExW(0, className, L"", WS_CHILD | WS_VISIBLE | style,
            20, 20, 140, 140, host.window, reinterpret_cast<HMENU>(301), instance, nullptr) : nullptr;
        const HWND ok = host.window ? CreateWindowExW(0, L"Button", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            200, 180, 90, 26, host.window, reinterpret_cast<HMENU>(1), instance, nullptr) : nullptr;
        if (!clock || !ok) return false;
        ShowWindow(host.window, SW_SHOWNOACTIVATE);
        UpdateWindow(host.window);
        Translation::CaptureContext context;
        context.surfaceId = L"77777777-7777-7777-7777-adadadadadad";
        context.generation = 1;
        context.revision = 1;
        return Translation::CaptureWindow(host.window, context, snapshot, error);
    };
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = capture(L"FluentShellTestClock", 0, snapshot, error);
    if (!captured) std::wcerr << L"passive picture capture rejection: " << error << L'\n';
    const auto picture = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& node) { return node.controlId == 301; });
    Check(captured && picture != snapshot.nodes.end() &&
          picture->kind == Translation::ControlKind::StaticBitmap &&
          picture->imageWidth == 140 && picture->imageHeight == 140 &&
          picture->imageData.size() == 140u * 140u * 4u,
        "a passive painted control was not projected as the picture it paints");
    Check(!capture(L"FluentShellTestClock", WS_TABSTOP, snapshot, error) &&
          error.find(L"FluentShellTestClock") != std::wstring::npos,
        "a painted control with a tab stop was projected as a picture");
    Check(!capture(L"FluentShellTestClock", WS_VSCROLL, snapshot, error),
        "a scrolling painted control was projected as a picture");
    Check(!capture(L"AfxTestView", 0, snapshot, error) && error.find(L"AfxTestView") != std::wstring::npos,
        "an MFC document surface was projected as a picture");
}
