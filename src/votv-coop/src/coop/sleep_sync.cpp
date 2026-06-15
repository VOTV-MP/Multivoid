// coop/sleep_sync.cpp -- see coop/sleep_sync.h.

#include "coop/sleep_sync.h"

#include "coop/chat_feed.h"
#include "coop/net/session.h"
#include "coop/time_sync.h"

#include "ue_wrap/log.h"
#include "ue_wrap/sleep.h"

#include <atomic>
#include <cstdio>

namespace coop::sleep_sync {
namespace {

namespace SLP = ue_wrap::sleep;

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- local peer state -------------------------------------------------------
bool g_lastInBed = false;       // the isSleep edge detector
bool g_waitUndone = false;      // WAITING enforcement latch (dilation undone for this bed entry)
bool g_accelerate = false;      // the 20x phase is on (host decides; mirrored from the wire on clients)
bool g_dreamProbSuppressed = false;  // we hold gamemode.dreamProbability at 0
void* g_gmInst = nullptr;       // gamemode instance the policy/edge state was applied to

// ---- host tally -------------------------------------------------------------
bool g_inBed[coop::net::kMaxPeers] = {};
uint8_t g_lastCount = 0, g_lastTotal = 0;

bool IsHost(coop::net::Session* s) { return s->role() == coop::net::Role::Host; }

void PushCounterLine(uint8_t count, uint8_t total) {
    if (count == 0) return;
    wchar_t buf[64];
    ::swprintf(buf, 64, L"%u/%u players sleeping", static_cast<unsigned>(count),
               static_cast<unsigned>(total));
    coop::chat_feed::Push(buf);
}

void ApplyDreamProbPolicy(coop::net::Session* s) {
    // CLIENTS: nightmares suppressed for the whole session (host-only rolls).
    // HOST: suppressed while idle/waiting; the -1 sentinel (use bed.dreamProb)
    // is restored only DURING the accelerate phase, making the host the single
    // nightmare roller of the gated night.
    const bool wantSuppressed = !(IsHost(s) && g_accelerate);
    if (SLP::SetDreamProbability(wantSuppressed ? 0.f : -1.f))
        g_dreamProbSuppressed = wantSuppressed;
}

// HOST: recount + broadcast the tally; fire phase transitions.
void HostRetally(coop::net::Session* s);

// Apply the accelerate phase locally (both roles). The dilation only applies
// to a peer that is actually in bed -- a peer that raced its exit stays at
// 1.0 (its inBed=false report is in flight; the host will End).
void ApplyAccelerateLocal(coop::net::Session* s, bool on) {
    if (g_accelerate == on) return;
    g_accelerate = on;
    if (on) {
        if (SLP::IsSleeping()) {
            SLP::SetGlobalTimeDilation(20.f);
            // The cinematic base view belongs to THIS phase (user directive
            // 2026-06-13): the WAITING hold parked the camera at the bed;
            // hand it to the gamemode's own sleepCam now.
            if (SLP::SetSleepViewTarget(SLP::SleepCam()))
                UE_LOGI("sleep_sync: camera -> sleepCam (the timelapse view)");
        }
        if (!IsHost(s)) coop::time_sync::SetSleepAccelerate(true);
        coop::chat_feed::Push(L"Everyone is asleep -- the night passes...");
        UE_LOGI("sleep_sync: ACCELERATE (local dilation -> %s)",
                SLP::IsSleeping() ? "20" : "1 (not in bed)");
    } else {
        if (!IsHost(s)) coop::time_sync::SetSleepAccelerate(false);
    }
    ApplyDreamProbPolicy(s);
}

// Apply the END locally (both roles). `natural` grants the full night's rest.
// ORDER MATTERS: wakeup() first (the 10% gearer gift rolls iff need >= 99 at
// call time -- the native waker keeps its own native roll; mirrors must not
// add one), the need write after.
void ApplyEndLocal(coop::net::Session* s, bool natural) {
    ApplyAccelerateLocal(s, false);
    if (SLP::IsSleeping()) SLP::CallWakeup();
    if (natural) SLP::WriteSleepNeed(100.f);
    coop::chat_feed::Push(natural ? L"Good morning -- everyone is rested."
                                  : L"Sleep interrupted -- everyone wakes up.");
    UE_LOGI("sleep_sync: END (natural=%d)", natural ? 1 : 0);
}

void HostBroadcast(coop::net::Session* s, uint8_t op, uint8_t flag,
                   uint8_t count, uint8_t total) {
    coop::net::SleepStatePayload p{};
    p.op = op;
    p.flag = flag;
    p.count = count;
    p.total = total;
    s->SendReliable(coop::net::ReliableKind::SleepState, &p, sizeof(p));
}

void HostRetally(coop::net::Session* s) {
    // total = the host + every world-ready client (a mid-join peer is not in
    // the world yet and must not block the gate; once world-ready it counts
    // -- and being awake, it holds the gate open until it sleeps too).
    uint8_t total = 1, count = g_inBed[0] ? 1 : 0;
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        if (!s->IsSlotWorldReady(slot)) continue;
        ++total;
        if (g_inBed[slot]) ++count;
    }
    if (count != g_lastCount || total != g_lastTotal) {
        g_lastCount = count;
        g_lastTotal = total;
        // The counter is waiting-room UI only: during the accelerate phase a
        // count drop is the END transition, and "1/2 players sleeping"
        // flashing right before "Sleep interrupted" reads as nonsense (the
        // 00:28 probe smoke).
        if (!g_accelerate && count > 0 && count < total) {
            HostBroadcast(s, 1 /*Tally*/, 0, count, total);
            PushCounterLine(count, total);
            UE_LOGI("sleep_sync: tally %u/%u in bed", static_cast<unsigned>(count),
                    static_cast<unsigned>(total));
        }
    }
    if (!g_accelerate && count > 0 && count == total) {
        HostBroadcast(s, 2 /*Accelerate*/, 0, count, total);
        ApplyAccelerateLocal(s, true);
    } else if (g_accelerate && count < total) {
        // ANY drop ends the night. Natural iff the HOST is the one who woke
        // with a full need (its native wake-check fired at >= 100; clients
        // are clamped below the threshold and cannot end naturally; every
        // other cause -- manual exit, hunger, event, nightmare -- interrupts).
        float hostNeed = 0.f;
        const bool natural = !g_inBed[0] && SLP::ReadSleepNeed(hostNeed) && hostNeed >= 99.5f;
        // Zero the tally NOW: every peer's wakeup is about to report a falling
        // edge; processing those against stale true flags would paint spurious
        // "N/M sleeping" lines after the morning line.
        for (bool& b : g_inBed) b = false;
        g_lastCount = 0;
        HostBroadcast(s, 3 /*End*/, natural ? 1 : 0, 0, total);
        ApplyEndLocal(s, natural);
    }
}

void ReportLocalEdge(coop::net::Session* s, bool inBed) {
    if (IsHost(s)) {
        g_inBed[0] = inBed;
        HostRetally(s);
    } else {
        coop::net::SleepStatePayload p{};
        p.op = 0;  // Report
        p.flag = inBed ? 1 : 0;
        s->SendReliableToSlot(0, coop::net::ReliableKind::SleepState, &p, sizeof(p));
    }
    UE_LOGI("sleep_sync: local %s bed", inBed ? "entered" : "left");
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!SLP::EnsureResolved()) return;
    void* gm = SLP::Gamemode();
    if (!gm) return;

