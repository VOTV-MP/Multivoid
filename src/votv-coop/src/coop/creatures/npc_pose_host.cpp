// coop/npc_pose_host.cpp -- HOST-side NPC transform egress (v37).
//
// Extracted from npc_sync.cpp (2026-06-07) per the 800-LOC soft cap; npc_sync.cpp
// had grown to 869 once the pose stream landed. Owns the two host-only paths that
// READ live NPC actor transforms + push them onto the wire:
//   - TickPoseStream():            per-tick EntityPose batch (unreliable, ~sendHz)
//                                  so client mirrors MOVE between spawn + destroy.
//   - QueueConnectBroadcastForSlot: connect-edge EntitySpawn re-send (reliable) so
//                                  a fresh joiner mirrors NPCs that spawned before
//                                  it joined.
// Both iterate MirrorManager<Npc> with the same actor-bind + liveness guard and
// run on the game thread (net_pump::Tick asserts GT). Receiver-side mirror drive
// is npc_mirror.cpp; host PRE/POST/interceptor + lifecycle stay in npc_sync.cpp.

#include "coop/creatures/npc_sync.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"  // PropMirrors/NpcMirrors/WaMirrors
#include "coop/element/npc.h"
#include "coop/creatures/kerfur_entity.h"  // scope A: GetOriginOffEidForEid -- carry the off->active retire eid
#include "coop/creatures/npc_world_enum.h" // 2026-07-03: DrainPendingExSpawns (EX_CallMath catch)
#include "coop/dev/kerfur_census.h"        // [dev] kerfur_census=1: periodic HOST census (5-vs-6 measurement)
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/kerfur.h"  // HasSaveKey (savePersisted flag) + ReadKerfurState (mirror fidelity)
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"  // NormalizeAxis

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::npc_sync {
namespace {

namespace R = ue_wrap::reflection;

// The single host/mirror Npc set (same template singleton npc_sync.cpp +
// npc_mirror.cpp wrap; MirrorManager<Npc>::Instance() owns the storage).
using coop::element::NpcMirrors;   // canonical accessor (coop/element/mirror_managers.h)

}  // namespace

void QueueConnectBroadcastForSlot(int peerSlot) {
    // HOST-only: re-send EntitySpawn (class + CURRENT transform) for every already-spawned NPC
    // to the freshly-connected client `peerSlot`, so a joiner mirrors NPCs that spawned BEFORE it
    // joined (user 2026-06-04). The client's npc_mirror::OnEntitySpawn materializes each; the
    // MirrorManager::Install is idempotent so a re-send to an already-mirroring peer is a no-op.
    auto* s = GetSession();
    if (!s || s->role() != coop::net::Role::Host) return;
    if (peerSlot < 1) return;  // SendReliableToSlot range-validates the upper bound

    std::vector<coop::element::Npc*> elems;  // connect edge (cold) -- a per-call vector is fine
    NpcMirrors().Snapshot(elems);
    int sent = 0, unbound = 0;
    for (coop::element::Npc* el : elems) {
        if (!el) continue;
        void* actor = el->GetActor();
        // Skip elements with no bound actor / a GC-purged actor; a real game-spawned NPC binds in POST.
        if (!actor || !R::IsLiveByIndex(actor, el->GetInternalIdx())) { ++unbound; continue; }
        coop::net::EntitySpawnPayload p{};
        const std::string& tn = el->GetTypeName();
        p.className.len = 0;
        for (size_t i = 0; i < tn.size() && i < 63; ++i)
            p.className.data[p.className.len++] = tn[i];
        p.elementId = static_cast<uint32_t>(el->GetId());
        const auto loc = ue_wrap::engine::GetActorLocation(actor);
        const auto rot = ue_wrap::engine::GetActorRotation(actor);
        p.locX = loc.X; p.locY = loc.Y; p.locZ = loc.Z;
        p.rotPitch = rot.Pitch; p.rotYaw = rot.Yaw; p.rotRoll = rot.Roll;
        // v75: mark save-persisted NPCs so the joiner ADOPTS its own local twin (class-match in
        // coop/npc_adoption) instead of spawning a duplicate. A non-None int_save Key == this NPC
        // is a save object both peers booted (the kerfur); a host-spawned transient enemy has no
        // key -> savePersisted=0 -> the receiver fresh-spawns a mirror. (v74 shipped the key VALUE
        // for key-equality adoption -- abandoned: the kerfur's key is minted RANDOM per load, so it
        // differs across peers; only the PRESENCE of a key is portable.)
        p.savePersisted = ue_wrap::kerfur::HasSaveKey(actor) ? 1 : 0;
        // scope A (v91 deterministic): carry the off->active dup RETIRE key for a kerfur that was OFF at the
        // blob instant and the host turned ON in the join window. GetOriginOffEidForEid returns the host EID
        // of the off-prop it replaced (kInvalidId for an always-active kerfur). The joiner retires its stale
        // local off-prop MIRROR bound at that EXACT eid (kerfur_reconcile). This rides the npc EntitySpawn --
        // the channel that DOES reach the joiner -- not the KerfurConvert reliable (pre-world-gated mid-join:
        // v56 B2; hands-on 16:37 + 13:21 root).
        const coop::element::ElementId offEid = coop::kerfur_entity::GetOriginOffEidForEid(el->GetId());
        p.retireOffEid = (offEid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(offEid);
        if (s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::EntitySpawn, &p, sizeof(p)))
            ++sent;
    }
    UE_LOGI("npc-sync: connect-snapshot -- sent %d existing NPC(s) to slot %d (%zu Npc Element(s), %d unbound-skipped)",
            sent, peerSlot, elems.size(), unbound);
}

void TickPoseStream() {
    // HOST-only: read each live NPC's transform + CMC velocity each tick + publish ONE EntityPose
    // batch for the net thread to fan out, so the client mirrors MOVE + animate (they otherwise sit
    // at the spawn pose). Always publishes (an EMPTY batch clears it -> stop sending when NPCs vanish).
    // dev-spawned NPCs bind in POST (verified 2026-06-07) so they stream like any real game NPC.
    // Game thread (the net-pump tick asserts GT) -> the scratch statics below are single-threaded.
    auto* s = GetSession();
    if (!s || s->role() != coop::net::Role::Host || !s->connected()) return;

    // 2026-07-03 wisp lane: enroll EX_CallMath-caught spawns queued since the last tick (the
    // Func-thunk fires PRE-Finish; by this tick FinishSpawningActor ran -> real transform).
    coop::npc_world_enum::DrainPendingExSpawns();

    // DIAGNOSTIC ([dev] kerfur_census=1): periodic HOST kerfur census so a hands-on can diff host vs client
    // counts after the world settles (the 5-vs-6 measurement -- the host had no self-census before 2026-06-30).
    // No-op unless the flag is set; throttled internally. Read-only.
    coop::kerfur_census::Tick();

    // Reused scratch (no per-tick heap alloc): Snapshot() clears+fills elems; batch is cleared then
    // refilled in-place and handed to SetLocalNpcPoseBatch BY CONST-REF (it copies), so this static
    // keeps its buffer across ticks. Safe because TickPoseStream is game-thread-only.
    static std::vector<coop::element::Npc*> elems;
    NpcMirrors().Snapshot(elems);
    static std::vector<coop::net::EntityPoseSnapshot> batch;
    batch.clear();

    // 2026-07-03: fair-share rotation for >kMaxNpcBatchEntries tracked NPCs (the 32-wisp swarm
    // overflows the 31-entry MTU cap). Without it Snapshot's stable order starves the SAME tail
    // NPCs every tick (frozen mirrors); rotating the start index spreads the loss evenly
    // (~31/N of ticks each -- the MTA far-sync rotation shape). No wire change.
    static size_t s_rotateCursor = 0;
    const size_t total = elems.size();
    const size_t start = (total > static_cast<size_t>(coop::net::kMaxNpcBatchEntries))
                             ? (s_rotateCursor++ % total) : 0;

    int truncated = 0;
    for (size_t k = 0; k < total; ++k) {
        coop::element::Npc* el = elems[(start + k) % total];
        if (!el) continue;
        void* actor = el->GetActor();
        if (!actor) continue;  // never-bound (POST pending) -- not a death
        if (!R::IsLiveByIndex(actor, el->GetInternalIdx())) {
            // 2026-07-03: a BOUND actor that is no longer live died through a path the
            // K2_DestroyActor PRE cannot see (the wisp's EX_VirtualFunction self-destroy at
            // dawn/proximity). Without this the Element leaks and the client mirror ghosts
            // forever. Eid-keyed retire + EntityDestroy broadcast; deferred teardown.
            coop::npc_sync::SyncDestroyedNpcByEid(el->GetId(), actor);
            continue;
        }
        if (static_cast<int>(batch.size()) >= coop::net::kMaxNpcBatchEntries) { ++truncated; continue; }
        coop::net::EntityPoseSnapshot snap{};
        snap.elementId = static_cast<uint32_t>(el->GetId());
        const auto loc = ue_wrap::engine::GetActorLocation(actor);
        const auto rot = ue_wrap::engine::GetActorRotation(actor);
        const auto vel = ue_wrap::engine::GetActorVelocity(actor);
        snap.x = loc.X; snap.y = loc.Y; snap.z = loc.Z;
        snap.yaw = ue_wrap::NormalizeAxis(rot.Yaw);
        snap.speed = std::sqrt(vel.X * vel.X + vel.Y * vel.Y);  // horizontal velocity magnitude
        snap.stateBits = ue_wrap::puppet::ReadCharacterIsFalling(actor) ? coop::net::kStateBitInAir : uint8_t{0};
        // v39 head-look: read the kerfur AnimBP's resolved `lookAt` WORLD target (the head/neck
        // FAnimNode_LookAt aim point). Only kerfur-family NPCs carry it (ReadKerfurLookAt is
        // class-gated -> false for non-kerfur NPCs); the bit tells the client when lookAt is valid.
        ue_wrap::FVector lookAt{};
        if (ue_wrap::puppet::ReadKerfurLookAt(actor, lookAt)) {
            snap.lookAtX = lookAt.X; snap.lookAtY = lookAt.Y; snap.lookAtZ = lookAt.Z;
            snap.stateBits |= coop::net::kEntityPoseBitHasLookAt;
            static bool s_loggedLook = false;
            if (!s_loggedLook) { s_loggedLook = true;
                UE_LOGI("npc-headlook: host streaming kerfur lookAt=(%.0f,%.0f,%.0f) eid=%u (first)",
                        lookAt.X, lookAt.Y, lookAt.Z, snap.elementId); }
        }
        // v40 body-facing: the kerfur actor BP aims the VISIBLE body (ACharacter::Mesh world yaw) at
        // the LOCAL player, decoupled from the actor root above. Read the resolved mesh world yaw so
        // the mirror reproduces the host's body facing (its actor tick is off -> can't compute it).
        float bodyYaw = 0.f;
        if (ue_wrap::puppet::ReadKerfurBodyYaw(actor, bodyYaw)) {
            snap.bodyYaw = ue_wrap::NormalizeAxis(bodyYaw);
            snap.stateBits |= coop::net::kEntityPoseBitHasBodyYaw;
            static bool s_loggedBody = false;
            if (!s_loggedBody) { s_loggedBody = true;
                UE_LOGI("npc-bodyfacing: host streaming kerfur bodyYaw=%.1f eid=%u (first)",
                        snap.bodyYaw, snap.elementId); }
        }
        // v74 host-authoritative kerfur cosmetic/command state: the parked mirror runs no AI
        // (actor tick off + timers neutralized) so it can't pick its own command/face. Stream
        // the host kerfur's State (enum_kerfurCommand -> AnimBP state machine) + isSpooky + face
        // index so the mirror's body animation matches. Class-gated (non-kerfur NPCs skip).
        uint8_t kState = 0, kFace = 0; bool kSpooky = false;
        if (ue_wrap::kerfur::ReadKerfurState(actor, kState, kSpooky, kFace)) {
            snap.kerfState = kState;
            snap.kerfFace  = kFace;
            snap.stateBits |= coop::net::kEntityPoseBitHasKerfurState;
            if (kSpooky) snap.stateBits |= coop::net::kEntityPoseBitKerfurSpooky;
        }
        batch.push_back(snap);
    }
    if (truncated > 0) {
        static bool s_warnedTrunc = false;
        if (!s_warnedTrunc) { s_warnedTrunc = true;
            UE_LOGW("npc-pose: >%d NPCs this tick -- %d rotated out of this batch (fair-share "
                    "rotation active: each NPC streams ~%d/N of ticks; a persistent count this "
                    "high may warrant multi-datagram batches)",
                    coop::net::kMaxNpcBatchEntries, truncated, coop::net::kMaxNpcBatchEntries); }
    }
    static bool s_loggedFirst = false;
    if (!batch.empty() && !s_loggedFirst) { s_loggedFirst = true;
        UE_LOGI("npc-pose: streaming %zu NPC(s) -> peers (first batch)", batch.size()); }
    s->SetLocalNpcPoseBatch(batch);  // by const-ref copy; our static `batch` keeps its buffer
}

}  // namespace coop::npc_sync
