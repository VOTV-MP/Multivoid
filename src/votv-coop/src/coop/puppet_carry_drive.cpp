// coop/puppet_carry_drive.cpp -- see coop/puppet_carry_drive.h.

#include "coop/puppet_carry_drive.h"

#include "coop/players_registry.h"
#include "coop/remote_player.h"
#include "coop/trash_channel.h"     // IsCarrying (the carry latch = the drive's lifetime)
#include "ue_wrap/engine.h"          // SetActorLocation
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"      // IsLiveByIndex / InternalIndexOf
#include "ue_wrap/types.h"           // FVector

#include <cstdint>
#include <vector>

namespace coop::puppet_carry_drive {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// The probe (votv-puppet-grab-feasibility-RE-2026-06-22) read grabLen frozen at 150 on the puppet --
// that is the BP-authored hold distance. We use it directly (the puppet's grabLen Timeline never
// advances, so there is no live value to read).
constexpr float kGrabLenCm = 150.f;

struct PuppetHeld {
    uint32_t eid   = 0;        // the trash entity
    uint8_t  slot  = 0;        // the peer whose puppet holds it
    void*    clump = nullptr;  // the held garbageClump actor (cross-tick: validate via clumpIdx)
    int32_t  clumpIdx = -1;    // GUObjectArray index captured at NotePuppetHeld (IsLiveByIndex)
};

std::vector<PuppetHeld> g_held;  // GT-only; at most one per peer

}  // namespace

void NotePuppetHeld(coop::element::ElementId eid, uint8_t slot, void* clump) {
    if (eid == 0u || eid == coop::element::kInvalidId || !clump) return;
    const uint32_t e = static_cast<uint32_t>(eid);
    for (auto& h : g_held) {                       // idempotent: a re-register updates in place
        if (h.eid == e) {
            h.slot = slot; h.clump = clump; h.clumpIdx = R::InternalIndexOf(clump);
            UE_LOGI("[PUPPET-DRIVE] re-note eid=%u slot=%u clump=%p", e, slot, clump);
            return;
        }
    }
    g_held.push_back(PuppetHeld{e, slot, clump, R::InternalIndexOf(clump)});
    UE_LOGI("[PUPPET-DRIVE] NOTE eid=%u slot=%u clump=%p -- per-tick hand-follow ON "
            "(the puppet tick does not drive the PHC; the host drives the hold pose)", e, slot, clump);
}

void Tick() {
    for (auto it = g_held.begin(); it != g_held.end(); ) {
        // Guard 1: the clump still live? (cross-tick cached pointer -> IsLiveByIndex, never bare IsLive.)
        if (!R::IsLiveByIndex(it->clump, it->clumpIdx)) {
            UE_LOGI("[PUPPET-DRIVE] eid=%u slot=%u -- clump gone (re-piled/GC) -> drive OFF", it->eid, it->slot);
            it = g_held.erase(it);
            continue;
        }
        // Guard 2: the carry latch still open? A re-pile land COMMIT (trash_channel::TickCarry) closes it;
        // the clump has become a pile -> stop following. (TickCarry runs BEFORE this in TickGameplay.)
        if (!coop::trash_channel::IsCarrying(static_cast<coop::element::ElementId>(it->eid))) {
            UE_LOGI("[PUPPET-DRIVE] eid=%u slot=%u -- carry latch closed (landed) -> drive OFF", it->eid, it->slot);
            it = g_held.erase(it);
            continue;
        }
        // Guard 3: the puppet still live?
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(it->slot);
        if (!rp || !rp->valid()) {
            UE_LOGI("[PUPPET-DRIVE] eid=%u slot=%u -- puppet gone -> drive OFF", it->eid, it->slot);
            it = g_held.erase(it);
            continue;
        }
        // Drive: position the held clump at the puppet's hand = head + syncedAim * grabLen. The puppet's
        // synced aim is already streamed (curYaw_/curPitch_), so the clump tracks where the remote player
        // looks. SetActorLocation is a teleport that wins over the (un-driven, frozen) PHC target.
        const ue_wrap::FVector head = rp->GetHeadPosition();
        const ue_wrap::FVector fwd  = rp->GetSyncedAimDirection();
        const ue_wrap::FVector hold{ head.X + fwd.X * kGrabLenCm,
                                     head.Y + fwd.Y * kGrabLenCm,
                                     head.Z + fwd.Z * kGrabLenCm };
        E::SetActorLocation(it->clump, hold);
        // Throttled trace (first ~3 ticks of a hold, then every 60th) for the harness to assert the
        // hand-follow is active + the holdPoint moves with the puppet aim.
        static thread_local int sTick = 0;
        if ((sTick++ % 60) == 0)
            UE_LOGI("[PUPPET-DRIVE] DRIVING eid=%u slot=%u -> holdPoint=(%.1f,%.1f,%.1f)",
                    it->eid, it->slot, hold.X, hold.Y, hold.Z);
        ++it;
    }
}

void OnPeerLeft(uint8_t slot) {
    for (auto it = g_held.begin(); it != g_held.end(); ) {
        if (it->slot == slot) {
            UE_LOGI("[PUPPET-DRIVE] OnPeerLeft slot=%u -- dropping held eid=%u", slot, it->eid);
            it = g_held.erase(it);
        } else { ++it; }
    }
}

void OnDisconnect() {
    g_held.clear();
}

}  // namespace coop::puppet_carry_drive
