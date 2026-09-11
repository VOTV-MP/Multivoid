// coop/dispatch/event_dispatch_entity.cpp -- the entity-lifecycle and held-item reliable kinds:
// PropStickState, PropRelease, PropSpawn, PropDestroy, PropConvert, PropSnapPos, the
// owner-entity lane, EntitySpawn and EntityDestroy, the world-actor pair, PyramidGather and
// ItemActivate. Each case validates at the trust boundary and hands off to its module. See
// coop/dispatch/event_dispatch.h.

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/element/player.h"
#include "coop/element/registry.h"

#include "coop/player/item_activate.h"
#include "coop/session/join_progress.h"
#include "coop/creatures/npc_mirror.h"
#include "coop/creatures/owner_entity_sync.h"
#include "coop/items/hook_anchor.h"
#include "coop/items/hook_sync.h"  // the owner-entity lane
#include "coop/element/quiescence_drain.h"  // the pending position corrections
#include "coop/props/save_identity_bind.h"  // UpdateChipHostPos
#include "coop/player/players_registry.h"
#include "coop/world/world_actor_sync.h"  // the world-actor mirror receivers
#include "coop/creatures/piramid_sync.h"      // the PyramidGather receiver
#include "coop/props/prop_save_data.h"    // the per-prop save record lane
#include "coop/props/prop_stick_sync.h"
#include "coop/player/remote_player.h"
#include "coop/props/remote_prop.h"
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/join_membership_sweep.h"  // HasLoadTailQuiesced
#include "coop/props/trash_pile_sync.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cmath>
#include <cstring>
#include <string>

