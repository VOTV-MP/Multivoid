// ui/overlay_cursor.cpp -- see ui/overlay_cursor.h.
//
// Ported in shape from MTA's `CLocalGUI::Draw`
// (reference/mtasa-blue/Client/core/CGUI.cpp:800-848), MIT. Divergences from that source
// are commented at their site with the reason, per RULE 2026-05-28.

#include "ui/overlay_cursor.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/world_identity.h"

#include <atomic>
#include <cstdint>

namespace ui::overlay_cursor {

namespace WID = ue_wrap::world_identity;

namespace {

bool  g_hadCapture = false;
POINT g_stored{};
bool  g_storedValid = false;

// When the GAME last drove the OS pointer. NOT a gate -- see FrameTransition: it is
// EVIDENCE, logged beside every decision, because the one time it was promoted to a gate
// a single run falsified its premise. Written from the SetCursorPos detour (whatever
// thread that runs on), read on the render thread; a relaxed atomic is enough for a
// number that is only ever printed.
std::atomic<uint64_t> g_lastGameWriteMs{0};

}  // namespace

void NoteGameCursorWrite() {
    g_lastGameWriteMs.store(::GetTickCount64(), std::memory_order_relaxed);
}

void FrameTransition(HWND hwnd, bool captureActive, SetCursorPosFn origSetCursorPos) {
    if (captureActive == g_hadCapture) return;  // no transition: do nothing at all
    // The capture state is tracked EVEN WHEN WE DECLINE TO WARP, so that a later
    // transition -- once the game IS driving the pointer -- sees a true edge rather than
    // a stale one accumulated across every menu-side open and close.
    g_hadCapture = captureActive;
    if (!hwnd || !origSetCursorPos) return;
    {
        // THE DECISION TERM IS THE WORLD, and the cursor-write age beside it is EVIDENCE,
        // not a second gate. Read the header for why a warp is wrong outside gameplay.
        //
        // A "warp only while the game is actually driving the pointer" gate was designed
        // here first, on the strength of overlay_diag.h's note that "the game recentres
        // the pointer ~118x/s while playing". THE NOTE IS TRUE -- tools/cursor_probe.py
        // on 2026-08-26 printed scpCalls 1340 -> 2779 over ~12 s, i.e. ~120/s. What is
        // false is that the signal can GATE this transition: measured in the same runs,
        // every capture transition reported everSeen=0, because capture flips at the
        // first present and the game's recentre loop has not started yet. A freshness
        // gate would therefore have skipped the FIRST transition of every session and
        // warped correctly only afterwards -- silently wrong exactly where it matters.
        //
        // (I recorded the opposite conclusion here first -- "the game issued ZERO
        // SetCursorPos calls" -- from a single everSeen=0 reading taken AT a transition,
        // and generalised it to 18 seconds of runtime I had not measured. cursor_probe's
        // own counter refuted it minutes later. The age stays logged below so the next
        // reader gets the number rather than either of my summaries of it.)
        const WID::WorldKind kind = WID::CurrentWorldKind();
        const uint64_t last = g_lastGameWriteMs.load(std::memory_order_relaxed);
        const uint64_t age  = last ? (::GetTickCount64() - last) : 0;
        // POSITIVE Gameplay only. `Unknown` is published for ~1 s across every travel and
        // for the whole boot window, and it NEVER means "not in gameplay" -- so nothing
        // fires on it. (`docs/COOP_SYNC` reading-order 4j: every `!` over a bool
        // projection of this enum is a bug candidate; this is the shape that avoids one.)
        if (kind != WID::WorldKind::Gameplay) {
            UE_LOGI("overlay_cursor: transition -> capture=%d SKIPPED (worldKind=%s, so the game "
                    "is not mouselooking and there is no pointer ownership to hand back) "
                    "[gameCursorWrites everSeen=%d ageMs=%llu]",
                    captureActive ? 1 : 0,
                    kind == WID::WorldKind::Other ? "Other" : "Unknown", last ? 1 : 0,
                    static_cast<unsigned long long>(age));
            return;
        }
        UE_LOGI("overlay_cursor: transition -> capture=%d WARPING (worldKind=Gameplay) "
                "[gameCursorWrites everSeen=%d ageMs=%llu]",
                captureActive ? 1 : 0, last ? 1 : 0, static_cast<unsigned long long>(age));
    }

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
