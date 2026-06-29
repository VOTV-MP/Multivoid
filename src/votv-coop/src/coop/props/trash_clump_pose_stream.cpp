// coop/trash_clump_pose_stream.cpp -- see coop/trash_clump_pose_stream.h.

#include "coop/props/trash_clump_pose_stream.h"

#include "coop/props/active_drive.h"        // ActiveDrive + BeginLerpToPose / AdvanceLerp (the shared interp)
#include "coop/net/protocol.h"        // TrashClumpPoseSnapshot
#include "coop/net/session.h"         // TakeRemoteTrashCarryBatch
#include "coop/props/trash_channel.h"       // IsInboundStreamCtxFresh / CtxForEid (the stale-pose / pile-jump guard)
#include "coop/props/trash_proxy.h"         // ProxyActorForEid (resolve the per-eid target)
#include "ue_wrap/hot_path_guard.h"   // UE_ASSERT_GAME_THREAD
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"       // IsLive

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace coop::trash_clump_pose_stream {
namespace {

namespace R  = ue_wrap::reflection;
namespace AD = coop::active_drive;

// One ActiveDrive per host-driven trash clump, keyed by eid (NOT a per-slot array -- N clients can each
// carry a clump at once). GAME-THREAD only (every entry point runs on the net-pump game thread).
std::unordered_map<uint32_t, AD::ActiveDrive> g_carryDrives;

// Throttled apply log (first few + every 60th) per the perf discipline.
uint32_t g_applyCount = 0;

}  // namespace

void TickApplyAndDrive(coop::net::Session& s) {
    UE_ASSERT_GAME_THREAD("trash_clump_pose_stream::TickApplyAndDrive");
    const uint64_t nowMs = AD::NowMs();

    // 1. Drain the latest host batch (consume-once; an empty/absent batch leaves the existing drives to
    //    AdvanceLerp between packets / freeze on a stop). Only the HOST populates it -> on a host this
    //    returns false and we just advance nothing.
    std::vector<coop::net::TrashClumpPoseSnapshot> batch;
    if (s.TakeRemoteTrashCarryBatch(batch)) {
        for (const auto& snap : batch) {
            const coop::element::ElementId E = static_cast<coop::element::ElementId>(snap.eid);
            // ctx-gate (requireCurrentGen, like remote_prop's PropPose gate): a carry pose for a generation
            // whose ToClump convert we have NOT adopted yet (ctx != known) is HELD -- applying it would drive
            // the still-PRE-convert PILE rendering to the carry point + fire its grab cue before the re-skin
            // (the pile-jump + double-grab-cue glitch). The reliable convert always lands; the continuous
            // stream supersedes the held pose, so the carry just starts AT the convert.
            if (!coop::trash_channel::IsInboundStreamCtxFresh(snap.eid, snap.ctx, /*requireCurrentGen=*/true))
                continue;
            void* proxy = coop::trash_proxy::ProxyActorForEid(E);
            if (!proxy) continue;  // no live proxy for this eid yet (its PropSpawn / convert has not landed)
            AD::ActiveDrive& d = g_carryDrives[snap.eid];
            if (d.actor != proxy || d.lastEid != snap.eid) {   // first pose for this eid, or the proxy actor changed
                AD::ResetDriveState(d);
                d.actor   = proxy;
                d.isProxy = true;            // host-authoritative follower: freeze on a gap, never drop to physics
                d.lastEid = snap.eid;
            }
            AD::BeginLerpToPose(d, ue_wrap::FVector{snap.x, snap.y, snap.z},
                                ue_wrap::FRotator{snap.pitch, snap.yaw, snap.roll}, nowMs);
            const uint32_t n = ++g_applyCount;
            if (n <= 3 || (n % 60) == 0)
                UE_LOGI("[TRASH-CARRY] CLIENT APPLY eid=%u ctx=%u -> target(%.1f,%.1f,%.1f) (per-eid carry drive)",
                        snap.eid, static_cast<unsigned>(snap.ctx), snap.x, snap.y, snap.z);
        }
    }

    // 2. Advance EVERY drive one tick (whether or not a new pose arrived): smooth follow between sendHz
    //    poses + freeze at the last target on a stream gap. Prune a drive whose proxy actor died (a retire
    //    that did not route through ClearDriveForEid -- defensive; AdvanceLerp itself no-ops on a dead actor).
    for (auto it = g_carryDrives.begin(); it != g_carryDrives.end(); ) {
        AD::ActiveDrive& d = it->second;
        if (!d.actor || !R::IsLive(d.actor)) { it = g_carryDrives.erase(it); continue; }
        AD::AdvanceLerp(d, nowMs);
        ++it;
    }
}

void ClearDriveForEid(coop::element::ElementId eid) {
    UE_ASSERT_GAME_THREAD("trash_clump_pose_stream::ClearDriveForEid");
    if (g_carryDrives.erase(static_cast<uint32_t>(eid)))
        UE_LOGI("[TRASH-CARRY] CLIENT clear carry drive eid=%u (land / retire)", static_cast<unsigned>(eid));
}

void OnDisconnect() {
    g_carryDrives.clear();
}

}  // namespace coop::trash_clump_pose_stream
