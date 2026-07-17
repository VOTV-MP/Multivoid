// coop/desk_cursor_sync.cpp -- see coop/desk_cursor_sync.h.
//
// v109: the coords-panel LIVE cursor (ui_coordinates.viewCoordinate) as continuous
// MOTION, fixing the 3Hz-reliable-snap jaggy. Sibling of the hand-item motion stream
// (MsgType::HandPose): the reliable DishAimState carries the committed-coord IDENTITY
// (discrete locks), THIS carries the sweep.
//
// v115 redesign (qf 2026-07-17 "that holds"): the CURSOR axis is DECOUPLED from the
// desk CLAIM axis -- natively the glide integrator (spaceRenderer uber @518-970:
// movement := Vector2DInterpTo(movement, dir*vel, dt, coordinateDrift=0.5);
// setCoordinateLocation(cursor+movement)) has NO focus/claim gate, so the cursor
// keeps moving after the presser dismounts (measured decay: a max flick reaches
// sub-visible speed in ~12.4 s).
//
// SENDER: streams viewCoordinate while WE hold the desk claim, AND KEEPS streaming
// through release while the glide still moves the cursor (settle: |dPos| < 0.25 px
// over a 500 ms window; hard cap 15 s; cut early if another peer claims -- they own
// the cursor now).
//
// RECEIVER: applies whatever slot currently streams (holder, else the last holder's
// tail) while samples stay fresh (<700 ms); the release EDGE still replays the
// native intComs_unfocused (the dim -- native does the same at dismount) but does
// NOT reset the interp -- a claim flap or the momentum tail render seamlessly. The
// interp resets only when the stream goes idle. At the stream-START edge the local
// spaceRenderer.movement is zeroed once (a residual local glide would co-write
// against the incoming stream -- the integrator is ungated).
//
// INTERP: identical-target packets (a sender frame with no new position still sends
// -- same pos, new seq) no longer reopen the ease window, and the window ADAPTS to
// the measured position-change inter-arrival (EMA, clamp 25..80 ms) -- kills both
// the 60/60 aliasing beat and the staircase when the sender's frame rate dips.
// Writes go through WriteCursorOnly (a pure viewCoordinate memcpy -- NEVER
// updCursorLocations; the widget's own Tick repaints from the field).

#include "coop/interactables/desk_cursor_sync.h"

#include "coop/element/lerp_window.h"
#include "coop/interactables/desk_snd_fx.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/net/session.h"

#include "ue_wrap/console_desk.h"
#include "ue_wrap/log.h"
#include "ue_wrap/space_renderer.h"

#include <atomic>
#include <chrono>
#include <cmath>

