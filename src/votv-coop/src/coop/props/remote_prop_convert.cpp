// coop/props/remote_prop_convert.cpp -- the PropConvert receiver: the pile-clump re-skin of
// eid E in place. Declared in remote_prop.h; game thread only, as the rest of remote_prop.

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
    // The bind model: oldEid == newEid == E, no fresh eid, no second entity. Resolve our current
    // rendering of E, spawn or re-skin the new rendering bound to the same E, rebind, then
    // echo-destroy the old one.
    const uint32_t E = payload.newEid;
    const bool wantClump = (payload.kind == coop::net::propconvert_kind::kToClump);
    const char* edge = wantClump ? "GRAB(pile->clump)" : "LAND(clump->pile)";
    if (E == 0u || E == coop::element::kInvalidId) {
        UE_LOGW("[PILE] CLIENT recv convert %s -- INVALID eid E=%u, dropping (no entity to re-skin)", edge, E);
        return nullptr;
    }
    // Logged before the context gate, so a convert that arrived and was dropped is still visible
    // in the client log. Event-driven, not hot.
    UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u known=%u isProxy=%d -- RECEPTION (pre-gate)",
            edge, E, static_cast<unsigned>(payload.ctx),
            static_cast<unsigned>(coop::trash_channel::CtxForEid(E)),
            coop::trash_proxy::IsProxy(E) ? 1 : 0);
    // Adopt the host's context for E and drop a stale or out-of-order convert (a duplicate, or
    // one older than a transition already applied).
    if (!coop::trash_channel::AdoptInboundConvertCtx(E, payload.ctx)) return nullptr;
    // A land carrying a save-time key means the host self-seeded this eid at an in-window grab
    // and stamped the pile's pre-grab position. Arm a pending save-time twin so the quiescence
    // sweep retires our stale native at the old position; here, after the context gate and before
    // the proxy branch, so it fires whether or not a grab created a proxy first.
    if (!wantClump && payload.hasMatchPos)
        coop::element::quiescence_drain::ArmPendingSaveTimeTwin(
            E, ue_wrap::FVector{payload.matchX, payload.matchY, payload.matchZ}, payload.chipType);
    // The proxy path: when E's mirror is our host-authoritative trash proxy, re-skin it in place.
    // The eid-to-actor binding is never touched, so there is no fresh spawn, no orphan and no
    // duplicate; a rooted proxy never goes stale, so the mirror-not-found spawn path below is
    // unreachable for it.
    if (coop::trash_proxy::IsProxy(E)) {
        // A land on a proxy: the carried clump proxy is settling into a resting pile. Nativize it:
        // rebind E in place onto a rooted real pile native (which brings the native hover GUI,
        // rotation, collision and occlusion), then retire the proxy actor; its element stays, now
        // owned by the native. The inverse of the grab hand-off below: rebind first, destroy the
        // old actor after, so E's element never leaves the manager. The payload's class on a land
        // is the re-piled pile's class, so Materialize spawns the pile class.
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
                // Materialize applied the host's mesh rotation to the native's mesh component and
                // positioned it at spawn. Read the location back and log the drift against the host
                // payload.
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
            // Materialize failed (the pile class not loaded): fall back to the proxy re-skin so the
            // pile still lands, snapped to the authoritative rest.
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
        // A grab on a proxy (an un-nativized pile proxy being carried): re-skin to the carried
        // clump in place. No teleport or drive reset; the live carry pose stream drives its
        // position with pose and lerp, and snapping on every grab convert stuttered the carry. A
        // grabbed native pile goes to the morph hand-off below instead (it is not a proxy).
        void* proxy = coop::trash_proxy::ReskinProxy(E, payload.chipType, /*isClump=*/true,
                                                     ue_wrap::FVector{payload.scaleX, payload.scaleY, payload.scaleZ});
        UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u -> PROXY re-skinned IN PLACE to CLUMP chipType=%u "
                "[SYNC-MIRROR OK -- no spawn-fresh, no dup]",
                edge, E, static_cast<unsigned>(payload.ctx), static_cast<unsigned>(payload.chipType));
        // A ToClump matching our pending grab confirms the carry.
        coop::trash_channel::NoteClientConvertObserved(E, true);
        return proxy;
    }
    // A trash convert that beat its spawn (no proxy for E yet). Spawn the proxy here in the
    // convert's own form rather than through the spawn path, which derives the proxy's form from
    // the spawn class; a trailing real spawn then hits the proxy's convergence branch, which
    // keeps the convert-owned form. A class the proxy registry cannot resolve as trash (not
    // loaded yet) falls through to the spawn-and-rebind path below.
    if (coop::trash_proxy::IsTrashProxyClass(remote_prop_spawn::ClassNameToWString(payload.pileClass))) {
        // The grab hand-off: when E resolves to a bound save-loaded native pile and this is a grab,
        // the pile leaves the save-loaded resting state to become a host-runtime carried clump,
        // which has no native. The native cannot morph to a clump (a different class) and it is not
        // a proxy, so a plain spawn and register would be rejected against the still-live native by
        // the duplicate-eid guard and the pile would never convert. Hand the eid to the runtime
        // proxy: rebind the element onto the fresh clump proxy in place, then retire the orphaned
        // native (drop its bound-mirror mark, then destroy). Gated on the grab direction, so a
        // non-grab convert on a bound native falls to the safe reject path.
        void* boundNative = ResolveLiveActorByEid(E);
        const bool morphBoundNative =
            wantClump && boundNative && coop::prop_element_tracker::IsBoundMirrorNative(boundNative);
        ue_wrap::FVector  loc{payload.locX, payload.locY, payload.locZ};
        ue_wrap::FRotator rot{payload.rotPitch, payload.rotYaw, payload.rotRoll};
        // The land half: E is already bound to a save-loaded native pile (adopted at join) and this
        // is a re-pile land, so the native is already the correct resting form. Claim it:
        // reposition and re-skin to the host's landed transform, then return, suppressing the proxy
        // spawn. A spawn here would be rejected by the register against the still-bound native,
        // leaving a proxy in the registry and the element bound to the native, two live actors for
        // one eid that the save-time-twin sweep cannot see (it skips bound natives).
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
                coop::native_pile_mirror::Unpin(boundNative);                    // release a nativized runtime pile's pin (no-op on a save-loaded native) -- else a rooted PendingKill leaks
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
        // The convert-beat-spawn land: the host grabbed and moved a save-loaded pile mid-join and
        // the land reached us before any spawn. SpawnProxy was given the moved rest position; force
        // the transform explicitly anyway and read it back, the drift log showing whether the spawn
        // transform had taken. Land only (a land has a final rest pose); a carry is pose-stream
        // driven and never snapped.
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
        // The carry-state contract: every branch that handles a ToClump matching our pending grab
        // must confirm it, else the client carry never arms, the throw toggle never fires, the
        // carried pile sticks and the host slot stays latched (every later grab denied). A land
        // re-enters via the proxy branch (the actor is a proxy by then) and clears symmetrically.
        // Host no-op (no pending grab).
        coop::trash_channel::NoteClientConvertObserved(E, wantClump);
        return proxy;
    }
    void* cur = ResolveLiveActorByEid(E);
    const bool hadMirror = (cur != nullptr);   // was a SYNC-MIRROR of E present to re-skin?
    // Idempotency: if our rendering of E already matches the target form (an echo, a duplicate,
    // or a grab-race loser's convert arriving after we rendered the same class), no-op; the
    // winner's held-pose stream drives it.
    if (cur && ue_wrap::prop::IsGarbageClump(cur) == wantClump) {
        UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u -- already %s, idempotent no-op (echo/dup)",
                edge, E, static_cast<unsigned>(payload.ctx), wantClump ? "clump" : "pile");
        return cur;
    }
    // Spawn the new rendering bound to E: a grab gives a kinematic clump (not simulating; the
    // fresh spawn also disarms the clump's self-convert hit notify) the held-pose stream drives,
    // a land a settled, grabbable pile. skipBind: E is bound explicitly below; fromConvert: the
    // eid dedupe is skipped so the still-live old rendering does not converge the spawn.
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
    // Rebind E onto the new rendering. RegisterPropMirror is the single rebind entry point: it
    // routes on the element's own mirror flag (a mirror rebinds in place; the host's own local
    // element goes through the tracker's rebind, keeping the actor-to-eid reverse consistent), so
    // this is correct even when cur is momentarily dead.
    const std::wstring cls = R::ClassNameOf(next);
    RegisterPropMirror(E, next, L"", cls, senderSlot, /*rebindInPlace=*/true);
    // Echo-destroy the old rendering after the rebind, so E always resolves to a live actor.
    if (cur && cur != next) {
        // Owned by remote_prop_destroy.cpp, which holds the cached destroy function.
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