    // Fresh gamemode (boot / level reload): re-prime the edge state + re-apply
    // the nightmare policy on the new instance (the signal_catch shape). A
    // connected gamemode swap is unreachable in the session lifecycle today
    // (level travel disconnects first), but a stranded accelerate/tally would
    // be poisonous if it ever became reachable -- clear it all (audit note 1).
    if (gm != g_gmInst) {
        g_gmInst = gm;
        g_lastInBed = SLP::IsSleeping();
        g_waitUndone = false;
        g_accelerate = false;
        for (bool& b : g_inBed) b = false;
        g_lastCount = g_lastTotal = 0;
        if (s->connected()) ApplyDreamProbPolicy(s);
    }

    if (!s->connected()) {
        // Solo session (host with no peers yet): stock SP behavior, nothing
        // gated. The policy/state re-arms on the first connect edge below.
        return;
    }

    const bool inBed = SLP::IsSleeping();

    // One-shot per connect edge: make sure the nightmare policy is applied
    // (covers the host-was-already-running case + the first client connect).
    if (!g_dreamProbSuppressed && !g_accelerate) ApplyDreamProbPolicy(s);

    // WAITING enforcement: a peer in bed outside the accelerate phase runs at
    // dilation 1.0. Edge-latched -- the only native setter is the sleep()
    // entry (@70543), so one undo per bed entry suffices; wakeup() itself
    // restores 1.0 on every exit path. Covers ALL entries: bed interaction,
    // the dev probe's reflected sleep(), and a host that was already asleep
    // when the first client connected.
    if (inBed && !g_accelerate && !g_waitUndone) {
        // Latch ONLY on success (perf-audit note A): a transient world-context
        // gap would otherwise skip the undo yet latch, stranding this peer at
        // the native 20x for the whole wait. On failure we just retry next tick.
        if (SLP::SetGlobalTimeDilation(1.0f)) {
            g_waitUndone = true;
            // Hold the camera AT THE BED while waiting (user directive
            // 2026-06-13): the native entry retargeted it to the sleepCam
            // base shot; the cinematic only belongs to the all-asleep phase
            // (ApplyAccelerateLocal hands it back). Null-safe: if the pawn/
            // controller is not up yet the view stays native -- cosmetic,
            // never a gate blocker.
            const bool held = SLP::SetSleepViewTarget(SLP::SleepingPawn());
            UE_LOGI("sleep_sync: WAITING -- dilation undone to 1.0 (gate not full)%s",
                    held ? ", camera held at the bed" : "");
        }
    } else if ((!inBed || g_accelerate) && g_waitUndone) {
        g_waitUndone = false;
    }

