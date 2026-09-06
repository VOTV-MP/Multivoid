// coop/interactables/atv_hit_guard.cpp -- see coop/interactables/atv_hit_guard.h.

#include "coop/interactables/atv_hit_guard.h"

#include "coop/config/config.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"

#include <atomic>
#include <cstdint>

// InstallHitGuard and PublishOwned sit at namespace scope because they are the definitions the
// header declares; everything the header does not name is in the anonymous namespace, since
// proximity is not confinement and these symbols must not have external linkage.
namespace coop::atv_hit_guard {

namespace A = ue_wrap::atv;
namespace R = ue_wrap::reflection;

// The tick is not the brain. Turning a mirror's actor tick off was meant to keep the
// accumulators and the wheel torque on one machine, and it moved the vehicle instead: the
// tick re-applies the rig's centre of mass every frame before any gate, so a rig whose centre
// of mass is never set rests and settles elsewhere, while the things tick-off was meant to
// stop are already single-peer by the game's own gating (the wheel torque is guarded by the
// driven flag, and the battery-drain terms select on driven, lights and turbo, all local-only
// state). So the mirror ticks. The owned-tick flag keeps its two real jobs: electing the idle
// syncer, and telling the collision interceptor whose copy may author damage. The interceptor
// is the only thing that makes a mirror differ from a native ATV, and it has to be: a hit
// delegate is dispatched by the physics scene and was never tick-gated.

namespace {

// The collision half, and the table it reads. All seven of the ATV's hit delegates reach
// authored state: the body's feeds the health math that explodes the vehicle at zero, the
// wheels' burn tire durability and eject a wheel at zero, the capsule's pops a hint. With the
// rig simulating on every peer, a non-owner running them would blow up a vehicle its authority
// still has. But the five wheel delegates also maintain the rig's own SHAPE, so cancelling
// them suppressed a notification carrying two unrelated things and took the second: it left a
// mirror resting tens of centimetres low, visible only as ride height and never in the
// suspension distances. What a non-owner's dispatched wheel hits cost instead: it burns its
// own tire durability and can eject a tire its author still has, a narrower divergence whose
// fix is durability on the wire, not re-suppression. The predicate cannot read the ATV table,
// since the interceptor contract does not promise the game thread, so Tick publishes the ATVs
// this peer owns into this atomic array and the callback scans it. For an ATV absent from the
// table the guard ZEROES the impulse and dispatches anyway -- it never cancels a hit
// notification, because the handler's other effects are the ones being kept. Sized for every
// ATV a host can own at once; overflow is not graceful, so it logs once.
std::atomic<void*> g_ownedAtvs[kMaxOwned];
std::atomic<bool>  g_guardActive{false};   // false in single-player: the game must keep its damage
std::atomic<unsigned long long> g_hitNeutered{0};
std::atomic<unsigned long long> g_hitAllowed{0};
std::atomic<bool>  g_hitGuardArmed{false};              // all 7 delegates registered -- else the lane runs INERT
// The byte offset of the impulse inside each delegate's params frame, resolved once at install
// by reflection, per function: they are seven distinct UFunction objects sharing one
// signature. -1 is unresolved.
int32_t g_impulseOff[7] = {-1, -1, -1, -1, -1, -1, -1};
// A hit that could not be neutered because its offset never resolved; permanently 0, since the
// install refuses to arm without all seven, and counted anyway because the alternative is a
// silent fall-through to a mirror authoring damage.
std::atomic<unsigned long long> g_hitUnresolved{0};

}  // namespace

void PublishOwned(void** owned, int n) {
    for (int i = 0; i < kMaxOwned; ++i)
        g_ownedAtvs[i].store(i < n ? owned[i] : nullptr, std::memory_order_release);
}

namespace {

// One callback per delegate index, so the guard can treat a subset differently: the
// interceptor hands the callback the object and the params frame but not which function
// fired, so one shared callback could not tell the body's collision from a wheel's; a
// template instantiates seven function pointers around one body. Neuter the impulse; do not
// cancel the notification. The delegate carries two things: the impulse reaches only the
// damage math (the wheel handlers scale wear and dirt by it, the body handler subtracts from
// health and explodes at zero), while the wheels-on-surface flag is written from a literal,
// independent of the impulse, and that flag gates the suspension force and the mass scale. A
// zero vector over the impulse before dispatch gives a non-owner no wear, no dirt, no health
// loss, no explosion and, since durability can no longer reach zero, no ejected wheel and no
// orphan wheel prop, while the rig keeps its shape. Zero wire bytes, and no race: the mirror
// never accumulates, so there is no window in which it could cross zero between corrections.
bool NeuterHit(void* self, void* params, int idx) {
    if (!g_guardActive.load(std::memory_order_acquire)) return false;  // not in a session: never touch
    for (int i = 0; i < kMaxOwned; ++i) {
        void* p = g_ownedAtvs[i].load(std::memory_order_acquire);
        if (!p) break;
        if (p == self) { g_hitAllowed.fetch_add(1, std::memory_order_relaxed); return false; }
    }
    const int32_t off = g_impulseOff[idx];
    if (!params || off < 0) {
        // Cannot neuter. Not cancelled as a fallback, which would reinstate the sag, trading a
        // damage divergence for a geometry one; let through, counted.
        g_hitUnresolved.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // A vector of three floats, written rather than skipped: the graph reads the local
    // unconditionally.
    float* v = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(params) + off);
    v[0] = 0.f; v[1] = 0.f; v[2] = 0.f;
    g_hitNeutered.fetch_add(1, std::memory_order_relaxed);
    return false;  // ALWAYS dispatch: the handler's other effects are the ones we are keeping
}

// One callback per index: the interceptor hands over the object and the frame but not the
// function, and each delegate has its own resolved offset.
template <int Idx>
bool OnAtvHitPre(void* self, void* params) { return NeuterHit(self, params, Idx); }

}  // namespace

void InstallHitGuard() {
    if (g_hitGuardArmed.load(std::memory_order_relaxed)) return;
    // The attempt is latched, not only the success: below are seven object-array walks and a full
    // ini read, and an armed flag that is false on every failure path would redo them on every
    // call in a session where the delegates do not resolve.
    static std::atomic<bool> sTried{false};
    if (sTried.exchange(true, std::memory_order_acq_rel)) return;
    void* fns[8] = {};
    const int n = A::ResolveHitDelegates(fns, 8);
    // Each delegate's own impulse offset: seven functions share one signature, so the offsets will
    // agree, and they are resolved per function anyway, since this is a raw write into an engine
    // frame and shared-signature-therefore-shared-offset is an inference.
    int offsOk = 0;
    for (int i = 0; i < 7; ++i) {
        if (!fns[i]) continue;
        g_impulseOff[i] = R::FindParamOffset(fns[i], L"NormalImpulse");
        if (g_impulseOff[i] >= 0) ++offsOk;
    }
    using Cb = bool (*)(void*, void*);
    // The index order is the delegate-name order, and the resolver writes positionally, so the
    // functions and the callbacks name the same delegate by construction; a null on a miss skips
    // the registration, and the count then falls short of seven, which fails the lane closed
    // below.
    static const Cb kCallbacks[7] = {
        &OnAtvHitPre<0>, &OnAtvHitPre<1>, &OnAtvHitPre<2>, &OnAtvHitPre<3>,
        &OnAtvHitPre<4>, &OnAtvHitPre<5>, &OnAtvHitPre<6>,
    };
    int ok = 0;
    for (int i = 0; i < 7; ++i)
        if (fns[i] && ue_wrap::game_thread::RegisterInterceptor(fns[i], kCallbacks[i])) ++ok;
    if (ok == 7 && offsOk == 7) {
        g_hitGuardArmed.store(true, std::memory_order_relaxed);
        // What the line may claim: a local-consistency control, not an anti-cheat one. Health is
        // not on the wire, so a peer editing the mask in its own ini desynchronises only its own
        // copy, and a line implying a guarantee nobody built is how one gets cited later.
        UE_LOGI("atv: hit guard armed -- 7/7 ComponentHit delegates intercepted, NormalImpulse @+%d "
                "(bit0 mesh, bit1 Capsule, bits2-6 the wheels). A non-owner's hits DISPATCH with a "
                "ZEROED impulse: no wear, no dirt, no health loss, no explode, no ejectWheel -- "
                "while wheelsOnSurface still sets, so the rig keeps its shape. Local consistency, "
                "not anti-cheat: `health` is not on the wire, so editing this locally desyncs only "
                "your own copy.", g_impulseOff[0]);
    } else {
        // Fail closed: without all seven the simulate-and-correct model does not run at all, and
        // the tick mirrors nothing, so peers diverge visibly rather than one silently destroying a
        // vehicle the other still has.
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
