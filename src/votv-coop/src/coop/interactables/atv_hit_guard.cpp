// coop/interactables/atv_hit_guard.cpp -- see header. Body MOVED VERBATIM from atv_sync.cpp on
// 2026-08-30; equivalence proven by the body-diff instrument against a frozen pre-cut copy, with
// mutants shown to fail it.

#include "coop/interactables/atv_hit_guard.h"

#include "coop/config/config.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"

#include <atomic>
#include <cstdint>

// InstallHitGuard and PublishOwned sit at namespace scope because they ARE the definitions the
// header declares -- wrapping them would mean renaming inside moved text or a forwarder that
// recurses. Everything the header does NOT name is in the anonymous namespace below: an audit
// (2026-08-30) pointed out that "confined by coop::atv_hit_guard" is not confinement at all --
// those six symbols had external linkage, so the state this cut was meant to un-share by
// proximity was merely re-shared by linkage.
namespace coop::atv_hit_guard {

namespace A = ue_wrap::atv;
namespace R = ue_wrap::reflection;

// THE TICK IS NOT THE BRAIN -- MEASURED 2026-08-29, and it retired a pillar of this design.
//
// The plan was "brains OFF, physics ON": SetActorTickEnabled(false) on every non-owner, so the
// accumulators and the wheel torque ran on one machine. The first two-peer run refuted it. Both
// peers start at a byte-identical pose; 100 s later, with the mirror's tick off, they were
// 42.7 cm apart and the mirror sat 37 cm LOWER, its suspension hanging 2.4 cm further out.
//
// The bytecode says why. ATV_C's tick (ReceiveTick -> ExecuteUbergraph_ATV @32947) reaches
// `@29894: mesh.SetCenterOfMass(VLerp(..., tirescount/4))` UNCONDITIONALLY, every frame, before
// any gate -- the centre of mass is rig CONFIGURATION, re-applied per tick, not gameplay logic. A
// rig whose centre of mass is never set rests, rolls and settles somewhere else. Meanwhile the
// things tick-off was supposed to stop are ALREADY single-peer by the game's own gating:
// `@29949: IFNOT(isDriven) POP` guards applyWheelTorque, and every battery-drain term at
// @33970-@34123 is SelectFloat(x, 0, isDriven|isDrive|lights|turbo) -- all local-only state that
// is false on a machine where nobody is sitting in it.
//
// So tick-off prevented nothing and moved the vehicle. It is gone (RULE 2), along with
// ue_wrap::atv::SetBrainEnabled, which now has no caller. `ownsTick` SURVIVES and keeps both of
// its real jobs: electing the idle syncer, and telling the collision interceptor whose copy may
// author damage. The interceptor is now the ONLY thing that makes a mirror differ from a native
// ATV -- and it has to be, because a ComponentHit delegate is dispatched by the physics scene and
// was never tick-gated in the first place.

// ---- the COLLISION half of "brains off" ---------------------------------------------------
// SetActorTickEnabled does not reach a ComponentHit delegate: it is dispatched by the physics
// scene. `[V]` all seven of ATV_C's reach real authored state -- impulse() subtracts
// |NormalImpulse|/500000*2*getBumperMult() from `health` and calls explode() at <=0, processTire()
// burns tire durability and ejectWheel()s at 0, and the Capsule one pops a lib_C::addHint at the
// local player. With the rig now simulating on every peer, a non-owner running them would blow up
// a vehicle whose authority still has it. So they are cancelled PRE-dispatch on a non-owner.
//
// ...BUT NOT ALL SEVEN, AND THE CENSUS ABOVE IS WHY THAT WAS MISSED -- measured 2026-08-30.
// That census is a list of what each handler AUTHORS. It is accurate and it is not the whole
// story: the five WHEEL delegates ALSO maintain the rig's own shape. Cancelling them was a broad
// suppression of a notification that carries two unrelated things, and it took the second one
// with it (principle 4: patch the site, never the class of call).
//
// THE MEASUREMENT, four smoke runs differing in ONE variable each, host authoring and client
// mirroring one parked ATV (research/atv_runs/20260830-1057..1105):
//
//   corrector | this guard | A2 settled gap | the two rigs' SHAPES differ by
//   ----------+------------+----------------+-------------------------------
//   ON        | all seven  | 25-40 cm FAIL  | ~40 cm      (six runs, every driven run ever)
//   OFF       | all seven  | 30.4 cm  FAIL  | 19.05 cm
//   ON        | none       |  3.0 cm  PASS  |  0.61 cm
//   OFF       | none       |  5.3 cm  PASS  |  0.12 cm
//   ON        | BODY ONLY  | 13.6 cm  PASS  |  0.14 cm    <- shipped, and the run DROVE it
//
// The pose corrector is innocent: with this guard off it produces the BEST cell of the four. The
// defect the whole ATV arc has been chasing since 2026-08-29 -- "a mirrored ATV rests 25-40 cm
// low", blamed in turn on tick-off, on the terrain differing under the vehicle, on kCorrGain, on
// a velocity write waking a settled body -- was this line, all along. The quantity that makes it
// visible is RIDE HEIGHT (the body's Z above the mean of its own three rig bodies); susFR/FL/BK
// could never show it, being 3-D distances over a ~92 cm mostly-horizontal arm, so a 40 cm
// vertical deformation moves them ~1.1 cm and reads as normal suspension travel.
//
// WHAT THIS COSTS, stated rather than hidden: a mirror now runs processTire(), so it burns its
// own tire durability and can ejectWheel() a tire its author still has. That is a real divergence
// and it is narrower than the one it replaces -- but the RIGHT fix for it is not to re-suppress
// the handler, it is to put tire durability on the wire under the author, the way `health`
// already is. Filed in docs/vehicles/ATV.md 17.
//
// The predicate cannot read g_atvs: the interceptor contract does not promise the game thread.
// Instead Tick publishes the small set of ATVs THIS peer owns the tick for into an atomic array,
// and the callback is a pointer scan over it. DEFAULT IS CANCEL, which is the safe direction --
// a stale/empty set costs the authority some collision damage (a loss, and a quiet one), while
// the opposite fails toward a mirror destroying itself.
// Sized for "every ATV a host can own at once", not for the common case: on a host with nobody
// driving, EVERY ATV is owned, and `list_props` row 'atv' means the spawn menu can make more of
// them (docs/vehicles/ATV.md 11.4). Overflowing is not a graceful degradation -- the guard's
// CANCEL default would suppress collisions on the one machine that owns the ATV -- so it also
// logs once. 8 bytes a slot.
namespace {

std::atomic<void*> g_ownedAtvs[kMaxOwned];
std::atomic<bool>  g_guardActive{false};   // false in single-player: the game must keep its damage
std::atomic<unsigned long long> g_hitCancelled{0};
std::atomic<unsigned long long> g_hitAllowed{0};
std::atomic<bool>  g_hitGuardArmed{false};              // all 7 delegates registered -- else the lane runs INERT
// WHICH delegates a non-owner is denied. Read ONCE at install (the interceptor callback runs on
// whatever thread ProcessEvent is on, and a config read is not something to do per hit at 200 Hz
// -- run 2 logged 17,638 cancels in 90 s on ONE parked vehicle).
std::atomic<unsigned> g_cancelMask{0x7F};

}  // namespace

void PublishOwned(void** owned, int n) {
    for (int i = 0; i < kMaxOwned; ++i)
        g_ownedAtvs[i].store(i < n ? owned[i] : nullptr, std::memory_order_release);
}

namespace {

// One callback per delegate INDEX, so the guard can deny a subset. The index has to come from
// somewhere: RegisterInterceptor hands the callback `self` and the params frame but not which
// UFunction fired, so a single shared callback cannot tell the body's collision from a wheel's --
// which is exactly the distinction that turned out to matter. A template instantiates seven
// distinct function pointers around one body rather than seven copies of it.
bool CancelHit(void* self, unsigned bit) {
    if (!g_guardActive.load(std::memory_order_acquire)) return false;  // not in a session: never suppress
    for (int i = 0; i < kMaxOwned; ++i) {
        void* p = g_ownedAtvs[i].load(std::memory_order_acquire);
        if (!p) break;
        if (p == self) { g_hitAllowed.fetch_add(1, std::memory_order_relaxed); return false; }
    }
    if (!(g_cancelMask.load(std::memory_order_relaxed) & bit)) {
        // Deliberately NOT counted as "allowed": that counter means "this peer owns the rig".
        // Folding a mask-permitted hit into it would make the two arms of the measurement
        // indistinguishable in the one line anybody reads.
        return false;
    }
    g_hitCancelled.fetch_add(1, std::memory_order_relaxed);
    return true;   // cancel-on-true: the BndEvt stub never jumps into the ubergraph
}

template <unsigned Bit>
bool OnAtvHitPre(void* self, void* /*params*/) { return CancelHit(self, Bit); }

}  // namespace

void InstallHitGuard() {
    if (g_hitGuardArmed.load(std::memory_order_relaxed)) return;
    void* fns[8] = {};
    const int n = A::ResolveHitDelegates(fns, 8);
    const unsigned mask = static_cast<unsigned>(
        ::coop::config::ResolveInt(::coop::config_registry::rows::atv_hit_guard_mask)) & 0x7Fu;
    g_cancelMask.store(mask, std::memory_order_relaxed);
    using Cb = bool (*)(void*, void*);
    // Index order is kHitDelegateNames order, and ResolveHitDelegates walks that same array, so
    // fns[i] and kCallbacks[i] name the same delegate by construction rather than by agreement.
    static const Cb kCallbacks[7] = {
        &OnAtvHitPre<1u << 0>, &OnAtvHitPre<1u << 1>, &OnAtvHitPre<1u << 2>, &OnAtvHitPre<1u << 3>,
        &OnAtvHitPre<1u << 4>, &OnAtvHitPre<1u << 5>, &OnAtvHitPre<1u << 6>,
    };
    int ok = 0;
    for (int i = 0; i < n && i < 7; ++i)
        if (ue_wrap::game_thread::RegisterInterceptor(fns[i], kCallbacks[i])) ++ok;
    if (ok == 7) {
        g_hitGuardArmed.store(true, std::memory_order_relaxed);
        UE_LOGI("atv: hit guard armed -- 7/7 ComponentHit delegates intercepted, cancel mask 0x%02X "
                "(a non-owner cannot author damage/explode/ejectWheel on the masked ones)", mask);
    } else {
        // FAIL CLOSED. Without all seven we will not run the simulate-and-correct model at all:
        // Tick leaves every ATV's brain ON and mirrors nothing, so peers diverge visibly rather
        // than one of them silently destroying a vehicle the other still has.
        UE_LOGE("atv: hit guard NOT armed (%d/7 resolved, %d/7 registered) -- ATV sync stays INERT "
                "this session; the interceptor table may be full (kMaxInterceptors)", n, ok);
    }
}

void SetActive(bool active) { g_guardActive.store(active, std::memory_order_release); }

bool Armed() { return g_hitGuardArmed.load(std::memory_order_relaxed); }

bool Owns(void* actor) {
    if (!actor) return false;
    if (!g_guardActive.load(std::memory_order_acquire)) return false;
    for (int i = 0; i < kMaxOwned; ++i) {
        void* p = g_ownedAtvs[i].load(std::memory_order_acquire);
        if (!p) return false;
        if (p == actor) return true;
    }
    return false;
}

Counters ReadCounters() {
    return Counters{ g_hitCancelled.load(), g_hitAllowed.load(),
                     g_hitGuardArmed.load(std::memory_order_relaxed) };
}

}  // namespace coop::atv_hit_guard
