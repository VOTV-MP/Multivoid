// coop/event_dispatch_entity.cpp -- the entity-lifecycle + held-item reliable-kind
// case bodies (PropRelease / PropSpawn / PropDestroy / PropConvert / EntitySpawn /
// EntityDestroy / ItemActivate), extracted VERBATIM from event_feed.cpp's Update
// switch (2026-06-11 modularity extraction; see coop/event_dispatch.h).

#include "event_dispatch.h"  // co-located private header (src tree, not include/)

#include "coop/element/player.h"
#include "coop/element/registry.h"

#include "coop/player/item_activate.h"
#include "coop/session/join_progress.h"
#include "coop/creatures/npc_mirror.h"
#include "coop/element/quiescence_drain.h"  // b3: ArmPendingPosCorrection / ApplyPendingPosCorrections (PropSnapPos)
#include "coop/props/save_identity_bind.h"  // b3 OWNER: UpdateChipSavePosAndGetOld (retrack identity key on PropSnapPos)
#include "coop/player/players_registry.h"
#include "coop/creatures/world_actor_sync.h"  // v80 (B3b): non-Character event-actor mirror receivers
#include "coop/props/prop_stick_sync.h"
#include "coop/player/remote_player.h"
#include "coop/props/remote_prop.h"
#include "coop/props/remote_prop_spawn.h"
#include "coop/props/join_membership_sweep.h"  // anti-smear 2026-06-30: claim+sweep extracted out of remote_prop_spawn
#include "coop/props/trash_pile_sync.h"

