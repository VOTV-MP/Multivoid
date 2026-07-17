// coop/deck_play_sync.cpp -- see coop/interactables/deck_play_sync.h.

#include "coop/interactables/deck_play_sync.h"

#include "coop/config/config.h"
#include "coop/interactables/desk_input_sync.h"
#include "coop/interactables/desk_snd_fx.h"
#include "coop/net/session.h"

#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_audio.h"
#include "ue_wrap/desk/saved_signals.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/ufunction_hook.h"

#include <atomic>
#include <chrono>

namespace coop::deck_play_sync {
namespace {

namespace CD = ue_wrap::console_desk;
namespace DA = ue_wrap::desk_audio;

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

bool g_hooksInstalled = false;

// Armed only while the session runs AND is wired (the desk_snd_fx shape) --
// SP / menu playback never rings.
std::atomic<bool> g_armed{false};

// The fin() PE bracket depth: >0 while the desk's OnAudioFinished delegate
// callback is executing on this peer (its synchronous stopSound -> Deactivate
// is the NATURAL end -- swallowed, never broadcast). Atomic per the
// "sometimes a task-graph worker" PE-detour note (residual d).
std::atomic<int> g_finDepth{0};

// GEN GUARD state (GT-only writes: detour + Tick + apply all run on GT).
// g_seenGen  = the highest generation observed (own mints included).
// g_appliedGen = the gen of the playback CURRENTLY live on this peer
//                (0 = none). Set by an organic play mint or a wire play
//                apply; cleared by any stop (wire, organic, or fin).
uint32_t g_seenGen = 0;
uint32_t g_appliedGen = 0;

// detour -> Tick handoff ring (the desk_snd_fx contract: the Func-patch
// callback runs deep inside dispatch -- no locks, no engine calls, no sends).
struct RingEntry {
    uint8_t op;           // 0=play 1=stop
    int32_t selectIndex;  // play only
};
constexpr int kRingCap = 8;
RingEntry g_ring[kRingCap];
int  g_ringN = 0;
bool g_ringOverflowWarned = false;

// Seam-fire evidence (the probe-first instrument, qf R5/R6): logged every
// 60 s when nonzero -- the smoke reads the ambient Deactivate rate; the
// take reads the real-world one. Same always-on shape as the shipped v115
// counters (one line/min max; NOT the closed-measurement diag-battery class).
std::atomic<uint64_t> g_firesDeactivate{0};
std::atomic<uint64_t> g_sigHits{0};
Clock::time_point g_nextCounterLog{};

// Dev self-test ([dev] deck_selftest=1): host-only reflected organic
// Activate -> (2 s later) Deactivate on signalSound. Proves patch fire /
// routing match / organic classification / wire crossing / gen mint; the
// client's gate-precheck WARN and stale-gen drop lines are the expected
// receiver-side evidence on a fresh smoke save (empty deck list).
int g_selfTestStage = 0;  // 0=idle 1=armed-activate 2=armed-deactivate 3=done
Clock::time_point g_selfTestDue{};

// ---- the two seam detours (GT, deep inside engine dispatch) ----

void OnDeckActivate(void* context, void* /*sourceObject*/, void* /*result*/) {
    // (Total Activate fires are already counted by the v115 desk_snd counter.)
    if (coop::desk_snd_fx::InWireApply() || !g_armed.load(std::memory_order_relaxed)) return;
    if (!DA::IsSignalSound(context)) return;
    g_sigHits.fetch_add(1, std::memory_order_relaxed);
    int32_t idx = -1;
    CD::Scalars sc;
    if (CD::ReadScalars(sc)) idx = sc.playSelectIndex;  // post-validation read (mid-playSignal)
    if (g_ringN >= kRingCap) {
        if (!g_ringOverflowWarned) {
            g_ringOverflowWarned = true;
            UE_LOGW("deck_play: ring overflow -- dropping (log-once)");
        }
        return;
    }
    g_ring[g_ringN++] = {0, idx};
}

void OnDeckDeactivate(void* context, void* /*sourceObject*/, void* /*result*/) {
    g_firesDeactivate.fetch_add(1, std::memory_order_relaxed);
    if (coop::desk_snd_fx::InWireApply() || !g_armed.load(std::memory_order_relaxed)) return;
    if (!DA::IsSignalSound(context)) return;
    g_sigHits.fetch_add(1, std::memory_order_relaxed);
    if (g_finDepth.load(std::memory_order_relaxed) > 0) {
        // Natural track end: every peer's own copy fin-stops itself --
        // swallow, but the local playback session IS over (a later organic
        // no-playback stopSound must not broadcast a stale gen).
        g_appliedGen = 0;
        return;
    }
    if (g_appliedGen == 0) return;  // stopSound with nothing playing (power
                                    // toggle / import with idle deck) -- no
                                    // session to terminate, nothing to send
    if (g_ringN >= kRingCap) {
        if (!g_ringOverflowWarned) {
            g_ringOverflowWarned = true;
            UE_LOGW("deck_play: ring overflow -- dropping (log-once)");
        }
        return;
    }
    g_ring[g_ringN++] = {1, -1};
}

// fin() PE bracket (RegisterPre/PostObserver -- the delegate-only dispatch
// census makes fin the single natural-end door; if fin proves EX-dispatched
// these never fire and the gen guard carries correctness alone).
void OnFinPre(void* /*self*/, void* /*function*/, void* /*params*/) {
    g_finDepth.fetch_add(1, std::memory_order_relaxed);
}
void OnFinPost(void* /*self*/, void* /*function*/, void* /*params*/) {
    g_finDepth.fetch_sub(1, std::memory_order_relaxed);
}

void SendEdge(coop::net::Session* s, uint8_t op, int32_t idx, uint32_t gen) {
    coop::net::PlayDeckEventPayload p{};
    p.op = op;
    p.selectIndex = idx;
    p.gen = gen;
    if (s->role() == coop::net::Role::Host)
        s->SendReliable(coop::net::ReliableKind::PlayDeckEvent, &p, sizeof(p));
    else
        s->SendReliableToSlot(0, coop::net::ReliableKind::PlayDeckEvent, &p, sizeof(p));
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) { g_armed.store(false, std::memory_order_relaxed); return; }
    g_armed.store(s->connected(), std::memory_order_relaxed);

