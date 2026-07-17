#include "harness/screenshot.h"

#include "coop/session/shutdown.h"
#include "ue_wrap/core/log.h"

#include <windows.h>
#include <gdiplus.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace harness::screenshot {
namespace {

std::atomic<bool> g_running{false};

// GDI+ must be initialized once per process before any Bitmap/encoder use.
// Both Capture() (programmatic, any thread) and the F12 watcher rely on it, so
// init lazily exactly once instead of only on the watcher thread.
std::once_flag g_gdiOnce;
void EnsureGdiPlus() {
    std::call_once(g_gdiOnce, [] {
        Gdiplus::GdiplusStartupInput gi;
        ULONG_PTR token = 0;  // intentionally leaked: GDI+ lives for the process
        Gdiplus::GdiplusStartup(&token, &gi, nullptr);
    });
}

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

// Capture the client area to a PNG via PrintWindow (PW_RENDERFULLCONTENT, no focus
// theft). Works for flat UMG/UI (e.g. the OMEGA screen). NOTE: VOTV's 3D gameplay
// is a hardware DX swapchain that GDI cannot read from inside the game's own
// process -> a black frame. For autonomous GAMEPLAY verification use the external
// tools/capture-window.ps1 (a separate process: foreground + screen BitBlt grabs
// the DWM-composited frame). This in-process path is a UI-capture dev aid only.
bool CapturePng(HWND hwnd, const std::wstring& path) {
    RECT rc{};
    if (!::GetClientRect(hwnd, &rc)) return false;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    HDC hdcWin = ::GetDC(hwnd);
    HDC hdcMem = ::CreateCompatibleDC(hdcWin);
    HBITMAP bmp = ::CreateCompatibleBitmap(hdcWin, w, h);
    HGDIOBJ old = ::SelectObject(hdcMem, bmp);

    const bool haveFrame = ::PrintWindow(hwnd, hdcMem, 2 /*PW_RENDERFULLCONTENT*/) != 0;

    bool ok = false;
    if (haveFrame) {
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
    bool prevDown = false;
    while (g_running.load(std::memory_order_relaxed) && !coop::shutdown::IsShuttingDown()) {
        const bool down = (::GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (down && !prevDown) Capture(L"f12");
        prevDown = down;
        ::Sleep(40);
    }
    return 0;
}

}  // namespace

bool Capture(const wchar_t* label) {
    EnsureGdiPlus();
    const std::wstring dir = ModuleDir() + L"\\coop-screenshots";
    ::CreateDirectoryW(dir.c_str(), nullptr);

    HWND h = FindGameWindow();
    if (!h) { UE_LOGW("screenshot: Capture('%ls') -- no game window found", label ? label : L""); return false; }

    SYSTEMTIME t{};
    ::GetLocalTime(&t);
    wchar_t name[96] = {};
    swprintf(name, 96, L"coop-%04d%02d%02d-%02d%02d%02d-%ls.png",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             (label && *label) ? label : L"shot");
    const std::wstring path = dir + L"\\" + name;
    const bool ok = CapturePng(h, path);
    UE_LOGI("screenshot: '%ls' -> %ls (%s)", label ? label : L"", path.c_str(), ok ? "ok" : "FAILED");
    return ok;
}

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
