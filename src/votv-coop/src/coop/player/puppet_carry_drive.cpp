// coop/player/puppet_carry_drive.cpp -- see coop/player/puppet_carry_drive.h.

#include "coop/player/puppet_carry_drive.h"

#include "coop/net/protocol.h"       // TrashClumpPoseSnapshot (the carry pose batch entry)
#include "coop/net/session.h"        // PublishTrashCarryPose (host publish)
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/trash_channel.h"     // IsCarrying / HasPendingSettle / CtxForEid / OnHolderGone
#include "ue_wrap/engine/engine.h"          // TryGetActorLocation / GetActorRotation
#include "ue_wrap/engine/engine_component.h"   // SetComponentTickEnabled (the handle's own tick)
#include "ue_wrap/engine/engine_mainplayer.h"  // ReadMainPlayerGrabHandle / SetPhysicsHandleTarget / camera
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"      // IsLiveByIndex / InternalIndexOf
#include "ue_wrap/core/types.h"           // FVector / FRotator / NormalizeAxis

#include <cmath>
#include <cstdint>
#include <vector>

namespace coop::puppet_carry_drive {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// grabLen reads frozen at 150 on the puppet -- the BP-authored hold distance. We use it directly,
// because the puppet's grabLen Timeline never advances and there is no live value to read. (The
// remote player's scroll-wheel change of its own grabLen, 50..150, is not on the wire.)
constexpr float kGrabLenCm = 150.f;

struct Quat { float x = 0.f, y = 0.f, z = 0.f, w = 1.f; };

Quat FromRotator(const ue_wrap::FRotator& r) {
    Quat q;
    E::RotatorToQuat(r.Pitch, r.Yaw, r.Roll, q.x, q.y, q.z, q.w);
    return q;
}

// a * b: the rotation b, then the rotation a.
Quat Mul(const Quat& a, const Quat& b) {
    return Quat{ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                 a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                 a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}


struct PuppetHeld {
    uint32_t eid   = 0;        // the trash entity
    uint8_t  slot  = 0;        // the peer whose puppet holds it
    void*    clump = nullptr;  // the held garbageClump actor (cross-tick: validate via clumpIdx)
    int32_t  clumpIdx = -1;    // GUObjectArray index captured at NotePuppetHeld (IsLiveByIndex)
    // FLIGHT (set by NoteLetGo: a throw, a holder gone): the puppet's handle let go, so the host
    // STOPS feeding it and the clump is a free body. We keep streaming its pose each tick so every
    // client renders the ARC, until the carry latch closes (the land's commit, the clump at rest) ->
    // the entry is dropped.
    bool     flying = false;
    // How far the clump trails the hold point, worst case this carry (cm). The hold is the game's own
    // physics handle, a force-limited spring: a free carry trails by a few centimetres, and a clump
    // pressed against something heavy trails by as much as the player pushes -- which is the point.
    float    maxLagCm = 0.f;
    // The clump's orientation at the grab, in the view's frame. Natively the player's `grabrot`
    // arrow takes the held component's rotation at the grab and turns with the camera, so a hold
    // keeps the thing as it was picked up and carries it round with the look. A puppet's grabrot
    // is inert (it reads zero whatever the puppet does: measured), so the same composition is
    // done here: target rotation = the view now * this.
    Quat     inView;
};

// The clump's rotation in the frame of the puppet's view, both as they are now (the grab). With no
// puppet, or no readable clump rotation, the identity: the clump is carried at the view's orientation.
Quat GrabOrientationInView(uint8_t slot, void* clump) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    if (!rp || !rp->valid()) return Quat{};
    ue_wrap::FRotator clumpRot{};
    if (!E::TryGetActorRotation(clump, clumpRot)) {
        UE_LOGW("puppet_carry_drive: slot %u clump %p -- its rotation could not be read at the grab; carried at "
                "the view's orientation", static_cast<unsigned>(slot), clump);
        return Quat{};
    }
    Quat view = FromRotator(rp->GetSyncedAimRotation());
    view.x = -view.x; view.y = -view.y; view.z = -view.z;   // the inverse of a unit quaternion
    return Mul(view, FromRotator(clumpRot));
}

std::vector<PuppetHeld> g_held;  // GT-only; per peer at most one carry, and any flights it let go before

}  // namespace

