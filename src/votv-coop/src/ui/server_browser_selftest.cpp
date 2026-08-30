// ui/server_browser_selftest.cpp -- see ui/server_browser_selftest.h.
//
// EXTRACTED VERBATIM from ui/server_browser_native.cpp 2026-08-26. The scrim and ESC
// phases below are the shipped body moved without a behavioural edit; the only changes
// were the seam (`g_scrimW` -> the `scrim` parameter, `SelfCheckTick` -> `Tick`) and the
// namespace. The T0 scroll phases were added afterwards, in their own commit.

#include "ui/server_browser_selftest.h"

#include "ui/server_browser_native.h"   // IsOpen()/Open() -- the click phases drive the real screen
#include "ui/input_focus.h"            // synthesized input only lands in a FOREGROUND window
#include "ui/imgui_overlay.h"          // CaptureOwners() -- who is eating the mouse
#include "ui/server_browser_actions.h"  // the HOST button this drives
#include "ui/host_window_native.h"     // ...and what it must open

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
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
// (Four phases stood here until 2026-08-30: aim at the list, read hover, nudge one pixel,
// re-read. They existed to discriminate 'this widget is dead' from 'no input is reaching
// the game at all', and they did their job -- the answer is now permanent knowledge in
// native_screen::ChildAtCursor's header and in the DISARMED guard above, and production no
// longer asks Slate about a row at all. Kept running, they re-proved a known negative every
// run and printed an eight-line visibility chain to do it. RULE 2.)
// THE ACTION BAR, and it runs HERE -- after the re-open, BEFORE any row is selected --
// because that is the only window in which CONNECT's decline branch is reachable, and the
// decline branch is the one that can be driven without starting a real join and tearing the
// rest of the run out from under itself.
//
// WHY IT NEEDED ITS OWN PHASES AT ALL. Every outcome of CONNECT is a SENTENCE IN THE FOOTER,
// and a sentence is observable to a human and to nothing else -- so "the button is wired"
// and "the button does nothing" produced identical evidence. That is precisely the state row
// hover sat in for three days while looking fine. `server_browser_actions::LastOutcome()`
// exists to end that, and these phases are what read it.
constexpr int kActRefMove     = 90;
constexpr int kActRefDown     = 98;
constexpr int kActRefUp       = 102;
constexpr int kActRefVerify   = 106;
constexpr int kActConnMove    = 112;
constexpr int kActConnDown    = 120;
constexpr int kActConnUp      = 124;
constexpr int kActConnVerify  = 128;
// ROW HOVER AND SELECTION, before the X, because a browser whose rows cannot be picked is
// not a browser -- and neither had ever been asserted by anything.
constexpr int kRowMove        = 132;
constexpr int kRowRead        = 140;
constexpr int kRowDown        = 142;
constexpr int kRowUp          = 146;
constexpr int kRowVerify      = 150;
// ...and then HOLD STILL TWICE, so a human can see what the two state channels drew.
//
// WHY A LOG LINE CANNOT CLOSE THIS. `ROW SELECT PASS` above proves the STATE changed --
// `SelectedRowId()` returns a lobby. It says nothing about PIXELS. The row skin is two
// UImage tints written through UFunctions, and every failure mode of that (the wrong child
// index, a raw write that does not repaint, a colour built in the wrong space) leaves the
// state verdict green and the screen unchanged. mp.py's own closing line is the standing
// warning: "a log line is not a layout".
//
// TWO SHOTS, because the property the user asked for is a RELATION between rows and one
// frame cannot hold both halves of it:
//   A -- the cursor parked on a row that is NOT the selected one. That frame carries all
//        three states at once: the selected row purple with an ordinary grey frame, the
//        hovered row with a yellow frame and yellow text, and five idle rows between them.
//   B -- the cursor moved back ONTO the selected row. This is the user's rule itself
//        ("выделение держится только на нем, а hover игнорится"): the row must still be
//        purple and must NOT have gained a yellow frame. A green A with a yellow B is
//        precedence not working, and nothing else distinguishes those two outcomes.
// Each holds for kShotHoldMs, the same 6 s window the scroll verdicts already use to cover
// mp.py's 3 s capture poll.
constexpr int kSkinAimOther   = 151;
constexpr int kSkinHoldOther  = 152;  // HOLDING -- wall clock
constexpr int kSkinAimSelf    = 153;
constexpr int kSkinHoldSelf   = 154;  // HOLDING -- wall clock
constexpr int kClickMove      = 156;
constexpr int kClickSample    = 164;  // eight ticks after the move -- the scrim's budget
constexpr int kClickDown      = 166;
constexpr int kClickUp        = 170;
constexpr int kClickVerify    = 180;
// ...and LAST, the HOST link, because it is what the user actually asked for: a hosting
// window they can reach. It runs after the X phases because clicking HOST closes the
// browser, so nothing about the browser can be asserted after it.
constexpr int kHostReopen     = 186;
constexpr int kHostMove       = 194;
constexpr int kHostDown       = 204;
constexpr int kHostUp         = 208;
constexpr int kHostVerify     = 220;
// ...and finally the WORLD LIST inside that window, which is a different screen's rows in a
// different ScrollBox. It gets its own phases because it was a CRITICAL of its own: those
// rows were hit-tested with `IsHovered`, which does not answer inside a ScrollBox, so no
// world could be picked and HOST could only ever start a New game.
constexpr int kWorldMove      = 228;
constexpr int kWorldRead      = 236;
constexpr int kWorldDown      = 238;
constexpr int kWorldUp        = 242;
constexpr int kWorldVerify    = 252;

