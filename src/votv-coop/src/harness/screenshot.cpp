#include "harness/screenshot.h"

#include "ue_wrap/log.h"

#include <windows.h>
#include <gdiplus.h>

#include <atomic>
#include <string>
#include <vector>

namespace harness::screenshot {
namespace {

std::atomic<bool> g_running{false};

// Directory of this mod DLL (the game's Win64 dir) -- screenshots land beside it.
std::wstring ModuleDir() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? L"." : p.substr(0, sep);
}

// The biggest visible top-level window owned by our process -- the game view.
struct FindCtx { DWORD pid; HWND best; long long bestArea; };
BOOL CALLBACK EnumCb(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<FindCtx*>(lp);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !::IsWindowVisible(h)) return TRUE;
    RECT r{};
    if (!::GetClientRect(h, &r)) return TRUE;
    const long long area = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
    if (area > c->bestArea) { c->bestArea = area; c->best = h; }
    return TRUE;
}
HWND FindGameWindow() {
    FindCtx ctx{::GetCurrentProcessId(), nullptr, 0};
    ::EnumWindows(&EnumCb, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// Resolve the GDI+ encoder CLSID for a MIME type (e.g. L"image/png").
bool GetEncoderClsid(const wchar_t* mime, CLSID* out) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) { *out = codecs[i].Clsid; return true; }
    }
    return false;
}

// PrintWindow the client area into a PNG. PW_RENDERFULLCONTENT (2) grabs the
// game's swapchain content without bringing the window to the foreground.
bool CapturePng(HWND hwnd, const std::wstring& path) {
    RECT rc{};
    if (!::GetClientRect(hwnd, &rc)) return false;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    HDC hdcWin = ::GetDC(hwnd);
    HDC hdcMem = ::CreateCompatibleDC(hdcWin);
    HBITMAP bmp = ::CreateCompatibleBitmap(hdcWin, w, h);
    HGDIOBJ old = ::SelectObject(hdcMem, bmp);

    const BOOL printed = ::PrintWindow(hwnd, hdcMem, 2 /*PW_RENDERFULLCONTENT*/);

    bool ok = false;
    if (printed) {
        Gdiplus::Bitmap gb(bmp, nullptr);
        CLSID png{};
        if (GetEncoderClsid(L"image/png", &png)) {
            ok = (gb.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok);
        }
    }

    ::SelectObject(hdcMem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(hdcMem);
    ::ReleaseDC(hwnd, hdcWin);
    return ok;
}

DWORD WINAPI WatcherThread(LPVOID) {
    Gdiplus::GdiplusStartupInput gi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gi, nullptr);

    const std::wstring dir = ModuleDir() + L"\\coop-screenshots";
    ::CreateDirectoryW(dir.c_str(), nullptr);

    bool prevDown = false;
    while (g_running.load(std::memory_order_relaxed)) {
        const bool down = (::GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (down && !prevDown) {
            HWND h = FindGameWindow();
            if (h) {
                SYSTEMTIME t{};
                ::GetLocalTime(&t);
                wchar_t name[64] = {};
                swprintf(name, 64, L"coop-%04d%02d%02d-%02d%02d%02d.png",
                         t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
                const std::wstring path = dir + L"\\" + name;
                const bool ok = CapturePng(h, path);
                UE_LOGI("screenshot: F12 -> %ls (%s)", path.c_str(), ok ? "ok" : "FAILED");
            } else {
                UE_LOGW("screenshot: F12 -- no game window found");
            }
        }
        prevDown = down;
        ::Sleep(40);
    }

    Gdiplus::GdiplusShutdown(token);
    return 0;
}

}  // namespace

void StartHotkeyWatcher() {
    if (g_running.exchange(true)) return;  // already running
    if (HANDLE t = ::CreateThread(nullptr, 0, WatcherThread, nullptr, 0, nullptr)) {
        ::CloseHandle(t);
        UE_LOGI("screenshot: F12 hotkey watcher started (saves to coop-screenshots/)");
    } else {
        g_running.store(false);
        UE_LOGE("screenshot: failed to start F12 watcher thread");
    }
}

}  // namespace harness::screenshot