    // The isSleep edge -> report (host tallies itself directly).
    if (inBed != g_lastInBed) {
        g_lastInBed = inBed;
        ReportLocalEdge(s, inBed);
    }

    // CLIENT clamp during the phase: only the HOST ends the night naturally
    // (one authority, no first-to-fill race; the full need is granted to
    // everyone at a natural END anyway). Clamp at 98 -- STRICTLY below both
    // the natural-wake check (>= 100) and wakeup's gearer-gift threshold
    // (>= 99 at call time): a 99 clamp would make every clamped client roll
    // the 10% gift inside the END wakeup. 1 float read per tick, only while
    // accelerated and in bed.
    if (g_accelerate && !IsHost(s) && inBed) {
        float need = 0.f;
        if (SLP::ReadSleepNeed(need) && need > 98.f) SLP::WriteSleepNeed(98.f);
    }
}

void OnReliable(const coop::net::SleepStatePayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (IsHost(s)) {
        // Host ingests Reports only; trust = the transport sender slot.
        if (p.op != 0) return;
        if (senderSlot == 0 || senderSlot >= coop::net::kMaxPeers) return;
        if (g_inBed[senderSlot] == (p.flag != 0)) return;
        g_inBed[senderSlot] = p.flag != 0;
        HostRetally(s);
        return;
    }
    // Client: phase broadcasts from the host only (transport-trusted slot 0).
    if (senderSlot != 0) return;
    switch (p.op) {
    case 1:  // Tally
        PushCounterLine(p.count, p.total);
        break;
    case 2:  // Accelerate
        ApplyAccelerateLocal(s, true);
        break;
    case 3:  // End
        ApplyEndLocal(s, p.flag != 0);
        break;
    default:
        break;
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !IsHost(s)) return;
    if (peerSlot <= 0 || peerSlot >= static_cast<int>(coop::net::kMaxPeers)) return;
    g_inBed[peerSlot] = false;  // a joiner arrives awake
    // A running phase cannot survive an awake arrival; a waiting tally just
    // re-counts (the joiner now holds the gate open until it sleeps too).
    HostRetally(s);
}

void OnDisconnectForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !IsHost(s)) return;
    if (slot <= 0 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    if (!g_inBed[slot]) {
        // An awake leaver may have been the one holding the gate open.
        HostRetally(s);
        return;
    }
    g_inBed[slot] = false;
    HostRetally(s);
}

void OnDisconnect() {
    auto* s = g_session.load(std::memory_order_acquire);
    // Restore the SP nightmare sentinel + normal time policy. A peer still in
    // bed keeps sleeping -- with the session gone its world is SP again and
    // the native 20x timelapse is exactly the stock behavior; time_sync's own
    // OnDisconnect restores the client TimeScale.
    if (g_dreamProbSuppressed) {
        SLP::SetDreamProbability(-1.f);
        g_dreamProbSuppressed = false;
    }
    if (g_accelerate && s && s->role() == coop::net::Role::Client)
        coop::time_sync::SetSleepAccelerate(false);
    g_accelerate = false;
    g_waitUndone = false;
    g_lastInBed = false;
    g_gmInst = nullptr;
    for (bool& b : g_inBed) b = false;
    g_lastCount = g_lastTotal = 0;
}

}  // namespace coop::sleep_sync