// The forced offset for the positive control. Far past any real content extent, so a
// getter that returns it UNCHANGED has told us it echoes the request rather than reading
// Slate -- which is the one failure mode that would let a green T0 mean nothing.
constexpr float    kHugeOffset   = 1.0e6f;
constexpr uint64_t kRowWaitMs    = 30000;  // rows arrive over HTTP; 30 s covers a cold fetch at the 5 s cadence
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
// PLACE THE OS CURSOR ON A WIDGET, converting out of the widget's space first.
//
// `WidgetScreenRect` reports CLIENT pixels (Slate's LocalToAbsolute); `SetCursorPos`
// takes DESKTOP pixels. SIX sites in this file handed the former straight to the
// latter, under a comment asserting "Slate's absolute space is desktop pixels, the
// same space SetCursorPos takes, so this needs no ClientToScreen and no DPI factor".
// That was false -- and it is why every browser run passed for days while a player
// could not click anything: production's hit test made the SAME mistake, so the two
// errors cancelled and the instrument could not see the defect it existed to catch.
// Measured 2026-08-30: client origin (320,180) on the lab rig, a whole list-height.
// The seventh site (the X button) already converted, which is why the close-button
// phase was the one that behaved.
// PLACE THE POINTER AT A POINT EXPRESSED IN SLATE'S ABSOLUTE SPACE -- which is DESKTOP
// pixels, so this is a straight `SetCursorPos` and every caller may hand it a rect from
// `WidgetScreenRect` unchanged.
//
// IT DID A `ClientToScreen` FOR ONE DAY, AND THAT WAS A REGRESSION I INTRODUCED. On
// 2026-08-30 the row hit test was found comparing the cursor against these rects and
// "corrected" on the theory that the rects were CLIENT pixels; the harness was changed to
// match. The theory was wrong in the harness's direction too: adding the client origin
// here moved every aim by exactly that origin -- `measured`, the row phase asked for
// (1280,592) and the pointer landed at (1600,772) on a rig whose client area starts at
// (320,180) -- which put the cursor BELOW the last live row, where -1 is the correct
// answer. Three runs then reported ROW SELECT FAIL for a hit test that was working.
//
// The comment this replaced said Slate's absolute space is desktop pixels and needs no
// conversion. I deleted it as false. It was true, and the run that proved it is the child
// table in `native_screen`'s probe: rows at desktop y 496..752 with the panel at (796,496),
// i.e. the same space `GetCursorPos` reports.
void PlaceCursorOnAbsolute(float absX, float absY) {
    ::SetCursorPos(static_cast<long>(absX), static_cast<long>(absY));
}

int      g_rowHovered        = -1;   // ...and the same for the row the click phase aims at
int      g_worldBefore       = -2;   // -2 = unsampled; -1 is New game, a real value

// One row's height, mirroring server_browser_native's kRowH. Kept as a constant rather
// than measured because the aim only has to land INSIDE a row, and the verdict prints the
// row index it actually got -- so a drift here shows up as a different index, not as a
// false failure.
constexpr float kRowPx = 64.f;

// ...and the HOSTING WINDOW's rows are 56. Two screens, two row heights; using one
// constant for both landed inside row 0 only by margin.
constexpr float kHostRowPx = 56.f;