void NotePuppetHeld(coop::element::ElementId eid, uint8_t slot, void* clump) {
    if (eid == 0u || eid == coop::element::kInvalidId || !clump) return;
    const uint32_t e = static_cast<uint32_t>(eid);
    for (auto& h : g_held) {                       // idempotent: a re-register updates in place
        if (h.eid == e) {
            h.slot = slot; h.clump = clump; h.clumpIdx = R::InternalIndexOf(clump); h.flying = false;
            h.inView = GrabOrientationInView(slot, clump);
            UE_LOGI("[PUPPET-DRIVE] re-note eid=%u slot=%u clump=%p", e, slot, clump);
            return;
        }
    }
    PuppetHeld held{e, slot, clump, R::InternalIndexOf(clump), false};
    held.inView = GrabOrientationInView(slot, clump);
    g_held.push_back(held);
    // The handle moves its hold toward the target in its own component tick; a puppet's actor tick
    // is off, and nothing says its components' are on.
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* phc = (rp && rp->valid()) ? E::ReadMainPlayerGrabHandle(rp->GetActor()) : nullptr;
    const bool ticking = phc && E::SetComponentTickEnabled(phc, true);
    UE_LOGI("[PUPPET-DRIVE] NOTE eid=%u slot=%u clump=%p handle=%p ticking=%d -- the host advances the "
            "puppet's handle target each tick (the puppet's own tick, which does it natively, is off)",
            e, slot, clump, phc, ticking ? 1 : 0);
}

void NoteLetGo(coop::element::ElementId eid) {
    const uint32_t e = static_cast<uint32_t>(eid);
    for (auto& h : g_held) {
        if (h.eid == e) {
            h.flying = true;   // stop feeding the handle; keep streaming the free body until the latch closes
            UE_LOGI("[PUPPET-DRIVE] LET GO eid=%u slot=%u -- hand-drive OFF, flight pose stream ON "
                    "(physics owns the arc; the land or the rest ends it)", e, h.slot);
            return;
        }
    }
}

