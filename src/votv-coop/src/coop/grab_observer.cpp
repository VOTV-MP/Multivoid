// coop/grab_observer.cpp -- physics-prop grab/release/throw observers.
//
// Extracted from harness/harness.cpp during the 2026-05-25 modularity refactor
// (modular file-size rule -- harness.cpp had grown to 3126 LOC).
//
// See coop/grab_observer.h for the public interface. Full RE of the grab
// pipeline: research/findings/votv-physics-interaction-deep-re-2026-05-23.md.

#include "coop/grab_observer.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/reflected_offset.h"
#include "ue_wrap/types.h"

#include <atomic>
#include <cstdint>

namespace coop::grab_observer {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// File-static: idempotency flag for Install(). NetPumpTick calls every frame
// until this becomes true. Once installed, the entire body short-circuits.
// Atomic per the project-wide Install() pattern (memory: install-idempotent-
// o1-steady-state) -- ProcessEvent observers may be invoked from task-graph
// workers under parallel anim, and any future cross-thread Install caller
// needs an acquire/release-ordered latch.
std::atomic<bool> g_installed{false};
// Retry throttle: when classes haven't resolved yet (OMEGA splash window,
// 15-30s typical), R::FindClass walks the full GUObjectArray with a wstring
// alloc per entry. Pumping that at 125 Hz reproduces the install-loop bomb
// (memory: install-idempotent-o1-steady-state). Wait N ticks between retries.
// 60 ticks ~= 0.5s at 125 Hz, matching npc_sync::s_installRetryCountdown.
std::atomic<int> g_installRetryCountdown{0};

// --- Primary: UPhysicsHandleComponent (LIGHT grab path). `self` IS the
// handle (owned by mainPlayer_C as `grabHandle` @+0x688). To learn which
// prop is held, dereference the OuterPrivate chain or read the owning
// mainPlayer_C's grabbing_actor.

void GrabObserver_PHC_Grab(void* self, void* /*function*/, void* /*params*/) {
    UE_LOGI("grab_hook[PHC.Grab]: handle=%p (pickup -- light grab path)", self);
}

void GrabObserver_PHC_GrabWithRotation(void* self, void* /*function*/, void* /*params*/) {
    UE_LOGI("grab_hook[PHC.GrabWithRot]: handle=%p (pickup w/ rotation)", self);
}

void GrabObserver_PHC_SetTarget(void* self, void* /*function*/, void* /*params*/) {
    // Per-tick driver -- fires every frame the BP graph wants to move the held
    // prop. Hot. Log first 3 calls + every 30th after, so a short autonomous
    // test (5 calls) still sees observer activity but a real grab (~60 Hz x
    // seconds) doesn't drown the log. ATOMIC because ProcessEvent CAN dispatch
    // from a task-graph worker under parallel anim (per game_thread.h:91-92);
    // the relaxed fetch_add is cheap and the counter semantics tolerate it.
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n % 30) == 0) {
        UE_LOGI("grab_hook[PHC.SetTarget]: handle=%p (call #%llu)", self,
                static_cast<unsigned long long>(n));
    }
}

void GrabObserver_PHC_SetTargetWithRotation(void* self, void* /*function*/, void* /*params*/) {
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n % 30) == 0) {
        UE_LOGI("grab_hook[PHC.SetTargetWithRot]: handle=%p (call #%llu)", self,
                static_cast<unsigned long long>(n));
    }
}

void GrabObserver_PHC_Release_PRE(void* self, void* /*function*/, void* /*params*/) {
    // Pre-dispatch: read GrabbedComponent BEFORE PhysX clears it (offset +176
    // confirmed by IDA decompile of ReleaseComponent_Impl @0x142D7C670).
    if (!self) {
        UE_LOGI("grab_hook[PHC.Release PRE]: handle=null");
        return;
    }
    void* comp = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(self) + P::off::UPhysicsHandleComponent_GrabbedComponent);
    UE_LOGI("grab_hook[PHC.Release PRE]: handle=%p released_component=%p", self, comp);
}

// --- Primary: UPhysicsConstraintComponent (HEAVY drag path). `self` IS the
// constraint component, owned by mainPlayer_C as `heavyGrab` (+0x4F0).
// Confirmed via SDK dump (mainPlayer.hpp:12). VOTV uses a physics CONSTRAINT
// joint between the player and a heavy prop instead of a kinematic handle.

