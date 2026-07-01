// capture.exe — tiny screenshot helper (GDI BitBlt + GDI+ PNG), no .NET deps.
//   capture.exe pid <pid> <out.png> [pad]   capture union of that PID's visible titled windows
//   capture.exe rect <x> <y> <w> <h> <out.png>
//   capture.exe screen <out.png>            full virtual screen
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <cstdlib>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
using namespace Gdiplus;

static int GetEncoderClsid(const WCHAR* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (!size) return -1;
    auto* info = (ImageCodecInfo*)malloc(size);
    if (!info) return -1;
    GetImageEncoders(num, size, info);
    int found = -1;
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(info[i].MimeType, mime) == 0) { *clsid = info[i].Clsid; found = (int)i; break; }
    free(info);
    return found;
}

static bool SavePng(int x, int y, int w, int h, const wchar_t* path) {
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
    Bitmap gb(bmp, nullptr);
    CLSID png;
    bool ok = false;
    if (GetEncoderClsid(L"image/png", &png) >= 0)
        ok = (gb.Save(path, &png, nullptr) == Ok);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

static DWORD g_pid;
static std::vector<RECT> g_rects;
static BOOL CALLBACK EnumProc(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == g_pid && IsWindowVisible(h) && GetWindowTextLengthW(h) > 0) {
        RECT r;
        if (GetWindowRect(h, &r)) g_rects.push_back(r);
    }
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    GdiplusStartupInput gi;
    ULONG_PTR tok;
    GdiplusStartup(&tok, &gi, nullptr);
    int rc = 1;

    if (argc >= 4 && _wcsicmp(argv[1], L"pid") == 0) {
        g_pid = (DWORD)_wtoi(argv[2]);
        const wchar_t* out = argv[3];
        int pad = (argc >= 5) ? _wtoi(argv[4]) : 32;
        g_rects.clear();
        EnumWindows(EnumProc, 0);
        if (!g_rects.empty()) {
            RECT u = g_rects[0];
            for (auto& r : g_rects) {
                u.left = min(u.left, r.left); u.top = min(u.top, r.top);
                u.right = max(u.right, r.right); u.bottom = max(u.bottom, r.bottom);
            }
            int x = u.left - pad, y = u.top - pad;
            int w = (u.right - u.left) + 2 * pad, h = (u.bottom - u.top) + 2 * pad;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            rc = SavePng(x, y, w, h, out) ? 0 : 1;
            wprintf(L"pid %u: %zu window(s), region %dx%d -> %s (%s)\n",
                    g_pid, g_rects.size(), w, h, out, rc == 0 ? L"ok" : L"FAILED");
        } else {
            wprintf(L"pid %u: no visible titled windows\n", g_pid);
        }
    } else if (argc >= 7 && _wcsicmp(argv[1], L"rect") == 0) {
        rc = SavePng(_wtoi(argv[2]), _wtoi(argv[3]), _wtoi(argv[4]), _wtoi(argv[5]), argv[6]) ? 0 : 1;
    } else if (argc >= 3 && _wcsicmp(argv[1], L"screen") == 0) {
        int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        rc = SavePng(x, y, w, h, argv[2]) ? 0 : 1;
    } else {
        wprintf(L"usage: capture pid <pid> <out.png> [pad] | rect x y w h out.png | screen out.png\n");
    }
    GdiplusShutdown(tok);
    return rc;
}
