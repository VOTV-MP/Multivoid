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
std::atomic<unsigned long long> g_hitNeutered{0};
std::atomic<unsigned long long> g_hitAllowed{0};
std::atomic<bool>  g_hitGuardArmed{false};              // all 7 delegates registered -- else the lane runs INERT
// Byte offset of `NormalImpulse` inside each delegate's params frame, resolved ONCE at install by
// reflection (never assumed, and never derived from another delegate's: they are seven distinct
// UFunction objects even though they share one signature). -1 = unresolved, which is why the
// counter below exists.
int32_t g_impulseOff[7] = {-1, -1, -1, -1, -1, -1, -1};
// A hit we could NOT neuter because its offset never resolved. Should be permanently 0: the
// install refuses to arm without all seven. It is counted anyway because the alternative is a
// silent fall-through to "let the mirror author damage", which is the defect this lane exists to
// stop, and an unobservable hole is the exact failure the 2026-08-30 audit was about.
std::atomic<unsigned long long> g_hitUnresolved{0};

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
// NEUTER THE IMPULSE; DO NOT CANCEL THE NOTIFICATION (design #5, 2026-08-30).
//
// The delegate carries TWO things and we only ever wanted to stop one. `[V]` the BndEvt stub is
// eight statements -- five `UBER[K2Node_ComponentBoundEvent_*] := <param>` then
// `ExecuteUbergraph_ATV(<seg>)` -- and inside the segment the impulse reaches ONLY the damage
// math: five sites pass it to `processTire` (`sev = VSize(impact/mesh.GetMass())/100/1.5`, whose
// else-branch also scales dirt by `impact/mass`) and two pass it to `impulse()` (which subtracts
// |NormalImpulse|/500000*2*getBumperMult() from `health` and calls `explode()` at <=0). Meanwhile
// `wheelsOnSurface[index] = true` is written from `EX_True` -- a literal, independent of the
// impulse -- and THAT is what gates the suspension `AddForce` (uber 1228-1237) and `SetMassScale`
// (1198-1202).
//
// So writing a zero vector over `NormalImpulse` before dispatch gives a non-owner: no tire wear,
// no dirt, no health loss, no `explode()`, and -- because durability can no longer reach 0 -- no
// `ejectWheel`, hence never an orphan wheel prop whose per-process random key nothing could ever
// reconcile. And it keeps the rig shape, which is what cancelling those five delegates cost us
// (the 25-40 cm sag of ATV.md 17). Cancelling was the coarse instrument;
// [[lesson-a-notification-carries-more-than-the-effect-you-are-suppressing]] argued for this and
// the lane applied it by halves twice before getting here.
//
// Zero wire bytes, no protocol change, and no race: the mirror never accumulates at all, so there
// is no window in which it could cross zero between two corrections.
bool NeuterHit(void* self, void* params, int idx) {
    if (!g_guardActive.load(std::memory_order_acquire)) return false;  // not in a session: never touch
    for (int i = 0; i < kMaxOwned; ++i) {
        void* p = g_ownedAtvs[i].load(std::memory_order_acquire);
        if (!p) break;
        if (p == self) { g_hitAllowed.fetch_add(1, std::memory_order_relaxed); return false; }
    }
    const int32_t off = g_impulseOff[idx];
    if (!params || off < 0) {
        // Cannot neuter. Do NOT cancel as a fallback: that reinstates the measured 25-40 cm sag,
        // trading a damage divergence for a geometry one. Let it through, loudly countable.
        g_hitUnresolved.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // FVector = three floats. Written, not skipped: the ubergraph reads this local unconditionally.
    float* v = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(params) + off);
    v[0] = 0.f; v[1] = 0.f; v[2] = 0.f;
    g_hitNeutered.fetch_add(1, std::memory_order_relaxed);
    return false;  // ALWAYS dispatch: the handler's other effects are the ones we are keeping
}

// One callback per delegate INDEX -- RegisterInterceptor hands `self` and the params frame but
// not which UFunction fired, and each delegate has its own resolved offset.
template <int Idx>
bool OnAtvHitPre(void* self, void* params) { return NeuterHit(self, params, Idx); }

}  // namespace