void GrabObserver_PCC_SetConstrainedComponents(void* self, void* /*function*/, void* /*params*/) {
    UE_LOGI("grab_hook[PCC.SetConstrainedComponents]: constraint=%p (heavy grab START)", self);
}

void GrabObserver_PCC_BreakConstraint_PRE(void* self, void* /*function*/, void* /*params*/) {
    UE_LOGI("grab_hook[PCC.BreakConstraint PRE]: constraint=%p (heavy grab END)", self);
}

// --- Throw signal: UPrimitiveComponent::AddImpulse. Fires whenever the BP
// throws a held prop (throwHoldingProp BP-pure is inlined and calls this on
// the released component). Also fires for non-throw impulses (explosions,
// hit reactions) -- the host disambiguates via context (was a grab active
// right before this fired? then it's a throw).

void GrabObserver_PrimComp_AddImpulse(void* self, void* /*function*/, void* params) {
    // Diagnostic-only post-RE 2026-05-24: the release-edge in NetPumpTick
    // reads the body's inherited velocity directly via prop::GetPhysicsVelocity
    // (which captures any AddImpulse the engine just applied AS PART OF
    // that velocity). So we just log here -- no cross-thread cache to populate.
    if (!self || !params) return;
    // Param frame layout (verified by autonomous test):
    //   FVector impulse    @ 0   (12 bytes)
    //   FName   BoneName   @ 12  (8 bytes)
    //   bool    bVelChange @ 20  (1 byte) + pad
    const ue_wrap::FVector imp = *reinterpret_cast<ue_wrap::FVector*>(params);
    UE_LOGI("grab_hook[PrimComp.AddImpulse]: component=%p impulse=(%.1f, %.1f, %.1f) (diagnostic, not shipped)",
            self, imp.X, imp.Y, imp.Z);
}

// "Cheap insurance" PRE-observers for SetPhysicsLinearVelocity +
// SetPhysicsAngularVelocityInDegrees. If BP explicitly calls these on a
// released prop (instead of relying on inherited PhysX tracking velocity),
// the param frame carries the LITERAL launch velocity we want -- the
// GetPhysicsVelocity read in NetPumpTick may then be reading post-step
// values. These observers DON'T cache cross-thread; they just log. A future
// commit can switch the wire-capture to the observer (lock-free atomic
// cache, RELEASE/ACQUIRE fenced -- same shape as the retired
// g_lastImpulse* pattern).
//
// Param frame layout for both:
//   FVector NewVel/NewAngVel  @ 0   (12 bytes)
//   bool    bAddToCurrent     @ 12  (1 byte) + pad
//   FName   BoneName          @ 16  (8 bytes)
void GrabObserver_PrimComp_SetLinearVelocity_PRE(void* self, void* /*function*/, void* params) {
    if (!self || !params) return;
    // remote_prop::DriveSetLinearVelocity calls this UFunction at 125 Hz per
    // held-prop slot. Unthrottled UE_LOGI here funnels OutputDebugStringW at
    // hundreds of Hz under multi-peer multi-prop and stalls the render thread.
    // Same throttle policy as PHC.SetTarget.
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 3 && (n % 60) != 0) return;
    const ue_wrap::FVector v = *reinterpret_cast<ue_wrap::FVector*>(params);
    UE_LOGI("grab_hook[PrimComp.SetPhysicsLinearVelocity PRE]: component=%p NewVel=(%.1f, %.1f, %.1f) (call #%llu)",
            self, v.X, v.Y, v.Z, static_cast<unsigned long long>(n));
}

void GrabObserver_PrimComp_SetAngularVelocity_PRE(void* self, void* /*function*/, void* params) {
    if (!self || !params) return;
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 3 && (n % 60) != 0) return;
    const ue_wrap::FVector v = *reinterpret_cast<ue_wrap::FVector*>(params);
    UE_LOGI("grab_hook[PrimComp.SetPhysicsAngularVelocityInDegrees PRE]: component=%p NewAngVel=(%.1f, %.1f, %.1f) (call #%llu)",
            self, v.X, v.Y, v.Z, static_cast<unsigned long long>(n));
}