namespace coop::event_feed {

bool HandleEntityEvent(net::Session& session,
                       const net::Session::ReliableMessage& msg,
                       void* localPlayer) {
    (void)session;
    switch (msg.kind) {
    case net::ReliableKind::PropStickState: {
        // A peer stuck a wall-attachable (the camera) to a surface. Symmetric prop state, relayed
        // by the host; prop_stick_sync stops any drive, re-poses and replays the BP's own
        // forceStick.
        if (msg.payloadLen < sizeof(net::PropStickStatePayload)) {
            UE_LOGW("event_feed: PropStickState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropStickStatePayload));
            break;
        }
        net::PropStickStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // NaN and Inf rejected before any engine write.
        const float vals[6] = {p.locX, p.locY, p.locZ,
                               p.rotPitch, p.rotYaw, p.rotRoll};
        bool bad = false;
        for (float v : vals) {
            if (!std::isfinite(v)) { bad = true; break; }
        }
        if (bad) {
            UE_LOGW("event_feed: PropStickState non-finite pose -- dropping");
            break;
        }
        const uint8_t senderSlot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::prop_stick_sync::OnStickState(p, senderSlot);
        break;
    }
    case net::ReliableKind::PropRelease: {
        // A peer released a held prop: remote_prop re-enables physics, sets the velocities and
        // fires the prop's thrown event past the throw threshold.
        if (msg.payloadLen < sizeof(net::PropReleasePayload)) {
            UE_LOGW("event_feed: PropRelease payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropReleasePayload));
            break;
        }
        // senderPeerSlot indexes the per-slot drive; range-checked so a malformed value cannot
        // index out of the array.
        if (msg.senderPeerSlot < 0 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: PropRelease invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        net::PropReleasePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // A NaN, Inf or absurd velocity would reach the physics setters; rejected before dispatch.
        const float vals[6] = {p.linVelX, p.linVelY, p.linVelZ,
                               p.angVelX, p.angVelY, p.angVelZ};
        bool finite = true;
        for (float v : vals) {
            if (!std::isfinite(v)) { finite = false; break; }
        }
        if (!finite) {
            UE_LOGW("event_feed: PropRelease velocity non-finite -- dropping");
            break;
        }
        // The bounds: real throws peak at a few thousand cm/s and a fast tumble at a few thousand
        // deg/s; 1e6 is generous headroom, below anything that would teleport a body in one tick.
        constexpr float kMaxLinVel = 1.0e6f;
        constexpr float kMaxAngVel = 1.0e6f;
        if (std::fabs(p.linVelX) > kMaxLinVel ||
            std::fabs(p.linVelY) > kMaxLinVel ||
            std::fabs(p.linVelZ) > kMaxLinVel ||
            std::fabs(p.angVelX) > kMaxAngVel ||
            std::fabs(p.angVelY) > kMaxAngVel ||
            std::fabs(p.angVelZ) > kMaxAngVel) {
            UE_LOGW("event_feed: PropRelease velocity out of bounds (lin=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f,%.1f)) -- dropping",
                    p.linVelX, p.linVelY, p.linVelZ,
                    p.angVelX, p.angVelY, p.angVelZ);
            break;
        }
        remote_prop::OnRelease(msg.senderPeerSlot, p, localPlayer);
        break;
    }
    case net::ReliableKind::PropSpawn: {
        // A peer dropped an item: a matching prop is spawned locally, so later PropPose updates
        // resolve.
        if (msg.payloadLen < sizeof(net::PropSpawnPayload)) {
            UE_LOGW("event_feed: PropSpawn payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropSpawnPayload));
            break;
        }
        net::PropSpawnPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // All 18 floats (location, rotation, scale, two velocities, the save-time match key)
        // checked: a NaN match key would slip past the twin-destroy's distance guard (NaN compares
        // false) and destroy a wrong native pile. The match key is zero on an unstamped payload, so
        // checking all 18 is always safe.
        const float vals[18] = {
            p.locX, p.locY, p.locZ,
            p.rotPitch, p.rotYaw, p.rotRoll,
            p.scaleX, p.scaleY, p.scaleZ,
            p.initLinVelX, p.initLinVelY, p.initLinVelZ,
            p.initAngVelX, p.initAngVelY, p.initAngVelZ,
            p.matchX, p.matchY, p.matchZ  // the save-time match key
        };
        bool finite = true;
        for (int i = 0; i < 18; ++i) {
            if (!std::isfinite(vals[i])) { finite = false; break; }
        }
        if (!finite) {
            UE_LOGW("event_feed: PropSpawn floats non-finite -- dropping");
            break;
        }
        constexpr float kMaxCoord = 1.0e6f;
        constexpr float kMaxVel   = 1.0e6f;
        if (std::fabs(p.locX) > kMaxCoord || std::fabs(p.locY) > kMaxCoord ||
            std::fabs(p.locZ) > kMaxCoord) {
            UE_LOGW("event_feed: PropSpawn location out of bounds (%.1f, %.1f, %.1f)",
                    p.locX, p.locY, p.locZ);
            break;
        }
        if (std::fabs(p.initLinVelX) > kMaxVel || std::fabs(p.initLinVelY) > kMaxVel ||
            std::fabs(p.initLinVelZ) > kMaxVel ||
            std::fabs(p.initAngVelX) > kMaxVel || std::fabs(p.initAngVelY) > kMaxVel ||
            std::fabs(p.initAngVelZ) > kMaxVel) {
            UE_LOGW("event_feed: PropSpawn velocity out of bounds");
            break;
        }
        // The class and key lengths, clamped to the struct caps; the sender could lie.
        if (p.className.len > 63) {
            UE_LOGW("event_feed: PropSpawn className.len=%u > 63 -- dropping", p.className.len);
            break;
        }
        if (p.key.len > 31) {
            UE_LOGW("event_feed: PropSpawn key.len=%u > 31 -- dropping", p.key.len);
            break;
        }
        // The eid range. A client may only allocate in its own peer range (a client-sourced
        // PropSpawn carries a peer-range eid), and an eid outside its sender's range is forged or a
        // relay loop and must not reach the allocator. The host may send either range: a snapshot
        // bracket re-expresses existing entities, client-born ones included, and a re-bracket (a
        // cave travel, a host save load) must re-express a client's own dropped items back to it,
        // or the adoption sweep would destroy them as unclaimed.
        if (msg.senderPeerSlot >= 0) {
            const bool senderIsHost = (msg.senderPeerSlot == 0);
            const bool ok = senderIsHost
                ? (coop::element::Registry::IsAllowedHostAllocatedEid(p.elementId) ||
                   coop::element::Registry::IsAllowedPeerAllocatedEid(p.elementId))
                : coop::element::Registry::IsAllowedPeerAllocatedEid(p.elementId);
            if (!ok) {
                UE_LOGW("event_feed: PropSpawn elementId=0x%08x out of allowed "
                        "%s range (senderPeerSlot=%d) -- dropping",
                        p.elementId,
                        senderIsHost ? "host(any)" : "peer",
                        msg.senderPeerSlot);
                break;
            }
        }
        // Stale-generation defence lives in the packet header's sender epoch, latched in
        // Session::HandleMessage. The intermediate mushroom variant is refused here: growth is
        // host-authoritative and the mature variant arrives when the host's timer fires;
        // prop_lifecycle's client-side suppression is the other half, so the class is never spawned
        // locally and never accepted from the wire.
        {
            std::wstring cls;
            cls.reserve(p.className.len);
            for (uint8_t i = 0; i < p.className.len; ++i) {
                cls.push_back(static_cast<wchar_t>(static_cast<unsigned char>(p.className.data[i])));
            }
            if (cls == ue_wrap::profile::name::PropMushroomGrowingClass) {
                UE_LOGI("event_feed: PropSpawn drop -- intermediate-variant class '%ls' suppressed on this peer (host-authoritative; mature variant will arrive when host transforms)",
                        cls.c_str());
                break;
            }
        }
        remote_prop_spawn::OnSpawn(p, msg.senderPeerSlot, localPlayer);
        // Advance the join loading bar; outside a join this is one relaxed atomic load.
        coop::join_progress::NotePropApplied();
        break;
    }
    case net::ReliableKind::PropDestroy: {
        // A peer destroyed a prop: the matching local actor is destroyed, echo-suppressed so it
        // does not bounce back. Trust: with a bidirectional destroy a client can command the host
        // to destroy any prop by key. Accepted for cooperative play among trusted peers; an
        // authority model (the host validating against world state) is the mitigation if that
        // changes.
        if (msg.payloadLen < sizeof(net::PropDestroyPayload)) {
            UE_LOGW("event_feed: PropDestroy payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropDestroyPayload));
            break;
        }
        net::PropDestroyPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.key.len > 31) {
            UE_LOGW("event_feed: PropDestroy key.len=%u > 31 -- dropping", p.key.len);
            break;
        }
        // The eid range: a destroy is not an allocation, it references an existing shared entity
        // the other peer may have allocated (whoever grabs a shared pile broadcasts its destroy by
        // an eid in the original owner's range), so either range is accepted; only an id in neither
        // is rejected.
        if (p.elementId != 0 && p.elementId != coop::element::kInvalidId &&
            !coop::element::Registry::IsAllowedHostAllocatedEid(p.elementId) &&
            !coop::element::Registry::IsAllowedPeerAllocatedEid(p.elementId)) {
            UE_LOGW("event_feed: PropDestroy elementId=0x%08x not a valid allocated id "
                    "(out of both ranges) -- dropping", p.elementId);
            break;
        }
        // localPlayer is passed so OnDestroy can release a held physics-handle grab of the doomed
        // actor before K2_DestroyActor; the handle would otherwise read a dangling component next
        // frame. The keyed echo guard for the dispenser-pile counter channel: a wire destroy must
        // not re-broadcast from our own death watch.
        if (p.key.len > 0) {
            std::wstring dkey;
            dkey.reserve(p.key.len);
            for (uint8_t i = 0; i < p.key.len && i < 31; ++i)
                dkey.push_back(static_cast<wchar_t>(static_cast<unsigned char>(p.key.data[i])));
            coop::trash_pile_sync::NotifyWireDestroy(dkey);
        }
        remote_prop::OnDestroy(p, localPlayer);
        break;
    }
    case net::ReliableKind::PropConvert: {
        // The atomic trash-clump swap: the mirror ball destroyed by the old eid and the
        // authoritative pile spawned by the new one in one handler, then enrolled in our death
        // watch so a local re-grab propagates the destroy by identity. Same validation as PropSpawn
        // and PropDestroy.
        if (msg.payloadLen < sizeof(net::PropConvertPayload)) {
            UE_LOGW("event_feed: PropConvert payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropConvertPayload));
            break;
        }
        net::PropConvertPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        const float cvals[6] = { p.locX, p.locY, p.locZ,
                                 p.rotPitch, p.rotYaw, p.rotRoll };
        bool cfinite = true;
        for (float v : cvals) { if (!std::isfinite(v)) { cfinite = false; break; } }
        if (!cfinite) {
            UE_LOGW("event_feed: PropConvert floats non-finite -- dropping");
            break;
        }
        constexpr float kMaxConvCoord = 1.0e6f;
        if (std::fabs(p.locX) > kMaxConvCoord || std::fabs(p.locY) > kMaxConvCoord ||
            std::fabs(p.locZ) > kMaxConvCoord) {
            UE_LOGW("event_feed: PropConvert location out of bounds (%.1f, %.1f, %.1f)",
                    p.locX, p.locY, p.locZ);
            break;
        }
        if (p.pileClass.len > 63) {
            UE_LOGW("event_feed: PropConvert pileClass.len=%u > 63 -- dropping", p.pileClass.len);
            break;
        }
        // Either range: a convert re-skins an existing entity's eid, which is the original owner's
        // (host-minted), not the sender's; a client grabbing a host-owned pile sends a host-range
        // eid from a client slot. Only an id in neither range is rejected.
        auto eidOutOfBothRanges = [](uint32_t e) {
            return e != 0u && e != coop::element::kInvalidId &&
                   !coop::element::Registry::IsAllowedHostAllocatedEid(e) &&
                   !coop::element::Registry::IsAllowedPeerAllocatedEid(e);
        };
        if (eidOutOfBothRanges(p.oldEid) || eidOutOfBothRanges(p.newEid)) {
            UE_LOGW("event_feed: PropConvert eids (old=0x%08x new=0x%08x) out of both allocated "
                    "ranges (senderPeerSlot=%d) -- dropping", p.oldEid, p.newEid, msg.senderPeerSlot);
            break;
        }
        // OnConvert spawns the pile through the mirror spawn path, which binds it so a later grab
        // resolves its eid. A null return is a failed spawn (a transient class miss), logged since
        // the re-grab destroy would not propagate.
        if (!remote_prop::OnConvert(p, localPlayer, msg.senderPeerSlot)) {
            UE_LOGW("event_feed: PropConvert newEid=%u pile spawn FAILED -- a re-grab of this "
                    "pile won't propagate its destroy", p.newEid);
        }
        break;
    }
    case net::ReliableKind::PropSnapPos: {
        // A join-window position correction for a save-authoritative chipPile the host moved while
        // our reliable channel was not ready. Host-only, not relayed. Validated, then a pending
        // correction is armed; the bound native is snapped at the quiescence sweep, or immediately
        // if the tail already quiesced.
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: PropSnapPos from non-host senderPeerSlot=%d -- dropping (host-only)",
                    msg.senderPeerSlot);
            break;
        }
        if (msg.payloadLen < sizeof(net::PropSnapPosPayload)) {
            UE_LOGW("event_feed: PropSnapPos payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropSnapPosPayload));
            break;
        }
        net::PropSnapPosPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        const float svals[6] = { p.locX, p.locY, p.locZ, p.rotPitch, p.rotYaw, p.rotRoll };
        bool sfinite = true;
        for (float v : svals) { if (!std::isfinite(v)) { sfinite = false; break; } }
        if (!sfinite) { UE_LOGW("event_feed: PropSnapPos floats non-finite -- dropping"); break; }
        constexpr float kMaxSnapCoord = 1.0e6f;
        if (std::fabs(p.locX) > kMaxSnapCoord || std::fabs(p.locY) > kMaxSnapCoord ||
            std::fabs(p.locZ) > kMaxSnapCoord) {
            UE_LOGW("event_feed: PropSnapPos location out of bounds (%.1f,%.1f,%.1f) -- dropping",
                    p.locX, p.locY, p.locZ);
            break;
        }
        if (p.eid == 0u || p.eid == coop::element::kInvalidId ||
            !coop::element::Registry::IsAllowedHostAllocatedEid(p.eid)) {
            UE_LOGW("event_feed: PropSnapPos eid=0x%08x not a valid host-allocated id -- dropping", p.eid);
            break;
        }
        coop::element::quiescence_drain::ArmPendingPosCorrection(
            p.eid, ue_wrap::FVector{p.locX, p.locY, p.locZ},
            ue_wrap::FRotator{p.rotPitch, p.rotYaw, p.rotRoll});
        // The correction is also identity: the new position is recorded as the entry's
        // host-position overlay (the save position stays immutable, since a purge re-create spawns
        // there), so a re-bind prefers the surviving actor at the new position; and if the entity
        // genuinely moved, a host-vacate twin is armed at the save position so the sweep retires
        // whatever native lingers there, on the host's word.
        {
            ue_wrap::FVector savePos{};
            if (coop::save_identity_bind::UpdateChipHostPos(
                    p.eid, ue_wrap::FVector{p.locX, p.locY, p.locZ}, savePos))
                coop::element::quiescence_drain::ArmHostVacateTwin(p.eid, savePos);
        }
        if (coop::join_membership_sweep::HasLoadTailQuiesced())
            coop::element::quiescence_drain::ApplyPendingPosCorrections();
        break;
    }
    case net::ReliableKind::OwnerEntitySpawn: {
        // The owner-entity lane, peer-owned and relayed: any peer may announce its own stalker
        // entity. Identity is (sender slot, sequence); class validation lives in owner_entity_sync.
        if (msg.payloadLen < sizeof(net::OwnerEntitySpawnPayload)) {
            UE_LOGW("event_feed: OwnerEntitySpawn payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::OwnerEntitySpawnPayload));
            break;
        }
        net::OwnerEntitySpawnPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        const int slot = msg.senderPeerSlot;
        ue_wrap::game_thread::Post([p, slot] {
            ::coop::owner_entity_sync::OnSpawnMsg(p, slot);
        });
        break;
    }
    case net::ReliableKind::OwnerEntityPose: {
        if (msg.payloadLen < sizeof(net::OwnerEntityPosePayload)) break;
        net::OwnerEntityPosePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        const int slot = msg.senderPeerSlot;
        ue_wrap::game_thread::Post([p, slot] {
            ::coop::owner_entity_sync::OnPoseMsg(p, slot);
        });
        break;
    }
    case net::ReliableKind::OwnerEntityDestroy: {
        if (msg.payloadLen < sizeof(net::OwnerEntityDestroyPayload)) break;
        net::OwnerEntityDestroyPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        const int slot = msg.senderPeerSlot;
        ue_wrap::game_thread::Post([p, slot] {
            ::coop::owner_entity_sync::OnDestroyMsg(p, slot);
        });
        break;
    }
    case net::ReliableKind::HookState: {
        // The hook lane's owner half, peer-owned and relayed: any peer may announce a hook it
        // fired. Identity is (sender slot, seq); every field is validated in hook_sync, which is
        // where the class table and the world bounds live.
        if (msg.payloadLen < sizeof(net::HookStatePayload)) {
            UE_LOGW("event_feed: HookState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::HookStatePayload));
            break;
        }
        net::HookStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        const int slot = msg.senderPeerSlot;
        ue_wrap::game_thread::Post([p, slot] {
            ::coop::hook_sync::OnStateMsg(p, slot);
        });
        break;
    }
    case net::ReliableKind::HookDestroy: {
        if (msg.payloadLen < sizeof(net::HookDestroyPayload)) break;
        net::HookDestroyPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        const int slot = msg.senderPeerSlot;
        ue_wrap::game_thread::Post([p, slot] {
            ::coop::hook_sync::OnDestroyMsg(p, slot);
        });
        break;
    }
    case net::ReliableKind::HookAnchorCommit: {
        // Client to host only: a hook that anchored both ends and is being handed over. The
        // arbiter in hook_anchor decides what of the sender's record it is willing to apply.
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: HookAnchorCommit payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload c{};
        std::memcpy(&c, msg.payload, sizeof(c));
        const uint8_t slot = (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                                 ? static_cast<uint8_t>(msg.senderPeerSlot)
                                 : static_cast<uint8_t>(0xFF);
        ue_wrap::game_thread::Post([c, slot] {
            ::coop::hook_anchor::OnCommitChunk(c, slot);
        });
        break;
    }
    case net::ReliableKind::HookAnchored: {
        // Host to all: the host has taken one. One atomic statement on every receiver.
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) break;
        net::BlobChunkPayload c{};
        std::memcpy(&c, msg.payload, sizeof(c));
        const uint8_t slot = (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                                 ? static_cast<uint8_t>(msg.senderPeerSlot)
                                 : static_cast<uint8_t>(0xFF);
        ue_wrap::game_thread::Post([c, slot] {
            ::coop::hook_anchor::OnAnchoredChunk(c, slot);
        });
        break;
    }
    case net::ReliableKind::EntitySpawn: {
        // A host NPC spawn. Size and the class-name length are checked here; npc_mirror does the
        // per-field validation. Its UFunction calls are game-thread only, so it is posted.
        if (msg.payloadLen < sizeof(net::EntitySpawnPayload)) {
            UE_LOGW("event_feed: EntitySpawn payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::EntitySpawnPayload));
            break;
        }
        // Host-authoritative: without the sender gate a client could flood crafted class names and
        // force a GUObjectArray walk per packet on the host's game thread.
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: EntitySpawn from non-host senderPeerSlot=%d "
                    "-- dropping (NPC sync is host-only)",
                    msg.senderPeerSlot);
            break;
        }
        net::EntitySpawnPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        if (p.className.len > 63) {
            UE_LOGW("event_feed: EntitySpawn className.len=%u > 63 -- dropping",
                    p.className.len);
            break;
        }
        net::EntitySpawnPayload pCopy = p;
        ue_wrap::game_thread::Post([pCopy] {
            ::coop::npc_mirror::OnEntitySpawn(pCopy);
        });
        break;
    }
    case net::ReliableKind::EntityDestroy: {
        // A host NPC destroy, posted to the game thread.
        if (msg.payloadLen < sizeof(net::EntityDestroyPayload)) {
            UE_LOGW("event_feed: EntityDestroy payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::EntityDestroyPayload));
            break;
        }
        // Host-authoritative: a client could otherwise destroy any NPC id it learned from a spawn.
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: EntityDestroy from non-host senderPeerSlot=%d "
                    "-- dropping (NPC sync is host-only)",
                    msg.senderPeerSlot);
            break;
        }
        net::EntityDestroyPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        net::EntityDestroyPayload pCopy = p;
        ue_wrap::game_thread::Post([pCopy] {
            ::coop::npc_mirror::OnEntityDestroy(pCopy);
        });
        break;
    }
    case net::ReliableKind::WorldActorSpawn: {
        // A host world-actor spawn (the non-character analogue of EntitySpawn). Host-authoritative
        // for the same reason; the per-field validation lives in world_actor_sync, on the game
        // thread.
        if (msg.payloadLen < sizeof(net::WorldActorSpawnPayload)) {
            UE_LOGW("event_feed: WorldActorSpawn payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::WorldActorSpawnPayload));
            break;
        }
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: WorldActorSpawn from non-host senderPeerSlot=%d -- dropping (host-only)",
                    msg.senderPeerSlot);
            break;
        }
        net::WorldActorSpawnPayload pWa{};
        std::memcpy(&pWa, msg.payload, sizeof(pWa));
        if (pWa.className.len > 63) {
            UE_LOGW("event_feed: WorldActorSpawn className.len=%u > 63 -- dropping", pWa.className.len);
            break;
        }
        // The birth blob's own length claim, checked here and again in the receiver: a length
        // over-running its buffer is a protocol violation.
        if (pWa.birthLen > sizeof(pWa.birth)) {
            UE_LOGW("event_feed: WorldActorSpawn birthLen=%u > %zu -- dropping",
                    pWa.birthLen, sizeof(pWa.birth));
            break;
        }
        net::WorldActorSpawnPayload pWaCopy = pWa;
        ue_wrap::game_thread::Post([pWaCopy] {
            ::coop::world_actor_sync::OnWorldActorSpawn(pWaCopy);
        });
        break;
    }
    case net::ReliableKind::WorldActorDestroy: {
        // A host world-actor destroy. Host-authoritative.
        if (msg.payloadLen < sizeof(net::EntityDestroyPayload)) {
            UE_LOGW("event_feed: WorldActorDestroy payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::EntityDestroyPayload));
            break;
        }
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: WorldActorDestroy from non-host senderPeerSlot=%d -- dropping (host-only)",
                    msg.senderPeerSlot);
            break;
        }
        net::EntityDestroyPayload pWaD{};
        std::memcpy(&pWaD, msg.payload, sizeof(pWaD));
        net::EntityDestroyPayload pWaDCopy = pWaD;
        ue_wrap::game_thread::Post([pWaDCopy] {
            ::coop::world_actor_sync::OnWorldActorDestroy(pWaDCopy);
        });
        break;
    }
    case net::ReliableKind::PyramidGather: {
        // The host's pyramid committed a wisp gather; the client replays the native branch on its
        // mirrors. Host-authoritative, since only the host runs the pyramid.
        if (msg.payloadLen < sizeof(net::PyramidGatherPayload)) {
            UE_LOGW("event_feed: PyramidGather payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PyramidGatherPayload));
            break;
        }
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: PyramidGather from non-host senderPeerSlot=%d -- dropping (host-only)",
                    msg.senderPeerSlot);
            break;
        }
        net::PyramidGatherPayload pGather{};
        std::memcpy(&pGather, msg.payload, sizeof(pGather));
        ue_wrap::game_thread::Post([pGather] {
            ::coop::piramid_sync::OnPyramidGather(pGather);
        });
        break;
    }
    case net::ReliableKind::ItemActivate: {
        // A peer's item state changed with a world effect both peers must see (the flashlight,
        // applied to the puppet's light).
        if (msg.payloadLen < sizeof(net::ItemActivatePayload)) {
            UE_LOGW("event_feed: ItemActivate payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::ItemActivatePayload));
            break;
        }
        net::ItemActivatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // The state is a byte, and only 0 and 1 are valid.
        if (p.state != 0 && p.state != 1) {
            UE_LOGW("event_feed: ItemActivate state=%u out of range -- dropping",
                    static_cast<unsigned>(p.state));
            break;
        }
        // Reserved flag bits must be zero, so a future bit is not silently triggerable by a newer
        // peer.
        if (p.flags & ~coop::net::kItemActivateFlag_HasActorKey) {
            UE_LOGW("event_feed: ItemActivate flags=0x%02x has reserved bits "
                    "set -- dropping",
                    static_cast<unsigned>(p.flags));
            break;
        }
        // Intensity and cone angles go straight to the light setters: NaN is undefined behaviour in
        // the renderer, and 1e30 a blinding screen.
        if (!std::isfinite(p.intensity) ||
            !std::isfinite(p.outerConeAngle) ||
            !std::isfinite(p.innerConeAngle)) {
            UE_LOGW("event_feed: ItemActivate floats non-finite "
                    "(intensity=%.2f outer=%.2f inner=%.2f) -- dropping",
                    p.intensity, p.outerConeAngle, p.innerConeAngle);
            break;
        }
        constexpr float kMaxItemIntensity = 1.0e6f;  // unitless ~10 normal range
        constexpr float kMaxConeAngle     = 180.0f;  // physical degree ceiling
        if (std::fabs(p.intensity) > kMaxItemIntensity ||
            std::fabs(p.outerConeAngle) > kMaxConeAngle ||
            std::fabs(p.innerConeAngle) > kMaxConeAngle) {
            UE_LOGW("event_feed: ItemActivate floats out of bounds "
                    "(intensity=%.2f outer=%.2f inner=%.2f) -- dropping",
                    p.intensity, p.outerConeAngle, p.innerConeAngle);
            break;
        }
        // The self-echo guard: the wire field is the sender's local Player Element id, and equal to
        // ours it is a loopback. The peer-slot fallback fires only when the sender id is unset (a
        // pre-handshake sender); with a valid id the compare is authoritative, so a legitimate
        // packet from a peer at slot X arriving before our own slot reassignment is not misread as
        // an echo.
        const auto selfEid =
            coop::players::Registry::Get().LocalPlayerElementId();
        const bool selfEchoByEid =
            (p.senderElementId != 0u &&
             p.senderElementId != coop::element::kInvalidId &&
             selfEid != coop::element::kInvalidId &&
             p.senderElementId == selfEid);
        const uint8_t selfPeerId = coop::players::Registry::Get().LocalPeerId();
        const bool senderElementIdMissing =
            (p.senderElementId == 0u ||
             p.senderElementId == coop::element::kInvalidId);
        const bool selfEchoByPeerSlotFallback =
            (senderElementIdMissing &&
             msg.senderPeerSlot >= 0 &&
             static_cast<uint8_t>(msg.senderPeerSlot) == selfPeerId);
        if (selfEchoByEid || selfEchoByPeerSlotFallback) {
            UE_LOGI("event_feed: ItemActivate self-echo "
                    "(senderElementId=0x%08x senderPeerSlot=%d via=%s) -- dropping",
                    p.senderElementId, msg.senderPeerSlot,
                    selfEchoByEid ? "eid" : "peerSlot-fallback");
            break;
        }
        // The role-range check on the sender's element id.
        if (!VerifySenderEidRange(msg.senderPeerSlot, p.senderElementId,
                                   "ItemActivate")) {
            break;
        }
        // The sender's element id resolved to a peer slot, falling back to the header slot before
        // the mirror is established.
        uint8_t resolvedSlot = coop::players::kPeerIdUnknown;
        if (p.senderElementId != 0u &&
            p.senderElementId != coop::element::kInvalidId) {
            auto* el = coop::element::Registry::Get().Get(p.senderElementId);
            if (el && el->GetType() == coop::element::ElementType::Player) {
                resolvedSlot =
                    static_cast<coop::element::Player*>(el)->PeerSlot();
            }
        }
        if (resolvedSlot >= net::kMaxPeers) {
            if (msg.senderPeerSlot >= 0 &&
                msg.senderPeerSlot < net::kMaxPeers) {
                resolvedSlot = static_cast<uint8_t>(msg.senderPeerSlot);
            } else {
                UE_LOGW("event_feed: ItemActivate could not resolve sender "
                        "slot (senderElementId=0x%08x senderPeerSlot=%d) -- dropping",
                        p.senderElementId, msg.senderPeerSlot);
                break;
            }
        }
        // The puppet may be null when this packet beats the first pose (the puppet spawns on the
        // first pose, and a reliable message can arrive first in a connect burst);
        // ApplyToPuppetOrDefer stashes the payload until it appears. Only the slot and the payload
        // are captured, and the puppet is re-fetched inside the lambda: Destroy can run between the
        // post and the dispatch and recycle the slot.
        net::ItemActivatePayload pCopy = p;
        const uint8_t peerSlotCopy = resolvedSlot;
        ue_wrap::game_thread::Post([peerSlotCopy, pCopy] {
            ::coop::RemotePlayer* rp =
                ::coop::players::Registry::Get().Puppet(peerSlotCopy);
            void* puppetNow = (rp && rp->valid()) ? rp->GetActor() : nullptr;
            ::coop::item_activate::ApplyToPuppetOrDefer(peerSlotCopy, puppetNow, pCopy);
        });
        break;
    }
    case net::ReliableKind::PropSaveData:
    case net::ReliableKind::PropSaveDataIntent: {
        // A prop's own save record, chunked. The body carries the Key, so nothing here needs to
        // resolve an actor; the module lands it or parks it by identity.
        if (msg.payloadLen < sizeof(net::BlobChunkPayload)) {
            UE_LOGW("event_feed: PropSaveData payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::BlobChunkPayload));
            break;
        }
        net::BlobChunkPayload sd{};
        std::memcpy(&sd, msg.payload, sizeof(sd));
        const uint8_t sdslot =
            (msg.senderPeerSlot >= 0 && msg.senderPeerSlot < net::kMaxPeers)
                ? static_cast<uint8_t>(msg.senderPeerSlot)
                : static_cast<uint8_t>(0xFF);
        coop::prop_save_data::OnChunk(
            session, sd, sdslot, msg.kind == net::ReliableKind::PropSaveDataIntent);
        break;
    }
    default:
        return false;  // not an entity-family kind -> event_feed tries the next family
    }
    return true;  // an entity-family kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
