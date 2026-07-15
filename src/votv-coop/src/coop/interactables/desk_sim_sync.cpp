// coop/desk_sim_sync.cpp -- see coop/interactables/desk_sim_sync.h.

#include "coop/interactables/desk_sim_sync.h"

#include "coop/element/lerp_window.h"
#include "coop/net/session.h"

#include "ue_wrap/console_desk.h"
#include "ue_wrap/log.h"

#include <atomic>
#include <chrono>
#include <cmath>

namespace coop::desk_sim_sync {
namespace {

namespace CD = ue_wrap::console_desk;

std::atomic<coop::net::Session*> g_session{nullptr};

uint64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// The interp window: 1.5x the 100 ms (10 Hz) send interval -- the jitter bridge, kept
// short because the sim outputs move slowly (a knob ramp is seconds; decoded fills over
// minutes). TAKE-tunable if a ramp still steps.
constexpr int kInterpWindowMs = 150;

// Multi-channel error-window ease (the desk_cursor_sync CursorInterp shape, generalized
// to the 8-float output vector: one shared LerpWindow, all channels advance together
// since they arrive on the same datagram). Snap on the first sample; interpolate after.
struct SimInterp {
    static constexpr int N = 8;
    float cur[N] = {};
    float target[N] = {};
    float err[N] = {};
    coop::LerpWindow window_;
    bool primed = false;

    void SetTarget(const float* t) {
        if (!primed) {
            for (int i = 0; i < N; ++i) { cur[i] = target[i] = t[i]; err[i] = 0.f; }
            window_.Close();
            primed = true;
            return;
        }
        Advance();  // advance-before-rebase (the interp-starvation fix, world_actor shape)
        for (int i = 0; i < N; ++i) { target[i] = t[i]; err[i] = t[i] - cur[i]; }
        window_.Open(NowMs(), kInterpWindowMs);
    }
    void Advance() {
        if (!window_.IsOpen()) return;
        bool arrived = false;
        const float dA = window_.Advance(NowMs(), &arrived);
        for (int i = 0; i < N; ++i) cur[i] += err[i] * dA;
        if (arrived) for (int i = 0; i < N; ++i) cur[i] = target[i];
    }
    void Reset() { primed = false; window_.Close(); }
};

SimInterp g_interp;
uint64_t  g_nextRepaintMs = 0;   // ~3 Hz full-screen repaint pulse (raw-write is per-tick)

CD::SimOutputs ToOutputs(const float* c) {
    CD::SimOutputs o;
    o.decoded = c[0]; o.resDetec = c[1]; o.rate = c[2]; o.frData = c[3];
    o.poData  = c[4]; o.frOffset = c[5]; o.poOffset = c[6]; o.cooldown = c[7];
    return o;
}

bool AllFinite(const coop::net::DeskSimSnapshot& s) {
    const float v[] = { s.decoded, s.resDetec, s.rate, s.frData,
                        s.poData, s.frOffset, s.poOffset, s.cooldown };
    for (float f : v) if (!std::isfinite(f)) return false;
    return true;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;

    // ---- HOST: publish the live sim outputs (its BP is the authority). ----
    if (s->role() == coop::net::Role::Host) {
        if (CD::EnsureResolved()) {
            CD::SimOutputs cur;
            if (CD::ReadSimOutputs(cur)) {
                coop::net::DeskSimSnapshot snap{ cur.decoded, cur.resDetec, cur.rate, cur.frData,
                                                 cur.poData, cur.frOffset, cur.poOffset, cur.cooldown };
                s->SetHostDeskSim(true, snap);
            }
        }
        g_interp.Reset();  // host is the source, never a mirror
        return;
    }

    // ---- CLIENT: interpolate the host's vector + OVERWRITE the local sim. ----
    if (!s->connected() || !CD::EnsureResolved()) return;
    coop::net::DeskSimSnapshot snap;
    bool isNew = false;
    if (!s->TryGetHostDeskSim(snap, &isNew)) return;
    if (isNew && AllFinite(snap)) {
        const float t[SimInterp::N] = { snap.decoded, snap.resDetec, snap.rate, snap.frData,
                                        snap.poData, snap.frOffset, snap.poOffset, snap.cooldown };
        g_interp.SetTarget(t);
    }
    g_interp.Advance();
    // Raw-write every tick for smoothness; pulse the full repaint at ~3 Hz.
    const uint64_t nowMs = NowMs();
    const bool repaint = nowMs >= g_nextRepaintMs;
    if (repaint) g_nextRepaintMs = nowMs + 333;
    CD::WriteSimOutputs(ToOutputs(g_interp.cur), repaint);
}

void OnDisconnect() {
    g_interp.Reset();
    g_nextRepaintMs = 0;
    if (auto* s = g_session.load(std::memory_order_acquire))
        s->SetHostDeskSim(false, {});
}

}  // namespace coop::desk_sim_sync
