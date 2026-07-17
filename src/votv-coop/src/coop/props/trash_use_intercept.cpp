// coop/props/trash_use_intercept.cpp -- the CLIENT-grab bridge on the "use" (E) input.
//
// Extracted from trash_collect_sync.cpp (2026-07-07 modularization B1a). This is the
// InpActEvt_use PRE-interceptor family: on a CLIENT it cancels the native pile grab /
// carry-throw press (and the paired use_deny "EHHH") and routes GrabIntent/ThrowIntent
// to the host; on the HOST it always runs native. The re-pile / drop OBSERVERS and the
// held-item broadcast stay in trash_collect_sync -- this file owns ONLY the use-input
// interception + the LMB hard-throw bridge. Its Install/OnDisconnect are delegated to by
// trash_collect_sync::Install/OnDisconnect. Game thread only (input dispatch).
//
// docs/piles/08 (the client-grab full chain + the three "use" delegate seams _41/_38/_42
// and the _58/_59 fire seams) is the authority for the mechanism; the code moved verbatim.

#include "coop/props/trash_use_intercept.h"

// OnPileUseIntercept runs the CLIENT-grab full chain (docs/piles/08), so it pulls in the
// same broad dependency set trash_collect_sync did; the observer-only headers among these
// are harmless (unused-include) and can be trimmed later.
#include "coop/dev/spawn_order_probe.h"
#include "coop/element/element.h"
#include "coop/element/quiescence_drain.h"   // ArmGhostSweep (v106b)
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

// Own cached Session (the PRE callbacks are param-less UFunction hooks -> they must read a
// cached pointer). Set by Install (delegated from trash_collect_sync::Install), cleared by
// OnDisconnect. Game-thread read inside the callbacks. (trash_collect_sync keeps its OWN
// g_session for its observers -- each subsystem caches the session it needs; both are set
// from the one top-level Install call, so they never diverge.)
std::atomic<coop::net::Session*> g_session{nullptr};
bool g_grabObserverInstalled = false;  // InpActEvt_use PRE registration latch (process life)

// GESTURE-PAIRING latch: a CANCELLED client use-PRESS must cancel its paired IE_Released
// (_42) too -- the BP never saw the press, so its release handler would run against a
// press-less state and hit useAction's use_deny. Armed by every cancelled press seam,
// consumed by the next _42 fire. Game-thread only.
bool g_cancelPairedUseRelease = false;

// ---- The client-grab INTERCEPTOR on InpActEvt_use (docs/piles/08) ------------------------------------
//
// DECISIVE RE (votv-pile-grab-observable-hook-RE-2026-06-08-pass1.md): a chipPile grab is the E-press
// input action AmainPlayer_C::InpActEvt_use_..._41 -> icast(lookAtActor)->playerGrabbed (spawn clump +
// pickupObjectDirect(clump) + K2_DestroyActor(self)), all dispatched BP-internally (EX_LocalVirtualFunction
// -> ProcessInternal) and INVISIBLE to our ProcessEvent detour. InpActEvt_use itself IS ProcessEvent-visible
// (the native input->UFunction dispatch), and it is a thin stub: `CALL ExecuteUbergraph_mainPlayer(112867)`
// -- the whole use flow (the grab AND `useAction`, which plays `use_deny` on its fail branches) is that one
// event's ubergraph entry.
//
// This is a PRE-INTERCEPTOR (returns true -> the native InpActEvt_use dispatch is CANCELLED entirely, never
// reaching the ubergraph). On the CLIENT, whenever we route the press to the host (a pile GRAB or a carry
// THROW), we return true: the native grab AND its `use_deny` "EHHH" both die at the source (RE 2026-07-01 --
// nulling lookAtActor was a HALF-suppression: it killed the grab but `useAction` re-traces on its own and
// still hit a deny branch, playing use_deny in parallel with our synthesized pickup cue -- the suppression
// axis, not delivery). For any non-pile E-press (and for the HOST, which grabs natively) we return false ->
// the native use runs unchanged (devices, legit denies, the host's own grab). Puppets are unpossessed ->
// never process input -> this only ever fires for the local player.
// The "use" action has THREE delegate bindings (mainPlayer.json Export 483): _41 (IE_Pressed, the grab press we
// fully intercept below), _38 (a SECOND IE_Pressed), and _42 (IE_Released). All three reach useAction's use_deny
// "EHHH". Hooking only _41 left _38 firing on every E-press -> the native deny played in PARALLEL with our
// cancelled grab (the 19:06 regression -- RE 2026-07-01: the deny is on a SEPARATE seam than the one _41
// cancels). This SIDE-EFFECT-FREE suppressor covers the SECOND PRESS seam (_38): on a CLIENT pile interaction
// it only CANCELS the dispatch (kills the parallel deny) -- it does NOT send a grab/throw intent or play a cue
// (_41 is the sole author of those). Same recognition as OnPileUseIntercept's client branch: carrying, or aimed
// at ANY native pile (bound or unbound -- the 2026-07-03 twin-grab gate) / proxy pile. On cancel it ARMS the
// paired-release latch (a cancelled press's _42 must die
// with it -- see g_cancelPairedUseRelease; the RELEASE seam is pairing-only, NOT condition-derived).
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
            // ANY native pile aim -- bound OR unbound -- mirrors _41's shape (audit
            // 45bdb7ac W-2): _41 cancels the unbound press too now (the twin-grab
            // gate), so _38 must die with it or the native deny "EHHH" plays alone.
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

