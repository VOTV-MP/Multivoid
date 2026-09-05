// coop/props/trash_use_intercept.cpp -- the client-grab bridge on the use input: the interceptor
// family on the player's use input event. On a client it cancels the native pile grab and
// carry-throw press (and the paired deny sound) and routes a grab or throw intent to the host;
// on the host it always runs native. The re-pile and drop observers and the held-item
// broadcast stay in trash_collect_sync, which delegates its install and disconnect here; this
// file owns only the use-input interception and the left-button hard-throw bridge. Game thread
// only.

#include "coop/props/trash_use_intercept.h"

// The client-grab chain pulls in the same broad dependency set trash_collect_sync did.
#include "coop/dev/spawn_order_probe.h"
#include "coop/element/element.h"
#include "coop/element/quiescence_drain.h"   // ArmGhostSweep
#include "coop/creatures/kerfur_entity.h"    // IsKerfurActor / IsKerfurPropClass
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_sound.h"           // client-own-grab pickup cue
#include "coop/props/prop_synth_key.h"
#include "coop/props/remote_prop.h"          // ResolveMirrorEidByActor
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/save_identity_bind.h"
#include "coop/props/save_identity_map.h"
#include "coop/props/trash_channel.h"      // ClientCarryEid / SendGrabIntent / SendThrowIntent / ClearClientCarry
#include "coop/props/trash_proxy.h"        // EidForAimedPileProxy / ProxyActorForEid (client-grab camera-ray cone)
#include "coop/save/save_transfer.h"      // RecordGrabTimePileXform
#include "ue_wrap/engine/engine.h"                // ReadMainPlayerLookAtActor / GetCamera{Location,Rotation}
#include "ue_wrap/core/game_thread.h"           // RegisterInterceptor / RegisterPreObserver
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"                  // IsChipPile
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"           // MainPlayer class + use/fire input-event fn names
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>

