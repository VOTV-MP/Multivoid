// coop/desk_sim_sync.cpp -- see coop/interactables/desk_sim_sync.h.

#include "coop/interactables/desk_sim_sync.h"

#include "coop/net/session.h"

#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/core/log.h"

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

// The interp window: 1.5x the 100 ms (10 Hz) send interval -- the jitter bridge.
constexpr uint64_t kInterpWindowMs = 150;

// v112 PER-CHANNEL interp (BUGS-v111 bug 4 fix). The v111 shape -- one shared
// LerpWindow reopened by EVERY packet -- meant a channel whose target stopped
// moving (the detector needle latched at exactly 1.0) never ARRIVED: the window
// rebase re-derived err each packet and the ease asymptoted a hair under the
// host's bitwise-exact 1.0, so the client's own detector block re-crossed the
// <1.0 gate every frame = the stuck beep loop. Now each channel keeps its OWN
// deadline: an incoming target that is bitwise-unchanged KEEPS the deadline ->
// the channel arrives -> cur[i] = target[i] EXACT SNAP (the mirrored-threshold-
// latch lesson); only a CHANGED target rebases err + deadline.
struct SimInterp {
    static constexpr int N = 7;
    float    cur[N] = {};
    float    target[N] = {};
    float    err[N] = {};
    uint64_t deadline[N] = {};   // 0 = arrived/idle
    uint64_t lastAdvance = 0;
    bool primed = false;

    void SetTarget(const float* t, uint64_t now) {
        if (!primed) {
            for (int i = 0; i < N; ++i) { cur[i] = target[i] = t[i]; err[i] = 0.f; deadline[i] = 0; }
            lastAdvance = now;
            primed = true;
            return;
        }
        Advance(now);  // advance-before-rebase (the interp-starvation fix, world_actor shape)
        for (int i = 0; i < N; ++i) {
            if (t[i] == target[i]) continue;  // unchanged -> KEEP the deadline (arrive + snap)
            target[i] = t[i];
            err[i] = t[i] - cur[i];
            deadline[i] = now + kInterpWindowMs;
        }
    }

    void Advance(uint64_t now) {
        if (!primed) return;
        const uint64_t dtMs = now > lastAdvance ? now - lastAdvance : 0;
        lastAdvance = now;
        for (int i = 0; i < N; ++i) {
            if (deadline[i] == 0) continue;          // arrived -- cur[i] IS target[i]
            if (now >= deadline[i]) {                // EXACT snap at arrival
                cur[i] = target[i];
                err[i] = 0.f;
                deadline[i] = 0;
                continue;
            }
            const uint64_t leftMs = deadline[i] - now;
            const float frac = static_cast<float>(dtMs) / static_cast<float>(leftMs + dtMs);
            cur[i] += err[i] * frac;
            err[i] = target[i] - cur[i];
        }
    }

    void Reset() { primed = false; for (int i = 0; i < N; ++i) deadline[i] = 0; }
};

SimInterp g_interp;
uint64_t  g_nextRepaintMs = 0;   // ~3 Hz full-screen repaint pulse (raw-write is per-tick)

CD::SimOutputs ToOutputs(const float* c) {
    CD::SimOutputs o;
    o.decoded = c[0]; o.resDetec = c[1]; o.rate = c[2]; o.frData = c[3];
    o.poData  = c[4]; o.frOffset = c[5]; o.poOffset = c[6];
    return o;
}

bool AllFinite(const coop::net::DeskSimSnapshot& s) {
    const float v[] = { s.decoded, s.resDetec, s.rate, s.frData,
                        s.poData, s.frOffset, s.poOffset };
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
                                                 cur.poData, cur.frOffset, cur.poOffset };
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
    const uint64_t nowMs = NowMs();
    if (isNew && AllFinite(snap)) {
        const float t[SimInterp::N] = { snap.decoded, snap.resDetec, snap.rate, snap.frData,
                                        snap.poData, snap.frOffset, snap.poOffset };
        g_interp.SetTarget(t, nowMs);
    }
    if (!g_interp.primed) return;
    g_interp.Advance(nowMs);
    // Raw-write every tick for smoothness; pulse the full repaint at ~3 Hz.
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
