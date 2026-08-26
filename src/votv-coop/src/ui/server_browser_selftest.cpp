// ui/server_browser_selftest.cpp -- see ui/server_browser_selftest.h.
//
// EXTRACTED VERBATIM from ui/server_browser_native.cpp 2026-08-26. The scrim and ESC
// phases below are the shipped body moved without a behavioural edit; the only changes
// were the seam (`g_scrimW` -> the `scrim` parameter, `SelfCheckTick` -> `Tick`) and the
// namespace. The T0 scroll phases were added afterwards, in their own commit.

#include "ui/server_browser_selftest.h"

#include "ui/server_browser_native.h"   // IsOpen()/Open() -- the click phases drive the real screen

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/umg_build.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace ui::server_browser_selftest {
namespace {

namespace E = ue_wrap::engine;
namespace U = ue_wrap::umg;
namespace P = ue_wrap::profile;

// ---- the phase schedule ------------------------------------------------------------
// NAMED, NEVER LITERAL. A positional case ladder renumbered by hand is the exact hazard
// s24b measured on console_desk's g_fields table: a missed index is invisible to BOTH a
// literal diff and the compiler, because every value is still a valid int. Inserting the
// T0 block ahead of the shipped scrim/ESC phases required renumbering all six of them, so
// they bind by name and a future insertion is one edit to this block.
//
// The unit is ONE MENU TICK, i.e. one frame (~8.5 ms at the 117 fps this menu measured).
// The eight-tick gaps are the interval the shipped scrim phases already trust: a cursor
// move and its sample MUST be separate ticks, because IsHovered() read in the same tick as
// the move answers about the PREVIOUS pointer position.
constexpr int kScrollWait     = 0;   // HOLDING -- waits for rows; does not advance
constexpr int kScrollProbe    = 1;
constexpr int kScrollSetBig   = 2;
constexpr int kScrollReadBig  = 12;
constexpr int kScrollHoldA    = 13;  // HOLDING -- wall clock, so a 3 s capture poll lands
constexpr int kScrollSetZero  = 14;
constexpr int kScrollMoveCur  = 22;
constexpr int kScrollWheelPre = 32;
constexpr int kScrollNotch2   = 34;
constexpr int kScrollNotch3   = 36;
constexpr int kScrollNotch4   = 38;
constexpr int kScrollReadPost = 50;
constexpr int kScrollHoldB    = 51;  // HOLDING -- wall clock
constexpr int kScrimMoveOut   = 52;
constexpr int kScrimSampleOut = 60;
constexpr int kScrimMoveIn    = 61;
constexpr int kScrimSampleIn  = 69;
constexpr int kEscPress       = 76;
constexpr int kEscObserve     = 79;
constexpr int kEscRelease     = 82;
// ...then re-open and drive the X, because ESC closing the screen is not evidence that
// the CHROME closes it, and the chrome is what a player will actually reach for.
constexpr int kReopen         = 88;
constexpr int kClickMove      = 100;
constexpr int kClickHover     = 108;
constexpr int kClickDown      = 110;
constexpr int kClickUp        = 114;
constexpr int kClickVerify    = 124;

// The forced offset for the positive control. Far past any real content extent, so a
// getter that returns it UNCHANGED has told us it echoes the request rather than reading
// Slate -- which is the one failure mode that would let a green T0 mean nothing.
constexpr float    kHugeOffset   = 1.0e6f;
constexpr uint64_t kRowWaitMs    = 30000;  // rows arrive over HTTP; 30 s covers a cold fetch
constexpr uint64_t kShotHoldMs   = 6000;   // mp.py polls the log every 3 s
constexpr uint64_t kWindowWaitMs = 15000;  // how long a null active window is a WAIT, not a fault

// THE PRECONDITION, AND WHY IT TAKES TWO TERMS. The first version of this gate waited on
// `GetScrollOffsetOfEnd() > 0` alone, on the theory that a positive maximum implies
// content. MEASURED FALSE 2026-08-26 (run 1): an EMPTY UScrollBox -- rows=0, one tick
// after Show(), before the first lobby fetch returned -- reports offsetOfEnd = 1.0. The
// gate opened, and the whole positive control then ran against a box with nothing in it,
// which is a reading at a degenerate edge dressed up as a measurement.
//
// So both terms, and neither is an epsilon test. ROWS answers "is there content", and it
// is the quantity the fixture actually controls. OVERFLOW answers "is there anywhere to
// go", at a threshold of one full row (server_browser_native.cpp's kRowH) rather than
// >0, so no layout artifact can satisfy it. The viewport measured ~8 rows at 1920x1080,
// so 12 rows overflow it at any plausible window size.
constexpr int   kMinRows     = 12;
constexpr float kMinOverflow = 64.f;   // == kRowH: one whole row past the viewport

int g_selfCheckStep     = -1;  // -1 = idle; the dev scrim self-check's phase counter
int g_scrimOutside      = -1;  // -1 = not sampled, never a negative (an unrun phase is not a NO)
int g_scrimInsideWindow = -1;

// ---- T0 state ----------------------------------------------------------------------
// Every reading starts at a value no measurement can produce, so an unrun phase is
// distinguishable from a phase that ran and read zero. Zero is a LEGITIMATE answer to
// three of these (a list at the top; a list that cannot scroll; a wheel that moved
// nothing), which is precisely why the sentinel has to be negative.
uint64_t g_windowWaitStartMs = 0;
uint64_t g_scrollWaitStartMs = 0;
uint64_t g_holdUntilMs       = 0;
float    g_endAtRest         = -1.f;   // GetScrollOffsetOfEnd -- the max offset that exists
float    g_offAtRest         = -1.f;
float    g_offAfterBig       = -1.f;
float    g_offAfterZero      = -1.f;
float    g_fracAtRest        = -1.f;   // GetViewOffsetFraction -- where the view ACTUALLY is
float    g_fracAfterBig      = -1.f;
float    g_wheelPre          = -1.f;   // ScrollOffset: the REQUEST, kept only as context
float    g_wheelPost         = -1.f;
float    g_fracWheelPre      = -1.f;   // the verdict rides on these two
float    g_fracWheelPost     = -1.f;
int      g_listHovered       = -1;
int      g_rowsSeen          = -1;
bool     g_controlPassed     = false;
int      g_closeHovered      = -1;   // -1 = not sampled; an unrun phase is not a NO

// The four fields that decide whether a wheel event moves a UScrollBox. We HAND-SPAWN the
// box, so each is whatever the engine CDO carries and none of them has ever been read on
// this build. Printing them is what turns "the wheel did nothing" from a dead end into a
// named cause -- ConsumeMouseWheel=Never(2), a zero multiplier and a Horizontal
// orientation each produce exactly that symptom and are indistinguishable without this.
void LogWheelFields(void* list) {
    const auto* base = reinterpret_cast<const uint8_t*>(list);
    const uint8_t orientation = base[P::off::UScrollBox_Orientation];
    const uint8_t consume     = base[P::off::UScrollBox_ConsumeMouseWheel];
    const uint8_t animate     = base[P::off::UScrollBox_AnimateWheelScroll];
    float mult = 0.f;
    std::memcpy(&mult, base + P::off::UScrollBox_WheelScrollMult, sizeof(mult));
    UE_LOGW("server_browser_native: scroll fields -- Orientation=%u (Vertical=1) "
            "ConsumeMouseWheel=%u (WhenScrollingPossible=0 Always=1 Never=2) "
            "AnimateWheelScrolling=%u WheelScrollMultiplier=%.3f",
            orientation, consume, animate, mult);
}

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
void Tick(void* scrim, void* list, void* closeBtn) {
    if (g_selfCheckStep < 0 || !scrim) return;
    // A NULL ACTIVE WINDOW IS A WAIT, NOT A FAULT -- and never a SILENT one.
    //
    // `GetActiveWindow()` reports the active window OF THE CALLING THREAD, and it reads
    // null transiently while the menu is still being constructed. This used to disarm the
    // instrument outright on the first null, printing nothing, so the whole self-check
    // vanished and the run reported every verdict as "never ran" -- which reads as a
    // missing feature rather than as a probe that gave up. MEASURED 2026-08-26: the run at
    // 13:24 passed all four verdicts and the one at 13:27 logged not a single phase, from
    // the same binary path, differing only in when the first tick landed. Moving this call
    // above the `!g_shown` return (so the click phases can re-open the screen) is what
    // widened that window.
    HWND hwnd = ::GetActiveWindow();
    RECT cr{};
    if (!hwnd || !::GetClientRect(hwnd, &cr)) {
        if (!g_windowWaitStartMs) g_windowWaitStartMs = ::GetTickCount64();
        if (::GetTickCount64() - g_windowWaitStartMs >= kWindowWaitMs) {
            UE_LOGE("server_browser_native: SELFTEST DISARMED -- no active window for this "
                    "thread after %llu ms (hwnd=%p), so no phase can run. Every verdict "
                    "below will be reported as ABSENT; that is this line's fault, not the "
                    "feature's.",
                    static_cast<unsigned long long>(kWindowWaitMs), hwnd);
            g_selfCheckStep = -1;
        }
        return;
    }
    g_windowWaitStartMs = 0;   // it came back; a later blip starts its own patience
    auto moveTo = [&](int cx, int cy) {
        POINT pt{cx, cy};
        if (::ClientToScreen(hwnd, &pt)) ::SetCursorPos(pt.x, pt.y);
    };
    const int w = cr.right - cr.left, h = cr.bottom - cr.top;
    const uint64_t nowMs = ::GetTickCount64();
    switch (g_selfCheckStep) {
        // ---- T0: DOES THE WHEEL SCROLL THIS WIDGET AT ALL -------------------------
        //
        // Three of the eight steps in docs/MULTIPLAYER_UI.md section 8c.-1 are entirely
        // about scrolling -- build a scroll drive, preserve an offset across a rebuild,
        // choose between one-widget-per-row and a viewport pool -- and no wheel event had
        // ever reached this widget. Nothing in the plan priced the work of MAKING it
        // scroll if the answer came back no, which is why this runs first.
        //
        // IT IS NOT A SCREENSHOT TEST. An unchanged capture is three-way ambiguous: the
        // wheel never arrived / the box does not scroll / the capture beat Slate's layout.
        // The offset is read back through GetScrollOffset instead, and a POSITIVE CONTROL
        // runs before the question so a green answer can mean something. The shots are
        // corroboration and never the gate.
        case kScrollWait:
            // HOLDING. Rows arrive over HTTP on a 1 Hz refresh, so at Show() there is
            // nothing in the list and GetScrollOffsetOfEnd is legitimately 0. Wait for the
            // CONTENT TO OVERFLOW rather than for a row count against an assumed viewport
            // height -- the engine already computes the exact quantity the question needs.
            if (!list) {
                UE_LOGE("server_browser_native: SCROLL CONTROL SKIP -- no list widget");
                g_selfCheckStep = kScrimMoveOut;
                return;
            }
            if (!g_scrollWaitStartMs) g_scrollWaitStartMs = nowMs;
            {
                float end  = 0.f;
                if (!U::ScrollOffsetOfEnd(list, end)) {
                    UE_LOGE("server_browser_native: SCROLL CONTROL SKIP -- "
                            "UScrollBox::GetScrollOffsetOfEnd did not resolve or did not "
                            "call; the instrument is absent, not the behaviour");
                    g_selfCheckStep = kScrimMoveOut;
                    return;
                }
                const int rows = U::ChildCount(list);
                if (rows >= kMinRows && end >= kMinOverflow) break;   // both terms -- proceed
                if (nowMs - g_scrollWaitStartMs >= kRowWaitMs) {
                    g_rowsSeen = rows;
                    UE_LOGE("server_browser_native: SCROLL CONTROL SKIP -- after %llu ms "
                            "the list still has rows=%d (want %d) and overflow=%.1f (want "
                            "%.0f). With nothing past the viewport there is nowhere to "
                            "scroll and the wheel question is not askable. Seed rows "
                            "first: tools/fake_master.py, pointed at by "
                            "VOTVCOOP_MASTER_URL (mp.py browser --fake-master N).",
                            static_cast<unsigned long long>(kRowWaitMs), rows, kMinRows,
                            static_cast<double>(end), static_cast<double>(kMinOverflow));
                    g_selfCheckStep = kScrimMoveOut;
                }
            }
            return;   // HOLDING: do not advance the counter
        case kScrollProbe:
            g_rowsSeen = U::ChildCount(list);
            U::ScrollOffset(list, g_offAtRest);
            U::ScrollOffsetOfEnd(list, g_endAtRest);
            U::ViewOffsetFraction(list, g_fracAtRest);
            LogWheelFields(list);
            UE_LOGW("server_browser_native: scroll at rest -- rows=%d offset=%.1f "
                    "offsetOfEnd=%.1f viewFraction=%.4f",
                    g_rowsSeen, g_offAtRest, g_endAtRest, g_fracAtRest);
            break;
        case kScrollSetBig:
            // THE POSITIVE CONTROL. Ask for an offset far past the end. What comes back
            // decides whether anything downstream is measurable at all:
            //   ~= offsetOfEnd -> Slate CLAMPED it, so the box scrolls and the getter
            //                     reads real state. The instrument works.
            //   ~= kHugeOffset -> the getter ECHOES the request. Blind; nothing it says
            //                     about the wheel would mean anything.
            //   0             -> the box refused to move at all.
            U::SetScrollOffset(list, kHugeOffset);
            break;
        case kScrollReadBig: {
            // THE VERDICT IS THE FRACTION, NOT THE OFFSET. The first version of this
            // control asked whether Slate had CLAMPED the absolute Set, and failed the
            // whole probe when it had not -- which was a true observation about
            // GetScrollOffset (it echoes; measured 2026-08-26 on an empty box AND on 30
            // rows with 1391 units of real overflow) applied to the wrong question. T0
            // does not need clamping. It needs to know the view MOVED, and
            // GetViewOffsetFraction reads exactly that -- the scrollbar's own
            // distance-from-top, which is physical post-layout state. The offset is still
            // logged, as the request it is.
            U::ScrollOffset(list, g_offAfterBig);
            U::ViewOffsetFraction(list, g_fracAfterBig);
            const bool moved = g_fracAfterBig - g_fracAtRest > 0.1f;
            g_controlPassed  = moved && g_endAtRest > 0.f;
            if (g_controlPassed)
                UE_LOGW("server_browser_native: SCROLL CONTROL PASS -- a forced offset "
                        "moved the view fraction %.4f -> %.4f over a maximum of %.1f, so "
                        "this widget DOES scroll and GetViewOffsetFraction reads real "
                        "state. (GetScrollOffset returned %.1f for a request of %.0f -- it "
                        "echoes the request, which is why it is not the verdict.) The "
                        "capture below should show the list at the BOTTOM.",
                        g_fracAtRest, g_fracAfterBig, g_endAtRest, g_offAfterBig,
                        kHugeOffset);
            else
                UE_LOGE("server_browser_native: SCROLL CONTROL FAIL -- a forced offset of "
                        "%.0f left the view fraction at %.4f (was %.4f) with a maximum of "
                        "%.1f. The box did not move when told to, so a wheel that moves "
                        "nothing would be uninterpretable.",
                        kHugeOffset, g_fracAfterBig, g_fracAtRest, g_endAtRest);
            g_holdUntilMs = nowMs + kShotHoldMs;
            break;
        }
        case kScrollHoldA:
            if (nowMs < g_holdUntilMs) return;   // HOLDING: give the capture poll a window
            break;
        case kScrollSetZero:
            U::SetScrollOffset(list, 0.f);
            if (!g_controlPassed) {
                UE_LOGE("server_browser_native: WHEEL VERDICT SKIPPED -- the control did "
                        "not pass, so no wheel result could be read either way");
                // Jump, but only after the same hold every other verdict gets. Without it
                // the scrim and ESC phases run within ~250 ms and ESC closes the screen
                // before the 3 s capture poll arrives -- which is how run 5's "post-wheel"
                // shot ended up showing the main menu instead of the browser.
                g_holdUntilMs = nowMs + kShotHoldMs;
                g_selfCheckStep = kScrollHoldB;
                return;
            }
            break;
        case kScrollMoveCur:
            U::ScrollOffset(list, g_offAfterZero);
            moveTo(w / 2, h / 2);   // the list occupies the middle of the window
            break;
        case kScrollWheelPre:
            // Ask Slate whether the cursor is actually over the list before blaming the
            // wheel. RUNG 2 measured that IsHovered() answers and discriminates on a
            // BOUNDED widget; a false here means the notches went somewhere else, which
            // is a different finding from "the box ignores the wheel".
            g_listHovered = E::WidgetIsHovered(list) ? 1 : 0;
            U::ScrollOffset(list, g_wheelPre);
            U::ViewOffsetFraction(list, g_fracWheelPre);
            UE_LOGW("server_browser_native: injecting wheel notches at the list centre "
                    "(hovered=%d, viewFraction before=%.4f, request field=%.1f)",
                    g_listHovered, g_fracWheelPre, g_wheelPre);
            ::mouse_event(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(-WHEEL_DELTA), 0);
            break;
        case kScrollNotch2:
        case kScrollNotch3:
        case kScrollNotch4:
            // Four notches over eight ticks, not four in one. A real hand delivers them
            // spaced, and one notch may be inside the tolerance a settling read allows.
            ::mouse_event(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(-WHEEL_DELTA), 0);
            break;
        case kScrollReadPost: {
            U::ScrollOffset(list, g_wheelPost);
            U::ViewOffsetFraction(list, g_fracWheelPost);
            const float delta = g_fracWheelPost - g_fracWheelPre;
            // Verdict on MOVEMENT, not on direction. Which sign a negative WHEEL_DELTA
            // produces here is unmeasured, and T0 asks whether the wheel reaches this
            // widget at all -- so the magnitude decides and the sign is reported.
            // The threshold is one row's worth of the total travel: four notches that
            // move the view less than that have not scrolled it in any sense a user would
            // recognise, and a bare != would fire on layout noise.
            const float oneRow = g_endAtRest > 0.f ? (kMinOverflow / g_endAtRest) : 1.f;
            if (std::fabs(delta) >= oneRow)
                UE_LOGW("server_browser_native: WHEEL VERDICT YES -- four notches moved "
                        "the view fraction %.4f -> %.4f (delta %+.4f, one row = %.4f, "
                        "hovered=%d). The wheel reaches this widget and scrolls it.",
                        g_fracWheelPre, g_fracWheelPost, delta, oneRow, g_listHovered);
            else if (g_listHovered == 0)
                UE_LOGE("server_browser_native: WHEEL VERDICT NO -- view fraction %.4f -> "
                        "%.4f, but IsHovered was FALSE, so the notches were not delivered "
                        "over the list. This is a harness fault, not a widget answer.",
                        g_fracWheelPre, g_fracWheelPost);
            else
                UE_LOGE("server_browser_native: WHEEL VERDICT NO -- the cursor was over "
                        "the list (hovered=%d) and the box provably scrolls (the forced "
                        "offset moved it to %.4f), yet four notches moved the view "
                        "fraction only %.4f -> %.4f (delta %+.4f, one row = %.4f). The "
                        "wheel does not reach this widget: read the scroll-fields line "
                        "above, then section 8c.-1's T0 row -- no step prices this.",
                        g_listHovered, g_fracAfterBig, g_fracWheelPre, g_fracWheelPost,
                        delta, oneRow);
            g_holdUntilMs = nowMs + kShotHoldMs;
            break;
        }
        case kScrollHoldB:
            if (nowMs < g_holdUntilMs) return;   // HOLDING
            g_selfCheckStep = kScrimMoveOut;     // the next phase, by either route in
            return;
        // ---- the scrim (shipped; renumbered only) ---------------------------------
        case kScrimMoveOut:  // OUTSIDE the window: far left, over the menu's own button column.
            moveTo(w / 12, h * 3 / 4);
            break;
        case kScrimSampleOut:
            g_scrimOutside = E::WidgetIsHovered(scrim) ? 1 : 0;
            break;
        case kScrimMoveIn:  // INSIDE the window, where the list is.
            moveTo(w / 2, h / 2);
            break;
        case kScrimSampleIn:
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
        case kEscPress:
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
        case kEscObserve:
            // The ESC test was binary until run 1 failed it: "did not close" could mean
            // the synthesized key never entered the system, or that it did and the
            // production poll or Hide() failed to act. Those are different bugs and the
            // verdict could not tell them apart. Sample the same global state the poll
            // reads, mid-hold, so it can.
            UE_LOGW("server_browser_native: ESC held -- GetAsyncKeyState(VK_ESCAPE) reads "
                    "%s at this tick, which is what the production poll sees",
                    (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) ? "DOWN" : "UP");
            break;
        case kEscRelease:
            ::keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
            break;
        case kReopen:
            // ESC has closed the screen by now. Re-open it through the PUBLIC Open(), the
            // same call the MULTIPLAYER button makes, so the X gets driven against a
            // screen that came up the ordinary way rather than one we never let close.
            if (!closeBtn) {
                UE_LOGE("server_browser_native: CLOSE BUTTON SKIP -- no X was built, so "
                        "whether the chrome closes this screen is UNMEASURED");
                g_selfCheckStep = -1;
                return;
            }
            ui::server_browser_native::Open();
            break;
        case kClickMove: {
            // The X sits at the top-right of a 980x620 window centred in the client area.
            // This is an ESTIMATE, and the next phase checks it rather than assuming it --
            // a miss here is "the button is not where we think", which is a different
            // finding from "the click does not work" and must not be reported as one.
            const int cx = w / 2 + 980 / 2 - 40;
            const int cy = h / 2 - 620 / 2 + 34;
            moveTo(cx, cy);
            break;
        }
        case kClickHover:
            g_closeHovered = E::WidgetIsHovered(closeBtn) ? 1 : 0;
            if (!g_closeHovered)
                UE_LOGW("server_browser_native: the X did not answer IsHovered at its "
                        "estimated position -- clicking anyway; read the verdict line, "
                        "which distinguishes a bad estimate from a dead button");
            break;
        case kClickDown:
            // PRESS and hold across ticks. The poll this drives fires on the RELEASE edge
            // and samples once per tick, so a down+up inside one tick is invisible to it --
            // the same trap the ESC phase records one screen up.
            ::mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            break;
        case kClickUp:
            ::mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            break;
        case kClickVerify:
            if (!ui::server_browser_native::IsOpen())
                UE_LOGW("server_browser_native: CLOSE BUTTON PASS -- a synthesized click "
                        "on the X closed the screen (hovered=%d). The chrome is a real "
                        "way out, not just a drawing.", g_closeHovered);
            else if (g_closeHovered == 0)
                UE_LOGE("server_browser_native: CLOSE BUTTON FAIL -- the screen is still "
                        "open, but IsHovered read FALSE at the estimated position, so the "
                        "click probably missed the X. Fix the estimate before concluding "
                        "anything about the button.");
            else
                UE_LOGE("server_browser_native: CLOSE BUTTON FAIL -- the cursor WAS over "
                        "the X (hovered=1) and a full press-release was delivered, yet the "
                        "screen is still open. The button draws but does not close.");
            g_selfCheckStep = -1;
            return;
        default:
            break;
    }
    ++g_selfCheckStep;
}

void Arm() { g_selfCheckStep = 0; }

}  // namespace ui::server_browser_selftest