// The IE_Released seam (_42) is PAIRING-ONLY: cancel iff this release's press was cancelled (by _41's
// interceptor or _38's suppressor -- either arms the latch). NO condition re-derivation here: the press
// conditions are stale by release time (the throw press already cleared the carry; the post-drop aim
// wanders), and matching conditions against a NATIVE press's release would eat a legit drop (e.g.
// releasing a natively-held prop while aiming at a pile). One latch consume per gesture; user-rate.
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

    // CLIENT (Increment 2, the client-grab direction): the client has NO real actorChipPile_C -- every pile it
    // sees is an AStaticMeshActor PROXY (trash_proxy). Recognition is a CAMERA-RAY CONE (EidForAimedPileProxy):
    // the most-centered pile proxy within range of the aim ray. (The earlier "give the proxy collision so the
    // game's TraceTypeQuery1 trace hits it + read lookatActorCurrent" approach was DISPROVEN by the harness --
    // the worn pile mesh has no simple collision body, so the trace never hit; RULE 2, retired 2026-06-23. A
    // future garbageCollider-analog SHAPE component is the faithful fix for both occlusion-correct aim AND the
    // walk-through-pile movement-block.) NO suppress-native needed: the proxy fails every native int_player_C
    // cast on E-press, so the local grab no-ops on its own. The host re-validates + enforces one-hold-per-peer.
    if (s->role() != coop::net::Role::Host) {
        // TOGGLE: carrying a clump already? an E-press is a THROW (release it, regardless of aim). Else, if
        // aimed at a mirrored pile, an E-press is a GRAB request. (A player holds at most one trash clump.)
        const coop::element::ElementId carry = coop::trash_channel::ClientCarryEid();
        if (carry != coop::element::kInvalidId) {
            // Self-heal (belt-and-suspenders for the audit HIGH): if we think we're carrying but the proxy is
            // gone (a missed/dropped host abort edge), clear the stale toggle + fall through to a fresh grab
            // rather than throwing a dead eid forever.
            if (!coop::trash_proxy::ProxyActorForEid(carry)) {
                UE_LOGW("[THROW-INTENT] CLIENT carry eid=%u has NO live proxy -- stale toggle, clearing + falling "
                        "through to grab", static_cast<unsigned>(carry));
                coop::trash_channel::ClearClientCarry(static_cast<uint32_t>(carry));
            } else {
                UE_LOGI("[THROW-INTENT] CLIENT E-PRESS while carrying eid=%u -> requesting release(E-drop) from host "
                        "(native use CANCELLED -- no use_deny)", static_cast<unsigned>(carry));
                coop::trash_channel::SendThrowIntent(*s, static_cast<uint32_t>(carry),
                                                     coop::net::throw_mode::kRelease, ue_wrap::FVector{});
                g_cancelPairedUseRelease = true;  // this press's _42 release must die with it (deny lives there too)
                return true;  // handled: cancel the native InpActEvt_use (the client holds no native clump -> would deny)
            }
        }
        // (X) native-authoritative GRAB recognition (items 5+6). A save-loaded pile the client BOUND as the
        // host-range mirror (save_identity_bind) is a REAL actorChipPile_C native -> it IS the game's own
        // lookAtActor (the int_player_C interaction trace), occlusion-correct + collision-blocked, unlike a
        // bare proxy. PREFER it: read lookAtActor; if it's a bound native pile, CANCEL the whole native
        // InpActEvt_use dispatch (return true below) -- the ubergraph's grab (icast(lookAtActor)->playerGrabbed)
        // AND `useAction`'s `use_deny` both die at the source. (RULE 2, 2026-07-01: the old NULL-lookAtActor
        // suppression is RETIRED -- it was a HALF-suppression that killed the grab but let useAction re-trace
        // and play use_deny "EHHH" in parallel; cancelling the dispatch subsumes it, no field mutation.) Route
        // the grab via GrabIntent (host-auth, the sole author of the shared mutation; the host runs
        // playerGrabbed on the requester's puppet). The camera-cone below stays only for UNBOUND proxy piles.
        {
            void* aimedNative = ue_wrap::engine::ReadMainPlayerLookAtActor(self);
            if (aimedNative && ue_wrap::prop::IsChipPile(aimedNative)) {
                if (PT::IsBoundMirrorNative(aimedNative)) {
                    const coop::element::ElementId beid = coop::remote_prop::ResolveMirrorEidByActor(aimedNative);
                    if (beid != coop::element::kInvalidId) {
                        // Pickup cue: the native grab (which plays the `use` click + material soft cue in its
                        // playerGrabbed chain) is CANCELLED for this press, so synthesize the same feedback locally
                        // at the pile -- the grabber hears its own grab (the host hears it natively via playerGrabbed
                        // on the puppet; observers via ResolveAndStartDrive).
                        coop::prop_sound::PlayUseClick(aimedNative);
                        coop::prop_sound::PlayGrabSound(aimedNative);
                        // [PILE-TYPE probe 2026-07-04] pos+chipType of the pile the CLIENT is aiming at --
                        // the host's "[GRAB-INTENT] EXEC ... at(...) chipType=" for the same eid must match;
                        // a mismatch names the ordinal identity misalignment (the 18:45 wrong-type morph).
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
                // UNBOUND native pile (2026-07-03, the 11:49 client-local clump chain): a REAL
                // actorChipPile_C in the client's aim that NO eid owns -- a HOST-VACATE twin the
                // sweep has not retired yet (the 11:48:51 seed: twin@old grabbed 7 s before the
                // sweep pass), a mid-bind-window native, or a descendant of an earlier native
                // grab. Letting the native use run here is how the self-perpetuating client-only
                // chain seeds: native grab -> local prop_garbageClump_C -> native land -> another
                // unbound pile, all invisible to the host (and a grabbed twin becomes a CLUMP the
                // pile sweep can no longer retire). On a connected client EVERY pile interaction
                // is host-authoritative -- a pile no eid owns must not be interactable at all.
                if (coop::join_membership_sweep::HasLoadTailQuiesced()) {
                    // v106b (RULE 2 -- user 2026-07-07: "why wait for an E-press per ghost"): the
                    // v106 inline one-pile retire is RETIRED. The wholesale owner is the reconcile
                    // pass's GHOST-RETIRE tail (save_identity_bind::BindUnboundReCreates), which
                    // re-binds what a map key still claims and retires EVERY provably identity-less
                    // native at once. This press is positive evidence a ghost slipped the event
                    // triggers -> ARM the pass (it runs within the 250ms reconcile debounce) and
                    // cancel the press (a native no eid owns must not be interactable).
                    UE_LOGW("[GRAB-INTENT] CLIENT E-PRESS on UNBOUND native pile %p POST-quiescence "
                            "-- identity-less ghost -> arming the wholesale GHOST-RETIRE reconcile "
                            "(all ghosts adjudicated at once)", aimedNative);
                    coop::element::quiescence_drain::ArmGhostSweep();
                    g_cancelPairedUseRelease = true;
                    return true;
                }
                // Pre-quiescence: the bind window is still open -- CANCEL the press (the
                // pile binds or retires within seconds; a re-press then routes normally).
                UE_LOGW("[GRAB-INTENT] CLIENT E-PRESS on UNBOUND native pile %p -- native use CANCELLED "
                        "(no eid owns it: bind-window; it binds or retires shortly, re-press then)",
                        aimedNative);
                g_cancelPairedUseRelease = true;
                return true;
            }
        }
        // Aim ray from the live view camera (FALLBACK -- unbound proxy piles only). Forward from the camera
        // rotation (deg->rad): the unit vector the cone tests each pile proxy against. maxRange 400 cm (a
        // generous reach), minDot 0.94 (~20 deg cone -- forgiving so a slightly-off aim still grabs).
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
        // Pickup cue at the aimed proxy pile (same as the bound-native branch -- the native use is CANCELLED
        // below, so the grabber hears nothing without this). ProxyActorForEid gives the proxy actor.
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

    // HOST: aim at a REAL chipPile (it implements int_player_C -> it IS lookAtActor). The host ALWAYS runs the
    // native use (it grabs natively) -- this branch only records the pending grab; every host path returns false.
    void* aimed = ue_wrap::engine::ReadMainPlayerLookAtActor(self);  // the pile (PRE-conversion, alive)
    if (!aimed || !ue_wrap::prop::IsChipPile(aimed)) return false;   // E-press not aimed at a pile -> native use runs
    // The HOST grab syncs WITHOUT arming anything here -- the BP's own BeginDeferred(clump) is caught at
    // the thunk's GRAB direction (the v106 birth certificate); the held-edge adopts the spawned clump onto
    // the pile's eid + broadcasts PropConvert{ToClump}. This branch logs diagnostics only.
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
    // (v106: the self-seed + pre-grab-xform record + pending-grab arm MOVED to the
    // BeginDeferred thunk's GRAB direction -- ONE owner at the seam the clump is
    // actually born, covering use-HOLD grabs this press seam never sees. This host
    // branch is diagnostics-only now.)
    return false;  // HOST: never cancel -- the native grab must run
}

// (X) LMB hard-throw bridge (2026-06-26). The NATIVE throw input is InpActEvt_fire (LMB) -> throwHoldingProp
// -> throwShit(velocity_fromCamera) -- a SEPARATE input from use/E (which is grab + the E-drop release). The
// client holds NO native clump (host-authoritative; it renders a proxy), so the native fire's throwHoldingProp
// finds no grabbing_component and no-ops -- and we never routed it, hence LMB did nothing (the 12:35 symptom).
// Here: while carrying a clump proxy, an LMB press requests a hardThrow with the client's INSTANTANEOUS camera-
// forward (Variant B -- the throw flies exactly where the client looks at the press, native-faithful); the host
// applies the native velocity formula (cameraFwd * 15000/max(mass,10) + puppetVel, NO cap) with the real clump
// mass. The native fire is left to run (it no-ops with no held native prop; nothing to suppress -- there is no
// single lookAtActor-style field, and the client holds no equipment while carrying a clump). Client-only: the
// host throws natively via its own InpActEvt_fire. E-drop (the #3 path) is untouched.
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
    // The client's instantaneous camera-forward unit vector (same derivation as the grab cone). The native
    // throw is camera-DIRECTED, so this is the authoritative aim; the host applies the mass-scaled speed.
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

    // The "use" action's OTHER two bindings (_38 = 2nd IE_Pressed, _42 = IE_Released) also reach useAction's
    // use_deny -- hooking only _41 left the client hearing "EHHH" on every E-press (parallel deny). _38 gets the
    // press-condition suppressor; _42 gets the PAIRING-ONLY release suppressor. Best-effort (a missing ordinal
    // after a BP recook just means the deny returns on that seam -- sdk_check flags it).
    int denySeams = 0;
    if (void* uf38 = R::FindFunction(cls, P::name::MainPlayerUseInputEventFn38))
        if (GT::RegisterInterceptor(uf38, &OnPileUseDenySuppress)) ++denySeams;
    if (void* uf42 = R::FindFunction(cls, P::name::MainPlayerUseInputEventFn42R))
        if (GT::RegisterInterceptor(uf42, &OnPileUseReleaseSuppress)) ++denySeams;
    UE_LOGI("trash_use_intercept: use_deny suppressors installed on %d/2 extra 'use' seam(s) (_38 2nd-Pressed = press "
            "conditions; _42 Released = paired-with-cancelled-press only) -- a cancelled client E-press now dies "
            "on ALL its seams incl. its OWN release -> no 'EHHH' on grab OR drop",
            denySeams);

    // LMB hard-throw bridge: PRE-observe BOTH fire handlers (_58/_59 = press/release). The OnFirePre gate on
    // ClientCarryEid + SendThrowIntent's optimistic carry-clear mean only the press edge that finds a live
    // carry acts; the other no-ops. Best-effort (a missing handler just means no LMB-throw on that edge).
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
    // Session teardown resets the gesture latch: a press cancelled just before the disconnect
    // must not eat a release dispatched in the next session.
    g_cancelPairedUseRelease = false;
}

}  // namespace coop::trash_use_intercept
