// ui/overlay_cursor.cpp -- see ui/overlay_cursor.h.
//
// Ported in shape from MTA's `CLocalGUI::Draw`
// (reference/mtasa-blue/Client/core/CGUI.cpp:800-848), MIT. Divergences from that source
// are commented at their site with the reason, per RULE 2026-05-28.

#include "ui/overlay_cursor.h"

#include "ue_wrap/core/log.h"

namespace ui::overlay_cursor {
namespace {

bool  g_hadCapture = false;
POINT g_stored{};
bool  g_storedValid = false;

}  // namespace

void FrameTransition(HWND hwnd, bool captureActive, SetCursorPosFn origSetCursorPos) {
    if (captureActive == g_hadCapture) return;  // no transition: do nothing at all
    g_hadCapture = captureActive;
    if (!hwnd || !origSetCursorPos) return;

    RECT cr{};
    if (!::GetClientRect(hwnd, &cr)) return;
    POINT origin{cr.left, cr.top};
    if (!::ClientToScreen(hwnd, &origin)) return;
    const POINT centre{origin.x + (cr.right - cr.left) / 2,
                       origin.y + (cr.bottom - cr.top) / 2};

    if (captureActive) {
        // ENTER. Restore where the player left the pointer last time a surface was up, so
        // it appears under their hand rather than wherever the game's last recentre put
        // it. MTA restores unconditionally from a POINT that starts zeroed; we gate on
        // `g_storedValid` instead, because a (0,0) restore on the FIRST surface of the
        // session would park the pointer in the screen corner -- which is the exact
        // symptom this arc spent two probe runs mistaking for a root cause.
        POINT want = g_storedValid ? g_stored : centre;
        // Clamp into the client rect: a stored position from a previous window size (or a
        // resolution change between surfaces) would otherwise restore off-screen.
        if (want.x < origin.x || want.x >= origin.x + (cr.right - cr.left) ||
            want.y < origin.y || want.y >= origin.y + (cr.bottom - cr.top))
            want = centre;
        ::SetLastError(0);
        const BOOL ok = origSetCursorPos(want.x, want.y);
        POINT got{};
        ::GetCursorPos(&got);
        // Write-then-verify: `SetCursorPos` is silently CLAMPED by whatever `ClipCursor`
        // rect is live, and we do not own that rect. A mismatch is a real diagnosis (the
        // clip is fighting us), not something to absorb.
        if (!ok || got.x != want.x || got.y != want.y)
            UE_LOGW("overlay_cursor: enter-restore asked (%ld,%ld) got (%ld,%ld) ok=%d "
                    "err=%lu -- a ClipCursor rect is likely clamping",
                    want.x, want.y, got.x, got.y, (int)ok, ::GetLastError());
    } else {
        // EXIT. Store, then recentre -- MTA's own comment: "Set the mouse back to the
        // center of the screen (to prevent the game from reacting to its movement)".
        // Without this the game resumes mouselook with the whole accumulated offset
        // between the centre and wherever the player parked the pointer.
        // Divergence: MTA centres the SCREEN, we centre the CLIENT RECT, because this
        // game runs windowed in every dev and most player configurations and the screen
        // centre is not inside the window.
        POINT p{};
        if (::GetCursorPos(&p)) { g_stored = p; g_storedValid = true; }
        origSetCursorPos(centre.x, centre.y);
    }
}

}  // namespace ui::overlay_cursor
