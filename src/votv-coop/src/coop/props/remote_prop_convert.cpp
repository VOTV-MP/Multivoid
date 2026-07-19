// coop/props/remote_prop_convert.cpp -- the PropConvert receiver (the pile<->clump bind-model
// re-skin of eid E in place).
//
// EXTRACTED from remote_prop.cpp 2026-07-19 (s28 modular cut: the TU was 1180 LOC, past the 800
// soft cap; the convert path is a distinct concept -- the same anti-smear lane split as
// remote_prop_spawn (M-1 2026-05-29) and remote_prop_destroy (2026-06-30)). OnConvert stays
// declared in remote_prop.h; the body is verbatim.
//
// Game-thread only (same contract as the rest of remote_prop).

#include "coop/props/remote_prop.h"
#include "remote_prop_internal.h"  // ResolveLiveActorByEid + DestroyEchoSuppressed (impl-private seam)

#include "coop/element/quiescence_drain.h"   // ArmPendingSaveTimeTwin (capture-only)
#include "coop/props/native_pile_mirror.h"   // Materialize / RepositionBoundNative (nativize a landed pile)
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_sound.h"
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/trash_channel.h"
#include "coop/props/trash_clump_pose_stream.h"
#include "coop/props/trash_proxy.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <string>

namespace coop::remote_prop {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

void* OnConvert(const coop::net::PropConvertPayload& payload, void* localPlayer, int senderSlot) {
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::OnConvert)");
    // docs/piles/08 host-authoritative trash channel -- bind-model re-skin of eid E in place (oldEid ==
    // newEid == E). NO fresh eid, NO second entity. Resolve our current rendering of E (the SYNC-MIRROR
    // the host expects us to have), spawn the NEW rendering bound to the SAME E, rebind, echo-destroy old.
    const uint32_t E = payload.newEid;
    const bool wantClump = (payload.kind == coop::net::propconvert_kind::kToClump);
    const char* edge = wantClump ? "GRAB(pile->clump)" : "LAND(clump->pile)";
    if (E == 0u || E == coop::element::kInvalidId) {
        UE_LOGW("[PILE] CLIENT recv convert %s -- INVALID eid E=%u, dropping (no entity to re-skin)", edge, E);
        return nullptr;
    }
    // [diag 2026-06-21] RECEPTION marker -- logged BEFORE the ctx-gate so "the ToClump convert ARRIVED"
    // is explicit (its absence in the client log = the convert never reached us; e.g. the join race).
    // Shows known (our current ctx for E) + whether we have a proxy to re-skin. Event-driven, not hot.
    UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u known=%u isProxy=%d -- RECEPTION (pre-gate)",
            edge, E, static_cast<unsigned>(payload.ctx),
            static_cast<unsigned>(coop::trash_channel::CtxForEid(E)),
            coop::trash_proxy::IsProxy(E) ? 1 : 0);
    // docs/piles/08: adopt the host's authoritative sync-time-context for E, and DROP a stale/out-of-order
    // convert (a duplicate, or one older than a transition we already applied). ctx==0 = legacy/non-trash.
    if (!coop::trash_channel::AdoptInboundConvertCtx(E, payload.ctx)) return nullptr;
    // docs/piles/09 (4th mirror-identity instance): a kToPile LAND carrying a save-time key means the host
    // self-seeded this eid at an in-window grab + stamped its PRE-GRAB position. Arm a pending save-time twin
    // so the bracket-independent quiescence sweep (SweepReconcileSaveTimeTwins) retires our stale native@old
    // -- the L1 cure, keyed at the grab edge so it survives the in-window eid change. Done here (after the ctx
    // gate, before the proxy branch) so it fires whether or not the ToClump created a proxy first.
    if (!wantClump && payload.hasMatchPos)
        coop::element::quiescence_drain::ArmPendingSaveTimeTwin(
            E, ue_wrap::FVector{payload.matchX, payload.matchY, payload.matchZ}, payload.chipType);
    // PROXY PATH (phase 1, THE dup fix): if E's mirror is our host-authoritative trash proxy, re-skin it
    // IN PLACE (pile<->clump). The eid->actor binding is NEVER touched -> no spawn-fresh, no orphan, no
    // dup; and a rooted proxy never goes stale, so the "mirror NOT-FOUND -> spawn fresh" path that caused
    // the dup is structurally unreachable. Non-proxy converts (Aprop_C / kerfur, or a rare trash convert
    // that beat its OnSpawn) keep the legacy spawn+rebind path below.
    if (coop::trash_proxy::IsProxy(E)) {
        // INCREMENT 2 (2026-07-01) -- ToPile LAND on a proxy = the carried CLUMP proxy is settling into a
        // resting PILE. NATIVIZE it: rebind E in place onto a rooted real actorChipPile_C native (which IS
        // int_player_C -> native hover GUI + native random rotation + collision + occlusion, all free), then
        // retire the proxy ACTOR (its Element stays, now owned by the native). This is the exact INVERSE of
        // the ToClump morph hand-off (native -> clump proxy, ~:907): rebind-in-place FIRST, destroy the old
        // ACTOR after, so E's Element never leaves the manager (the destroy-before-load hazard the kerfur arc
        // was bitten by). payload.pileClass on a LAND = ClassNameOf(the re-piled actorChipPile_C) (trash_
        // channel.cpp:169), so Materialize spawns the pile class.
        if (!wantClump) {
            coop::trash_clump_pose_stream::ClearDriveForEid(E);   // stop the carry pose stream at the land
            const std::wstring pileCls = remote_prop_spawn::ClassNameToWString(payload.pileClass);
            void* native = coop::native_pile_mirror::Materialize(
                E, pileCls, payload.chipType,
                ue_wrap::FVector{payload.locX, payload.locY, payload.locZ},
                ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll},  // host mesh-world rotation (consumed)
                ue_wrap::FVector{payload.scaleX, payload.scaleY, payload.scaleZ},
                senderSlot, /*skipBind=*/false, /*rebindInPlace=*/true);
            if (native) {
                coop::trash_proxy::RetireProxyActorOnly(E);       // destroy the proxy actor; Element KEPT (rebound to native)
                // Materialize applied the host's mesh-world rotation to the native's mesh component (host->client,
                // same axis as chipType) + positioned it via SpawnActor(loc). Read the native's world location
                // back + log drift vs the host payload (the automated "client renders the host pose" gate).
                const ue_wrap::FVector got = E::GetActorLocation(native);
                const float dx = got.X - payload.locX, dy = got.Y - payload.locY, dz = got.Z - payload.locZ;
                UE_LOGI("[PILE] CLIENT ToPile LAND eid=%u ctx=%u -> NATIVIZED native=%p at (%.1f,%.1f,%.1f) "
                        "host=(%.1f,%.1f,%.1f) drift=%.2fcm [native hover GUI + rotation + collision -- proxy retired]",
                        E, static_cast<unsigned>(payload.ctx), native, got.X, got.Y, got.Z,
                        payload.locX, payload.locY, payload.locZ, std::sqrt(dx * dx + dy * dy + dz * dz));
                coop::prop_sound::PlayLandSound(native);  // host-authoritative LAND event: the material impact thud
                coop::trash_channel::NoteClientConvertObserved(E, false);
                return native;
            }
            // Materialize FAILED (pile class not loaded -- defensive) -> fall back to the proxy re-skin so the
            // pile still lands (no-regression): re-skin in place + snap to the authoritative LANDED rest.
            void* proxy = coop::trash_proxy::ReskinProxy(E, payload.chipType, /*isClump=*/false,
                                                         ue_wrap::FVector{payload.scaleX, payload.scaleY, payload.scaleZ});
            if (proxy) {
                ClearAnyDriveFor(proxy);
                E::SetActorLocation(proxy, ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
                E::SetActorRotation(proxy, ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
                UE_LOGW("[PILE] CLIENT ToPile LAND eid=%u -- native materialize FAILED, fell back to proxy re-skin "
                        "(pile class '%ls' not loaded?)", E, pileCls.c_str());
                coop::prop_sound::PlayLandSound(proxy);  // land thud even on the fallback path
            }
            coop::trash_channel::NoteClientConvertObserved(E, false);
            return proxy;
        }
        // ToClump GRAB on a proxy (an un-nativized pile proxy from the join bracket / convert-beat-spawn being
        // carried) -> re-skin to the carried clump IN PLACE. NO teleport / drive-reset: the live carry pose
        // stream drives its position with pose+lerp (2026-06-22 carry fix -- snapping on every grab convert was
        // the 2fps teleport). A native pile grabbed goes to the ~:907 morph hand-off instead (IsProxy=false).
        void* proxy = coop::trash_proxy::ReskinProxy(E, payload.chipType, /*isClump=*/true,
                                                     ue_wrap::FVector{payload.scaleX, payload.scaleY, payload.scaleZ});
        UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u -> PROXY re-skinned IN PLACE to CLUMP chipType=%u "
                "[SYNC-MIRROR OK -- no spawn-fresh, no dup]",
                edge, E, static_cast<unsigned>(payload.ctx), static_cast<unsigned>(payload.chipType));
        // v85: reconcile the CLIENT carry-state toggle -- a ToClump matching our pending grab confirms the carry.
        coop::trash_channel::NoteClientConvertObserved(E, true);
        return proxy;
    }
    // HIGH-1: a trash convert that BEAT its OnSpawn (no proxy for E yet). Spawn the proxy HERE in the
    // unambiguous wantClump form (the convert's authoritative direction) and bind E, rather than falling
    // to the legacy path below -- that routes through remote_prop_spawn::OnSpawn, which derives the
    // proxy's form from the spawn CLASS (a fragile coupling). A trailing real PropSpawn then hits
    // SpawnProxy's convergence branch, which (by design) does NOT re-skin, so the form stays correct.
    // Non-trash converts (Aprop_C / kerfur) fall through to the legacy spawn+rebind path below.
    if (coop::trash_proxy::IsTrashProxyClass(remote_prop_spawn::ClassNameToWString(payload.pileClass))) {
        // (X) morph hand-off: if E currently resolves to a BOUND save-loaded NATIVE pile (kept alive by the
        // native-authoritative guards) and this is a ToClump (grab), the pile is leaving the save-loaded
        // resting state to become a host-RUNTIME carried clump -- which has no native. The native can't morph
        // to a clump (different class) and IsProxy(E) is false (it was never a proxy), so the plain SpawnProxy
        // + RegisterPropMirror below would REJECT against the still-live native (rebindInPlace=false -> the
        // duplicate-eid guard) and the pile would never convert. Hand the eid to the runtime proxy: re-skin the
        // Element onto the fresh clump proxy (rebindInPlace) + retire the orphaned native (clear its bound-
        // mirror mark via UnmarkKnownKeyedProp, then destroy). The design's gap-(b) hand-off: a carried clump
        // is host-runtime, not a save-loaded native. Gated on wantClump so a spurious non-grab convert on a
        // bound native falls to the safe reject path (never silently swaps a live native pile for a proxy).
        void* boundNative = ResolveLiveActorByEid(E);
        const bool morphBoundNative =
            wantClump && boundNative && coop::prop_element_tracker::IsBoundMirrorNative(boundNative);
        ue_wrap::FVector  loc{payload.locX, payload.locY, payload.locZ};
        ue_wrap::FRotator rot{payload.rotPitch, payload.rotYaw, payload.rotRoll};
        // (X-LAND) the missing symmetric HALF of the GRAB morph hand-off above. E is ALREADY bound to a
        // save-loaded NATIVE pile (adopted at join) and this is a re-pile LAND -- the native IS already the
        // correct resting form. CLAIM it: reposition + re-skin to the host's landed transform, then RETURN --
        // SUPPRESSING the SpawnProxy entirely. If we spawned, RegisterPropMirror(rebindInPlace=false) would
        // REJECT against the still-bound native (identity_create.cpp:111) -> a proxy in g_proxies + the Element
        // still bound to the native = a split-tracked DUP (two live actors, one eid) the save-time-twin sweep
        // can't see (it skips IsBoundMirrorNative). Suppress-the-spawn (NOT spawn-then-clean) avoids the same
        // destroy-before-load hazard the whole track fights. This dup was a STRUCTURAL hole from this branch's
        // birth (GRAB had morphBoundNative, LAND never did), not a nativization regression.
        // [[feedback-recurring-bug-is-architectural]]
        if (!wantClump && boundNative && coop::prop_element_tracker::IsBoundMirrorNative(boundNative)) {
            coop::native_pile_mirror::RepositionBoundNative(
                boundNative, payload.chipType, loc, rot,
                ue_wrap::FVector{payload.scaleX, payload.scaleY, payload.scaleZ});
            const ue_wrap::FVector got = E::GetActorLocation(boundNative);
            const float dx = got.X - loc.X, dy = got.Y - loc.Y, dz = got.Z - loc.Z;
            UE_LOGI("[PILE] CLIENT ToPile LAND eid=%u ctx=%u -> CLAIMED bound save-loaded native=%p "
                    "repositioned to (%.1f,%.1f,%.1f) host=(%.1f,%.1f,%.1f) drift=%.2fcm "
                    "[create-edge reconcile -- no parallel spawn, no dup]",
                    E, static_cast<unsigned>(payload.ctx), boundNative,
                    got.X, got.Y, got.Z, loc.X, loc.Y, loc.Z, std::sqrt(dx * dx + dy * dy + dz * dz));
            coop::prop_sound::PlayLandSound(boundNative);  // land thud on the claim-reuse edge
            coop::trash_channel::NoteClientConvertObserved(E, false);
            return boundNative;
        }
        void* proxy = coop::trash_proxy::SpawnProxy(E, payload.chipType, /*isClump=*/wantClump,
                                                    senderSlot, loc, rot,
                                                    ue_wrap::FVector{payload.scaleX, payload.scaleY, payload.scaleZ});
        if (proxy) {
            RegisterPropMirror(E, proxy, L"", R::ClassNameOf(proxy), senderSlot, /*rebindInPlace=*/morphBoundNative);
            if (morphBoundNative) {
                coop::prop_element_tracker::UnmarkKnownKeyedProp(boundNative);  // drop the bound-mirror mark
                R::RemoveFromRoot(boundNative);                                 // un-root a nativized runtime pile (no-op on a save-loaded native) -- else a rooted PendingKill leaks
                E::DestroyActor(boundNative);                                   // retire the orphaned native pile
                UE_LOGI("[PILE] CLIENT convert %s eid=%u -> bound save-loaded NATIVE pile GRABBED -> handed to "
                        "runtime clump proxy=%p (native retired; native-authoritative hand-off)", edge, E, proxy);
            } else {
                UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u -> proxy SPAWNED %s (convert beat its spawn) "
                        "[SYNC-MIRROR OK -- no dup]", edge, E, static_cast<unsigned>(payload.ctx),
                        wantClump ? "CLUMP" : "PILE");
            }
        } else {
            UE_LOGW("[PILE] CLIENT recv convert %s eid=%u -- proxy spawn-on-convert FAILED (DESYNC)", edge, E);
        }
        // b2 (2026-06-26, docs/piles/09 positional): the convert-beat-spawn LAND case (#2 pile-move-in-window:
        // host grabbed+moved a save-loaded pile mid-join, the ToPile convert reached us before any OnSpawn, so
        // we SpawnProxy HERE). SpawnProxy is given loc=the MOVED rest position, so per the code the proxy is
        // already there -- but unlike the re-skin path (~:1018) this branch never read the transform back, so a
        // SpawnActor-transform that silently no-op'd (a static-mobility quirk) was INVISIBLE in the log and the
        // pile could render at the OLD spot until interaction reconverged it. Mirror the re-skin snap: force the
        // moved transform explicitly (belt-and-suspenders) AND read it back + log drift. drift~0 -> the proxy was
        // already at moved (any user-seen divergence is physics-settle/perception -> needs b2.1 host-streams-
        // settled-pos, not this); drift>0 -> the spawn transform had NOT taken and this snap fixed it. ToPile
        // only (a LAND has a final rest pose); ToClump (carry) is pose-stream-driven, never snapped.
        if (proxy && !wantClump) {
            E::SetActorLocation(proxy, ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
            E::SetActorRotation(proxy, ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
            const ue_wrap::FVector  got  = E::GetActorLocation(proxy);
            const ue_wrap::FRotator gotR = E::GetActorRotation(proxy);
            const float dx = got.X - payload.locX, dy = got.Y - payload.locY, dz = got.Z - payload.locZ;
            UE_LOGI("[PILE] CLIENT ToPile SNAP(spawn-on-convert) eid=%u applied=(%.1f,%.1f,%.1f) "
                    "host=(%.1f,%.1f,%.1f) drift=%.2fcm | rot applied=(%.1f,%.1f,%.1f) host=(%.1f,%.1f,%.1f)",
                    E, got.X, got.Y, got.Z, payload.locX, payload.locY, payload.locZ,
                    std::sqrt(dx * dx + dy * dy + dz * dz),
                    gotR.Pitch, gotR.Yaw, gotR.Roll, payload.rotPitch, payload.rotYaw, payload.rotRoll);
            coop::prop_sound::PlayLandSound(proxy);  // land thud on the convert-beat-spawn LAND
        }
        // #3 release-path fix (2026-06-26): the IsProxy branch confirms the client carry-state toggle on a
        // ToClump matching our pending grab (line ~1039); THIS branch (a client grab of a bound save-loaded
        // NATIVE pile -> morph hand-off, OR a convert that beat its spawn) must honor the SAME contract, else
        // g_clientCarry never arms -> the THROW toggle never fires -> the carried pile sticks forever + the
        // host slot stays latched (every later grab DENIED). carry-confirm = "received a ToClump for our
        // pending grab", true for BOTH trash branches; the morph branch simply omitted it. ToPile (land)
        // re-enters via the IsProxy branch (the native is a proxy by then) -> symmetric clear. Host no-op
        // (g_clientPendingGrab==0). Spawn-fail (proxy==null) self-heals via the toggle's stale-proxy guard.
        coop::trash_channel::NoteClientConvertObserved(E, wantClump);
        return proxy;
    }
    void* cur = ResolveLiveActorByEid(E);
    const bool hadMirror = (cur != nullptr);   // was a SYNC-MIRROR of E present to re-skin?
    // Idempotency: if our rendering of E already matches the target edge (an echo, a duplicate, or a
    // grab-race loser's convert arriving after we already rendered the same class), this is a no-op
    // -- the winner's held-pose stream drives it. Prevents a spurious re-spawn + actor churn.
    if (cur && ue_wrap::prop::IsGarbageClump(cur) == wantClump) {
        UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u -- already %s, idempotent no-op (echo/dup)",
                edge, E, static_cast<unsigned>(payload.ctx), wantClump ? "clump" : "pile");
        return cur;
    }
    // Spawn the NEW rendering bound to E. ToClump -> a kinematic clump (physFlags=0 -> not simulating;
    // OnSpawn also disarms the clump's self-convert hit-notify) the held-pose stream then drives;
    // ToPile -> a settled, grabbable pile (physFlags=0 -> resting, QueryAndPhysics kept). skipBind:
    // OnConvert binds E explicitly below; fromConvert: skip the eid-dedup so the still-live OLD rendering
    // of E doesn't converge the spawn.
    coop::net::PropSpawnPayload p{};
    p.className = payload.pileClass;
    p.key.len   = 0;                         // chipPile/clump are eid-only (Key=None)
    p.locX = payload.locX; p.locY = payload.locY; p.locZ = payload.locZ;
    p.rotPitch = payload.rotPitch; p.rotYaw = payload.rotYaw; p.rotRoll = payload.rotRoll;
    p.scaleX = p.scaleY = p.scaleZ = 1.f;
    p.physFlags = 0;
    p.chipType = payload.chipType;
    p.initLinVelX = p.initLinVelY = p.initLinVelZ = 0.f;
    p.initAngVelX = p.initAngVelY = p.initAngVelZ = 0.f;
    p.elementId = E;
    void* next = nullptr;
    remote_prop_spawn::OnSpawn(p, senderSlot, localPlayer, /*fromConvert=*/true,
                              /*deferKerfur=*/true, &next, /*skipBind=*/true);
    if (!next) {
        UE_LOGW("[PILE] CLIENT recv convert %s eid=%u -- re-skin spawn FAILED, E left as the old actor (DESYNC)",
                edge, E);
        return nullptr;
    }
    // Rebind E onto the new rendering. RegisterPropMirror is the single rebind entry point -- it routes
    // on the Element's authoritative IsMirror() flag (a MIRROR -> SetActor; a host's OWN local element ->
    // RebindLocalElementActor, keeping the unified actor->eid reverse consistent), so this is correct even
    // when cur is momentarily dead (findings 3/6/7/15 -- the old eIsLocal-from-live-cur guess silently
    // mis-routed a local element through the mirror path and desynced the actor->eid reverse).
    const std::wstring cls = R::ClassNameOf(next);
    RegisterPropMirror(E, next, L"", cls, senderSlot, /*rebindInPlace=*/true);
    // Echo-destroy the OLD rendering AFTER the rebind (so E always resolves to a live actor -- no
    // flicker where E points at nothing). MarkIncomingDestroy suppresses our own destroy-broadcast.
    if (cur && cur != next) {
        // Echo-suppressed retire of the old rendering (ClearAnyDriveFor + K2_DestroyActor). Routed through
        // remote_prop_destroy.cpp so this TU no longer holds the cached destroy fn. [[feedback-one-owner-order-axis]]
        DestroyEchoSuppressed(cur);
    }
    UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u -> mirror %s, re-skinned to %s cls='%ls' at "
            "(%.1f,%.1f,%.1f) variant=%u%s",
            edge, E, static_cast<unsigned>(payload.ctx),
            hadMirror ? "FOUND" : "NOT-FOUND",
            wantClump ? "CLUMP" : "PILE", cls.c_str(),
            payload.locX, payload.locY, payload.locZ, static_cast<unsigned>(payload.chipType),
            hadMirror ? " [SYNC-MIRROR OK]"
                      : " [WARN: no local mirror of E existed -- spawned fresh; was desynced pre-convert]");
    return next;
}

}  // namespace coop::remote_prop
