// ui/server_browser_selftest.cpp -- the native browser's driven self-check: a phase ladder that
// scrolls the list, probes the scrim, presses ESC, clicks the action bar, a row, Back, HOST, the
// world list, the hosting window's exits, the input windows and the session-settings step, each
// verdict a log line the test rig asserts on. Runs only under the dev browser auto-open.

#include "ui/server_browser_selftest.h"

#include "ui/server_browser_native.h"   // IsOpen()/Open() -- the click phases drive the real screen
#include "ui/input_focus.h"            // synthesized input only lands in a FOREGROUND window
#include "ui/imgui_overlay.h"          // CaptureOwners() -- who is eating the mouse
#include "ui/server_browser_actions.h"  // the HOST button this drives
#include "ui/browser_input_screens.h"  // the input windows the last phases drive
#include "ui/host_session_settings.h"  // ...and what NEXT must open, one step further
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

// ---- the phase schedule ----
// Named, never literal: a positional case ladder renumbered by hand hides a missed index from
// both a diff and the compiler. The unit is one menu tick (~8.5 ms at the menu's 117 fps); a
// cursor move and its sample must be separate ticks, because IsHovered() read in the move's tick
// answers about the previous pointer position, so the gaps are eight ticks.
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
// Then re-open and drive Back: ESC closing the screen is no evidence that the chrome does.
constexpr int kReopen         = 88;
// The action bar, after the re-open and before any row is selected: the only window in which
// CONNECT's decline branch is reachable, and the decline branch is the one that can be driven
// without starting a real join. Every outcome of CONNECT is a sentence in the footer, observable
// to nobody but a human, so server_browser_actions::LastOutcome() exists and these phases read it.
constexpr int kActRefMove     = 90;
constexpr int kActRefDown     = 98;
constexpr int kActRefUp       = 102;
constexpr int kActRefVerify   = 106;
constexpr int kActConnMove    = 112;
constexpr int kActConnDown    = 120;
constexpr int kActConnUp      = 124;
constexpr int kActConnVerify  = 128;
// Row hover and selection, before Back: a browser whose rows cannot be picked is not a browser.
constexpr int kRowMove        = 132;
constexpr int kRowRead        = 140;
constexpr int kRowDown        = 142;
constexpr int kRowUp          = 146;
constexpr int kRowVerify      = 150;
// Then hold still twice, so a capture shows what the two state channels drew: ROW SELECT PASS
// proves the state changed and says nothing about pixels. Shot A parks the cursor on a row that
// is not the selected one (purple with a grey frame, the hovered row yellow, idle rows between);
// shot B moves it back onto the selected row, which must stay purple with no yellow frame. Each
// holds kShotHoldMs, the window the rig's 3 s capture poll needs.
constexpr int kSkinAimOther   = 151;
constexpr int kSkinHoldOther  = 152;  // HOLDING -- wall clock
constexpr int kSkinAimSelf    = 153;
constexpr int kSkinHoldSelf   = 154;  // HOLDING -- wall clock
constexpr int kClickMove      = 156;
constexpr int kClickSample    = 164;  // eight ticks after the move -- the scrim's budget
constexpr int kClickDown      = 166;
constexpr int kClickUp        = 170;
constexpr int kClickVerify    = 180;
// Then the HOST link, after the Back phases because clicking HOST closes the browser.
constexpr int kHostReopen     = 186;
constexpr int kHostMove       = 194;
constexpr int kHostDown       = 204;
constexpr int kHostUp         = 208;
constexpr int kHostVerify     = 220;
// Then the world list inside that window, a different screen's rows in a different ScrollBox:
// its own phases, since a row hit test through IsHovered does not answer inside a ScrollBox.
constexpr int kWorldMove      = 228;
constexpr int kWorldRead      = 236;
constexpr int kWorldDown      = 238;
constexpr int kWorldUp        = 242;
constexpr int kWorldVerify    = 252;
// Then the hosting window's exits. No native VOTV window has an X, so neither does ours: Back and
// ESC are the only ways out, and they fail independently (Back goes through the WidgetIsHovered
// predicate and inherits the capture-starved pointer that makes every native widget read
// not-hovered; ESC is a GetAsyncKeyState poll and survives that), so each gets a phase.
constexpr int kHostWindowHold = 253;   // one frame of the hosting window, for the eye; kWorldVerify + 1,
                                       // since the counter advances by one after a break
