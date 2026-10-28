// coop/dev/game_window.cpp -- see coop/dev/game_window.h.

#include "coop/dev/game_window.h"

#include "coop/input/game_window_proc.h"

namespace coop::dev::game_window {
namespace {

BOOL CALLBACK PickThreadWindow(HWND h, LPARAM lp) {
    if (!::IsWindowVisible(h)) return TRUE;
    RECT rc{};
    if (!::GetClientRect(h, &rc) || rc.right - rc.left < 64 || rc.bottom - rc.top < 64) return TRUE;
    *reinterpret_cast<HWND*>(lp) = h;
    return FALSE;  // first match wins
}

}  // namespace

HWND GameWindow() {
    // The window the mod subclasses is the one its keys and messages travel through, so it is the
    // one to post to; the thread scan below answers only before that subclass exists.
    if (void* subclassed = coop::input::game_window_proc::Hwnd()) return static_cast<HWND>(subclassed);
    HWND found = nullptr;
    ::EnumThreadWindows(::GetCurrentThreadId(), &PickThreadWindow, reinterpret_cast<LPARAM>(&found));
    return found;
}

namespace {

// lParam carries the repeat count and the scan code; the release sets the previous-state and transition bits.
LPARAM KeyParam(unsigned vk, bool up) {
    const LPARAM scan = static_cast<LPARAM>(::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
    return up ? (1 | scan | (1LL << 30) | (1LL << 31)) : (1 | scan);
}

}  // namespace

bool PostKeyDown(unsigned vk) {
    HWND hwnd = GameWindow();
    return hwnd && vk != 0 && ::PostMessageW(hwnd, WM_KEYDOWN, vk, KeyParam(vk, false)) != 0;
}

bool PostKeyUp(unsigned vk) {
    HWND hwnd = GameWindow();
    return hwnd && vk != 0 && ::PostMessageW(hwnd, WM_KEYUP, vk, KeyParam(vk, true)) != 0;
}

bool PostKeyPress(unsigned vk) {
    const bool down = PostKeyDown(vk);
    const bool up = PostKeyUp(vk);
    return down && up;
}

bool PostChar(wchar_t c) {
    HWND hwnd = GameWindow();
    if (!hwnd) return false;
    return ::PostMessageW(hwnd, WM_CHAR, static_cast<WPARAM>(c), 1) != 0;
}

unsigned VirtualKeyOf(const std::wstring& displayName) {
    if (displayName.size() == 1) {
        const wchar_t c = displayName[0];
        if (c >= L'A' && c <= L'Z') return static_cast<unsigned>(c);
        if (c >= L'a' && c <= L'z') return static_cast<unsigned>(c - L'a' + L'A');
        if (c >= L'0' && c <= L'9') return static_cast<unsigned>(c);
    }
    struct Named { const wchar_t* name; unsigned vk; };
    static const Named kNamed[] = {
        {L"Space Bar", VK_SPACE}, {L"Space", VK_SPACE}, {L"Enter", VK_RETURN}, {L"Tab", VK_TAB},
        {L"Backspace", VK_BACK},  {L"Escape", VK_ESCAPE},
    };
    for (const Named& n : kNamed)
        if (displayName == n.name) return n.vk;
    return 0;
}

}  // namespace coop::dev::game_window