// --- Secondary: BP-Timeline + input (`self` IS mainPlayer_C). These prove
// the upstream dispatch path and let us read mainPlayer_C grab-state fields.

void GrabObserver_InpActEvt_use(void* self, void* /*function*/, void* /*params*/) {
    // Fires on E-press. The BP graph downstream of this event decides
    // pickup-vs-drop from grabbing_actor and plays the Timeline accordingly.
    // Reading grabbing_actor here = state AT press time (post-observer fires
    // AFTER the BP graph, so it'll show NEW state). For PRE-state we'd need
    // a pre-observer; skipping for Stage 1 -- not actionable yet.
    if (!self) return;
    void* grabbing = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(self) + ue_wrap::reflected_offset::MainPlayer_grabbing_actor());
    UE_LOGI("grab_hook[InpActEvt.use]: self=%p grabbing_actor(after)=%p", self, grabbing);
}

void GrabObserver_grab_Update(void* self, void* /*function*/, void* /*params*/) {
    // Per-tick Timeline update. Log first 3 + every 30th after (same throttle
    // policy as PHC.SetTarget so a short autonomous test still sees activity).
    // Atomic for the same reason as PHC.SetTarget above.
    if (!self) return;
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 3 && (n % 30) != 0) return;
    void* grabbing = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(self) + ue_wrap::reflected_offset::MainPlayer_grabbing_actor());
    bool grabsHeavy = *reinterpret_cast<bool*>(
        reinterpret_cast<uint8_t*>(self) + ue_wrap::reflected_offset::MainPlayer_grabsHeavy());
    bool heavy = *reinterpret_cast<bool*>(
        reinterpret_cast<uint8_t*>(self) + ue_wrap::reflected_offset::MainPlayer_Heavy());
    float grabLen = *reinterpret_cast<float*>(
        reinterpret_cast<uint8_t*>(self) + ue_wrap::reflected_offset::MainPlayer_grabLen());
    UE_LOGI("grab_hook[grab.Update]: holding=%p grabsHeavy=%d Heavy=%d grabLen=%.1f (call #%llu)",
            grabbing, grabsHeavy ? 1 : 0, heavy ? 1 : 0, grabLen,
            static_cast<unsigned long long>(n));
}

void GrabObserver_grab_Finished_PRE(void* self, void* /*function*/, void* /*params*/) {
    // Pre-dispatch: read held prop BEFORE FinishedFunc clears it.
    if (!self) return;
    void* grabbing = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(self) + ue_wrap::reflected_offset::MainPlayer_grabbing_actor());
    UE_LOGI("grab_hook[grab.Finished PRE]: was holding=%p", grabbing);
}

}  // namespace