#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/sdk_profile.h"

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
        // v68: a peer stuck a wall-attachable (camera) to a surface. Symmetric
        // prop state (host relays client sticks). prop_stick_sync stops any
        // drive, re-poses and replays the BP's own forceStick (raw frozen-
        // write fallback on trace divergence).
        if (msg.payloadLen < sizeof(net::PropStickStatePayload)) {
            UE_LOGW("event_feed: PropStickState payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropStickStatePayload));
            break;
        }
        net::PropStickStatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // Pose trust-boundary: reject NaN/Inf before any engine write (the
        // PropRelease velocity precedent below).
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
        // v5: peer released a held prop. Dispatch to remote_prop which
        // re-enables SimulatePhysics + sets linear/angular velocity, and
        // fires Aprop_C.thrown if the launch crosses the throw threshold.
        if (msg.payloadLen < sizeof(net::PropReleasePayload)) {
            UE_LOGW("event_feed: PropRelease payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropReleasePayload));
            break;
        }
        // senderPeerSlot threads into remote_prop::OnRelease for routing
        // into g_drives[slot]. Range-check before use so a malformed -1
        // or out-of-range value can't OOB the slot array.
        if (msg.senderPeerSlot < 0 || msg.senderPeerSlot >= net::kMaxPeers) {
            UE_LOGW("event_feed: PropRelease invalid senderPeerSlot=%d -- dropping",
                    msg.senderPeerSlot);
            break;
        }
        net::PropReleasePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // Trust-boundary validation: a NaN/Inf or absurd-magnitude velocity
        // reaches UPrimitiveComponent::SetPhysicsLinearVelocity ->
        // SetPhysicsAngularVelocityInDegrees -> PhysX UB. Reject before
        // dispatch.
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
        // Linear velocity bound: realistic throws peak at a few thousand
        // cm/s. 1e6 cm/s = 10 km/s -- well beyond any legitimate throw and
        // below any value that would teleport a body to infinity in one
        // tick. Angular velocity bound: a fast tumble is ~3600 deg/s
        // (10 rps); 1e6 is generous headroom.
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
        // v5 Bug C: peer dropped an inventory item -- spawn a matching
        // Aprop_X_C locally so subsequent PropPose updates resolve.
        if (msg.payloadLen < sizeof(net::PropSpawnPayload)) {
            UE_LOGW("event_feed: PropSpawn payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::PropSpawnPayload));
            break;
        }
        net::PropSpawnPayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // Trust-boundary validation: any of the 18 floats (loc/rot/scale +
        // 2 vel vectors + v86 the Path 1c save-time match key) NaN/Inf or
        // out-of-bound -> SpawnActor + SetPhysics* could crash PhysX, and a
        // NaN matchX/Y/Z would slip past the twin-destroy's `dist > 1cm`
        // guard (NaN compares false) and destroy a wrong local native pile.
        // Reject before dispatch. matchX/Y/Z are 0 (finite) on a non-stamped
        // payload, so validating all 18 is always safe.
        const float vals[18] = {
            p.locX, p.locY, p.locZ,
            p.rotPitch, p.rotYaw, p.rotRoll,
            p.scaleX, p.scaleY, p.scaleZ,
            p.initLinVelX, p.initLinVelY, p.initLinVelZ,
            p.initAngVelX, p.initAngVelY, p.initAngVelZ,
            p.matchX, p.matchY, p.matchZ  // v86 Path 1c save-time key
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
        // Clamp class/key lengths defensively (they're uint8 but the
        // sender could lie). 63/31 are the struct caps.
        if (p.className.len > 63) {
            UE_LOGW("event_feed: PropSpawn className.len=%u > 63 -- dropping", p.className.len);
            break;
        }
        if (p.key.len > 31) {
            UE_LOGW("event_feed: PropSpawn key.len=%u > 31 -- dropping", p.key.len);
            break;
        }
        // PR-FOUNDATION-1 (2026-05-29): elementId range trust. The
        // sender's role determines which allocation range its
        // PropSpawn.elementId is allowed to land in (host -> host
        // range; client -> peer range per the A2 v12 contract:
        // chipPile/clump/trashBits broadcast client->host so a
        // client-sourced PropSpawn carries a peer-range eid). A
        // packet carrying an eid outside its sender's permitted
        // range is forged or relay-loop bugged and must be dropped
        // at the boundary before it reaches RegisterPropMirror and
        // collides with the receiver's own allocator. Closes
        // D2-2 / E-1's PropSpawn gap.
        //
        // Fork B 2a (2026-06-10): the HOST side is relaxed to EITHER
        // range. A snapshot bracket RE-EXPRESSES existing entities --
        // including client-born ones the host holds as mirror Elements
        // (the drain never filters by origin) -- and a re-bracket
        // (cave travel / host save-load) must re-express a client's
        // own dropped items back to it or the widened adoption sweep
        // would destroy them as unclaimed. Same argument already
        // shipped for PropDestroy ("a destroy is NOT an allocation --
        // it references an EXISTING shared entity") and matches the
        // documented MarkPropElement intent ("clients route them as
        // MIRROR entries which is correct"). A CLIENT sender may still
        // only allocate in its own peer range.
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
        // (v15 added a senderContext compare here for stale-generation
        // defense; v16 PR-FOUNDATION-1b moved that to the packet
        // header's senderEpoch latched in Session::HandleMessage.)
        // intermediate-variant classes that the receiver doesn't want
        // (mushroom7_C growing state). Host-authoritative growth
        // pipeline -- the mature variant (mushroom_C) will arrive when
        // host's transform-timer fires. Mirrors the role==Client +
        // IsClientSuppressedPropClass check in harness.cpp::
        // GrabObserver_Aprop_Init_POST so the suppression is symmetric:
        // never spawn locally AND never accept wire spawns of these.
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
        // v34: advance the join loading-screen bar. No-op unless a join snapshot is
        // in progress (join_progress is in the Receiving phase) -- so live PropSpawns
        // outside a join cost a single relaxed atomic load and return.
        coop::join_progress::NotePropApplied();
        break;
    }
    case net::ReliableKind::PropDestroy: {
        // v5 Inc2: peer destroyed a prop -- delete the matching local
        // actor (resolved via FindByKeyString). Receiver-side
        // K2_DestroyActor is echo-suppressed via the incoming-destroy
        // set so it doesn't bounce back to the sender.
        //
        // TRUST BOUNDARY: with bidirectional destroy, CLIENT can command
        // HOST to destroy any prop by wire-Key. Acceptable for LAN coop
        // (trusted peers); review before Internet coop -- a malicious
        // client could replay crafted Keys to destroy host's quest items.
        // Mitigation if needed: authority model (host validates destroy
        // requests against current world state / quest progress).
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
        // elementId range trust. UNLIKE PropSpawn, a destroy is NOT an allocation -- it
        // references an EXISTING shared entity, which the OTHER peer may have allocated.
        // The grab-destroy model is symmetric: whoever grabs a shared chipPile broadcasts
        // its destroy by eid, and that eid is in the ORIGINAL owner's range, not the
        // grabber's. So we must accept EITHER range here (was: IsAllowedSenderEid(role) ->
        // it dropped a client's PropDestroy of a host-owned pile = the cross-grab clump DUPE,
        // proven 2026-06-09 host log "0x918 out of allowed peer range"). We still reject a
        // genuinely invalid id (0 / kInvalidId / out of both ranges) so UnregisterPropMirror /
        // OnDestroy never see a forged out-of-bounds eid. (Trust note above already documents
        // that a LAN peer can command any prop destroy -- this is consistent with that model.)
        if (p.elementId != 0 && p.elementId != coop::element::kInvalidId &&
            !coop::element::Registry::IsAllowedHostAllocatedEid(p.elementId) &&
            !coop::element::Registry::IsAllowedPeerAllocatedEid(p.elementId)) {
            UE_LOGW("event_feed: PropDestroy elementId=0x%08x not a valid allocated id "
                    "(out of both ranges) -- dropping", p.elementId);
            break;
        }
        // (v15 also had a senderContext compare here -- moved to
        // header senderEpoch in v16 PR-FOUNDATION-1b.)
        // 2026-05-25 cross-peer destroy: pass localPlayer so OnDestroy
        // can release a held PHC grab (mainPlayer.grabbing_actor ==
        // doomed) before K2_DestroyActor. Prevents UPhysicsHandle
        // Component::TickComponent reading a dangling GrabbedComponent
        // ptr next frame.
        // (The chipPile death-watch echo guard that lived here -- NotifyPileConsumed(p.elementId) --
        // is GONE 2026-06-17 with the pile death-watch retirement: there is no per-pile watch entry to
        // drop, and the InpActEvt_use grab observer only fires on a local E-press, never on a
        // wire-driven destroy, so a wire destroy can never echo back out.)
        // v57: echo guard for the trashBitsPile counter channel, keyed -- a wire
        // destroy of a dispenser pile must not re-broadcast from OUR death-watch.
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
        // v52: the ATOMIC trash-clump ball->pile swap. Destroy the mirror ball by oldEid +
        // spawn the authoritative pile by newEid in one handler (remote_prop::OnConvert), then
        // enrol the spawned mirror pile in OUR death-watch so a local re-grab of it propagates
        // the destroy by identity. Same trust-boundary validation as PropSpawn/PropDestroy:
        // finite transform + in-range eids (both old + new must be within the sender's range).
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
        // v81 MORPH V2 either-range trust: a PropConvert re-skins an EXISTING shared entity's eid E
        // (oldEid==newEid==E), and E is the ORIGINAL owner's (host-minted) id -- NOT the sender's. When
        // a CLIENT grabs a host-owned pile and broadcasts the morph, E is host-range with a client
        // sender slot; the old per-role IsAllowedSenderEid(role) check would DROP it (the exact failure
        // a host-grab-only smoke masks -- symmetric with the PropDestroy either-range fix above). Accept
        // EITHER range; still reject a genuinely invalid id (0 / kInvalidId / out of both ranges).
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
        // OnConvert spawns the pile via remote_prop_spawn::OnSpawn (which RegisterPropMirror-binds it,
        // so a later grab resolves its eid via the InpActEvt_use observer -> PropDestroy). A null
        // return = the pile spawn failed (e.g. a transient FindClass miss before the chipPile BP class
        // loaded); rare, but log it since the re-grab destroy won't propagate.
        if (!remote_prop::OnConvert(p, localPlayer, msg.senderPeerSlot)) {
            UE_LOGW("event_feed: PropConvert newEid=%u pile spawn FAILED -- a re-grab of this "
                    "pile won't propagate its destroy", p.newEid);
        }
        break;
    }
    case net::ReliableKind::PropSnapPos: {
        // b3 (v90): a join-window position correction for a save-authoritative chipPile the host moved
        // while OUR reliable channel wasn't ready (the move's PropConvert was dropped). Host-authoritative:
        // a client never legitimately sends it (and it is not relayed). Validate at the trust boundary
        // (finite + in-bounds + in-range eid), then ARM a pending correction; the bound native is snapped
        // at the quiescence sweep (where the bind has registered it + the load tail is settled). If we
        // already quiesced (a late arrival after the sweep fired), apply immediately.
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
        // b3 OWNER (docs/piles/12): this PropSnapPos is the host's AUTHORITATIVE "E is at @new" -- use it for
        // IDENTITY, not just to nudge the actor. (1) record @new as the entry's HOST-POS OVERLAY (savePos
        // stays immutable -- it is where a purge re-create spawns; the old retrack made the eid permanently
        // unbindable, the eid=4435 dup + 4 Hz drain root); RE-BIND then prefers the surviving actor @new over
        // the stale copy @save. (2) if E genuinely moved, arm a host-vacate twin at the IMMUTABLE @save so the
        // sweep retires whatever save-loaded native lingers there -- on the host's word, no position-guess.
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
    case net::ReliableKind::EntitySpawn: {
        // Host-broadcast NPC spawn. Validate size + className.len at the
        // trust boundary here; npc_mirror::OnEntitySpawn does the full
        // per-field validation (finite + bounds + allowlist + dedup).
        // UFunction calls inside OnEntitySpawn are game-thread only, so
        // dispatch via GT::Post.
        if (msg.payloadLen < sizeof(net::EntitySpawnPayload)) {
            UE_LOGW("event_feed: EntitySpawn payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::EntitySpawnPayload));
            break;
        }
        // EntitySpawn is host-authoritative. Without a senderPeerSlot
        // trust gate, a malicious client could flood EntitySpawn packets
        // with crafted className strings, forcing R::FindClass
        // GUObjectArray walks on the host's game thread per packet (CPU
        // amplification).
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
        // Host-broadcast NPC destroy. Dispatch to npc_mirror::
        // OnEntityDestroy (game-thread UFunction call).
        if (msg.payloadLen < sizeof(net::EntityDestroyPayload)) {
            UE_LOGW("event_feed: EntityDestroy payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::EntityDestroyPayload));
            break;
        }
        // Host-authoritative -- reject non-host senders. A malicious
        // client could otherwise destroy any NPC element id it learned
        // from a legitimate EntitySpawn.
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
        // v80 (B3b): host-broadcast non-Character event-actor spawn (the WorldActor analogue of
        // EntitySpawn). Host-authoritative -- reject non-host senders (a malicious client could
        // otherwise flood crafted classNames forcing FindClass walks on the host game thread).
        // OnWorldActorSpawn does the full per-field validation (finite + bounds + allowlist + dedup);
        // its UFunction calls are game-thread only, so dispatch via GT::Post.
        if (msg.payloadLen < sizeof(net::EntitySpawnPayload)) {
            UE_LOGW("event_feed: WorldActorSpawn payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::EntitySpawnPayload));
            break;
        }
        if (msg.senderPeerSlot != 0) {
            UE_LOGW("event_feed: WorldActorSpawn from non-host senderPeerSlot=%d -- dropping (host-only)",
                    msg.senderPeerSlot);
            break;
        }
        net::EntitySpawnPayload pWa{};
        std::memcpy(&pWa, msg.payload, sizeof(pWa));
        if (pWa.className.len > 63) {
            UE_LOGW("event_feed: WorldActorSpawn className.len=%u > 63 -- dropping", pWa.className.len);
            break;
        }
        net::EntitySpawnPayload pWaCopy = pWa;
        ue_wrap::game_thread::Post([pWaCopy] {
            ::coop::world_actor_sync::OnWorldActorSpawn(pWaCopy);
        });
        break;
    }
    case net::ReliableKind::WorldActorDestroy: {
        // v80 (B3b): host-broadcast non-Character event-actor destroy (the WorldActor analogue of
        // EntityDestroy). Host-authoritative.
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
    case net::ReliableKind::ItemActivate: {
        // Phase 5F flashlight (and future radio/torch/lamp) -- peer's
        // item state changed and produces a WORLD effect both peers
        // must see. For Case (b) flashlight: apply to the puppet's
        // light_R. RE doc: research/findings/votv-flashlight-RE-
        // 2026-05-25.md.
        if (msg.payloadLen < sizeof(net::ItemActivatePayload)) {
            UE_LOGW("event_feed: ItemActivate payload too short (%zu < %zu)",
                    static_cast<size_t>(msg.payloadLen), sizeof(net::ItemActivatePayload));
            break;
        }
        net::ItemActivatePayload p{};
        std::memcpy(&p, msg.payload, sizeof(p));
        // Trust-boundary: state is a uint8 but only 0/1 are valid.
        if (p.state != 0 && p.state != 1) {
            UE_LOGW("event_feed: ItemActivate state=%u out of range -- dropping",
                    static_cast<unsigned>(p.state));
            break;
        }
        // Reserved flag bits must be zero. A future bit added in v15+
        // would otherwise be silently triggerable by a peer on an older
        // build.
        if (p.flags & ~coop::net::kItemActivateFlag_HasActorKey) {
            UE_LOGW("event_feed: ItemActivate flags=0x%02x has reserved bits "
                    "set -- dropping",
                    static_cast<unsigned>(p.flags));
            break;
        }
        // Intensity + cone angles are passed directly to UE light
        // component setters. Without finite + magnitude
        // checks, a peer can send NaN/Inf (UB inside the renderer)
        // or 1e30 (blinding white screen). Matches the validator
        // pattern every other float-bearing reliable kind uses.
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
        // Self-echo guard via ElementId equality (uint32 compare). The
        // wire field is the SENDER's local Player Element id; if it
        // equals our own local Player Element id, this packet is a
        // loopback bounce. The peer-slot fallback only fires when
        // senderElementId is the 0/unset sentinel (boot/seed
        // pre-handshake sender). With a valid senderElementId, the
        // ElementId compare is authoritative -- gating the peer-slot
        // compare on it prevents an N-peer reassignment race from
        // mis-classifying a legitimate cross-peer packet as a self
        // loopback (e.g. a packet from a peer at slot X arriving before
        // our own AssignPeerSlot reassigned us off slot X).
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
        // PR-FOUNDATION-1 (2026-05-29): role-range trust boundary on
        // senderElementId. v16 (PR-FOUNDATION-1b) replaces v14's
        // syncContext compare with the Session-layer senderEpoch
        // latch (applied before HandleMessage dispatches here).
        if (!VerifySenderEidRange(msg.senderPeerSlot, p.senderElementId,
                                   "ItemActivate")) {
            break;
        }
        // Resolve senderElementId -> peer slot via Registry::Get. Falls
        // back to msg.senderPeerSlot when the mirror hasn't been
        // established yet (early boot before Join/AssignPeerSlot landed).
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
        // The puppet may be null if this packet beat the first
        // PoseSnapshot (puppet spawned lazily on first pose; ItemActivate
        // rides the reliable channel and CAN arrive first under a
        // connect-edge burst). ApplyToPuppetOrDefer stashes the payload
        // when the puppet isn't ready; TickConnect drains it once the
        // puppet appears in the registry.
        //
        // Audit C1 (2026-05-27): capture only resolvedSlot + payload by
        // value into the lambda; re-fetch puppet INSIDE the lambda. The
        // raw void* would risk UAF because Destroy() can run on the
        // game thread between this post and the lambda dispatch,
        // recycling the GUObjectArray slot.
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
    default:
        return false;  // not an entity-family kind -> event_feed tries the next family
    }
    return true;  // an entity-family kind was matched (processed or validation-dropped)
}

}  // namespace coop::event_feed