constexpr int kHostBackMove   = 258;
constexpr int kHostBackDown   = 266;
constexpr int kHostBackUp     = 270;
constexpr int kHostBackVerify = 278;
constexpr int kHostEscReopen  = 286;
constexpr int kHostEscPress   = 292;
constexpr int kHostEscHold    = 298;
constexpr int kHostEscRelease = 302;
constexpr int kHostEscVerify  = 308;
// The two input windows, last because they are the only phases that leave a window other than
// the browser on screen; each asserts that it opens (the browser itself has no text entry) and
// holds long enough to be photographed.
constexpr int kInputDirectShot = 314;
constexpr int kInputDirectHold = 315;
constexpr int kInputNameOpen   = 322;
constexpr int kInputNameShot   = 328;
constexpr int kInputNameHold   = 329;
// Session settings, step two of hosting and the only caller of HostWithSave: reached by pressing
// Next on the hosting window and by nothing else (no dev auto-open for it), so these phases drive
// the real path. The Host button itself is never pressed: it would leave the rig hosting.
constexpr int kSessOpen       = 336;
constexpr int kSessNextMove   = 344;
constexpr int kSessNextDown   = 350;
constexpr int kSessNextUp     = 354;
constexpr int kSessVerify     = 362;
constexpr int kSessShotHold   = 363;
constexpr int kSessLockMove   = 370;
constexpr int kSessLockDown   = 376;
constexpr int kSessLockUp     = 380;
constexpr int kSessLockVerify = 388;
constexpr int kSessLockHold   = 389;
constexpr int kSessBackMove   = 396;
constexpr int kSessBackDown   = 402;
constexpr int kSessBackUp     = 406;
constexpr int kSessBackVerify = 414;

// The forced offset for the positive control, far past any real extent: a getter that returns it
// unchanged echoes the request rather than reading Slate.
constexpr float    kHugeOffset   = 1.0e6f;
constexpr uint64_t kRowWaitMs    = 30000;  // rows arrive over HTTP; 30 s covers a cold fetch at the 5 s cadence
constexpr uint64_t kShotHoldMs   = 6000;   // mp.py polls the log every 3 s
constexpr uint64_t kWindowWaitMs = 15000;  // how long a null active window is a WAIT, not a fault

// The precondition takes two terms: an empty UScrollBox one tick after Show() reports offsetOfEnd
// = 1.0 (measured), so a positive maximum does not imply content. Rows answers "is there content";
// overflow answers "is there anywhere to go", at one whole row rather than > 0. The viewport holds
// ~8 rows at 1920x1080, so 12 overflow it at any plausible size.
constexpr int   kMinRows     = 12;
constexpr float kMinOverflow = 64.f;   // == kRowH: one whole row past the viewport

int g_selfCheckStep     = -1;  // -1 = idle; the dev scrim self-check's phase counter
int g_scrimOutside      = -1;  // -1 = not sampled, never a negative (an unrun phase is not a NO)
int g_scrimInsideWindow = -1;
// Password length sampled before the lock click, so the verify step knows whether a mint was due
// (the box was empty) or keeping the value is the right outcome.
int g_lockPwLenBefore   = 0;
// And whether the lock was already on: a click on an already-locked row is a no-op by design,
// indistinguishable from a dead click without this.
bool g_lockWasLockedBefore = false;

// ---- scroll-probe state ----
// Every reading starts at a value no measurement can produce, so an unrun phase is
// distinguishable from one that read zero, which is a legitimate answer to three of these.
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
// Place the pointer at a point in Slate's absolute space, which is desktop pixels (measured: the
// child table in native_screen's probe puts rows at desktop y 496..752 with the panel at
// (796,496), the space GetCursorPos reports), so this is a straight SetCursorPos and every caller
// hands it a WidgetScreenRect unchanged. A ClientToScreen here moves every aim by the client
// origin and lands the cursor below the last live row.
void PlaceCursorOnAbsolute(float absX, float absY) {
    ::SetCursorPos(static_cast<long>(absX), static_cast<long>(absY));
}

int      g_rowHovered        = -1;   // ...and the same for the row the click phase aims at
int      g_worldBefore       = -2;   // -2 = unsampled; -1 is New game, a real value

// One row's height (server_browser_native's kRowH), a constant rather than measured: the aim only
// has to land inside a row, and the verdict prints the index it got, so a drift shows as a
// different index.
constexpr float kRowPx = 64.f;

// The hosting window's rows are 56; one constant for both landed inside row 0 only by margin.
constexpr float kHostRowPx = 56.f;

// UWidget::GetDesiredSize, the non-visual proof that a hand-built widget lays out at all;
// resolved lazily, since this runs only inside a dev self-check.
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