void InstallHitGuard() {
    if (g_hitGuardArmed.load(std::memory_order_relaxed)) return;
    // LATCH THE ATTEMPT, NOT ONLY THE SUCCESS. Below this line are seven GUObjectArray walks and
    // a mutexed full read of multivoid.ini from disk; `g_hitGuardArmed` is false on every failure
    // path, so a session in which the delegates do not resolve would redo all of it on every
    // call. Today the only thing preventing that is `g_installed` in atv_sync.cpp -- a latch in
    // ANOTHER FILE, guarding a function this file's own header calls idempotent. That is the
    // Install()-in-a-pump shape the project's audit template exists for, and it is one line to
    // stop depending on somebody else's early-out.
    static std::atomic<bool> sTried{false};
    if (sTried.exchange(true, std::memory_order_acq_rel)) return;
    void* fns[8] = {};
    const int n = A::ResolveHitDelegates(fns, 8);
    // Resolve each delegate's own `NormalImpulse` param offset. Seven distinct UFunctions share
    // one signature, so the offsets will agree -- resolved per function anyway, because "they
    // share a signature therefore they share an offset" is an inference and this is a raw memory
    // write into an engine frame.
    int offsOk = 0;
    for (int i = 0; i < 7; ++i) {
        if (!fns[i]) continue;
        g_impulseOff[i] = R::FindParamOffset(fns[i], L"NormalImpulse");
        if (g_impulseOff[i] >= 0) ++offsOk;
    }
    using Cb = bool (*)(void*, void*);
    // Index order is kHitDelegateNames order and ResolveHitDelegates writes POSITIONALLY (it was
    // changed to, 2026-08-30; it used to COMPACT and this same comment was written over it), so
    // fns[i] and kCallbacks[i] name the same delegate by construction and not by agreement.
    // fns[i] may be NULL on a miss -- registration is skipped and `ok` then falls short of 7,
    // which fails the whole lane closed below.
    static const Cb kCallbacks[7] = {
        &OnAtvHitPre<0>, &OnAtvHitPre<1>, &OnAtvHitPre<2>, &OnAtvHitPre<3>,
        &OnAtvHitPre<4>, &OnAtvHitPre<5>, &OnAtvHitPre<6>,
    };
    int ok = 0;
    for (int i = 0; i < 7; ++i)
        if (fns[i] && ue_wrap::game_thread::RegisterInterceptor(fns[i], kCallbacks[i])) ++ok;
    if (ok == 7 && offsOk == 7) {
        g_hitGuardArmed.store(true, std::memory_order_relaxed);
        // WHAT THIS LINE MAY CLAIM. It used to read "a non-owner cannot author damage/explode/
        // ejectWheel", which sounds like a security property and is not one: `health` is not on
        // the wire, so a peer that edits the mask in its own ini desynchronises only ITS OWN
        // copy. This is a local-consistency control, never an anti-cheat one, and a log line that
        // implies otherwise is how a guarantee nobody built gets cited later as if it existed.
        UE_LOGI("atv: hit guard armed -- 7/7 ComponentHit delegates intercepted, NormalImpulse @+%d "
                "(bit0 mesh, bit1 Capsule, bits2-6 the wheels). A non-owner's hits DISPATCH with a "
                "ZEROED impulse: no wear, no dirt, no health loss, no explode, no ejectWheel -- "
                "while wheelsOnSurface still sets, so the rig keeps its shape. Local consistency, "
                "not anti-cheat: `health` is not on the wire, so editing this locally desyncs only "
                "your own copy.", g_impulseOff[0]);
    } else {
        // FAIL CLOSED. Without all seven we will not run the simulate-and-correct model at all:
        // Tick leaves every ATV's brain ON and mirrors nothing, so peers diverge visibly rather
        // than one of them silently destroying a vehicle the other still has.
        UE_LOGE("atv: hit guard NOT armed (%d/7 delegates resolved, %d/7 registered, %d/7 impulse "
                "offsets) -- ATV sync stays INERT this session; the interceptor table may be full "
                "(kMaxInterceptors), or NormalImpulse moved in the params frame", n, ok, offsOk);
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
    return Counters{ g_hitNeutered.load(), g_hitAllowed.load(), g_hitUnresolved.load(),
                     g_hitGuardArmed.load(std::memory_order_relaxed) };
}

}  // namespace coop::atv_hit_guard