// UWidget::GetDesiredSize -- the non-visual instrument the native_ui_probe used to prove a
// hand-built widget lays out at all (GetDesiredSize (0,0) -> (654,64) was RUNG 1's whole
// verdict). Resolved lazily: this runs only inside a dev self-check, so paying a
// FindFunction once there is cheaper than resolving it at build time for every player.
ue_wrap::FVector2D DesiredSizeOf(void* widget) {
    ue_wrap::FVector2D v{0.f, 0.f};
    if (!widget) return v;
    static void* fn = [] {
        void* w = ue_wrap::reflection::FindClass(P::name::WidgetClass);
        return w ? ue_wrap::reflection::FindFunction(w, L"GetDesiredSize") : nullptr;
    }();
    if (!fn) return v;
    ue_wrap::ParamFrame f(fn);
    if (!Call(widget, f)) return v;
    f.GetRaw(L"ReturnValue", &v, sizeof(v));
    return v;
}

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
    // WE DRIVE INPUT, SO WE MUST OWN THE FOREGROUND -- and this is the root of every
    // intermittency this instrument showed on 2026-08-26.
    //
    // `keybd_event` and `mouse_event` inject into the SYSTEM input queue; they land in
    // whatever window is foreground at that instant, not in ours. So a run where the game
    // is not foreground silently sends ESC and the click somewhere else and then reports
    // "the screen did not close" -- an accusation against the feature for something the
    // harness did. That is exactly what happened: ESC failed at 13:24 and passed at 13:32,
    // the X click failed at 13:47 and passed at 13:32, and the wheel failed at 13:47 and
    // passed at 13:44, all on code paths that could not tell those pairs apart.
    //
    // `GetActiveWindow()` was the wrong question twice over: it reports the active window
    // of the CALLING THREAD (null transiently while the menu is still being built) and it
    // says nothing about who will receive injected input. `GetForegroundWindow()` plus the
    // existing ownership predicate answers the question we actually have. Not ours is a
    // bounded WAIT, and a LOUD disarm -- never a silent one, which is how the whole
    // self-check once vanished from a run and read as a missing feature.
    HWND hwnd = ::GetForegroundWindow();
    RECT cr{};
    if (!hwnd || !ui::input_focus::IsOurWindowForeground() || !::GetClientRect(hwnd, &cr)) {
        if (!g_windowWaitStartMs) g_windowWaitStartMs = ::GetTickCount64();
        if (::GetTickCount64() - g_windowWaitStartMs >= kWindowWaitMs) {
            UE_LOGE("server_browser_native: SELFTEST DISARMED -- our window was not the "
                    "FOREGROUND window for %llu ms (fg=%p), so any key or click this test "
                    "synthesizes would land in someone else's window. Every verdict below "
                    "is ABSENT because the harness stood down, not because the feature is "
                    "missing. Give the game focus and re-run.",
                    static_cast<unsigned long long>(kWindowWaitMs), hwnd);
            g_selfCheckStep = -1;
        }
        return;
    }
    g_windowWaitStartMs = 0;   // it came back; a later blip starts its own patience

    // THE SECOND PRECONDITION, AND THE ONE THIS FILE WAS MISSING FOR THREE DAYS.
    //
    // Owning the foreground is not enough: an ImGui surface that is up ALSO owns the mouse,
    // and while it does, `WndProcDetour` swallows every mouse message before the game sees
    // it and `SetCursorPosDetour` returns TRUE without moving the pointer. So this test can
    // aim perfectly at a widget that is perfectly built, and read `IsHovered() == false` on
    // all of it, because Slate was never told the pointer moved.
    //
    // That is not a hypothesis. MEASURED 2026-08-29: `SetCursorPos` asked for (1280,711),
    // returned ok=1 with err=0 and no ClipCursor rect in the way, and the pointer stayed at
    // desktop (320,160) -- client (0,0) -- for the whole run. Everything inside the window
    // read not-hovered; the full-screen scrim, which covers (0,0), read hovered. The owner
    // was `config_review`, armed at boot by an ordinary ini finding and never dismissed,
    // because nothing in an autonomous run clicks it away.
    //
    // The cost of not having this guard: every CLOSE BUTTON FAIL since the panel started
    // arming was an accusation against a button that was never given a pointer. Three
    // causes were proposed and each falsified -- a five-day-old commit, our own boot modal,
    // a mounted pak -- while the real one was printing itself in the same log. So this
    // refuses to produce a verdict at all rather than produce a false one, which is the
    // same call the foreground guard above already makes for the same reason.
    {
        const std::string owners = ui::imgui_overlay::CaptureOwners();
        if (owners != "none") {
            UE_LOGE("server_browser_native: SELFTEST DISARMED -- [%s] owns the mouse, so the "
                    "game receives no pointer messages and cursor writes are swallowed by our "
                    "own detour. Every hover and click verdict below is ABSENT because the "
                    "harness stood down; NONE of it is evidence about this screen. Close that "
                    "surface (or stop it arming) and re-run.", owners.c_str());
            g_selfCheckStep = -1;
            return;
        }
    }

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
        // ---- the action bar: REFRESH, then CONNECT's decline branch ------------------
        //
        // One helper drives both, because a button is a button: read the rect Slate cached
        // for it, put the real cursor on its centre, and let the production release-edge
        // poll route the click. Nothing here calls DoConnect or DoRefresh directly -- the
        // defect this screen has actually suffered is a control that draws and cannot be
        // reached, and a test that calls the handler proves only the tail of the path.
        case kActRefMove:
        case kActConnMove: {
            void* btn = (g_selfCheckStep == kActRefMove) ? ui::server_browser_actions::RefreshButton()
                                              : ui::server_browser_actions::ConnectButton();
            const char* what = (g_selfCheckStep == kActRefMove) ? "REFRESH" : "CONNECT";
            ue_wrap::FVector2D tl{}, sz{};
            if (!btn || !U::WidgetScreenRect(btn, tl, sz) || sz.X < 1.f || sz.Y < 1.f) {
                UE_LOGE("server_browser_native: ACTION BAR SKIP -- %s reports no usable "
                        "geometry (btn=%p %.0fx%.0f). Whether the action bar can be reached "
                        "at all is UNMEASURED; the row phases below still run.",
                        what, btn, sz.X, sz.Y);
                // Fall through to the row phases rather than aborting: the action bar is
                // not a precondition for anything below it.
                g_selfCheckStep = (g_selfCheckStep == kActRefMove) ? kActConnMove - 1 : kRowMove - 1;
                return;
            }
            UE_LOGW("server_browser_native: %s at desktop (%.0f,%.0f) %.0fx%.0f -- clicking it",
                    what, tl.X, tl.Y, sz.X, sz.Y);
            PlaceCursorOnAbsolute(tl.X + sz.X * 0.5f,
                                tl.Y + sz.Y * 0.5f);
            break;
        }
        case kActRefDown:
        case kActConnDown:
            ::mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            break;
        case kActRefUp:
        case kActConnUp:
            ::mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            break;
        case kActRefVerify: {
            const char* out = ui::server_browser_actions::LastOutcome();
            if (out && std::strcmp(out, "refresh") == 0)
                UE_LOGW("server_browser_native: REFRESH PASS -- a real click on REFRESH "
                        "reached the handler (outcome '%s'). The list can be re-fetched on "
                        "demand and not only on the 5 s timer.", out);
            else
                UE_LOGE("server_browser_native: REFRESH FAIL -- the click did not reach the "
                        "handler (outcome '%s'). Either the button is not hit-testable or "
                        "the release-edge poll is not routing to the action bar.",
                        out ? out : "(null)");
            break;
        }
        case kActConnVerify: {
            const char* out = ui::server_browser_actions::LastOutcome();
            // WITH NOTHING SELECTED, and that is the assertion. "connect:none" is the one
            // outcome that proves the whole path -- layout, hit test, routing, and
            // SelectedRow() answering honestly -- without starting a join that would tear
            // the rest of this run down. The ACCEPT branch differs by one line: it calls
            // `session_manager::JoinLobby`, the same entry point the hands-on-verified
            // ImGui browser calls, with a row `ROW SELECT PASS` below proves is the one the
            // player clicked. That line is NOT exercised here and the doc says so.
            const bool open = ui::server_browser_native::IsOpen();
            if (out && std::strcmp(out, "connect:none") == 0 && open)
                UE_LOGW("server_browser_native: CONNECT PASS -- a real click on CONNECT with "
                        "nothing selected reached the handler and DECLINED (outcome '%s', "
                        "screen still open). The button is wired; the accept branch is one "
                        "call to the shared JoinLobby and is not driven here.", out);
            else
                UE_LOGE("server_browser_native: CONNECT FAIL -- outcome '%s', screen open=%d. "
                        "Expected 'connect:none' with the screen still up. If the outcome is "
                        "empty the click never reached the action bar at all.",
                        out ? out : "(null)", open ? 1 : 0);
            break;
        }
        case kRowMove: {
            // AIM AT THE SECOND ROW, not the first: the first row's top edge is also the
            // list's top edge, so a rounding error there lands outside the list and the
            // failure would be the harness's. One and a half rows down is unambiguous.
            ue_wrap::FVector2D ltl{}, lsz{};
            if (!U::WidgetScreenRect(list, ltl, lsz) || lsz.Y < kRowPx * 2.f) {
                UE_LOGE("server_browser_native: ROW HOVER SKIP -- the list is %.0f px tall, "
                        "too short to hold the two rows this phase aims between", lsz.Y);
                g_selfCheckStep = kClickMove - 1;   // fall through to the X phases
                return;
            }
            PlaceCursorOnAbsolute(ltl.X + lsz.X * 0.5f,
                                ltl.Y + kRowPx * 1.5f);
            break;
        }
        case kRowRead: {
            g_rowHovered = ui::server_browser_native::HoveredRow();
            UE_LOGW("server_browser_native: ROW HOVER -- the pointer is one and a half rows "
                    "into the list and HoveredRow() reads %d. Anything below zero means the "
                    "highlight is dead, and with it the only way to choose a server.",
                    g_rowHovered);
            if (g_rowHovered < 0) {
                // TWO LINKS CAN PRODUCE THAT -1 and they need different fixes: the outer
                // containment gate said the pointer is not over the list, or it said yes and
                // no row's own hit test answered. Print both, plus the row's rect, so the
                // next edit lands on the link that is actually failing instead of on the one
                // that is easier to change.
                POINT cur{};
                ::GetCursorPos(&cur);
                ue_wrap::FVector2D ltl{}, lsz{};
                const bool haveList = U::WidgetScreenRect(list, ltl, lsz);
                const bool inList = haveList && cur.x >= ltl.X && cur.x < ltl.X + lsz.X &&
                                    cur.y >= ltl.Y && cur.y < ltl.Y + lsz.Y;
                const int32_t kids = U::ChildCount(list);
                UE_LOGW("server_browser_native:   gate -- cursor (%ld,%ld), list rect "
                        "%s(%.0f,%.0f) %.0fx%.0f, contains=%d, children=%d",
                        cur.x, cur.y, haveList ? "" : "UNREAD ", ltl.X, ltl.Y, lsz.X, lsz.Y,
                        inList ? 1 : 0, kids);
                // FIND THE ROW THE CURSOR IS ACTUALLY ON, then dump THAT one. The first
                // version of this printed rows 0-2 and asked whether their SizeBox was
                // hovered -- two mistakes at once: the cursor was over neither (the wheel
                // phases leave the list scrolled, so the top rows are off-screen and their
                // cached geometry is stale), and a SizeBox is SelfHitTestInvisible by
                // default, so it answers 0 whatever is true. It could not have found
                // anything.
                int aimed = -1;
                for (int32_t i = 0; i < kids; ++i) {
                    void* kid = U::ChildAt(list, i);
                    ue_wrap::FVector2D rtl{}, rsz{};
                    if (!kid || !U::WidgetScreenRect(kid, rtl, rsz)) continue;
                    // Intersected with the list, because a scrolled-out row's stale rect
                    // can still contain the cursor and would name the wrong row.
                    const float top = rtl.Y > ltl.Y ? rtl.Y : ltl.Y;
                    const float bot = (rtl.Y + rsz.Y) < (ltl.Y + lsz.Y) ? (rtl.Y + rsz.Y)
                                                                        : (ltl.Y + lsz.Y);
                    if (cur.y >= top && cur.y < bot && cur.x >= rtl.X &&
                        cur.x < rtl.X + rsz.X) { aimed = i; break; }
                }
                if (aimed < 0)
                    UE_LOGW("server_browser_native:   no row's rect contains the cursor, so "
                            "the list is showing a gap there or the rows are laid out "
                            "somewhere other than where the list says it is");
                else {
                    UE_LOGW("server_browser_native:   the cursor is geometrically on row %d "
                            "-- dumping its parts to find which one wins the hit test",
                            aimed);
                    ui::server_browser_native::LogRowHitDiagnostics(aimed);
                }
            }
            break;
        }
        case kRowDown:
            ::mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            break;
        case kRowUp:
            ::mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            break;
        case kRowVerify: {
            const char* sel = ui::server_browser_native::SelectedRowId();
            const bool picked = sel && *sel;
            if (picked && g_rowHovered >= 0)
                UE_LOGW("server_browser_native: ROW SELECT PASS -- hovering row %d and "
                        "clicking it selected lobby '%s'. A player can choose a server.",
                        g_rowHovered, sel);
            else if (g_rowHovered < 0)
                UE_LOGE("server_browser_native: ROW SELECT FAIL -- no row was hovered, so the "
                        "click had nothing to select. The hit test over the list is the "
                        "defect; the click path was never reached.");
            else
                UE_LOGE("server_browser_native: ROW SELECT FAIL -- row %d was hovered and a "
                        "full press-release was delivered, yet nothing is selected. The "
                        "hover is fine and the CLICK path is the defect.", g_rowHovered);
            // THE CLICK-MOMENT SHOT, AND IT IS THE ONE THAT MATTERS MOST -- the cursor is
            // NOT moved. Shots A and B below both move it first, and a move REPAINTS the
            // row it leaves and the row it lands on, which HEALS exactly the defect this
            // window contains. A post-ship audit found that the selection repaint updated
            // the fill and the frame but not the TEXT, so a just-clicked row sat purple
            // with yellow glyphs until the pointer happened to move -- invisible to both
            // shots, because both begin by moving. The hold starts here and the next phase
            // waits it out, so no renumbering of the ladder was needed to insert it.
            UE_LOGW("server_browser_native: ROW SKIN SHOT C -- the click has just landed on "
                    "row %d and the cursor has NOT moved. That row must read PURPLE with a "
                    "grey frame and NO yellow anywhere, glyphs included: it is selected, and "
                    "a selected row ignores the pointer that is still sitting on it.",
                    g_rowHovered);
            g_holdUntilMs = nowMs + kShotHoldMs;
            break;
        }
        case kSkinAimOther:
        case kSkinAimSelf: {
            // Aim by the SAME arithmetic kRowMove used, so "the selected row" here is the
            // row that was actually clicked: 1.5 rows in. The other aim is 4.5 rows in --
            // three rows lower, still inside the ~470 px list at any window size this rig
            // runs, and far enough that the two are never the same row.
            if (nowMs < g_holdUntilMs) return;   // HOLDING: the click-moment shot's window
            const bool self = (g_selfCheckStep == kSkinAimSelf);
            // NEVER handed straight to %s: SelectedRowId returns the raw pointer of a
            // std::string that is empty when nothing is selected, and a null would be UB
            // in the logger's vsnprintf. The caller two phases up already guards it.
            const char* selId = ui::server_browser_native::SelectedRowId();
            if (!selId) selId = "(none)";
            // BOTH TERMS, and the first version had only the second. A pixel height says
            // the list is tall enough to CONTAIN five rows; it does not say five rows
            // EXIST. With three lobbies the box is still 470 px, the aim lands in empty
            // space, nothing is hovered -- and the shot would be archived under a line
            // asserting a yellow highlight that is not in it. (Post-ship audit, 2026-08-30.)
            ue_wrap::FVector2D ltl{}, lsz{};
            const int32_t rows = U::ChildCount(list);
            if (!U::WidgetScreenRect(list, ltl, lsz) || lsz.Y < kRowPx * 5.f || rows < 5) {
                UE_LOGE("server_browser_native: ROW SKIN SHOT SKIP -- the list is %.0f px "
                        "tall with %d row(s); these shots need five. Whether the hover and "
                        "selection tints DRAW is UNMEASURED -- not passing.", lsz.Y, rows);
                g_selfCheckStep = kClickMove - 1;
                return;
            }
            PlaceCursorOnAbsolute(ltl.X + lsz.X * 0.5f,
                                ltl.Y + kRowPx * (self ? 1.5f : 4.5f));
            // The needle mp.py captures on. It names what the frame should show, so the
            // shot is falsifiable by looking at it rather than merely archived.
            if (self)
                UE_LOGW("server_browser_native: ROW SKIN SHOT B -- the cursor is back on the "
                        "SELECTED row (lobby '%s'). It must still be PURPLE and must NOT "
                        "have a yellow frame: a selected row ignores hover.",
                        selId);
            else
                UE_LOGW("server_browser_native: ROW SKIN SHOT A -- the cursor is three rows "
                        "BELOW the selected one (lobby '%s'). The frame should show that row "
                        "purple with a grey border, the row under the cursor with a YELLOW "
                        "border and yellow text, and the rest idle. (HoveredRow reads %d one "
                        "tick later; below zero means the aim missed and the shot asserts "
                        "nothing.)",
                        selId, ui::server_browser_native::HoveredRow());
            g_holdUntilMs = nowMs + kShotHoldMs;
            break;
        }
        case kSkinHoldOther:
        case kSkinHoldSelf:
            if (nowMs < g_holdUntilMs) return;   // HOLDING: give the capture poll a window
            break;
        case kClickMove: {
            // ASK THE ENGINE WHERE THE X IS. Do not compute it, and do not hunt for it.
            //
            // Two instruments stood here before, and both were the same mistake at
            // different sizes. The first moved to a hard-coded estimate -- top-right of a
            // 980x620 window, `w/2 + 490 - 40`, `h/2 - 310 + 34` -- which is a SECOND
            // implementation of a layout the engine had already performed, kept in step
            // with the real one by hand. `23481e3c` rewrote the title row and the estimate
            // went stale in the very next commit after the one that recorded CLOSE BUTTON
            // PASS. The second replaced it with a 70-point sweep of that region, asking
            // IsHovered at each point -- but the region itself was `w/2 + 980/2 - 134`,
            // the same three constants, so the sweep inherited the guess it was written to
            // retire and could only ever be wrong over a wider area.
            //
            // `WidgetScreenRect` reads Slate's own cached geometry, so it is correct under
            // any window size, any UI scale, and any future edit to this screen's layout.
            // The two failure modes stay distinguishable, which is the property both
            // earlier versions were reaching for: a rect that comes back empty means the
            // button was never given a place to be, and a good rect whose centre does not
            // answer IsHovered means the button is there and not hit-testable.
            ue_wrap::FVector2D tl{}, size{};
            const bool haveRect = U::WidgetScreenRect(closeBtn, tl, size);
            const ue_wrap::FVector2D want = DesiredSizeOf(closeBtn);
            if (!haveRect) {
                UE_LOGE("server_browser_native: CLOSE BUTTON SKIP -- Slate would not report "
                        "the X's geometry, so this run cannot say where it is. The link that "
                        "failed is named in the umg: line above; nothing below is a verdict "
                        "about the button.");
                g_selfCheckStep = -1;
                return;
            }
            // ALLOTTED vs DESIRED, printed together, because the gap between them is the
            // diagnosis. Equal and non-zero: the row gave the button what it asked for.
            // Allotted (0,0) against a desired (53,48): it laid out and was then given no
            // room -- a slot problem, invisible to any amount of clicking.
            UE_LOGW("server_browser_native: X geometry -- allotted %.0fx%.0f at desktop "
                    "(%.0f,%.0f), desired %.0fx%.0f, client %dx%d",
                    size.X, size.Y, tl.X, tl.Y, want.X, want.Y, w, h);
            // CALIBRATION, and it is not optional. A coordinate is meaningless without the
            // space it is in, and the first run of this probe put the X at desktop
            // (1711,396) -- which reads as "261 px outside a 980-wide centred window" only
            // if absolute space is 1:1 with client pixels. The SCRIM is the ruler: it is
            // known to span the whole screen (the phase above proves it by hover), so its
            // rect states the space's extent directly. The LIST is the second reading,
            // because it is the one widget whose place inside the window is unambiguous.
            {
                ue_wrap::FVector2D stl{}, ssz{}, ltl{}, lsz{};
                const bool haveScrim = U::WidgetScreenRect(scrim, stl, ssz);
                const bool haveList  = U::WidgetScreenRect(list, ltl, lsz);
                UE_LOGW("server_browser_native: space calibration -- scrim %s%.0fx%.0f at "
                        "(%.0f,%.0f), list %s%.0fx%.0f at (%.0f,%.0f). A scrim of exactly "
                        "the client size means absolute space IS client pixels; anything "
                        "smaller is the UI scale, and every other number here divides by it.",
                        haveScrim ? "" : "UNREAD ", ssz.X, ssz.Y, stl.X, stl.Y,
                        haveList ? "" : "UNREAD ", lsz.X, lsz.Y, ltl.X, ltl.Y);
            }
            if (size.X < 1.f || size.Y < 1.f) {
                UE_LOGE("server_browser_native: CLOSE BUTTON FAIL -- the X occupies %.0fx%.0f "
                        "px, so it has no hit area at all. This is a LAYOUT defect, not a "
                        "click one: no cursor position can reach it. Check BuildButton's "
                        "HorizontalBox slot against the title text's fill weight.",
                        size.X, size.Y);
                g_selfCheckStep = -1;
                return;
            }
            // Slate reports CLIENT pixels; SetCursorPos takes DESKTOP. The comment that
            // stood here claimed they were one space and that no ClientToScreen was
            // needed -- measured FALSE 2026-08-30 (client origin 320,180).
            PlaceCursorOnAbsolute(tl.X + size.X * 0.5f, tl.Y + size.Y * 0.5f);
            break;
        }
        case kClickSample:
            // Sampled a full eight ticks after the move, the interval the scrim phases
            // already trust: IsHovered read too soon answers about the PREVIOUS pointer
            // position. Recorded BEFORE the click so the verdict can separate "the cursor
            // never got there" from "it got there and the button did nothing".
            g_closeHovered = E::WidgetIsHovered(closeBtn) ? 1 : 0;
            UE_LOGW("server_browser_native: the X reads IsHovered=%d with the cursor at its "
                    "own centre (list=%d scrim=%d at the same moment) -- clicking there now",
                    g_closeHovered, E::WidgetIsHovered(list) ? 1 : 0,
                    E::WidgetIsHovered(scrim) ? 1 : 0);
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
                        "open, and IsHovered read FALSE with the cursor on the centre of "
                        "the rect Slate itself reported. The aim is not in question: the X "
                        "occupies that space and is not HIT-TESTABLE in it. Look at its "
                        "visibility and at what is painted over it, not at coordinates.");
            else
                UE_LOGE("server_browser_native: CLOSE BUTTON FAIL -- the cursor WAS over "
                        "the X (hovered=1) and a full press-release was delivered, yet the "
                        "screen is still open. The button draws but does not close.");
            break;   // the HOST phases follow; they re-open the screen themselves
        case kHostReopen:
            ui::server_browser_native::Open();
            break;
        case kHostMove: {
            void* host = ui::server_browser_actions::HostButton();
            ue_wrap::FVector2D tl{}, sz{};
            if (!host || !U::WidgetScreenRect(host, tl, sz) || sz.X < 1.f || sz.Y < 1.f) {
                UE_LOGE("server_browser_native: HOST LINK SKIP -- the HOST button has no "
                        "usable rect (built=%d), so whether the hosting window can be "
                        "reached from the browser is UNMEASURED", host ? 1 : 0);
                g_selfCheckStep = -1;
                return;
            }
            UE_LOGW("server_browser_native: HOST button at desktop (%.0f,%.0f) %.0fx%.0f "
                    "-- clicking it", tl.X, tl.Y, sz.X, sz.Y);
            PlaceCursorOnAbsolute(tl.X + sz.X * 0.5f,
                                tl.Y + sz.Y * 0.5f);
            break;
        }
        case kHostDown:
            ::mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            break;
        case kHostUp:
            ::mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            break;
        case kHostVerify: {
            // BOTH halves, because either one alone is a broken screen: the hosting window
            // must be up, and the browser must have got out of the way. They are siblings
            // in one switcher, so "both open" is not a state that can render.
            const bool hostUp     = ui::host_window_native::IsOpen();
            const bool browserOut = !ui::server_browser_native::IsOpen();
            if (hostUp && browserOut)
                UE_LOGW("server_browser_native: HOST LINK PASS -- a real click on HOST "
                        "opened the hosting window and closed the browser. The window "
                        "shipped 2026-08-29 with no way in but a dev flag; it has one now.");
            else
                UE_LOGE("server_browser_native: HOST LINK FAIL -- after a real click on the "
                        "HOST button: hosting window open=%d, browser closed=%d. Both must "
                        "be true; they are siblings in one switcher and only one can render.",
                        hostUp ? 1 : 0, browserOut ? 1 : 0);
            if (!hostUp) { g_selfCheckStep = -1; return; }   // no window: nothing to click in
            break;
        }
        case kWorldMove: {
            void* saveList = ui::host_window_native::SaveListWidget();
            const int rows = ui::host_window_native::SaveRowCount();
            ue_wrap::FVector2D tl{}, sz{};
            if (rows <= 0 || !saveList || !U::WidgetScreenRect(saveList, tl, sz) ||
                sz.Y < kHostRowPx) {
                UE_LOGE("host_window_native: WORLD LIST SKIP -- %d save row(s), list rect "
                        "%.0fx%.0f. This rig has no saves to pick, so whether the world list "
                        "can be clicked is UNMEASURED -- not passing.", rows, sz.X, sz.Y);
                g_selfCheckStep = -1;
                return;
            }
            // Half a row down: the FIRST save row, which is the one a player reaches for.
            UE_LOGW("host_window_native: world list has %d row(s) at desktop (%.0f,%.0f) "
                    "%.0fx%.0f -- aiming at the first", rows, tl.X, tl.Y, sz.X, sz.Y);
            // kHostRowPx, NOT the browser's kRowPx: this window's rows are 56 px and the
            // browser's are 64. Half of 64 still landed inside row 0 -- by margin, not by
            // construction, which is exactly the kind of aim that goes wrong silently the
            // day a layout constant moves.
            PlaceCursorOnAbsolute(tl.X + sz.X * 0.5f,
                                tl.Y + kHostRowPx * 0.5f);
            break;
        }
        case kWorldRead:
            g_worldBefore = ui::host_window_native::SelectedSave();
            break;
        case kWorldDown:
            ::mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            break;
        case kWorldUp:
            ::mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            break;
        case kWorldVerify: {
            const int now = ui::host_window_native::SelectedSave();
            // A CHANGE, not merely a non-negative value. Asserting `now >= 0` alone would
            // report PASS on a run where something was already selected and the click did
            // nothing -- silence dressed as success, which is the one thing this file is
            // written not to do.
            if (now >= 0 && now != g_worldBefore)
                UE_LOGW("host_window_native: WORLD LIST PASS -- a real click on the first "
                        "save row selected world %d (was %d). The world list is clickable, "
                        "so HOST can start something other than a new game.", now,
                        g_worldBefore);
            else
                UE_LOGE("host_window_native: WORLD LIST FAIL -- a real press-release on the "
                        "first save row left SelectedSave() at %d (was %d). The rows draw "
                        "and cannot be picked, so this window can only ever start a NEW "
                        "game.", now, g_worldBefore);
            g_selfCheckStep = -1;
            return;
        }
        default:
            break;
    }
    ++g_selfCheckStep;
}

void Arm() { g_selfCheckStep = 0; }

}  // namespace ui::server_browser_selftest
