// coop/desk_snd_fx.cpp -- see coop/interactables/desk_snd_fx.h.

#include "coop/interactables/desk_snd_fx.h"

#include "coop/config/config.h"
#include "coop/net/session.h"

#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_audio.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/ufunction_hook.h"

#include <atomic>
#include <chrono>
#include <cstring>

namespace coop::desk_snd_fx {
namespace {

namespace DA = ue_wrap::desk_audio;
using coop::net::DeskSndComp;
using coop::net::DeskSndFxPayload;
using coop::net::DeskSndOp;

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

// The wire-apply guard (GT-only; nested applies tolerated via a depth count).
int g_wireApplyDepth = 0;

bool g_hooksInstalled = false;

// ---- detour -> Tick handoff ring (GT-only: the Func-patch callback runs on
// the game thread, deep inside dispatch -- it must not lock or call the
// engine; Tick drains it the same frame). ----
struct RingEntry {
    uint8_t op;    // DeskSndOp
    uint8_t comp;  // DeskSndComp index
    char    cue[coop::net::kDeskSndCueCap];
};
constexpr int kRingCap = 16;
RingEntry g_ring[kRingCap];
int  g_ringN = 0;
bool g_ringOverflowWarned = false;

// fires/sec evidence counters (the qf R1 permanent-seam gate): total detour
// entries per hooked fn + desk-comp hits. Logged every 60 s when nonzero.
std::atomic<uint64_t> g_firesPlay{0}, g_firesSetActive{0}, g_firesActivate{0};
std::atomic<uint64_t> g_deskHits{0};
Clock::time_point g_nextCounterLog{};

// Loop bookkeeping. Indexed by (comp - kDeskSndFirstLoop): 0=corrds_loop,
// 1=pingLoop.
// g_wireLoopOn: THIS peer applied a wire ON (OnDisconnect cleanup owner).
// g_loopFrom (HOST only): the slot whose organic edge authored the current
// ON (leaver teardown owner). 0xFF = none.
bool    g_wireLoopOn[2] = {false, false};
uint8_t g_loopFrom[2] = {0xFF, 0xFF};
// Pending desired loop state while the desk is unresolved (join re-assert
// arriving before the joiner's desk resolves -- qf R3): -1 none, 0 off, 1 on.
int g_pendingLoop[2] = {-1, -1};

// Dev self-test ([dev] desk_snd_selftest=1): the host dispatches ONE organic
// playPingSound-equivalent ~5 s after the desk resolves; the client log must
// show the apply -- proves detour -> wire -> relay -> replay e2e in a smoke.
bool g_selfTestDone = false;
Clock::time_point g_selfTestDue{};

bool IsLoopComp(uint8_t comp) { return comp >= coop::net::kDeskSndFirstLoop &&
                                       comp < static_cast<uint8_t>(DeskSndComp::Count); }

void RingPush(uint8_t op, uint8_t comp, const char* cue) {
    if (g_ringN >= kRingCap) {
        if (!g_ringOverflowWarned) {
            g_ringOverflowWarned = true;
            UE_LOGW("desk_snd: fx ring overflow -- dropping (log-once)");
        }
        return;
    }
    RingEntry& e = g_ring[g_ringN++];
    e.op = op;
    e.comp = comp;
    e.cue[0] = '\0';
    if (cue) {
        const size_t n = std::strlen(cue);
        const size_t k = n < sizeof(e.cue) - 1 ? n : sizeof(e.cue) - 1;
        std::memcpy(e.cue, cue, k);
        e.cue[k] = '\0';
    }
}

// ---- the three Func-patch detours (GT, deep inside engine dispatch: flag
// check + <=6 pointer compares on the miss path; ring push on a desk hit;
// ZERO engine calls beyond the cached-offset reads in desk_audio). ----

// Armed only while the session runs AND is wired -- keeps SP / menu sounds
// from accumulating in the ring across a later session start (Tick sets,
// OnDisconnect clears). The counters keep counting regardless (evidence).
std::atomic<bool> g_armed{false};

void OnAudioPlay(void* context, void* /*sourceObject*/, void* /*result*/) {
    g_firesPlay.fetch_add(1, std::memory_order_relaxed);
    if (g_wireApplyDepth > 0 || !g_armed.load(std::memory_order_relaxed)) return;
    const int idx = DA::IndexOfComp(context);
    if (idx < 0) return;
    g_deskHits.fetch_add(1, std::memory_order_relaxed);
    char cue[coop::net::kDeskSndCueCap];
    if (!DA::ReadCueName(context, cue, sizeof(cue))) return;  // no cue -> nothing to mirror
    RingPush(static_cast<uint8_t>(DeskSndOp::Play), static_cast<uint8_t>(idx), cue);
}

void OnCompSetActiveLike(void* context) {
    if (g_wireApplyDepth > 0 || !g_armed.load(std::memory_order_relaxed)) return;
    const int idx = DA::IndexOfComp(context);
    if (idx < coop::net::kDeskSndFirstLoop) return;  // loops only (one-shots ride Play)
    g_deskHits.fetch_add(1, std::memory_order_relaxed);
    bool active = false;
    if (!DA::ReadLoopActive(idx, active)) return;  // post-state = the truth (bReset unreadable POST)
    RingPush(static_cast<uint8_t>(active ? DeskSndOp::LoopOn : DeskSndOp::LoopOff),
             static_cast<uint8_t>(idx), nullptr);
}

void OnCompSetActive(void* context, void* /*sourceObject*/, void* /*result*/) {
    g_firesSetActive.fetch_add(1, std::memory_order_relaxed);
    OnCompSetActiveLike(context);
}

void OnCompActivate(void* context, void* /*sourceObject*/, void* /*result*/) {
    g_firesActivate.fetch_add(1, std::memory_order_relaxed);
    OnCompSetActiveLike(context);
}

void SendFx(coop::net::Session* s, uint8_t op, uint8_t comp, const char* cue) {
    DeskSndFxPayload p{};
    p.op = op;
    p.comp = comp;
    if (cue && cue[0]) {
        const size_t n = std::strlen(cue);
        if (n >= sizeof(p.cue)) return;  // never truncate a wire identity
        std::memcpy(p.cue, cue, n);
        p.cueLen = static_cast<uint8_t>(n);
    }
    if (s->role() == coop::net::Role::Host) {
        s->SendReliable(coop::net::ReliableKind::DeskSndFx, &p, sizeof(p));
        // Host-authored loop edges update the attribution map directly.
        if (IsLoopComp(comp))
            g_loopFrom[comp - coop::net::kDeskSndFirstLoop] =
                (op == static_cast<uint8_t>(DeskSndOp::LoopOn)) ? 0 : 0xFF;
    } else {
        s->SendReliableToSlot(0, coop::net::ReliableKind::DeskSndFx, &p, sizeof(p));
    }
}

// Apply ONE fx locally (wire-caused: guard held by the caller).
bool ApplyFx(uint8_t op, uint8_t comp, const char* cue) {
    switch (static_cast<DeskSndOp>(op)) {
    case DeskSndOp::Play:
        return DA::ReplayPlay(comp, cue);
    case DeskSndOp::LoopOn:
    case DeskSndOp::LoopOff: {
        const bool on = (static_cast<DeskSndOp>(op) == DeskSndOp::LoopOn);
        if (!DA::ReplaySetActive(comp, on)) return false;
        if (IsLoopComp(comp)) g_wireLoopOn[comp - coop::net::kDeskSndFirstLoop] = on;
        return true;
    }
    default:
        return false;
    }
}

}  // namespace

bool InWireApply() { return g_wireApplyDepth > 0; }
ScopedWireApply::ScopedWireApply()  { ++g_wireApplyDepth; }
ScopedWireApply::~ScopedWireApply() { --g_wireApplyDepth; }

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) { g_armed.store(false, std::memory_order_relaxed); return; }
    g_armed.store(s->connected(), std::memory_order_relaxed);

    // Lazy latched hook install: the target UFunctions are ENGINE-class level
    // (resolvable pre-world); desk_audio backoff-throttles the retries.
    if (!g_hooksInstalled) {
        if (!DA::EnsureResolved()) return;
        const bool a = ue_wrap::ufunction_hook::InstallPostHook(DA::PlayFn(), &OnAudioPlay);
        const bool b = ue_wrap::ufunction_hook::InstallPostHook(DA::SetActiveFn(), &OnCompSetActive);
        const bool c = ue_wrap::ufunction_hook::InstallPostHook(DA::ActivateFn(), &OnCompActivate);
        g_hooksInstalled = true;  // latch (InstallPostHook is idempotent + process-lifetime)
        UE_LOGI("desk_snd: audio seams installed (Play=%d SetActive=%d Activate=%d)",
                a ? 1 : 0, b ? 1 : 0, c ? 1 : 0);
    }

    // Dev self-test arming (per-session: OnDisconnect clears the latch).
    // Armed from the CONNECTED edge + 20 s -- a joiner's desk resolves ~10 s
    // after its transport connects (measured smoke 2026-07-17: fx at +5 s was
    // dropped at the client's still-unresolved desk), so the probe must
    // outwait the peer's world load to be a valid e2e proof.
    static const bool s_selftest = coop::config::IsIniKeyTrue("desk_snd_selftest");
    if (s_selftest && !g_selfTestDone && g_selfTestDue == Clock::time_point{} &&
        s->role() == coop::net::Role::Host && s->connected())
        g_selfTestDue = Clock::now() + std::chrono::seconds(20);

    // Keep the comp cache fresh (throttled inside; never on the detour path).
    DA::EnsureResolved();

    // Flush the detour ring -> wire.
    if (g_ringN > 0 && s->connected()) {
        for (int i = 0; i < g_ringN; ++i)
            SendFx(s, g_ring[i].op, g_ring[i].comp, g_ring[i].cue);
        g_ringN = 0;
    } else if (g_ringN > 0) {
        g_ringN = 0;  // unwired: local SP sounds need no mirror; drop
    }

    // Pending loop retry (join re-assert landed before the desk resolved).
    for (int i = 0; i < 2; ++i) {
        if (g_pendingLoop[i] < 0) continue;
        ScopedWireApply guard;
        if (ApplyFx(static_cast<uint8_t>(g_pendingLoop[i] ? DeskSndOp::LoopOn : DeskSndOp::LoopOff),
                    static_cast<uint8_t>(coop::net::kDeskSndFirstLoop + i), nullptr)) {
            UE_LOGI("desk_snd: pending loop %d applied (%s)", i, g_pendingLoop[i] ? "on" : "off");
            g_pendingLoop[i] = -1;
        }
    }

    // Dev self-test: ONE organic-path dispatch on the host (unguarded ->
    // the detour forwards it like a real press). Re-arms while the desk is
    // still unresolved (pre-world) instead of latching a false FAILED.
    if (!g_selfTestDone && g_selfTestDue != Clock::time_point{} &&
        Clock::now() >= g_selfTestDue && s->connected()) {
        if (!DA::EnsureResolved()) {
            g_selfTestDue = Clock::now() + std::chrono::seconds(5);
        } else {
            g_selfTestDone = true;
            const bool ok = DA::ReplayPlay(static_cast<int>(DeskSndComp::PingSound), "newdesk_beep4");
            UE_LOGI("desk_snd: SELFTEST organic dispatch %s (expect a client-side apply line)",
                    ok ? "fired" : "FAILED");
        }
    }

    // fires/sec evidence (qf R1 gate): 60 s cadence, only when nonzero.
    const auto now = Clock::now();
    if (g_nextCounterLog == Clock::time_point{}) g_nextCounterLog = now + std::chrono::seconds(60);
    if (now >= g_nextCounterLog) {
        g_nextCounterLog = now + std::chrono::seconds(60);
        const uint64_t p = g_firesPlay.exchange(0, std::memory_order_relaxed);
        const uint64_t sa = g_firesSetActive.exchange(0, std::memory_order_relaxed);
        const uint64_t ac = g_firesActivate.exchange(0, std::memory_order_relaxed);
        const uint64_t dh = g_deskHits.exchange(0, std::memory_order_relaxed);
        if (p || sa || ac)
            UE_LOGI("desk_snd: seam counters /60s: Play=%llu SetActive=%llu Activate=%llu deskHits=%llu",
                    (unsigned long long)p, (unsigned long long)sa,
                    (unsigned long long)ac, (unsigned long long)dh);
    }
}