namespace coop::trash_use_intercept {

namespace {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace GT = ue_wrap::game_thread;
namespace PT = coop::prop_element_tracker;

// The cached session: the PRE callbacks are param-less UFunction hooks, so they read a cached
// pointer. Set by Install, cleared by OnDisconnect, read on the game thread;
// trash_collect_sync keeps its own for its observers, both set from the one top-level install.
std::atomic<coop::net::Session*> g_session{nullptr};
bool g_grabObserverInstalled = false;  // InpActEvt_use PRE registration latch (process life)

// The gesture-pairing latch: a cancelled client use press must cancel its paired release too,
// since the blueprint never saw the press and its release handler would run against a
// press-less state and hit the deny. Armed by every cancelled press seam, consumed by the next
// release. Game thread only.
bool g_cancelPairedUseRelease = false;

// The client-grab interceptor on the use input. A chip-pile grab is the use press, whose
// handler calls the looked-at actor's grab (spawn a clump, pick it up, destroy the pile)
// blueprint-internally, invisible to the ProcessEvent detour; the input event itself is
// visible, a thin stub into the ubergraph, and the whole use flow, the grab and the use action
// that plays the deny sound on its fail branches, hangs off it. A PRE interceptor: true
// cancels the native dispatch entirely. On a client, whenever the press is routed to the host
// (a pile grab or a carry throw) it returns true, so the grab and the deny both die at the
// source; nulling the looked-at actor was only a half-suppression, since the use action
// re-traces and still denies. Any other press, and the host, run native; puppets never process
// input, so this fires for the local player only. The use action has three bindings, a second
// press and a release beside the grab press, all reaching the deny, so the second press gets
// this side-effect-free suppressor: on a client pile interaction it only cancels the dispatch
// (the grab press alone sends the intent and plays the cue), with the same recognition
// (carrying, or aimed at any native or proxy pile), and on cancel it arms the release latch.
static bool OnPileUseDenySuppress(void* self, void* /*params*/) {
    if (!self) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() == coop::net::Role::Host) return false;  // client-only; host runs native
    bool cancel = false;
    if (coop::trash_channel::ClientCarryEid() != coop::element::kInvalidId) {
        cancel = true;  // carrying a clump -> this press is the throw toggle -> the native press would deny
    } else {
        void* aimedNative = ue_wrap::engine::ReadMainPlayerLookAtActor(self);
        if (aimedNative && ue_wrap::prop::IsChipPile(aimedNative)) {
            // Any native pile aim, bound or unbound, mirrors the grab interceptor, which cancels
            // the unbound press too; this seam must die with it or the deny plays alone.
            cancel = true;
        } else {
            const ue_wrap::FVector  camLoc = ue_wrap::engine::GetCameraLocation();
            const ue_wrap::FRotator camRot = ue_wrap::engine::GetCameraRotation();
            const float d2r = 3.14159265f / 180.f;
            const float yaw = camRot.Yaw * d2r, pitch = camRot.Pitch * d2r;
            const float cp = std::cos(pitch);
            const ue_wrap::FVector camFwd{ cp * std::cos(yaw), cp * std::sin(yaw), std::sin(pitch) };
            cancel = coop::trash_proxy::EidForAimedPileProxy(camLoc, camFwd, /*maxRangeCm=*/400.f,
                                                             /*minDot=*/0.94f) != coop::element::kInvalidId;
        }
    }
    if (cancel) g_cancelPairedUseRelease = true;
    return cancel;  // false: not a pile interaction -> native use runs (devices, other interactions, SP deny)
}

// The release seam is pairing-only: cancel iff this release's press was cancelled by either
// press seam. No condition re-derivation: the press conditions are stale by release time (the
// throw press cleared the carry, the aim wandered), and matching a native press's release
// would eat a legitimate drop. One latch consume per gesture.
static bool OnPileUseReleaseSuppress(void* /*self*/, void* /*params*/) {
    if (!g_cancelPairedUseRelease) return false;  // paired with a NATIVE press -> the native release must run
    g_cancelPairedUseRelease = false;
    UE_LOGI("[USE-RELEASE] paired E-release CANCELLED (its press was intercepted -- no use_deny on release)");
    return true;
}

static bool OnPileUseIntercept(void* self, void* /*params*/) {
    if (!self) return false;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return false;  // no coop session -> native use runs normally (SP / disconnected)

    // Client: it has no real chip pile; every pile it sees is a static-mesh proxy, and recognition
    // is a camera-ray cone, the most-centred proxy within range of the aim. Giving the proxy
    // collision so the game's own trace hit it was disproven: the pile mesh has no simple
    // collision body, so the trace never hit. The proxy itself needs no suppression, since it
    // fails every native interaction cast on press; the host re-validates and enforces one hold
    // per peer.
    if (s->role() != coop::net::Role::Host) {
        // Toggle: carrying a clump already, a press is a throw regardless of aim; else, aimed at a
        // mirrored pile, a press is a grab request. A player holds at most one clump.
        const coop::element::ElementId carry = coop::trash_channel::ClientCarryEid();
        if (carry != coop::element::kInvalidId) {
            // Self-heal: if we think we are carrying but the proxy is gone (a missed host abort
            // edge), clear the stale toggle and fall through to a fresh grab rather than throwing a
            // dead eid forever.
            if (!coop::trash_proxy::ProxyActorForEid(carry)) {
                UE_LOGW("[THROW-INTENT] CLIENT carry eid=%u has NO live proxy -- stale toggle, clearing + falling "
                        "through to grab", static_cast<unsigned>(carry));
                coop::trash_channel::ClearClientCarry(static_cast<uint32_t>(carry));
            } else {
                UE_LOGI("[THROW-INTENT] CLIENT E-PRESS while carrying eid=%u -> requesting release(E-drop) from host "
                        "(native use CANCELLED -- no use_deny)", static_cast<unsigned>(carry));
                coop::trash_channel::SendThrowIntent(*s, static_cast<uint32_t>(carry),
                                                     coop::net::throw_mode::kRelease, ue_wrap::FVector{});
                g_cancelPairedUseRelease = true;  // this press's release dies with it; the deny lives there too
                return true;  // handled: cancel the native InpActEvt_use (the client holds no native clump -> would deny)
            }
        }
        // Native-authoritative grab recognition first: a save-loaded pile the client bound as the
        // host-range mirror is a real chip pile, so it is the game's own looked-at actor,
        // occlusion-correct and collision-blocked, unlike a bare proxy. If the looked-at actor is a
        // bound native pile, the whole native dispatch is cancelled, so the grab and the deny both
        // die, and the grab goes to the host as an intent, the sole author of the shared mutation,
        // which runs the grab on the requester's puppet. The camera cone below stays for unbound
        // proxy piles only.
        {
            void* aimedNative = ue_wrap::engine::ReadMainPlayerLookAtActor(self);
            if (aimedNative && ue_wrap::prop::IsChipPile(aimedNative)) {
                if (PT::IsBoundMirrorNative(aimedNative)) {
                    const coop::element::ElementId beid = coop::remote_prop::ResolveMirrorEidByActor(aimedNative);
                    if (beid != coop::element::kInvalidId) {
                        // The pickup cue: the native grab, which plays the use click and the
                        // material cue, is cancelled, so the same feedback is synthesised at the
                        // pile; the host hears it natively on the puppet, observers through the
                        // drive.
                        coop::prop_sound::PlayUseClick(aimedNative);
                        coop::prop_sound::PlayGrabSound(aimedNative);
                        // The position and chip type of the pile the client aims at; the host's
                        // grab-intent line for the same eid must match, and a mismatch names an
                        // identity misalignment.
                        const ue_wrap::FVector cloc = ue_wrap::engine::GetActorLocation(aimedNative);
                        UE_LOGI("[GRAB-INTENT] CLIENT E-PRESS on BOUND native pile eid=%u at(%.1f,%.1f,%.1f) "
                                "chipType=%u (lookAtActor, occlusion-correct) -> native use CANCELLED "
                                "(no grab, no use_deny) + requesting grab from host",
                                static_cast<unsigned>(beid), cloc.X, cloc.Y, cloc.Z,
                                static_cast<unsigned>(ue_wrap::prop::GetChipType(aimedNative)));
                        coop::trash_channel::SendGrabIntent(*s, static_cast<uint32_t>(beid));
                        g_cancelPairedUseRelease = true;  // pair: the _42 release of this cancelled press dies too
                        return true;  // handled: cancel the native InpActEvt_use dispatch entirely
                    }
                }
                // An unbound native pile: a real chip pile in the client's aim that no eid owns (a
                // host-vacate twin the sweep has not retired, a mid-bind-window native, a
                // descendant of an earlier native grab). Letting the native use run here seeds a
                // self-perpetuating client-only chain (native grab, local clump, native land,
                // another unbound pile), all invisible to the host, and a grabbed twin becomes a
                // clump the sweep can no longer retire. On a connected client every pile
                // interaction is host-authoritative, so a pile no eid owns must not be
                // interactable.
                if (coop::join_membership_sweep::HasLoadTailQuiesced()) {
                    // Post-quiescence: the wholesale owner is the reconcile pass's ghost-retire
                    // tail, which re-binds what a map key still claims and retires every provably
                    // identity-less native at once. This press is positive evidence a ghost slipped
                    // the event triggers, so the pass is armed (it runs within the reconcile
                    // debounce) and the press cancelled.
                    UE_LOGW("[GRAB-INTENT] CLIENT E-PRESS on UNBOUND native pile %p POST-quiescence "
                            "-- identity-less ghost -> arming the wholesale GHOST-RETIRE reconcile "
                            "(all ghosts adjudicated at once)", aimedNative);
                    coop::element::quiescence_drain::ArmGhostSweep();
                    g_cancelPairedUseRelease = true;
                    return true;
                }
                // Pre-quiescence, the bind window still open: cancel the press; the pile binds or
                // retires within seconds, and a re-press then routes normally.
                UE_LOGW("[GRAB-INTENT] CLIENT E-PRESS on UNBOUND native pile %p -- native use CANCELLED "
                        "(no eid owns it: bind-window; it binds or retires shortly, re-press then)",
                        aimedNative);
                g_cancelPairedUseRelease = true;
                return true;
            }
        }
        // The aim ray from the live view camera, the fallback for unbound proxy piles: the unit
        // forward vector the cone tests each proxy against, a generous 400 cm reach and a forgiving
        // cone of about 20 degrees.
        const ue_wrap::FVector  camLoc = ue_wrap::engine::GetCameraLocation();
        const ue_wrap::FRotator camRot = ue_wrap::engine::GetCameraRotation();
        const float d2r = 3.14159265f / 180.f;
        const float yaw = camRot.Yaw * d2r, pitch = camRot.Pitch * d2r;
        const float cp = std::cos(pitch);
        const ue_wrap::FVector camFwd{ cp * std::cos(yaw), cp * std::sin(yaw), std::sin(pitch) };
        const coop::element::ElementId eid =
            coop::trash_proxy::EidForAimedPileProxy(camLoc, camFwd, /*maxRangeCm=*/400.f, /*minDot=*/0.94f);
        if (eid == coop::element::kInvalidId)
            return false;  // not aiming at a mirrored pile -> let the native use run (devices, other interactions)
        // The pickup cue at the aimed proxy, as in the bound-native branch; the native use is
        // cancelled below, so the grabber would otherwise hear nothing.
        if (void* proxyActor = coop::trash_proxy::ProxyActorForEid(eid)) {
            coop::prop_sound::PlayUseClick(proxyActor);
            coop::prop_sound::PlayGrabSound(proxyActor);
        }
        UE_LOGI("[GRAB-INTENT] CLIENT E-PRESS aimed at pile proxy eid=%u (camera-ray cone) -> native use CANCELLED "
                "(no use_deny) + requesting grab from host", static_cast<unsigned>(eid));
        coop::trash_channel::SendGrabIntent(*s, static_cast<uint32_t>(eid));
        g_cancelPairedUseRelease = true;  // pair: the _42 release of this cancelled press dies too
        return true;  // handled: cancel the native InpActEvt_use dispatch (a bare proxy would deny)
    }

    // Host: aimed at a real chip pile, the looked-at actor. The host always runs the native use,
    // which grabs; this branch only logs, and every host path returns false.
    void* aimed = ue_wrap::engine::ReadMainPlayerLookAtActor(self);  // the pile (PRE-conversion, alive)
    if (!aimed || !ue_wrap::prop::IsChipPile(aimed)) return false;   // E-press not aimed at a pile -> native use runs
    // The host grab syncs without arming anything here: the blueprint's own deferred clump spawn
    // is caught at the thunk's grab direction, and the held edge adopts the spawned clump onto the
    // pile's eid and broadcasts the convert.
    coop::element::ElementId fwdEid = PT::GetPropElementIdForActor(aimed);
    coop::element::ElementId mirEid = coop::remote_prop::ResolveMirrorEidByActor(aimed);
    ue_wrap::engine::MainPlayerGrabState gs{};
    ue_wrap::engine::ReadMainPlayerGrabState(self, gs);
    const unsigned fwd = (fwdEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(fwdEid);
    const unsigned mir = (mirEid == coop::element::kInvalidId) ? 0u : static_cast<unsigned>(mirEid);
    UE_LOGI("[PILE] HOST E-PRESS on pile %p -- localEid=%u mirrorEid=%u %s (carry slots: grabbingActor=%p "
            "holdingActor=%p)",
            aimed, fwd, mir,
            (fwd != 0 || mir != 0) ? "[TRACKED -> grab will sync]"
                                   : "[UNTRACKED -> grab will NOT sync; tracking gap]",
            gs.grabbingActor, gs.holdingActor);
    return false;  // HOST: never cancel -- the native grab must run
}

// The left-button hard-throw bridge. The native throw input is the fire event, which throws
// the held prop with a camera-directed velocity, a separate input from use, which grabs and
// drops. The client holds no native clump (it renders a proxy), so the native fire finds
// nothing to throw and no-ops. Here, while carrying a clump proxy, a fire press requests a
// hard throw with the client's instantaneous camera forward, and the host applies the native
// velocity formula with the real clump mass. The native fire is left to run, since it no-ops.
// Client only; the host throws natively.
static void OnFirePre(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) return;          // the host throws natively; this is the client bridge
    const coop::element::ElementId carry = coop::trash_channel::ClientCarryEid();
    if (carry == coop::element::kInvalidId) return;          // not carrying a clump -> let native fire do its thing
    if (!coop::trash_proxy::ProxyActorForEid(carry)) {       // stale toggle (proxy gone) -> clear + let native run
        coop::trash_channel::ClearClientCarry(static_cast<uint32_t>(carry));
        return;
    }
    // The client's instantaneous camera-forward unit vector, the same derivation as the grab cone;
    // the native throw is camera-directed, so this is the authoritative aim.
    const ue_wrap::FRotator camRot = ue_wrap::engine::GetCameraRotation();
    const float d2r = 3.14159265f / 180.f;
    const float yaw = camRot.Yaw * d2r, pitch = camRot.Pitch * d2r;
    const float cp = std::cos(pitch);
    const ue_wrap::FVector camFwd{ cp * std::cos(yaw), cp * std::sin(yaw), std::sin(pitch) };
    UE_LOGI("[THROW-INTENT] CLIENT LMB(fire) while carrying eid=%u -> requesting NATIVE hard-throw from host "
            "(camFwd=%.2f,%.2f,%.2f)", static_cast<unsigned>(carry), camFwd.X, camFwd.Y, camFwd.Z);
    coop::trash_channel::SendThrowIntent(*s, static_cast<uint32_t>(carry),
                                         coop::net::throw_mode::kHardThrow, camFwd);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);  // re-cache every call (reconnect)