// The four fields that decide whether a wheel event moves a UScrollBox; the box is hand-spawned,
// so each is whatever the CDO carries. ConsumeMouseWheel=Never, a zero multiplier and a
// Horizontal orientation each produce "the wheel did nothing" and are indistinguishable without
// this.
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

// The scrim is functional, not decorative (it eats a click that misses the window), so "it looks
// dim" is not evidence: put the cursor outside the window and ask Slate whether the scrim is under
// it, with a window hit checked too so a scrim that answers true everywhere is not a pass. Runs
// only under the dev browser auto-open, once; the move and the sample are separate ticks.
void Tick(void* scrim, void* list, void* exitBtn) {
    if (g_selfCheckStep < 0 || !scrim) return;
    // We drive input, so we must own the foreground: keybd_event and mouse_event inject into the
    // system queue and land in whatever window is foreground, so a run without focus sends ESC and
    // the click elsewhere and then blames the feature. GetForegroundWindow plus the ownership
    // predicate is the question; GetActiveWindow answers for the calling thread only. Not ours is a
    // bounded wait, then a loud disarm, never a silent one.
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

    // The second precondition: an ImGui surface that is up owns the mouse, WndProcDetour swallows
    // every mouse message and SetCursorPosDetour returns TRUE without moving the pointer, so a
    // perfect aim at a perfect widget reads IsHovered() == false everywhere (measured: the
    // config-review panel, armed at boot by an ini finding, held the pointer at client (0,0) for a
    // whole run). This refuses to produce a verdict rather than a false one.
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
        // ---- does the wheel scroll this widget at all ----
        // Not a screenshot test: an unchanged capture is three-way ambiguous (the wheel never
        // arrived, the box does not scroll, the capture beat Slate's layout). The offset is read
        // back instead, and a positive control runs first so a green answer means something.
        case kScrollWait:
            // Holding: rows arrive over HTTP, so at Show() the list is empty and
            // GetScrollOffsetOfEnd is legitimately 0. Wait for the content to overflow, the exact
            // quantity the question needs.
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
            // The positive control: ask for an offset far past the end. About offsetOfEnd means
            // Slate clamped it and the getter reads real state; about kHugeOffset means the getter
            // echoes the request (blind); 0 means the box refused to move.
            U::SetScrollOffset(list, kHugeOffset);
            break;
        case kScrollReadBig: {
            // The verdict is the view fraction, not the offset: GetScrollOffset echoes the request
            // (measured on an empty box and on 30 rows of real overflow), while
            // GetViewOffsetFraction reads the scrollbar's own post-layout distance from the top.
            // The offset is logged as the request it is.
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
                // Jump, but only after the same hold every other verdict gets: without it the scrim
                // and ESC phases run within ~250 ms and ESC closes the screen before the capture
                // poll arrives.
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
            // Ask Slate whether the cursor is over the list before blaming the wheel: a false here
            // means the notches went somewhere else, a different finding from "the box ignores the
            // wheel".
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
            // Four notches over eight ticks, not four in one: a real hand spaces them, and one
            // notch may sit inside a settling read's tolerance.
            ::mouse_event(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(-WHEEL_DELTA), 0);
            break;
        case kScrollReadPost: {
            U::ScrollOffset(list, g_wheelPost);
            U::ViewOffsetFraction(list, g_fracWheelPost);
            const float delta = g_fracWheelPost - g_fracWheelPre;
            // A verdict on movement, not direction: which sign a negative WHEEL_DELTA produces here
            // is unmeasured, and the question is whether the wheel reaches the widget at all. The
            // threshold is one row's worth of the total travel; a bare != would fire on layout
            // noise.
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
        // ---- the scrim ----
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
            // ESC: synthesize a real key so the production poll (GetAsyncKeyState in OnMenuTick)
            // answers, not a direct Hide(). Press and hold across ticks: down and up in one tick is
            // invisible to a per-tick poll. This line deliberately does not contain the string the
            // runner asserts on, or the runner's find() would match it and report a pass on a
            // failure.
            UE_LOGW("server_browser_native: ESC SELFTEST -- holding VK_ESCAPE for several ticks; "
                    "the close line below is the only evidence that counts");
            ::keybd_event(VK_ESCAPE, 0, 0, 0);
            break;
        case kEscObserve:
            // Sample the same global state the poll reads, mid-hold, so "did not close" can tell a
            // key that never entered the system from a poll or Hide() that failed to act.
            UE_LOGW("server_browser_native: ESC held -- GetAsyncKeyState(VK_ESCAPE) reads "
                    "%s at this tick, which is what the production poll sees",
                    (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) ? "DOWN" : "UP");
            break;
        case kEscRelease:
            ::keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
            break;
        case kReopen:
            // ESC has closed the screen; re-open it through the public Open(), the MULTIPLAYER
            // button's call, so Back is driven against a screen that came up the ordinary way.
            if (!exitBtn) {
                UE_LOGE("server_browser_native: BROWSER BACK SKIP -- no X was built, so "
                        "whether the chrome closes this screen is UNMEASURED");
                g_selfCheckStep = -1;
                return;
            }
            ui::server_browser_native::Open();
            break;
        // ---- the action bar: REFRESH, then CONNECT's decline branch ----
        // One helper drives both: read the rect Slate cached, put the real cursor on its centre,
        // let the production release-edge poll route the click. Nothing calls DoConnect or
        // DoRefresh directly: the defect this screen has suffered is a control that draws and
        // cannot be reached.
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
                // Fall through to the row phases rather than abort: the action bar is not a
                // precondition for anything below.
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
            // With nothing selected, and that is the assertion: "connect:none" proves the whole
            // path (layout, hit test, routing, SelectedRow() answering honestly) without starting a
            // join. The accept branch differs by one call to session_manager::JoinLobby and is not
            // driven here.
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
            // Aim at the second row: the first row's top edge is also the list's, so a rounding
            // error there would be the harness's failure. One and a half rows down is unambiguous.
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
                // Two links can produce that -1 and need different fixes: the containment gate said
                // the pointer is not over the list, or it said yes and no row's hit test answered.
                // Print both, plus the row's rect.
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
                // Find the row the cursor is actually on, then dump that one: the wheel phases
                // leave the list scrolled, so the top rows are off-screen with stale cached
                // geometry, and a SizeBox is SelfHitTestInvisible by default and answers 0 whatever
                // is true.
                int aimed = -1;
                for (int32_t i = 0; i < kids; ++i) {
                    void* kid = U::ChildAt(list, i);
                    ue_wrap::FVector2D rtl{}, rsz{};
                    if (!kid || !U::WidgetScreenRect(kid, rtl, rsz)) continue;
                    // Intersected with the list: a scrolled-out row's stale rect can still contain
                    // the cursor.
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
            // The click-moment shot, with the cursor not moved: shots A and B both move it first,
            // and a move repaints the row it leaves and the row it lands on, which heals exactly
            // the defect this window can contain (a selection repaint that updated the fill and the
            // frame but not the text). The hold starts here; the next phase waits it out.
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
            // Aim by the same arithmetic kRowMove used, so "the selected row" is the row that was
            // clicked, 1.5 rows in; the other aim is 4.5 rows in, inside the ~470 px list at any
            // window size this rig runs.
            if (nowMs < g_holdUntilMs) return;   // HOLDING: the click-moment shot's window
            const bool self = (g_selfCheckStep == kSkinAimSelf);
            // Never handed straight to %s: SelectedRowId returns a std::string's raw pointer, and a
            // null would be UB in the logger's vsnprintf.
            const char* selId = ui::server_browser_native::SelectedRowId();
            if (!selId) selId = "(none)";
            // Both terms: a pixel height says the list can contain five rows, not that five exist;
            // with three lobbies the aim lands in empty space and the shot would be archived under
            // a line asserting a highlight that is not in it.
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
            // The needle the rig captures on; it names what the frame should show, so the shot is
            // falsifiable by looking.
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
            // Ask the engine where the button is; do not compute it or hunt for it. A hard-coded
            // estimate is a second implementation of a layout the engine already performed, and a
            // sweep of that region inherits the guess. WidgetScreenRect reads Slate's own cached
            // geometry, correct under any window size, UI scale or layout edit, and keeps the two
            // failure modes apart: an empty rect means the button was never placed, a good rect
            // whose centre does not answer IsHovered means it is not hit-testable.
            ue_wrap::FVector2D tl{}, size{};
            const bool haveRect = U::WidgetScreenRect(exitBtn, tl, size);
            const ue_wrap::FVector2D want = DesiredSizeOf(exitBtn);
            if (!haveRect) {
                UE_LOGE("server_browser_native: BROWSER BACK SKIP -- Slate would not report "
                        "the X's geometry, so this run cannot say where it is. The link that "
                        "failed is named in the umg: line above; nothing below is a verdict "
                        "about the button.");
                g_selfCheckStep = -1;
                return;
            }
            // Allotted and desired printed together: equal and non-zero, the row gave the button
            // what it asked for; allotted (0,0) against a desired (53,48), a slot problem invisible
            // to any amount of clicking.
            UE_LOGW("server_browser_native: X geometry -- allotted %.0fx%.0f at desktop "
                    "(%.0f,%.0f), desired %.0fx%.0f, client %dx%d",
                    size.X, size.Y, tl.X, tl.Y, want.X, want.Y, w, h);
            // Calibration: a coordinate is meaningless without its space. The scrim is the ruler
            // (it spans the whole screen, proven by hover above), so its rect states the space's
            // extent; the list is the second reading.
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
                // And something reads it: at any scale but 1 this harness is void, not failing
                // (SetCursorPos takes desktop pixels, Slate reports absolute units, and they
                // coincide only while the viewport is unscaled). A window that came up 1392x782
                // instead of 1920x1080 once produced three false widget verdicts. So say VOID,
                // loudly, about the run.
                if (haveScrim && w > 0 && h > 0) {
                    const float sx = ssz.X / static_cast<float>(w);
                    const float sy = ssz.Y / static_cast<float>(h);
                    if (sx < 0.99f || sx > 1.01f || sy < 0.99f || sy > 1.01f)
                        UE_LOGE("server_browser_native: SPACE VOID -- the scrim spans "
                                "%.0fx%.0f Slate units over a %dx%d client, so absolute "
                                "space is scaled by %.3fx%.3f and is NOT desktop pixels. "
                                "Every driven click in this run aims at the wrong place and "
                                "every geometry verdict below is meaningless. The usual "
                                "cause is the game window not coming up at the size mp.py "
                                "asked for -- fix the window, then re-run; do not read the "
                                "failures as widget defects.",
                                ssz.X, ssz.Y, w, h, sx, sy);
                }
            }
            if (size.X < 1.f || size.Y < 1.f) {
                UE_LOGE("server_browser_native: BROWSER BACK FAIL -- the X occupies %.0fx%.0f "
                        "px, so it has no hit area at all. This is a LAYOUT defect, not a "
                        "click one: no cursor position can reach it. Check BuildButton's "
                        "HorizontalBox slot against the title text's fill weight.",
                        size.X, size.Y);
                g_selfCheckStep = -1;
                return;
            }
            // Slate's absolute space is desktop pixels at scale 1 (the calibration above), so the
            // rect is handed over unchanged.
            PlaceCursorOnAbsolute(tl.X + size.X * 0.5f, tl.Y + size.Y * 0.5f);
            break;
        }
        case kClickSample:
            // Sampled eight ticks after the move (IsHovered read too soon answers about the
            // previous position), and before the click, so the verdict can separate "never got
            // there" from "got there and nothing happened".
            g_closeHovered = E::WidgetIsHovered(exitBtn) ? 1 : 0;
            UE_LOGW("server_browser_native: the X reads IsHovered=%d with the cursor at its "
                    "own centre (list=%d scrim=%d at the same moment) -- clicking there now",
                    g_closeHovered, E::WidgetIsHovered(list) ? 1 : 0,
                    E::WidgetIsHovered(scrim) ? 1 : 0);
            break;
        case kClickDown:
            // Press and hold across ticks: the poll fires on the release edge and samples once per
            // tick, so a down and up inside one tick is invisible to it.
            ::mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            break;
        case kClickUp:
            ::mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            break;
        case kClickVerify:
            if (!ui::server_browser_native::IsOpen())
                UE_LOGW("server_browser_native: BROWSER BACK PASS -- a synthesized click "
                        "on Back closed the screen (hovered=%d). The chrome is a real "
                        "way out, not just a drawing.", g_closeHovered);
            else if (g_closeHovered == 0)
                UE_LOGE("server_browser_native: BROWSER BACK FAIL -- the screen is still "
                        "open, and IsHovered read FALSE with the cursor on the centre of "
                        "the rect Slate itself reported. The aim is not in question: the X "
                        "occupies that space and is not HIT-TESTABLE in it. Look at its "
                        "visibility and at what is painted over it, not at coordinates.");
            else
                UE_LOGE("server_browser_native: BROWSER BACK FAIL -- the cursor WAS over "
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
            // Both halves: the hosting window must be up and the browser must have got out of the
            // way; they are siblings in one switcher, so "both open" cannot render.
            const bool hostUp     = ui::host_window_native::IsOpen();
            const bool browserOut = !ui::server_browser_native::IsOpen();
            if (hostUp && browserOut)
                UE_LOGW("server_browser_native: HOST LINK PASS -- a real click on HOST "
                        "opened the hosting window and closed the browser.");
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
            // Half a row down: the first save row, the one a player reaches for.
            UE_LOGW("host_window_native: world list has %d row(s) at desktop (%.0f,%.0f) "
                    "%.0fx%.0f -- aiming at the first", rows, tl.X, tl.Y, sz.X, sz.Y);
            // kHostRowPx, not the browser's kRowPx: this window's rows are 56 px, and half of 64
            // landed inside row 0 by margin, not by construction.
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
            // A change, not merely a non-negative value: `now >= 0` alone would pass a run where
            // something was already selected and the click did nothing.
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
            // Hold, so the capture poll gets a frame of this window: its footer and its two lists
            // are what a picture can judge, and the last defect in it was found by eye (misaligned
            // footer buttons).
            g_holdUntilMs = nowMs + kShotHoldMs;
            break;
        }
        case kHostWindowHold:
            if (nowMs < g_holdUntilMs) return;
            g_selfCheckStep = kHostBackMove - 1;   // on to the hosting window's exits
            return;
        case kHostBackMove: {
            void* b = ui::host_window_native::BackButton();
            ue_wrap::FVector2D tl{}, sz{};
            if (!b || !U::WidgetScreenRect(b, tl, sz) || sz.X < 1.f) {
                UE_LOGE("host_window_native: HOST BACK SKIP -- the Back button %s, so whether "
                        "a player can leave this window is UNMEASURED. That is not a pass, and "
                        "with the X gone it is the only POINTER exit there is.",
                        b ? "has no readable rect" : "does not exist");
                g_selfCheckStep = -1;
                return;
            }
            PlaceCursorOnAbsolute(tl.X + sz.X * 0.5f, tl.Y + sz.Y * 0.5f);
            UE_LOGW("host_window_native: HOST BACK at desktop (%.0f,%.0f) %.0fx%.0f -- clicking it",
                    tl.X, tl.Y, sz.X, sz.Y);
            break;
        }
        case kHostBackDown:
            ::mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            break;
        case kHostBackUp:
            ::mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            break;
        case kHostBackVerify: {
            if (!ui::host_window_native::IsOpen())
                UE_LOGW("host_window_native: HOST BACK PASS -- a real click on Back closed the "
                        "hosting window. With the X gone this is the pointer exit, and it answers.");
            else
                UE_LOGE("host_window_native: HOST BACK FAIL -- the window is STILL OPEN after a "
                        "real press-release on the centre of the rect Slate reported for Back. "
                        "The X was deleted on the premise that this button answers; it does not, "
                        "so the window is pointer-inescapable and the deletion must revert.");
            break;
        }
        case kHostEscReopen:
            // ESC is a separate exit and owes a separate measurement: it is the one that survives a
            // capture-starved pointer, so a green Back says nothing about it.
            ui::host_window_native::Open();
            UE_LOGW("host_window_native: HOST ESC -- reopened the window to drive its keyboard exit");
            break;
        case kHostEscPress:
            ::keybd_event(VK_ESCAPE, 0, 0, 0);
            break;
        case kHostEscHold:
            UE_LOGW("host_window_native: HOST ESC held -- GetAsyncKeyState(VK_ESCAPE) reads %s at "
                    "this tick, which is what the production poll sees",
                    (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) ? "DOWN" : "UP");
            break;
        case kHostEscRelease:
            ::keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
            break;
        case kHostEscVerify: {
            if (!ui::host_window_native::IsOpen())
                UE_LOGW("host_window_native: HOST ESC PASS -- a real VK_ESCAPE press-release "
                        "closed the hosting window. Both of its exits are now measured.");
            else
                UE_LOGE("host_window_native: HOST ESC FAIL -- the window is STILL OPEN after a "
                        "real VK_ESCAPE press-release. With the X gone and this dead, a player "
                        "whose pointer is starved of mouse messages cannot leave at all.");
            // Ask for the direct-connect window so the next phase has something to measure;
            // deferred through Open(), like every consumer.
            ui::browser_input_screens::Open(ui::browser_input_screens::Kind::DirectConnect);
            // Break, not return: the step counter increments after the switch, so a return from a
            // case repeats the phase forever. A phase that ends the run sets step -1 and returns; a
            // phase that hands over breaks.
            break;
        }
        case kInputDirectShot: {
            if (!ui::browser_input_screens::IsOpen()) {
                UE_LOGE("server_browser_native: INPUT DIRECT FAIL -- the direct-connect window "
                        "did not open, so there is no way to connect by address at all. With "
                        "no text entry in the browser this window IS the capability.");
                g_selfCheckStep = -1;
                return;
            }
            UE_LOGW("server_browser_native: INPUT DIRECT PASS -- the direct-connect window "
                    "opened, prefilled and focused. This is where an address is typed; the "
                    "browser itself has no text entry, by the user's decision.");
            g_holdUntilMs = nowMs + kShotHoldMs;
            break;
        }
        case kInputDirectHold:
            // Holding: a window for the rig's 3 s capture poll. Returning without advancing is how
            // every hold here works; the counter increments after the switch.
            if (nowMs < g_holdUntilMs) return;
            ui::browser_input_screens::Open(ui::browser_input_screens::Kind::ChangeName);
            break;
        case kInputNameOpen:
            // One phase of slack: the open is deferred to the next menu tick, and the shot must not
            // fire on the window it is replacing.
            break;
        case kInputNameShot: {
            if (!ui::browser_input_screens::IsOpen()) {
                UE_LOGE("server_browser_native: INPUT NAME FAIL -- the change-name window did "
                        "not open.");
                g_selfCheckStep = -1;
                return;
            }
            UE_LOGW("server_browser_native: INPUT NAME PASS -- the change-name window opened. "
                    "The nickname is set here, which is the parity gap the browser used to "
                    "carry against the fallback surface.");
            g_holdUntilMs = nowMs + kShotHoldMs;
            break;
        }
        case kInputNameHold:
            if (nowMs < g_holdUntilMs) return;
            // Put the input window away and re-open step one, so the phases below have a real Next
            // button; Close() and Open() are both deferred and land on the same tick in order.
            ui::browser_input_screens::Close();
            break;
        case kSessOpen:
            ui::host_window_native::Open();
            UE_LOGW("host_session_settings: SESSION -- reopened the hosting window to drive "
                    "Next, the only door into step two");
            break;
        case kSessNextMove: {
            void* n = ui::host_window_native::NextButton();
            ue_wrap::FVector2D tl{}, sz{};
            if (!ui::host_window_native::IsOpen() || !n ||
                !U::WidgetScreenRect(n, tl, sz) || sz.X < 1.f) {
                UE_LOGE("host_session_settings: SESSION SKIP -- the hosting window is %s and "
                        "its Next button %s, so whether step two can be REACHED is "
                        "UNMEASURED. That is not a pass: step two is the only caller of "
                        "HostWithSave, so an unreachable one means nothing can be hosted.",
                        ui::host_window_native::IsOpen() ? "open" : "CLOSED",
                        n ? "has no readable rect" : "does not exist");
                g_selfCheckStep = -1;
                return;
            }
            PlaceCursorOnAbsolute(tl.X + sz.X * 0.5f, tl.Y + sz.Y * 0.5f);
            UE_LOGW("host_session_settings: NEXT at desktop (%.0f,%.0f) %.0fx%.0f -- clicking it",
                    tl.X, tl.Y, sz.X, sz.Y);
            break;
        }
        case kSessNextDown:
            ::mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            break;
        case kSessNextUp:
            ::mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            break;
        case kSessVerify: {
            // Both halves, as in the HOST LINK phase: step two up and step one gone; if step one
            // were still showing, its Back would restore an index that is now ours.
            const bool up   = ui::host_session_settings::IsOpen();
            const bool gone = !ui::host_window_native::IsOpen();
            if (up && gone)
                UE_LOGW("host_session_settings: SESSION PASS -- a real click on Next opened "
                        "the session-settings window and closed the hosting one. The two-step "
                        "hosting flow the user asked for is walkable end to end.");
            else
                UE_LOGE("host_session_settings: SESSION FAIL -- after a real click on Next: "
                        "settings open=%d, hosting window closed=%d. Both must be true.",
                        up ? 1 : 0, gone ? 1 : 0);
            if (!up) { g_selfCheckStep = -1; return; }
            g_holdUntilMs = nowMs + kShotHoldMs;
            break;
        }
        case kSessShotHold:
            if (nowMs < g_holdUntilMs) return;
            break;
        case kSessLockMove: {
            void* row = ui::host_session_settings::LockRow();
            ue_wrap::FVector2D tl{}, sz{};
            if (!row || !U::WidgetScreenRect(row, tl, sz) || sz.Y < 1.f) {
                UE_LOGE("host_session_settings: LOCK SKIP -- the \"Password required\" row %s, "
                        "so whether the lock can be turned on is UNMEASURED",
                        row ? "has no readable rect" : "does not exist");
                g_selfCheckStep = -1;
                return;
            }
            PlaceCursorOnAbsolute(tl.X + sz.X * 0.5f, tl.Y + sz.Y * 0.5f);
            // The length before the click decides which claim the verify step can make: SetLocked
            // mints only into an empty box, so on a rig that has hosted before the field is
            // restored from the ini and this click mints nothing.
            g_lockPwLenBefore = ui::host_session_settings::PasswordLength();
            g_lockWasLockedBefore = ui::host_session_settings::Locked();
            UE_LOGW("host_session_settings: LOCK row at desktop (%.0f,%.0f) %.0fx%.0f "
                    "(locked=%d, password length=%d before) -- clicking it",
                    tl.X, tl.Y, sz.X, sz.Y,
                    ui::host_session_settings::Locked() ? 1 : 0, g_lockPwLenBefore);
            break;
        }
        case kSessLockDown:
            ::mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            break;
        case kSessLockUp:
            ::mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            break;
        case kSessLockVerify: {
            // Two claims, because the row lighting up is not the feature: the click both set the
            // lock and minted a value, so a padlock over an empty box fails here rather than in
            // front of a player. The length is asserted, never the characters, and the expected
            // length is asked for, not written down here: a literal reported a working feature as
            // broken when the mint was shortened.
            const bool locked = ui::host_session_settings::Locked();
            const int  len    = ui::host_session_settings::PasswordLength();
            const int  want   = ui::host_session_settings::GeneratedPasswordLength();
            const bool minted = (g_lockPwLenBefore == 0);
            const int  expect = minted ? want : g_lockPwLenBefore;
            // And the lock must have actually moved: without this term the no-mint arm passes a
            // click that did nothing (a non-empty box with net.lobby_locked=1 opens already locked,
            // and SetLocked returns at its first line), which is the arm every rig that has hosted
            // before runs.
            const bool turnedOn = !g_lockWasLockedBefore;
            if (locked && turnedOn && len == expect)
                UE_LOGW("host_session_settings: LOCK PASS -- a real click on \"Password "
                        "required\" turned the lock on and %s. The host never has to invent "
                        "one.", minted
                            ? "minted a fresh password on the spot"
                            : "KEPT the password already in the box (no mint was due -- the "
                              "box was not empty, which is the documented behaviour)");
            else
                UE_LOGE("host_session_settings: LOCK FAIL -- after a real press-release on the "
                        "lock row: locked=%d (was %d), password length=%d, expected %d (%s). A "
                        "lock with no secret behind it is a padlock that lies to the host; a "
                        "lock that was already on measures nothing about the click.",
                        locked ? 1 : 0, g_lockWasLockedBefore ? 1 : 0, len, expect,
                        minted ? "a fresh mint was due" : "the existing value should have been kept");
            g_holdUntilMs = nowMs + kShotHoldMs;
            break;
        }
        case kSessLockHold:
            if (nowMs < g_holdUntilMs) return;
            break;
        case kSessBackMove: {
            void* b = ui::host_session_settings::BackButton();
            ue_wrap::FVector2D tl{}, sz{};
            if (!b || !U::WidgetScreenRect(b, tl, sz) || sz.X < 1.f) {
                UE_LOGE("host_session_settings: SESSION BACK SKIP -- the Back button %s. With "
                        "no X on any of these windows it is the only POINTER way out, so this "
                        "is not a pass.", b ? "has no readable rect" : "does not exist");
                g_selfCheckStep = -1;
                return;
            }
            PlaceCursorOnAbsolute(tl.X + sz.X * 0.5f, tl.Y + sz.Y * 0.5f);
            break;
        }
        case kSessBackDown:
            ::mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            break;
        case kSessBackUp:
            ::mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            break;
        case kSessBackVerify: {
            // Back goes to step one, not the main menu, and that is the whole claim; restoring the
            // switcher index alone is not enough, since the hosting window tracks its own shown
            // flag, so this asserts the flag and the index agree.
            const bool ours = ui::host_session_settings::IsOpen();
            const bool back = ui::host_window_native::IsOpen();
            if (!ours && back)
                UE_LOGW("host_session_settings: SESSION BACK PASS -- Back closed step two and "
                        "returned to step one, which is live again (not merely re-indexed).");
            else
                UE_LOGE("host_session_settings: SESSION BACK FAIL -- settings still open=%d, "
                        "hosting window live=%d. Back must land the player back on step one "
                        "with that window listening, or the flow is one-way.",
                        ours ? 1 : 0, back ? 1 : 0);
            ui::host_window_native::Close();
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