    // Lazy latched seam install: DA gives the engine-class UFunctions +
    // the signalSound cache; fin resolves with the desk class (console_desk).
    if (!g_hooksInstalled) {
        if (!DA::EnsureResolved()) return;
        void* finFn = CD::DeckFinFn();
        if (!finFn) {
            // Dead-guard discipline (perf-audit WARN): a game recook renaming
            // fin() would leave the whole lane silently dormant -- surface it
            // once instead of early-returning forever without a trace.
            static bool s_finWarned = false;
            if (!s_finWarned && CD::Instance()) {  // desk resolved but fin absent
                s_finWarned = true;
                UE_LOGW("deck_play: desk resolved but fin() unresolved -- lane dormant "
                        "until it appears (recook rename? log-once)");
            }
            return;  // console_desk ResolvePass retries (throttled)
        }
        const bool a = ue_wrap::ufunction_hook::InstallPostHook(DA::ActivateFn(), &OnDeckActivate);
        const bool d = ue_wrap::ufunction_hook::InstallPostHook(DA::DeactivateFn(), &OnDeckDeactivate);
        const bool fp = ue_wrap::game_thread::RegisterPreObserver(finFn, &OnFinPre);
        const bool fq = ue_wrap::game_thread::RegisterPostObserver(finFn, &OnFinPost);
        g_hooksInstalled = true;  // latch (installs are idempotent + process-lifetime)
        UE_LOGI("deck_play: seams installed (Activate=%d Deactivate=%d finPre=%d finPost=%d)",
                a ? 1 : 0, d ? 1 : 0, fp ? 1 : 0, fq ? 1 : 0);
    }

    DA::EnsureResolved();  // keep the comp cache fresh (throttled inside)

    // Flush the detour ring -> wire (the author side of the gen guard).
    if (g_ringN > 0 && s->connected()) {
        for (int i = 0; i < g_ringN; ++i) {
            const RingEntry& e = g_ring[i];
            if (e.op == 0) {
                const uint32_t gen = ++g_seenGen;
                g_appliedGen = gen;
                SendEdge(s, 0, e.selectIndex, gen);
                UE_LOGI("deck_play: organic play idx=%d gen=%u", e.selectIndex, gen);
            } else {
                // g_appliedGen was nonzero at detour time; it may have been
                // cleared by a same-frame fin -- re-check before authoring.
                if (g_appliedGen == 0) continue;
                const uint32_t gen = g_appliedGen;
                g_appliedGen = 0;
                SendEdge(s, 1, -1, gen);
                UE_LOGI("deck_play: organic stop gen=%u", gen);
            }
        }
        g_ringN = 0;
    } else if (g_ringN > 0) {
        g_ringN = 0;  // unwired: local-only playback needs no mirror
    }

    // Dev self-test (host, once per session, 25 s past connect -- outwaits
    // the peer's world load, the desk_snd measured bound).
    static const bool s_selftest = coop::config::IsIniKeyTrue("deck_selftest");
    if (s_selftest && s->role() == coop::net::Role::Host && s->connected()) {
        const auto now = Clock::now();
        if (g_selfTestStage == 0) {
            g_selfTestStage = 1;
            g_selfTestDue = now + std::chrono::seconds(25);
        } else if (g_selfTestStage == 1 && now >= g_selfTestDue) {
            if (!DA::EnsureResolved()) {
                g_selfTestDue = now + std::chrono::seconds(5);  // desk not up yet -- re-arm
            } else {
                const bool ok = DA::SelfTestSignalSound(true);
                UE_LOGI("deck_play: SELFTEST activate %s (expect organic play + client gate-WARN)",
                        ok ? "fired" : "FAILED");
                g_selfTestStage = 2;
                g_selfTestDue = now + std::chrono::seconds(2);
            }
        } else if (g_selfTestStage == 2 && now >= g_selfTestDue) {
            const bool ok = DA::SelfTestSignalSound(false);
            UE_LOGI("deck_play: SELFTEST deactivate %s (expect organic stop + client stale-drop)",
                    ok ? "fired" : "FAILED");
            g_selfTestStage = 3;
        }
    }