void Install() {
    if (g_installed.load(std::memory_order_acquire)) return;
    // Throttle retries to avoid 125 Hz x 4 R::FindClass walks during the
    // 15-30s pre-possession window (OMEGA splash, loading screen).
    if (g_installRetryCountdown.load(std::memory_order_relaxed) > 0) {
        g_installRetryCountdown.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    void* phcCls = R::FindClass(P::name::PhysicsHandleComponentClass);
    void* pccCls = R::FindClass(P::name::PhysicsConstraintComponentClass);
    void* primCls = R::FindClass(P::name::PrimitiveComponentClass);
    void* playerCls = R::FindClass(P::name::MainPlayerClass);
    if (!phcCls || !pccCls || !primCls || !playerCls) {
        UE_LOGW("grab_hook: class not found yet (PHC=%p, PCC=%p, PrimComp=%p, mainPlayer=%p) -- retry in 60 ticks",
                phcCls, pccCls, primCls, playerCls);
        g_installRetryCountdown.store(60, std::memory_order_relaxed);
        return;
    }

    auto reg = [](void* cls, const wchar_t* clsName, const wchar_t* fnName,
                  ue_wrap::game_thread::ProcessEventObserverFn cb, bool pre) {
        void* fn = R::FindFunction(cls, fnName);
        if (!fn) {
            UE_LOGW("grab_hook: UFunction '%ls' not found on %ls", fnName, clsName);
            return;
        }
        const bool ok = pre
            ? ue_wrap::game_thread::RegisterPreObserver(fn, cb)
            : ue_wrap::game_thread::RegisterPostObserver(fn, cb);
        if (!ok) {
            UE_LOGW("grab_hook: register %ls failed (table full or null cb)", fnName);
        } else {
            UE_LOGI("grab_hook: registered %s observer for %ls.%ls @ %p",
                    pre ? "PRE" : "POST", clsName, fnName, fn);
        }
    };

    // Cross-peer-destroy fix: eager-resolve the PHC.ReleaseComponent cache
    // used by ue_wrap::engine::ReleaseMainPlayerGrabIfHolding. Without
    // this, the first wire-received PropDestroy of
    // a held prop would hit the lazy resolve in remote_prop::OnDestroy --
    // and if PHC class were somehow not yet loaded, fall through to the
    // warn-and-clear fallback, leaving PHC.GrabbedComponent dangling.
    // Eager-resolve here (we already have phcCls confirmed loaded above)
    // closes that window.
    ue_wrap::engine::WarmupPhcReleaseCache();

    // Primary: engine PhysicsHandle (LIGHT grab path).
    reg(phcCls, P::name::PhysicsHandleComponentClass,
        P::name::GrabComponentAtLocationFn,             GrabObserver_PHC_Grab,                  /*pre=*/false);
    reg(phcCls, P::name::PhysicsHandleComponentClass,
        P::name::GrabComponentAtLocationWithRotationFn, GrabObserver_PHC_GrabWithRotation,      /*pre=*/false);
    reg(phcCls, P::name::PhysicsHandleComponentClass,
        P::name::SetTargetLocationFn,                   GrabObserver_PHC_SetTarget,             /*pre=*/false);
    reg(phcCls, P::name::PhysicsHandleComponentClass,
        P::name::SetTargetLocationAndRotationFn,        GrabObserver_PHC_SetTargetWithRotation, /*pre=*/false);
    reg(phcCls, P::name::PhysicsHandleComponentClass,
        P::name::ReleaseComponentFn,                    GrabObserver_PHC_Release_PRE,           /*pre=*/true);

    // Primary: engine PhysicsConstraint (HEAVY grab path -- different class).
    reg(pccCls, P::name::PhysicsConstraintComponentClass,
        P::name::SetConstrainedComponentsFn, GrabObserver_PCC_SetConstrainedComponents, /*pre=*/false);
    reg(pccCls, P::name::PhysicsConstraintComponentClass,
        P::name::BreakConstraintFn,          GrabObserver_PCC_BreakConstraint_PRE,      /*pre=*/true);

    // Throw signal: engine PrimitiveComponent.AddImpulse.
    reg(primCls, P::name::PrimitiveComponentClass,
        P::name::AddImpulseFn,               GrabObserver_PrimComp_AddImpulse,          /*pre=*/false);

    // Diagnostic-only velocity-set observers. Capture whether BP
    // explicitly calls SetPhysics*Velocity on release.
    reg(primCls, P::name::PrimitiveComponentClass,
        P::name::SetPhysicsLinearVelocityFn,           GrabObserver_PrimComp_SetLinearVelocity_PRE,  /*pre=*/true);
    reg(primCls, P::name::PrimitiveComponentClass,
        P::name::SetPhysicsAngularVelocityInDegreesFn, GrabObserver_PrimComp_SetAngularVelocity_PRE, /*pre=*/true);

    // Secondary: BP-Timeline + input on mainPlayer_C.
    reg(playerCls, P::name::MainPlayerClass,
        P::name::MainPlayerUseInputEventFn,  GrabObserver_InpActEvt_use,      /*pre=*/false);
    reg(playerCls, P::name::MainPlayerClass,
        P::name::MainPlayerGrabUpdateFn,     GrabObserver_grab_Update,        /*pre=*/false);
    reg(playerCls, P::name::MainPlayerClass,
        P::name::MainPlayerGrabFinishedFn,   GrabObserver_grab_Finished_PRE,  /*pre=*/true);

    g_installed.store(true, std::memory_order_release);
    UE_LOGI("grab_hook: Stage 1+ core observers installed (5 PHC + 2 PCC + 1 PrimComp.AddImpulse + 2 PrimComp.SetVel + 3 BP-Timeline) -- press E on a prop to see hook lines");
}

}  // namespace coop::grab_observer
