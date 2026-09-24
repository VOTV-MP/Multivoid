// coop/props/remote_prop_convert.cpp -- the PropConvert receiver: eid E changes form, pile to
// clump on a grab and clump to pile on a land. Declared in remote_prop.h; game thread only, as
// the rest of remote_prop.
//
// Both forms are the game's own actor, so a form change is a CLASS change and there is no
// re-skin in place: the successor is materialised parked, the element is rebound onto it AT the
// successor's birth, and only then is the predecessor destroyed. That ordering is what makes a
// dying actor that resolves no eid a husk by construction, so no destroy handler needs a morph
// special case.

#include "coop/props/remote_prop.h"
#include "remote_prop_internal.h"  // ResolveLiveActorByEid + DestroyEchoSuppressed (impl-private seam)

#include "coop/element/quiescence_drain.h"   // ArmPendingSaveTimeTwin (capture-only)
#include "coop/props/trash_mirror.h"         // Materialize / RepositionBoundNative
#include "coop/props/pile_look.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_sound.h"
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/trash_channel.h"
#include "coop/props/trash_clump_pose_stream.h"
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

void* OnConvert(const coop::net::PropConvertPayload& payload, void* /*localPlayer*/, int senderSlot) {
    UE_ASSERT_GAME_THREAD("g_drives (remote_prop::OnConvert)");
    // The bind model: oldEid == newEid == E, no fresh eid, no second entity.
    const uint32_t E = payload.newEid;
    const bool wantClump = (payload.kind == coop::net::propconvert_kind::kToClump);
    const char* edge = wantClump ? "GRAB(pile->clump)" : "LAND(clump->pile)";
    if (E == 0u || E == coop::element::kInvalidId) {
        UE_LOGW("[PILE] CLIENT recv convert %s -- INVALID eid E=%u, dropping (no entity to convert)", edge, E);
        return nullptr;
    }
    void* cur = ResolveLiveActorByEid(E);
    // Logged before the context gate, so a convert that arrived and was dropped is still visible
    // in the client log. Event-driven, not hot.
    UE_LOGI("[PILE] CLIENT recv convert %s eid=%u ctx=%u known=%u mirror=%p -- RECEPTION (pre-gate)",
            edge, E, static_cast<unsigned>(payload.ctx),
            static_cast<unsigned>(coop::trash_channel::CtxForEid(E)), cur);
    // Adopt the host's context for E and drop a stale or out-of-order convert (a duplicate, or
    // one older than a transition already applied).
    if (!coop::trash_channel::AdoptInboundConvertCtx(E, payload.ctx)) return nullptr;
    // A land carrying a save-time key means the host self-seeded this eid at an in-window grab and
    // stamped the pile's pre-grab position. Arm the order owner so the quiescence sweep reconciles
    // our native at the old position -- before any branch below, so it fires whatever this convert
    // then does.
    if (!wantClump && payload.hasMatchPos)
        coop::element::quiescence_drain::ArmPendingSaveTimeTwin(
            E, ue_wrap::FVector{payload.matchX, payload.matchY, payload.matchZ}, payload.chipType);

    const ue_wrap::FVector  loc{payload.locX, payload.locY, payload.locZ};
    const ue_wrap::FRotator rot{payload.rotPitch, payload.rotYaw, payload.rotRoll};  // the host actor's rotation
    const ue_wrap::FVector  scale{payload.scaleX, payload.scaleY, payload.scaleZ};

    // Already in the target form. A GRAB is then idempotent -- an echo, or a grab-race loser's
    // convert arriving after we rendered the same class -- and the carry pose stream drives it, so
    // nothing is written here: a snap on every grab convert stuttered the carry. A LAND is not
    // idempotent, because the pile has a resting pose the host has just decided: claim the actor
    // we already have, which is also what suppresses a parallel spawn the duplicate-eid guard
    // would reject and leave split-tracked.
    if (cur && ue_wrap::prop::IsGarbageClump(cur) == wantClump) {
        if (wantClump) {
            UE_LOGI("[PILE] CLIENT convert %s eid=%u ctx=%u -- already a clump, idempotent no-op (echo/dup)",
                    edge, E, static_cast<unsigned>(payload.ctx));
            coop::trash_channel::NoteClientConvertObserved(E, true);
            return cur;
        }
        coop::trash_clump_pose_stream::ClearDriveForEid(E);  // stop the carry pose stream at the land
        ClearAnyDriveFor(cur);
        coop::trash_mirror::RepositionBoundNative(cur, payload.chipType, loc, rot, scale);
        // After the re-skin, which re-runs init() and so draws a new look: the host's replaces it.
        coop::pile_look::OnHostLook(E, payload.look, senderSlot);
        coop::pile_look::LogLandLook("CLIENT", E, cur);
        ue_wrap::FVector got{};
        const bool gotRead = E::TryGetActorLocation(cur, got);
        const float dx = got.X - loc.X, dy = got.Y - loc.Y, dz = got.Z - loc.Z;
        const float drift = gotRead ? std::sqrt(dx * dx + dy * dy + dz * dz) : -1.f;   // -1: unread
        UE_LOGI("[PILE] CLIENT ToPile LAND eid=%u ctx=%u -> CLAIMED the bound native %p, repositioned to "
                "(%.1f,%.1f,%.1f)%s host=(%.1f,%.1f,%.1f) drift=%.2fcm [no parallel spawn, no dup]",
                E, static_cast<unsigned>(payload.ctx), cur, got.X, got.Y, got.Z, gotRead ? "" : " (unread)",
                loc.X, loc.Y, loc.Z, drift);
        coop::prop_sound::PlayLandSound(cur);
        coop::trash_channel::NoteClientConvertObserved(E, false);
        return cur;
    }

    // A to-clump for an eid with NO actor here and a pending bind: the clump's row missed this
    // client's own copy (the async load tail had not spawned it yet) and armed the late bind, and
    // this convert is the generation that rides behind every clump row. Standing a mirror up now
    // would put a second clump beside the copy the load is about to bring, and the late bind, finding
    // the eid bound at its key, would never claim that copy. The generation is adopted above; the
    // late bind owns the actor.
    if (wantClump && !cur && coop::element::quiescence_drain::HasPendingSaveTimeTwin(E)) {
        UE_LOGI("[PILE] CLIENT convert %s eid=%u ctx=%u -- generation adopted; the actor is the pending own-native "
                "bind's (no mirror spawned beside it)", edge, E, static_cast<unsigned>(payload.ctx));
        return nullptr;
    }

    // The form changes. The payload's class is the successor's own class on both edges, so it
    // names what to spawn. Materialize binds E onto it in place, which is the identity migrating
    // at the successor's BIRTH, before the predecessor dies.
    const std::wstring cls = remote_prop_spawn::ClassNameToWString(payload.pileClass);
    // Kept before the successor exists: Materialize binds E onto it, and the bind applies the look.
    // E's actor right now is the clump, which has no such mesh, so nothing is applied here.
    coop::pile_look::OnHostLook(E, payload.look, senderSlot);
    if (!wantClump)
        coop::trash_clump_pose_stream::ClearDriveForEid(E);  // the carry ends at the land
    void* next = coop::trash_mirror::Materialize(E, cls, payload.chipType, loc, rot, scale,
                                                 senderSlot, /*skipBind=*/false, /*rebindInPlace=*/true);
    if (!next) {
        UE_LOGW("[PILE] CLIENT convert %s eid=%u -- materialize of '%ls' FAILED, E left as the old actor (DESYNC)",
                edge, E, cls.c_str());
        return nullptr;
    }
    // Retire the predecessor. It no longer owns E (the rebind above moved the element), so its
    // destroy observer is already silent on identity; the echo suppression covers the rest, and
    // the unpin releases a runtime mirror's GC pin so a rooted pending-kill actor cannot leak.
    if (cur && cur != next) {
        ClearAnyDriveFor(cur);
        coop::prop_element_tracker::UnmarkKnownKeyedProp(cur);  // a save-loaded native drops its bound-mirror mark
        coop::trash_mirror::Unpin(cur);                         // no-op for a native we never pinned
        DestroyEchoSuppressed(cur);
    }
    if (!wantClump) coop::pile_look::LogLandLook("CLIENT", E, next);
    UE_LOGI("[PILE] CLIENT convert %s eid=%u ctx=%u -> %s cls='%ls' actor=%p at (%.1f,%.1f,%.1f) chipType=%u "
            "(predecessor %p retired)%s",
            edge, E, static_cast<unsigned>(payload.ctx), wantClump ? "CLUMP" : "PILE", cls.c_str(),
            next, loc.X, loc.Y, loc.Z, static_cast<unsigned>(payload.chipType), cur,
            cur ? " [SYNC-MIRROR OK]" : " [no local mirror of E existed -- materialised fresh]");
    if (!wantClump) coop::prop_sound::PlayLandSound(next);  // the host-authoritative LAND thud
    coop::trash_channel::NoteClientConvertObserved(E, wantClump);
    return next;
}

}  // namespace coop::remote_prop