namespace coop::desk_cursor_sync {
namespace {

namespace CD = ue_wrap::console_desk;
namespace SR = ue_wrap::space_renderer;

std::atomic<coop::net::Session*> g_session{nullptr};
const wchar_t* const kDeskClaim = L"desk";

uint64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---- sender-side momentum tail (qf R2: coordinateDrift=0.5 -> e-fold 2 s;
// a max flick decays to sub-visible ~12.4 s -- the cap is a runaway guard only).
constexpr uint64_t kTailCapMs = 15000;
constexpr uint64_t kSettleWindowMs = 500;
constexpr float    kSettleEpsPx = 0.25f;  // sub-visible on the ~5000 px canvas

// ---- receiver freshness: just above the settle window, so the tail's last
// sample parks the mirror exactly where the sender's glide rested.
constexpr uint64_t kStreamIdleMs = 700;

// ---- flap attribution (qf R3 Q4): a release+reclaim inside this window gets
// a WARN so a REAL occupancy flicker (if one exists) surfaces measured.
constexpr uint64_t kFlapWarnMs = 2000;

constexpr float kSnapPx = 4000.f;  // a jump beyond a window's plausible screen
                                   // motion -> snap (first sample / re-claim)

// The mirror interpolator (v115): error-window ease toward the newest sample,
// with identical-target dedupe + an adaptive window from the EMA of the
// position-CHANGE inter-arrival. Own tiny buffer -- NOT the actor-eid
// interpolator (a keyless screen coord has no actor / IsLiveByIndex).
struct CursorInterp {
    float curX = 0, curY = 0;
    float targetX = 0, targetY = 0;
    float errX = 0, errY = 0;
    coop::LerpWindow window_;
    bool primed = false;
    float emaChangeMs = 33.f;   // seeded at the 60 Hz interval
    uint64_t lastChangeMs = 0;

    void SetTarget(float tx, float ty) {
        const uint64_t now = NowMs();
        if (!primed) {
            curX = targetX = tx;
            curY = targetY = ty;
            errX = errY = 0.f;
            window_.Close();
            primed = true;
            lastChangeMs = now;
            return;
        }
        // Identical-target dedupe: a same-pos packet (sender frame produced no
        // new position) must not reopen the window -- keep easing the current one.
        if (tx == targetX && ty == targetY) return;
        const float dx = tx - curX, dy = ty - curY;
        if ((dx * dx + dy * dy) > kSnapPx * kSnapPx) {
            curX = targetX = tx;
            curY = targetY = ty;
            errX = errY = 0.f;
            window_.Close();
            lastChangeMs = now;
            return;
        }
        // Adaptive window: EMA of the position-change cadence (clamped), so a
        // 30 fps sender or arrival jitter widens the bridge instead of stepping.
        if (lastChangeMs) {
            float dt = static_cast<float>(now - lastChangeMs);
            if (dt < 1.f) dt = 1.f;
            if (dt > 250.f) dt = 250.f;  // a gap is a pause, not cadence
            emaChangeMs = emaChangeMs * 0.8f + dt * 0.2f;
        }
        lastChangeMs = now;
        float win = emaChangeMs * 1.25f;
        if (win < 25.f) win = 25.f;
        if (win > 80.f) win = 80.f;
        Advance();  // advance-before-rebase (the interp-starvation fix, world_actor shape)
        targetX = tx;
        targetY = ty;
        errX = tx - curX;
        errY = ty - curY;
        window_.Open(NowMs(), static_cast<int>(win));
    }
    void Advance() {
        if (!window_.IsOpen()) return;
        bool arrived = false;
        const float dA = window_.Advance(NowMs(), &arrived);
        curX += errX * dA;
        curY += errY * dA;
        if (arrived) { curX = targetX; curY = targetY; }
    }
    void Reset() { primed = false; window_.Close(); lastChangeMs = 0; emaChangeMs = 33.f; }
};

CursorInterp g_interp;

// Receiver state.
int      g_streamSlot = -1;      // the slot we are mirroring (-1 = none)
uint64_t g_lastSampleMs = 0;     // last isNew sample from g_streamSlot
bool     g_applying = false;     // stream-active latch (drives the start/idle edges)
int      g_lastHolder = -1;      // last tick's holder (release-edge detector)
uint64_t g_lastReleaseMs = 0;    // flap WARN bookkeeping
int      g_lastReleasedSlot = -1;  // WHO released (the flap WARN must not fire
                                   // on an ordinary X->Y handoff -- audit WARN)

// Sender state.
bool     g_streaming = false;    // are WE currently publishing our cursor?
bool     g_tail = false;         // post-release momentum tail active
uint64_t g_tailStartMs = 0;
float    g_lastSentX = 0, g_lastSentY = 0;
uint64_t g_lastMoveMs = 0;       // last time the published position moved > eps

void StopStreaming(coop::net::Session* s, const char* why) {
    if (!g_streaming) return;
    s->SetLocalDeskCursor(false, {});
    g_streaming = false;
    if (g_tail) {
        UE_LOGI("desk_cursor: momentum tail ended (%s, %.0f ms)", why,
                static_cast<double>(NowMs() - g_tailStartMs));
        g_tail = false;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;

    const uint8_t holderU = coop::device_occupancy::HolderOf(kDeskClaim);  // 0xFF == none
    const int  holder  = (holderU == 0xFF) ? -1 : static_cast<int>(holderU);
    const bool weHold  = coop::device_occupancy::LocalHolds(kDeskClaim);
    const uint64_t now = NowMs();

    // ---- SENDER: publish OUR live viewCoordinate while WE hold the desk,
    // and THROUGH release while the native glide still moves it. ----
    bool wantStream = weHold;
    if (!weHold && g_streaming) {
        if (!g_tail) { g_tail = true; g_tailStartMs = now; }
        const bool settled  = (now - g_lastMoveMs) > kSettleWindowMs;
        const bool capped   = (now - g_tailStartMs) > kTailCapMs;
        const bool usurped  = holder >= 0;  // another peer claimed: they own the cursor now
        wantStream = !settled && !capped && !usurped;
        if (!wantStream)
            StopStreaming(s, usurped ? "usurped" : (capped ? "capped" : "settled"));
    } else if (weHold && g_tail) {
        g_tail = false;  // re-claimed our own tail
    }

    if (wantStream) {
        if (CD::EnsureResolved()) {
            CD::DishAim aim;
            if (CD::ReadDishAim(aim)) {
                coop::net::DeskCursorPoseSnapshot snap{ aim.viewX, aim.viewY };
                s->SetLocalDeskCursor(true, snap);  // net thread streams it at sendHz
                if (!g_streaming) {
                    g_streaming = true;
                    g_lastMoveMs = now;
                    g_lastSentX = aim.viewX;
                    g_lastSentY = aim.viewY;
                }
                const float dx = aim.viewX - g_lastSentX, dy = aim.viewY - g_lastSentY;
                if ((dx * dx + dy * dy) > kSettleEpsPx * kSettleEpsPx) {
                    g_lastMoveMs = now;
                    g_lastSentX = aim.viewX;
                    g_lastSentY = aim.viewY;
                }
            }
        }
        if (weHold) {
            g_interp.Reset();     // we are the source now, not a mirror
            g_applying = false;   // drop any stale mirror latch (we may have been
            g_streamSlot = -1;    // applying the previous holder's stream)
            g_lastHolder = holder;
            return;
        }
        // Tail mode: we stream but no longer hold -- fall through so the
        // RECEIVER half still tracks a new holder appearing.
    } else if (g_streaming) {
        StopStreaming(s, "released-idle");
    }

    // ---- RECEIVER: mirror whichever slot currently streams. ----
    // Pick the source: the holder while one exists; otherwise keep the last
    // source (its momentum tail is still authoritative).
    if (holder >= 0 && holder != g_streamSlot) {
        g_streamSlot = holder;
        SR::ZeroMovement();  // kill any residual LOCAL glide: the remote stream
                             // owns the cursor now (ungated integrator co-write)
    }

    // Release edge: replay the native dim exactly once per release (native
    // intComs_unfocused fires at dismount on the presser too). NO interp reset
    // -- the tail keeps rendering, a flap keeps gliding.
    if (holder < 0 && g_lastHolder >= 0) {
        if (CD::EnsureResolved()) {
            coop::desk_snd_fx::ScopedWireApply guard;
            CD::CallIntComsUnfocused();
        }
        UE_LOGI("desk_cursor: holder released (was slot %d) -- native intComs_unfocused replayed",
                g_lastHolder);
        g_lastReleaseMs = now;
        g_lastReleasedSlot = g_lastHolder;
    }
    if (holder >= 0 && g_lastHolder < 0 && g_lastReleaseMs &&
        (now - g_lastReleaseMs) < kFlapWarnMs && holder == g_lastReleasedSlot) {
        // The SAME slot re-claimed within the window -- a real flap candidate
        // (an ordinary X->Y handoff must not fire this; audit 2026-07-17 WARN).
        UE_LOGW("desk_cursor: claim FLAP -- slot %d re-claimed %.0f ms after release "
                "(occupancy flicker attribution, qf R3)",
                holder, static_cast<double>(now - g_lastReleaseMs));
    }
    g_lastHolder = holder;

    // While WE stream (holder or our own tail), we are the author -- never mirror.
    if (g_streamSlot < 0 || g_streaming) return;

    if (CD::EnsureResolved()) {
        coop::net::DeskCursorPoseSnapshot snap;
        bool isNew = false;
        if (s->TryGetRemoteDeskCursor(g_streamSlot, snap, &isNew)) {
            if (isNew && std::isfinite(snap.viewX) && std::isfinite(snap.viewY)) {
                if (!g_applying) {
                    g_applying = true;
                    SR::ZeroMovement();  // stream-START edge (covers the join/first case)
                }
                g_lastSampleMs = now;
                g_interp.SetTarget(snap.viewX, snap.viewY);
            }
            if (g_applying) {
                g_interp.Advance();
                // Fire-line INSIDE the apply branch (grep 'desk_cursor: applying' --
                // the known-positive; a 0 there means the branch never ran).
                static uint64_t s_logThrottle = 0;
                if ((++s_logThrottle % 300) == 1)  // ~every 5 s at 60 Hz
                    UE_LOGI("desk_cursor: applying slot=%d holder=%d cur=(%.1f,%.1f) "
                            "target=(%.1f,%.1f) win=%d ema=%.0fms",
                            g_streamSlot, holder, g_interp.curX, g_interp.curY,
                            g_interp.targetX, g_interp.targetY,
                            g_interp.window_.IsOpen() ? 1 : 0,
                            static_cast<double>(g_interp.emaChangeMs));
                coop::desk_snd_fx::ScopedWireApply guard;
                CD::WriteCursorOnly(g_interp.curX, g_interp.curY);
            }
        }
    }

    // Stream idle: the tail (or the sender) went quiet -- rest reached.
    if (g_applying && (now - g_lastSampleMs) > kStreamIdleMs && holder < 0) {
        g_applying = false;
        g_streamSlot = -1;
        g_interp.Reset();
        UE_LOGI("desk_cursor: stream idle -- mirror at rest");
    }
}

void OnDisconnect() {
    g_interp.Reset();
    g_streamSlot = -1;
    g_lastHolder = -1;
    g_applying = false;
    g_lastSampleMs = 0;
    g_lastReleaseMs = 0;
    g_lastReleasedSlot = -1;
    g_tail = false;
    if (g_streaming) {
        if (auto* s = g_session.load(std::memory_order_acquire))
            s->SetLocalDeskCursor(false, {});
        g_streaming = false;
    }
}

}  // namespace coop::desk_cursor_sync
