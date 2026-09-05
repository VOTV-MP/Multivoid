// coop/props/grab_observer.cpp -- the physics-prop grab, release and throw observers. See
// coop/props/grab_observer.h for the interface.

#include "coop/props/grab_observer.h"

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <atomic>
#include <cstdint>

namespace coop::grab_observer {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// The idempotency flag for Install: the pump tick calls every frame until it is true, then
// the body short-circuits. Atomic per the install pattern, since observers may run from
// task-graph workers under parallel animation and a cross-thread caller needs an ordered
// latch.
std::atomic<bool> g_installed{false};
// The retry throttle: while the classes have not resolved (the splash window, tens of
// seconds), a class find walks the full object array with a string allocation per entry,
// and pumping that at 125 Hz is the install-loop bomb. Wait N ticks between retries; 60
// ticks is about half a second, matching the NPC sync's countdown.
std::atomic<int> g_installRetryCountdown{0};

// Primary: the physics-handle component, the light grab path. `self` is the handle, owned by
// the player as its grab handle; to learn which prop is held, read the owning player's
// grabbing actor.

void GrabObserver_PHC_Grab(void* self, void* /*function*/, void* /*params*/) {
    UE_LOGI("grab_hook[PHC.Grab]: handle=%p (pickup -- light grab path)", self);
}

void GrabObserver_PHC_GrabWithRotation(void* self, void* /*function*/, void* /*params*/) {
    UE_LOGI("grab_hook[PHC.GrabWithRot]: handle=%p (pickup w/ rotation)", self);
}

void GrabObserver_PHC_SetTarget(void* self, void* /*function*/, void* /*params*/) {
    // The per-tick driver, firing every frame the blueprint moves the held prop. Hot: log the
    // first three calls and every thirtieth after, so a short test still sees activity but a
    // real grab does not drown the log. Atomic because ProcessEvent can dispatch from a
    // task-graph worker under parallel animation; the relaxed increment is cheap and the counter
    // tolerates it.
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
    // Pre-dispatch: read the grabbed component before the physics clears it, through the engine
    // wrapper.
    if (!self) {
        UE_LOGI("grab_hook[PHC.Release PRE]: handle=null");
        return;
    }
    void* comp = ue_wrap::engine::ReadPhysicsHandleGrabbedComponent(self);
    UE_LOGI("grab_hook[PHC.Release PRE]: handle=%p released_component=%p", self, comp);
}

// Primary: the physics-constraint component, the heavy drag path. `self` is the constraint
// component, owned by the player as its heavy grab. The game uses a physics constraint joint
// between the player and a heavy prop instead of a kinematic handle.

void GrabObserver_PCC_SetConstrainedComponents(void* self, void* /*function*/, void* /*params*/) {
    UE_LOGI("grab_hook[PCC.SetConstrainedComponents]: constraint=%p (heavy grab START)", self);
}

void GrabObserver_PCC_BreakConstraint_PRE(void* self, void* /*function*/, void* /*params*/) {
    UE_LOGI("grab_hook[PCC.BreakConstraint PRE]: constraint=%p (heavy grab END)", self);
}

// The throw signal: the primitive component's add-impulse. Fires whenever the blueprint
// throws a held prop (the throw helper is inlined and calls this on the released component),
// and also for non-throw impulses (explosions, hit reactions); the host disambiguates by
// context (a grab active right before means a throw).

void GrabObserver_PrimComp_AddImpulse(void* self, void* /*function*/, void* params) {
    // Diagnostic only: the release edge in the pump reads the body's inherited velocity directly
    // (which captures any impulse the engine just applied), so this only logs; no cross-thread
    // cache.
    if (!self || !params) return;
    // The parameter frame: the impulse vector, then the bone name, then the velocity-change flag.
    const ue_wrap::FVector imp = *reinterpret_cast<ue_wrap::FVector*>(params);
    UE_LOGI("grab_hook[PrimComp.AddImpulse]: component=%p impulse=(%.1f, %.1f, %.1f) (diagnostic, not shipped)",
            self, imp.X, imp.Y, imp.Z);
}

// Cheap-insurance pre-observers for the linear and angular velocity setters. If the
// blueprint calls these explicitly on a released prop (instead of relying on the inherited
// physics velocity), the frame carries the literal launch velocity, and the velocity read in
// the pump may then be post-step. These only log; a future change can switch the wire
// capture to the observer with a lock-free cache. The frame for both: the new velocity, the
// add-to-current flag, the bone name.
void GrabObserver_PrimComp_SetLinearVelocity_PRE(void* self, void* /*function*/, void* params) {
    if (!self || !params) return;
    // The remote-prop drive calls this UFunction at 125 Hz per held-prop slot, and an
    // unthrottled log here funnels debug output at hundreds of hertz under several peers and
    // props, stalling the render thread. The same throttle policy as the set-target observer.
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

// Secondary: the blueprint timeline and input, where `self` is the player. These prove the
// upstream dispatch path and let us read the player's grab-state fields.

void GrabObserver_InpActEvt_use(void* self, void* /*function*/, void* /*params*/) {
    // Fires on the use press. The blueprint graph downstream of this event decides pickup or
    // drop from the grabbing actor and plays the timeline accordingly. Reading the grabbing
    // actor here is the state after the graph (a post-observer); a pre-observer would give the
    // prior state. The read goes through the engine wrapper.
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!ue_wrap::engine::ReadMainPlayerGrabState(self, gs)) return;
    UE_LOGI("grab_hook[InpActEvt.use]: self=%p grabbing_actor(after)=%p", self, gs.grabbingActor);
}

void GrabObserver_grab_Update(void* self, void* /*function*/, void* /*params*/) {
    // The per-tick timeline update. Log the first three and every thirtieth after (the same
    // throttle as the set-target observer, so a short test still sees activity); atomic for the
    // same reason.
    if (!self) return;
    static std::atomic<uint64_t> sCount{0};
    const uint64_t n = sCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 3 && (n % 30) != 0) return;
    // All four state reads in one wrapper call.
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!ue_wrap::engine::ReadMainPlayerGrabState(self, gs)) return;
    UE_LOGI("grab_hook[grab.Update]: holding=%p grabsHeavy=%d Heavy=%d grabLen=%.1f (call #%llu)",
            gs.grabbingActor, gs.grabsHeavy ? 1 : 0, gs.heavy ? 1 : 0, gs.grabLen,
            static_cast<unsigned long long>(n));
}