void OnDeskSndFx(const DeskSndFxPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (p.comp >= static_cast<uint8_t>(DeskSndComp::Count)) return;
    if (p.op > static_cast<uint8_t>(DeskSndOp::LoopOff)) return;
    if (p.cueLen >= sizeof(p.cue)) return;
    char cue[coop::net::kDeskSndCueCap];
    std::memcpy(cue, p.cue, p.cueLen);
    cue[p.cueLen] = '\0';
    if (static_cast<DeskSndOp>(p.op) == DeskSndOp::Play && !cue[0]) return;

    const bool isLoop = IsLoopComp(p.comp);
    if (!DA::EnsureResolved()) {
        if (isLoop) {  // loops are STATE -- park + retry (qf R3); one-shots are missable
            g_pendingLoop[p.comp - coop::net::kDeskSndFirstLoop] =
                (static_cast<DeskSndOp>(p.op) == DeskSndOp::LoopOn) ? 1 : 0;
        }
        return;
    }

    {
        ScopedWireApply guard;
        if (!ApplyFx(p.op, p.comp, cue)) {
            UE_LOGW("desk_snd: apply failed op=%u comp=%u cue='%s' (from slot %u)",
                    p.op, p.comp, cue, senderSlot);
            return;
        }
    }
    if (isLoop) {
        g_pendingLoop[p.comp - coop::net::kDeskSndFirstLoop] = -1;  // wire truth supersedes
        if (s->role() == coop::net::Role::Host)
            g_loopFrom[p.comp - coop::net::kDeskSndFirstLoop] =
                (static_cast<DeskSndOp>(p.op) == DeskSndOp::LoopOn) ? senderSlot : 0xFF;
    }
    static uint64_t s_n = 0;
    if ((++s_n % 32) == 1)  // 1-in-32 proof-of-life, not per-click spam
        UE_LOGI("desk_snd: applied op=%u comp=%u cue='%s' from slot %u (n=%llu)",
                p.op, p.comp, cue, senderSlot, (unsigned long long)s_n);
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (!DA::EnsureResolved()) return;  // no desk -> no loop state to re-assert
    for (int i = 0; i < 2; ++i) {
        bool on = false;
        if (!DA::ReadLoopActive(coop::net::kDeskSndFirstLoop + i, on) || !on) continue;
        // Component ground truth (covers host-pressed AND client-pressed loops
        // alike -- qf R5); send as a normal LoopOn to just this joiner.
        DeskSndFxPayload p{};
        p.op = static_cast<uint8_t>(DeskSndOp::LoopOn);
        p.comp = static_cast<uint8_t>(coop::net::kDeskSndFirstLoop + i);
        s->SendReliableToSlot(slot, coop::net::ReliableKind::DeskSndFx, &p, sizeof(p));
        UE_LOGI("desk_snd: join re-assert loop %d ON -> slot %d", i, slot);
    }
}