void Tick(coop::net::Session& s) {
    static thread_local int sTick = 0;
    ++sTick;
    for (auto it = g_held.begin(); it != g_held.end(); ) {
        const coop::element::ElementId E = static_cast<coop::element::ElementId>(it->eid);
        // Guard 1 (FIRST): the carry latch closed (TickCarry ran BEFORE this in TickGameplay) = the
        // land's commit, or the let-go clump came to rest as a clump. Either way nothing is left to
        // stream, and nothing to release.
        if (!coop::trash_channel::IsCarrying(E)) {
            UE_LOGI("[PUPPET-DRIVE] eid=%u slot=%u -- carry latch closed (landed, or at rest) -> drive OFF", it->eid, it->slot);
            it = g_held.erase(it);
            continue;
        }
        // Guard 2: the clump still live? (cross-tick cached pointer -> IsLiveByIndex, never bare IsLive.) The
        // re-pile DESTROYS the clump (K2_DestroyActor(self)) while a land-settle is pending (g_carry still
        // open for kLandSettleTicks) -- that is a LANDING, NOT a lost clump: erase WITHOUT releasing so the
        // settle can COMMIT the ToPile. Only a clump gone with NO pending settle is a genuine loss: release
        // the hold there so the eid is re-grabbable, or g_heldBy strands it un-grabbable.
        if (!R::IsLiveByIndex(it->clump, it->clumpIdx)) {
            // This row's clump is gone; the ENTITY may not be. A broom or a hand that took the pile
            // a throw landed as, in the very tick its land committed, re-opens E's carry on a new
            // clump (the latch reads open again, so guard 1 passes): a release here would retire a
            // live entity everywhere. The row is simply stale.
            void* now = coop::trash_channel::LiveActorOf(E);
            if (now && now != it->clump) {
                UE_LOGI("[PUPPET-DRIVE] eid=%u slot=%u -- this clump is gone and the entity lives on in %p "
                        "-> drive OFF, nothing released", it->eid, it->slot, now);
            } else if (coop::trash_channel::HasPendingSettle(E)) {
                UE_LOGI("[PUPPET-DRIVE] eid=%u slot=%u -- clump consumed by a re-pile (settle pending) -> drive OFF (land commits)",
                        it->eid, it->slot);
            } else {
                UE_LOGI("[PUPPET-DRIVE] eid=%u slot=%u -- clump gone with NO land -> drive OFF + release hold", it->eid, it->slot);
                coop::trash_channel::ReleaseClientHold(s, E);
            }
            it = g_held.erase(it);
            continue;
        }
        // Guard 3 (hand-drive only): can the puppet still hold? With no puppet there is no hand, and
        // a peer that fell holds nothing: the game drops what a fainting player has (mainPlayer:
        // dropGrabObject on the faint), but this clump is in the PUPPET's hand, where the remote
        // player's own drop cannot reach. The clump is a live body in the world: it is let go,
        // never retired, and streams on below like any release.
        coop::RemotePlayer* rp = it->flying ? nullptr : coop::players::Registry::Get().Puppet(it->slot);
        if (!it->flying && (!rp || !rp->valid() || rp->IsRagdollDisplayed())) {
            coop::trash_channel::OnHolderGone(s, E);
            it->flying = true;
        }
        const bool publish = s.TrashCarryPoseTurn(it->eid, /*ahead=*/true) != coop::net::PoseTurn::Wait;
        if (it->flying && !publish) { ++it; continue; }   // a free body between its turns: nothing to read
        // The hold target below needs no clump position; the lag metric and the pose do, and an
        // unreadable clump gives neither this tick.
        ue_wrap::FVector loc{};
        const bool locRead = E::TryGetActorLocation(it->clump, loc);
        if (!it->flying) {
            // The hold point: the puppet's camera + synced aim * grabLen, where the remote player
            // looks, with the grab's orientation carried round by the view. It goes to the puppet's
            // OWN physics handle as its target, the call the player's tick makes natively (mainPlayer:
            // grabHandle->SetTargetLocationAndRotation(Camera + forward * grabLen, grabrot)). The
            // clump stays in the solver, held by the handle's force-limited spring, so what it pushes
            // against pushes back: a light clump is stopped by a heavy box. Writing the clump's location
            // instead made it a kinematic collider, which shoves any dynamic body with no force limit.
            // The camera and not the nameplate anchor, which floats above the skull and low-passes
            // its height: measured 19 cm higher than the camera the game holds from.
            void* puppet = rp->GetActor();
            ue_wrap::FVector eye;
            // With neither the camera nor the head readable the handle keeps its last target.
            if (E::ReadMainPlayerCameraLocation(puppet, eye) || rp->TryGetHeadPosition(eye)) {
                const ue_wrap::FVector fwd = rp->GetSyncedAimDirection();
                const ue_wrap::FVector hold{ eye.X + fwd.X * kGrabLenCm,
                                             eye.Y + fwd.Y * kGrabLenCm,
                                             eye.Z + fwd.Z * kGrabLenCm };
                const Quat q = Mul(FromRotator(rp->GetSyncedAimRotation()), it->inView);
                E::SetPhysicsHandleTarget(E::ReadMainPlayerGrabHandle(puppet), hold,
                                          E::QuatToRotator(q.x, q.y, q.z, q.w));
                if (locRead) {
                    const float lx = loc.X - hold.X, ly = loc.Y - hold.Y, lz = loc.Z - hold.Z;
                    const float lag = std::sqrt(lx * lx + ly * ly + lz * lz);
                    if (lag > it->maxLagCm) it->maxLagCm = lag;
                }
            }
        }
        // STREAM the clump's CURRENT pose (hand pos when carrying, physics pos when flying) to ALL peers so
        // every client renders the carry + the throw arc. Host-authoritative + host-originated (the relay
        // can't echo to the grabber; a client drives only slot 0). eid+ctx keyed -> the receiver's per-eid
        // ActiveDrive interp; ctx is the carry generation (stale-pose guard on the client). A clump a
        // player moved, carried or thrown, goes ahead of the ones a broom set rolling.
        ue_wrap::FRotator rot{};
        if (publish && locRead && E::TryGetActorRotation(it->clump, rot)) {   // unread: no pose this tick
            coop::net::TrashClumpPoseSnapshot snap{};
            snap.eid   = it->eid;
            snap.x = loc.X; snap.y = loc.Y; snap.z = loc.Z;
            snap.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
            snap.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
            snap.roll  = ue_wrap::NormalizeAxis(rot.Roll);
            snap.ctx   = coop::trash_channel::CtxForEid(E);
            s.PublishTrashCarryPose(snap, /*ahead=*/true);
            if ((sTick % 60) == 0)
                UE_LOGI("[TRASH-CARRY] HOST PUBLISH eid=%u slot=%u %s -> (%.1f,%.1f,%.1f) ctx=%u maxLagCm=%.1f",
                        it->eid, it->slot, it->flying ? "FLIGHT" : "carry", loc.X, loc.Y, loc.Z,
                        static_cast<unsigned>(snap.ctx), it->maxLagCm);
        }
        ++it;
    }
}

void OnTakenOver(coop::element::ElementId eid) {
    const uint32_t e = static_cast<uint32_t>(eid);
    for (auto it = g_held.begin(); it != g_held.end(); ++it) {
        if (it->eid != e) continue;
        // Still in the puppet's hand: its handle lets go and its grabbing_actor clears, or two
        // handles pull one body and the clump's re-pile gate keeps reading the puppet as the holder.
        bool released = false;
        if (!it->flying) {
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(it->slot);
            if (rp && rp->valid()) released = E::ReleaseMainPlayerGrabIfHolding(rp->GetActor(), it->clump);
        }
        UE_LOGI("[PUPPET-DRIVE] eid=%u slot=%u -- taken over by the host's hand or a broom -> drive OFF (the "
                "entity lives on in the taker's clump; puppet handle released=%d)", it->eid, it->slot,
                released ? 1 : 0);
        g_held.erase(it);
        return;
    }
}

void OnDisconnect() {
    g_held.clear();
}

}  // namespace coop::puppet_carry_drive