void GrabObserver_grab_Finished_PRE(void* self, void* /*function*/, void* /*params*/) {
    // Pre-dispatch: read the held prop before the finished function clears it, through the
    // wrapper.
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (!ue_wrap::engine::ReadMainPlayerGrabState(self, gs)) return;
    UE_LOGI("grab_hook[grab.Finished PRE]: was holding=%p", gs.grabbingActor);
}

}  // namespace

void Install() {
    if (g_installed.load(std::memory_order_acquire)) return;
    // Throttle the retries to avoid four full class finds at 125 Hz during the pre-possession
    // window (the splash and the loading screen).
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

    // The cross-peer destroy fix: eager-resolve the physics-handle release cache the release
    // wrapper uses. Without it the first wire-received destroy of a held prop would hit the lazy
    // resolve in the destroy receiver, and if the class were somehow not loaded, fall through to
    // the warn-and-clear fallback, leaving the grabbed component dangling. Resolving here (the
    // class is confirmed loaded above) closes that window.
    ue_wrap::engine::WarmupPhcReleaseCache();

    // Primary: the engine physics handle (the light grab path).
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

    // Primary: the engine physics constraint (the heavy grab path, a different class).
    reg(pccCls, P::name::PhysicsConstraintComponentClass,
        P::name::SetConstrainedComponentsFn, GrabObserver_PCC_SetConstrainedComponents, /*pre=*/false);
    reg(pccCls, P::name::PhysicsConstraintComponentClass,
        P::name::BreakConstraintFn,          GrabObserver_PCC_BreakConstraint_PRE,      /*pre=*/true);

    // The throw signal: the primitive component's add-impulse.
    reg(primCls, P::name::PrimitiveComponentClass,
        P::name::AddImpulseFn,               GrabObserver_PrimComp_AddImpulse,          /*pre=*/false);

    // The diagnostic velocity-set observers: capture whether the blueprint explicitly sets the
    // velocity on release.
    reg(primCls, P::name::PrimitiveComponentClass,
        P::name::SetPhysicsLinearVelocityFn,           GrabObserver_PrimComp_SetLinearVelocity_PRE,  /*pre=*/true);
    reg(primCls, P::name::PrimitiveComponentClass,
        P::name::SetPhysicsAngularVelocityInDegreesFn, GrabObserver_PrimComp_SetAngularVelocity_PRE, /*pre=*/true);

    // Secondary: the blueprint timeline and input on the player.
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