    if (g_grabObserverInstalled) return;
    void* cls = R::FindClass(P::name::MainPlayerClass);
    if (!cls) return;  // mainPlayer_C not loaded yet -> retry on the next world-gated Install
    void* fn = R::FindFunction(cls, P::name::MainPlayerUseInputEventFn);
    if (!fn) {
        UE_LOGW("trash_use_intercept: InpActEvt_use UFunction not found -- pile grabs cannot drop peer mirrors");
        g_grabObserverInstalled = true;  // permanent give-up (don't re-walk the class forever)
        return;
    }
    if (!GT::RegisterInterceptor(fn, &OnPileUseIntercept)) {
        UE_LOGW("trash_use_intercept: InpActEvt_use PRE interceptor register failed (table full?)");
        return;  // not latched -> retry next Install
    }
    g_grabObserverInstalled = true;
    UE_LOGI("trash_use_intercept: pile use INTERCEPTOR installed on InpActEvt_use (host: records the pending grab, "
            "always runs native; client: cancels the native use for a pile GRAB/THROW -> no native grab, no "
            "use_deny 'EHHH'; routes GrabIntent/ThrowIntent to the host; docs/piles/08)");

    // The use action's other two bindings, the second press and the release, also reach the deny:
    // the second press gets the press-condition suppressor, the release the pairing-only one.
    // Best-effort: a missing ordinal after a recook just means the deny returns on that seam.
    int denySeams = 0;
    if (void* uf38 = R::FindFunction(cls, P::name::MainPlayerUseInputEventFn38))
        if (GT::RegisterInterceptor(uf38, &OnPileUseDenySuppress)) ++denySeams;
    if (void* uf42 = R::FindFunction(cls, P::name::MainPlayerUseInputEventFn42R))
        if (GT::RegisterInterceptor(uf42, &OnPileUseReleaseSuppress)) ++denySeams;
    UE_LOGI("trash_use_intercept: use_deny suppressors installed on %d/2 extra 'use' seam(s) (_38 2nd-Pressed = press "
            "conditions; _42 Released = paired-with-cancelled-press only) -- a cancelled client E-press now dies "
            "on ALL its seams incl. its OWN release -> no 'EHHH' on grab OR drop",
            denySeams);

    // The hard-throw bridge observes both fire handlers, press and release: the carry gate and the
    // intent's optimistic carry-clear mean only the edge that finds a live carry acts.
    // Best-effort.
    int fireObs = 0;
    for (const wchar_t* fireFn : { P::name::MainPlayerFireInputEventFn58, P::name::MainPlayerFireInputEventFn59 }) {
        void* ff = R::FindFunction(cls, fireFn);
        if (ff && GT::RegisterPreObserver(ff, &OnFirePre)) ++fireObs;
    }
    UE_LOGI("trash_use_intercept: LMB hard-throw observer installed on %d/2 InpActEvt_fire handler(s) "
            "(client routes LMB-while-carrying -> native camera-driven ThrowIntent)", fireObs);
}

void OnDisconnect() {
    g_session.store(nullptr, std::memory_order_release);
    // Session teardown resets the gesture latch: a press cancelled just before the disconnect must
    // not eat a release in the next session.
    g_cancelPairedUseRelease = false;
}

}  // namespace coop::trash_use_intercept
