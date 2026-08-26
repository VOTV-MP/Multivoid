// ui/server_browser_selftest.cpp -- see ui/server_browser_selftest.h.
//
// EXTRACTED VERBATIM from ui/server_browser_native.cpp 2026-08-26. The scrim and ESC
// phases below are the shipped body moved without a behavioural edit; the only changes
// are the seam (`g_scrimW` -> the `scrim` parameter, `SelfCheckTick` -> `Tick`) and the
// namespace. The T0 scroll phases were added afterwards, in their own commit.

#include "ui/server_browser_selftest.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

namespace ui::server_browser_selftest {
namespace {

namespace E = ue_wrap::engine;

int g_selfCheckStep     = -1;  // -1 = idle; the dev scrim self-check's phase counter
int g_scrimOutside      = -1;  // -1 = not sampled, never a negative (an unrun phase is not a NO)
int g_scrimInsideWindow = -1;

}  // namespace

// THE SCRIM IS FUNCTIONAL, NOT DECORATIVE -- it is what eats a click that misses the
// window, so "it looks dim in the screenshot" is not evidence. This asks the only question
// that matters, the way RUNG 2 established: put the cursor somewhere OUTSIDE the window
// (over the menu's own button list) and ask Slate whether the scrim is under it. A window
// hit is checked too, so a scrim that answers `true` everywhere is not read as a pass.
//
// Runs only under [dev] browser_autoopen and only once. The move and the sample are
// SEPARATE ticks: sampling IsHovered() in the same tick as the move reads the PREVIOUS
// pointer position (measured 2026-08-26 -- the probe made exactly that mistake).
void Tick(void* scrim, void* list) {
    (void)list;   // the seam the header declares; T0's scroll phases are the consumer
    if (g_selfCheckStep < 0 || !scrim) return;
    HWND hwnd = ::GetActiveWindow();
    RECT cr{};
    if (!hwnd || !::GetClientRect(hwnd, &cr)) { g_selfCheckStep = -1; return; }
    auto moveTo = [&](int cx, int cy) {
        POINT pt{cx, cy};
        if (::ClientToScreen(hwnd, &pt)) ::SetCursorPos(pt.x, pt.y);
    };
    const int w = cr.right - cr.left, h = cr.bottom - cr.top;
    switch (g_selfCheckStep) {
        case 0:  // OUTSIDE the window: far left, over the menu's own button column.
            moveTo(w / 12, h * 3 / 4);
            break;
        case 8:
            g_scrimOutside = E::WidgetIsHovered(scrim) ? 1 : 0;
            break;
        case 9:  // INSIDE the window, where the list is.
            moveTo(w / 2, h / 2);
            break;
        case 17:
            g_scrimInsideWindow = E::WidgetIsHovered(scrim) ? 1 : 0;
            if (g_scrimOutside == 1)
                UE_LOGW("server_browser_native: SCRIM SELFTEST PASS -- the scrim is hovered "
                        "OUTSIDE the window (%d) so it spans the screen and absorbs a stray "
                        "click; inside-the-window reading %d (the window's own widgets sit "
                        "above it, so either value is consistent there).",
                        g_scrimOutside, g_scrimInsideWindow);
            else
                UE_LOGE("server_browser_native: SCRIM SELFTEST FAIL -- outside=%d inside=%d. The "
                        "scrim does NOT cover the screen, so a click that misses the window "
                        "reaches VOTV's own menu buttons underneath.",
                        g_scrimOutside, g_scrimInsideWindow);
            break;
        case 24:
            // ESC SELFTEST. Until the chrome exists ESC is the ONLY way out, and an escape
            // hatch nobody has seen work is not an escape hatch. Synthesize a real key so
            // the production poll (GetAsyncKeyState in OnMenuTick) is what answers -- not a
            // direct Hide() call, which would prove only that Hide() compiles.
            // PRESS and hold. Down+up back-to-back in one tick is invisible to a per-tick
            // GetAsyncKeyState poll -- the key is already released before the next tick
            // samples it -- which is exactly how the first version of this selftest
            // "passed" while the hatch did nothing. A human holds a key for tens of ms,
            // i.e. several ticks; the synthesis has to do the same.
            //
            // NOTE the wording of this line: it deliberately does NOT contain the string
            // the runner asserts on. The first version quoted its own expected output, so
            // the runner's find() matched THIS line and reported ALL PASS on a failure.
            UE_LOGW("server_browser_native: ESC SELFTEST -- holding VK_ESCAPE for several ticks; "
                    "the close line below is the only evidence that counts");
            ::keybd_event(VK_ESCAPE, 0, 0, 0);
            break;
        case 30:
            ::keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
            g_selfCheckStep = -1;
            return;
        default:
            break;
    }
    ++g_selfCheckStep;
}

void Arm() { g_selfCheckStep = 0; }

}  // namespace ui::server_browser_selftest
