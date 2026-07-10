// ui/input_focus.cpp -- see ui/input_focus.h.

#include "ui/input_focus.h"

#include <windows.h>

#include <atomic>

namespace ui::input_focus {

bool IsOurWindowForeground() {
    HWND fg = ::GetForegroundWindow();
    if (!fg) return true;  // defensive: if we can't tell, don't break the hotkey
    DWORD ownerPid = 0;
    ::GetWindowThreadProcessId(fg, &ownerPid);
    return ownerPid == ::GetCurrentProcessId();
}

// Overlay-text-capture gate (2026-07-09). Set by the render thread each frame, read
// by the hotkey pollers on their own threads -> a plain atomic, relaxed (a single
// independent bool; a one-frame staleness is harmless for a held-key poll).
static std::atomic<bool> g_overlayCapturingText{false};

void SetOverlayCapturingText(bool capturing) {
    g_overlayCapturingText.store(capturing, std::memory_order_relaxed);
}

bool IsOverlayCapturingText() {
    return g_overlayCapturingText.load(std::memory_order_relaxed);
}

}  // namespace ui::input_focus