    // Seam-fire evidence, 60 s cadence when nonzero (the v115 counter shape).
    const auto now = Clock::now();
    if (g_nextCounterLog == Clock::time_point{}) g_nextCounterLog = now + std::chrono::seconds(60);
    if (now >= g_nextCounterLog) {
        g_nextCounterLog = now + std::chrono::seconds(60);
        const uint64_t de = g_firesDeactivate.exchange(0, std::memory_order_relaxed);
        const uint64_t sh = g_sigHits.exchange(0, std::memory_order_relaxed);
        if (de || sh)
            UE_LOGI("deck_play: seam counters /60s: Deactivate=%llu sigHits=%llu",
                    (unsigned long long)de, (unsigned long long)sh);
    }
}

void OnPlayDeck(const coop::net::PlayDeckEventPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (p.op > 1) {
        UE_LOGW("deck_play: op=%u out of range (from slot %u) -- dropping", p.op, senderSlot);
        return;
    }
    if (g_seenGen < p.gen) g_seenGen = p.gen;  // max-merge: our next mint stays above

    if (p.op == 0) {  // play
        if (p.selectIndex < 0) {
            UE_LOGW("deck_play: play idx=%d invalid (from slot %u) -- dropping",
                    p.selectIndex, senderSlot);
            return;
        }
        if (!CD::EnsureResolved() || !CD::Instance()) {
            UE_LOGW("deck_play: play gen=%u dropped -- desk unresolved (one silent track)", p.gen);
            return;
        }
        CD::Scalars sc;
        if (!CD::ReadScalars(sc)) return;
        const int32_t count = ue_wrap::saved_signals::Count();
        if (!sc.activePlay || count <= p.selectIndex) {
            // Divergence-capable gates only (activePlay lag / append in
            // flight); the decoded>=size gate cannot diverge (rows are
            // byte-identical). Skip = one silent track, self-heals -- and
            // never lets the mirror's native deny BEEP fire falsely.
            UE_LOGW("deck_play: play idx=%d gen=%u gates failed (activePlay=%d count=%d) "
                    "-- skipped (self-heals at the next press)",
                    p.selectIndex, p.gen, sc.activePlay ? 1 : 0, count);
            return;
        }
        // selectIndex rides through the v112 apply author: guarded field
        // write + echo-prime (the scroll-then-play race close, qf R3-Q4).
        coop::net::DeskInputPayload ip{};
        ip.field = static_cast<uint8_t>(coop::net::DeskInputField::PlaySelectIndex);
        ip.intVal = p.selectIndex;
        coop::desk_input_sync::OnDeskInput(ip, senderSlot);
        {
            coop::desk_snd_fx::ScopedWireApply guard;
            if (!CD::CallDeckPlaySignal()) {
                UE_LOGW("deck_play: playSignal replay failed (gen=%u)", p.gen);
                return;
            }
        }
        g_appliedGen = p.gen;
        static uint64_t s_n = 0;
        UE_LOGI("deck_play: applied play idx=%d gen=%u from slot %u (n=%llu)",
                p.selectIndex, p.gen, senderSlot, (unsigned long long)(++s_n));
        return;
    }

    // stop: the gen guard -- only the CURRENT playback's stop applies.
    if (g_appliedGen == 0 || p.gen != g_appliedGen) {
        UE_LOGI("deck_play: stale stop gen=%u dropped (applied=%u, from slot %u)",
                p.gen, g_appliedGen, senderSlot);
        return;
    }
    if (!CD::EnsureResolved() || !CD::Instance()) return;
    {
        coop::desk_snd_fx::ScopedWireApply guard;
        CD::CallDeckStopSound();
    }
    g_appliedGen = 0;
    UE_LOGI("deck_play: applied stop gen=%u from slot %u", p.gen, senderSlot);
}

void OnDisconnect() {
    g_seenGen = 0;
    g_appliedGen = 0;
    g_ringN = 0;
    g_ringOverflowWarned = false;
    g_finDepth.store(0, std::memory_order_relaxed);  // paranoia: pre/post pair synchronously
    g_selfTestStage = 0;
    g_selfTestDue = {};
    g_armed.store(false, std::memory_order_relaxed);
    // Seam hooks are process-lifetime (transparent forwarders); the armed
    // gate silences them outside a session.
}

}  // namespace coop::deck_play_sync
