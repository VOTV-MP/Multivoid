// coop/desk_cursor_sync.cpp -- see coop/desk_cursor_sync.h.
//
// v109: the coords-panel LIVE cursor (ui_coordinates.viewCoordinate) as continuous
// MOTION, fixing the 3Hz-reliable-snap jaggy. Sibling of the hand-item motion stream
// (MsgType::HandPose): the reliable DishAimState carries the committed-coord IDENTITY
// (discrete locks), THIS carries the sweep.
//
// SENDER (the desk-claim holder): each pump tick, read viewCoordinate and publish it
// via Session::SetLocalDeskCursor -> the net thread fans out a DeskCursorPose datagram
// at sendHz. CONTINUOUS while claimed (the PoseSnapshot/HandPose precedent -- no change
// gate, so the mirror always converges to the EXACT rest position; ~1.7 KB/s, trivial;
// this is why no reliable stop-pin is needed).
//
// RECEIVER (everyone else): interpolate toward the newest sample over a 50ms LerpWindow
// (the proven RemotePlayer/world_actor error-window ease; 50ms = 3x the 60Hz send
// interval -- the jitter bridge -- trimmed from the 75ms physics-body value because a
// 2D cursor snap is cheaper than a body teleport; TAKE-tunable) and write it via
// WriteCursorOnly (a pure viewCoordinate memcpy -- NEVER updCursorLocations, no 60Hz
// pingDishes setRot storm; the widget's own Tick repaints from the field). On the
// holder's release the mirror replays the desk's native intComs_unfocused (reset-on-
// release: setCursorOpacity dims 0.25, never hides -- matched to native, not invented).

#include "coop/interactables/desk_cursor_sync.h"

#include "coop/element/lerp_window.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/net/session.h"

#include "ue_wrap/console_desk.h"
#include "ue_wrap/log.h"

#include <atomic>
#include <chrono>
#include <cmath>

namespace coop::desk_cursor_sync {
namespace {

namespace CD = ue_wrap::console_desk;

std::atomic<coop::net::Session*> g_session{nullptr};
const wchar_t* const kDeskClaim = L"desk";

uint64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The mirror interpolator: the proven RemotePlayer/world_actor error-window ease,
// applied to the 2D screen cursor. curXY chases the newest sample over a fixed
// window; on arrival it snaps exact (kills float drift), matching LerpWindow's
// contract. Its OWN tiny buffer -- NOT the actor-eid interpolator (a keyless screen
// coord has no actor / IsLiveByIndex), same math per the /qf design.
constexpr int   kInterpWindowMs = 50;    // derived: 3x the 60Hz send interval (jitter
                                         // bridge), trimmed from the 75ms physics value.
                                         // TAKE-tunable: laggy -> 33ms/extrapolate;
                                         // snapping/starving -> up toward 75ms.
constexpr float kSnapPx = 4000.f;        // a jump beyond a window's plausible screen
                                         // motion -> snap (first sample / re-claim).

struct CursorInterp {
    float curX = 0, curY = 0;
    float targetX = 0, targetY = 0;
    float errX = 0, errY = 0;
    coop::LerpWindow window_;
    bool primed = false;

    void SetTarget(float tx, float ty) {
        const float dx = tx - curX, dy = ty - curY;
        if (!primed || (dx * dx + dy * dy) > kSnapPx * kSnapPx) {
            curX = targetX = tx;
            curY = targetY = ty;
            errX = errY = 0.f;
            window_.Close();
            primed = true;
            return;
        }
        Advance();  // advance-before-rebase (the interp-starvation fix, world_actor shape)
        targetX = tx;
        targetY = ty;
        errX = tx - curX;
        errY = ty - curY;
        window_.Open(NowMs(), kInterpWindowMs);
    }
    void Advance() {
        if (!window_.IsOpen()) return;
        bool arrived = false;
        const float dA = window_.Advance(NowMs(), &arrived);
        curX += errX * dA;
        curY += errY * dA;
        if (arrived) { curX = targetX; curY = targetY; }
    }
    void Reset() { primed = false; window_.Close(); }
};

CursorInterp g_interp;
int  g_lastHolder = -1;   // the desk holder we were mirroring last tick (-1 = none)
bool g_streaming  = false;  // are WE (the holder) currently publishing our cursor?

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

    // ---- SENDER: publish OUR live viewCoordinate while WE hold the desk. ----
    if (weHold) {
        if (CD::EnsureResolved()) {
            CD::DishAim aim;
            if (CD::ReadDishAim(aim)) {
                coop::net::DeskCursorPoseSnapshot snap{ aim.viewX, aim.viewY };
                s->SetLocalDeskCursor(true, snap);  // net thread streams it at sendHz
                g_streaming = true;
            }
        }
        g_interp.Reset();     // we are the source now, not a mirror
        g_lastHolder = holder;
        return;
    }
    // We do NOT hold: stop any stream we were publishing.
    if (g_streaming) {
        s->SetLocalDeskCursor(false, {});
        g_streaming = false;
    }

    // ---- Nobody holds: on the release EDGE, replay the native unfocus once. ----
    if (holder < 0) {
        if (g_lastHolder >= 0) {
            if (CD::EnsureResolved()) CD::CallIntComsUnfocused();
            UE_LOGI("desk_cursor: holder released (was slot %d) -- native intComs_unfocused replayed",
                    g_lastHolder);
            g_interp.Reset();
            g_lastHolder = -1;
        }
        return;
    }

    // ---- RECEIVER: a REMOTE peer holds -> interpolate + write their cursor. ----
    if (!CD::EnsureResolved()) return;
    coop::net::DeskCursorPoseSnapshot snap;
    bool isNew = false;
    if (s->TryGetRemoteDeskCursor(holder, snap, &isNew)) {
        if (isNew && std::isfinite(snap.viewX) && std::isfinite(snap.viewY))
            g_interp.SetTarget(snap.viewX, snap.viewY);
        g_interp.Advance();
        // Fire-line INSIDE the apply branch (grep 'desk_cursor: applying' -- the
        // known-positive; a 0 there means the branch never ran, not "no bug").
        static uint64_t s_logThrottle = 0;
        if ((++s_logThrottle % 300) == 1)  // ~every 5 s at 60 Hz: proof-of-life, not spam
            UE_LOGI("desk_cursor: applying holder=%d cur=(%.1f,%.1f) target=(%.1f,%.1f) win=%d",
                    holder, g_interp.curX, g_interp.curY, g_interp.targetX, g_interp.targetY,
                    g_interp.window_.IsOpen() ? 1 : 0);
        CD::WriteCursorOnly(g_interp.curX, g_interp.curY);
    }
    g_lastHolder = holder;
}

void OnDisconnect() {
    g_interp.Reset();
    g_lastHolder = -1;
    if (g_streaming) {
        if (auto* s = g_session.load(std::memory_order_acquire))
            s->SetLocalDeskCursor(false, {});
        g_streaming = false;
    }
}

}  // namespace coop::desk_cursor_sync