void OnPeerLeft(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    for (int i = 0; i < 2; ++i) {
        if (g_loopFrom[i] != static_cast<uint8_t>(slot)) continue;
        g_loopFrom[i] = 0xFF;
        // HOST-OWNED leaver teardown (qf R3): broadcast the OFF + apply locally.
        DeskSndFxPayload p{};
        p.op = static_cast<uint8_t>(DeskSndOp::LoopOff);
        p.comp = static_cast<uint8_t>(coop::net::kDeskSndFirstLoop + i);
        s->SendReliable(coop::net::ReliableKind::DeskSndFx, &p, sizeof(p));
        if (DA::EnsureResolved()) {
            ScopedWireApply guard;
            ApplyFx(p.op, p.comp, nullptr);
        }
        UE_LOGI("desk_snd: leaver slot %d owned loop %d -- OFF broadcast + applied", slot, i);
    }
}

void OnDisconnect() {
    // Suppression=loan discipline: wire-set loops must not outlive the session.
    if (DA::EnsureResolved()) {
        for (int i = 0; i < 2; ++i) {
            if (!g_wireLoopOn[i]) continue;
            ScopedWireApply guard;
            DA::ReplaySetActive(coop::net::kDeskSndFirstLoop + i, false);
            UE_LOGI("desk_snd: session end -- wire loop %d forced off", i);
        }
    }
    g_wireLoopOn[0] = g_wireLoopOn[1] = false;
    g_loopFrom[0] = g_loopFrom[1] = 0xFF;
    g_pendingLoop[0] = g_pendingLoop[1] = -1;
    g_ringN = 0;
    g_ringOverflowWarned = false;
    g_selfTestDone = false;
    g_selfTestDue = {};
    g_armed.store(false, std::memory_order_relaxed);
    // Hooks are process-lifetime (transparent forwarders); the detours go
    // quiet on running()==false via the session gate in Tick + the wire gate
    // here is unnecessary: with no session, SendFx never runs (ring dropped).
    ue_wrap::desk_audio::ResetCache();
}

}  // namespace coop::desk_snd_fx
