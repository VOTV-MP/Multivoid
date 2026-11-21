// coop/player/remote_player_based.cpp -- a puppet that rides the mover its pose names; see coop/player/pose_bases.h and the
// base members of coop/player/remote_player.h.
//
// While a pose names a base this peer resolves, the puppet's root is attached to the platform actor (K2_AttachToActor,
// KeepWorld: the precedent is puppet_seat.cpp's attach to an ATV), so the engine carries it with the platform in the same
// update and no tick order has to be won. Whenever the puppet is dirty (each tick of an open interpolation window, a turn in
// place) ApplyToEngine puts it at the platform's current location plus the interpolated offset, which the engine turns into the
// relative location; between those writes the attachment carries it. A change of base (step on, step off, jump off, a base that
// died) re-expresses the puppet's REAL place in the new space and drops the open window, as MTA does at its origin switch
// (CClientPed.cpp:5370-5412), so nothing snaps. MTA follows its origin only within the interpolation window and then lets the
// ped's physics carry it; a puppet has no movement of its own, so it is attached instead.
//
// One attach attempt per change of base. An attach the engine refuses, or one another attachment (a wisp's grab) replaces, leaves
// the claim treated as none -- the absolute pose places the puppet -- until the pose names another base or none.

#include "coop/player/remote_player.h"

#include "coop/player/pose_bases.h"
#include "coop/player/puppet_seat.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"

namespace coop {

namespace E = ue_wrap::engine;

RemotePlayer::BaseClaim RemotePlayer::ClaimOf(const coop::net::PoseSnapshot& snap) {
    BaseClaim claim;
    // A ragdolled body is pelvis-attached to its display (ApplyToEngine's Attached drive) and a seated one to its ATV
    // (puppet_seat::HoldsBody): those attachments own its transform, and a second parent would replace one of them.
    if (ragdoll_.Active() || (snap.stateBits & coop::net::kStateBitRagdoll) != 0 || coop::puppet_seat::HoldsBody(slot_))
        return claim;
    coop::pose_bases::Resolved at;
    if (!coop::pose_bases::Claim(snap, slot_, at)) {
        refusedBase_.Reset();   // the pose names none, or a base this peer does not resolve: a refusal ends with it
        return claim;
    }
    if (at.actor == refusedBase_.Get()) return claim;
    refusedBase_.Reset();
    claim.actor = at.actor;
    claim.id = snap.baseId;
    claim.origin = at.origin;
    return claim;
}

void RemotePlayer::UpdateBase(const coop::net::PoseSnapshot& snap) {
    // Another attachment replaced the platform as the puppet's parent (a wisp's grab, a seat): the base is lost, not re-taken.
    if (riding_) {
        void* const base = base_.Get();
        void* parent = nullptr;
        if (base && (!E::GetAttachParentActor(actor_, parent) || parent != base)) {
            ue_wrap::FVector world{};
            curPos_ = E::TryGetActorLocation(actor_, world) ? world : WorldPos();
            refusedBase_.Set(base);
            riding_ = false;
            base_.Reset();
            baseId_ = 0;
            window_.Close();
            dirty_ = true;
        }
    }
    const BaseClaim claim = ClaimOf(snap);
    if (claim.actor ? (!riding_ || base_.Get() != claim.actor) : riding_) {
        if (hasPose_) AdvanceInterp();   // the motion already due, in the old space
        ChangeBase(claim);
    }
}

void RemotePlayer::ChangeBase(const BaseClaim& to) {
    // The puppet's real place, so a re-based pose starts from where it stands and nothing jumps.
    ue_wrap::FVector world{};
    if (hasPose_ && !E::TryGetActorLocation(actor_, world)) world = WorldPos();
    if (riding_) {
        void* const old = base_.Get();
        void* parent = nullptr;
        // Detach only from the mover this owner attached to: a seat or a ragdoll display has taken the attachment over otherwise.
        if (old && E::GetAttachParentActor(actor_, parent) && parent == old) E::DetachActorFromParent(actor_);
    }
    riding_ = false;
    base_.Reset();
    baseId_ = 0;
    if (to.actor) {
        if (E::AttachActorToActorKeepWorld(actor_, to.actor, false)) {
            riding_ = true;
            base_.Set(to.actor);
            baseId_ = to.id;
            baseOrigin_ = to.origin;
        } else {
            refusedBase_.Set(to.actor);   // one attempt per base: the absolute pose places the puppet until the pose names another
            UE_LOGW("RemotePlayer: slot %u could not be attached to the mover its pose names; it is placed by the absolute pose "
                    "until the pose names another base", static_cast<unsigned>(slot_));
        }
    }
    if (hasPose_) curPos_ = riding_ ? ue_wrap::FVector{world.X - to.origin.X, world.Y - to.origin.Y, world.Z - to.origin.Z} : world;
    window_.Close();
    dirty_ = true;
}

ue_wrap::FVector RemotePlayer::WorldPos() const {
    if (!riding_) return curPos_;
    return {baseOrigin_.X + curPos_.X, baseOrigin_.Y + curPos_.Y, baseOrigin_.Z + curPos_.Z};
}

ue_wrap::FVector RemotePlayer::WorldPosForWrite() {
    if (!riding_) return curPos_;
    ue_wrap::FVector origin{};
    void* const base = base_.Get();
    if (base && E::TryGetActorLocation(base, origin)) {
        baseOrigin_ = origin;
        return {origin.X + curPos_.X, origin.Y + curPos_.Y, origin.Z + curPos_.Z};
    }
    // The platform is gone. The engine detaches an attached puppet from a destroyed parent itself, in place (the drill's baseend arm,
    // with and without an end-of-play sink of ours doing it: no parent and 1.0 cm moved in both, so the sink was deleted); the offset
    // becomes a world position at the last origin, and the next pose places the puppet by its own claim.
    curPos_ = WorldPos();
    riding_ = false;
    base_.Reset();
    baseId_ = 0;
    window_.Close();
    return curPos_;
}

}  // namespace coop
