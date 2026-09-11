// coop/props/prop_drive_stream.cpp -- see coop/props/prop_drive_stream.h.

#include "coop/props/prop_drive_stream.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/active_drive.h"
#include "coop/props/prop_element_tracker.h"  // FindLiveActorByKey: the index only, never the cold walk
#include "coop/props/prop_wire_parity.h"      // the join converge's own physics restore
#include "coop/props/remote_prop.h"           // ResolveLiveActorByEid, IsActorUnderAnyDrive, DriveSimulate
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::prop_drive_stream {
namespace {

namespace AD = coop::active_drive;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;

struct Drive {
    AD::ActiveDrive d;
    uint8_t gen = 0;
    bool physicsParked = false;   // this module turned simulation off, so the end turns it on
};
std::unordered_map<uint32_t, Drive>   g_drives;    // eid -> drive
std::unordered_map<uint32_t, uint8_t> g_endedGen;  // eid -> the generation its end edge closed

// A pose whose prop has not arrived here is retried on a throttle, never per tick: the host
// re-sends every tick the prop moves, and though the resolve is two O(1) lookups, a line per
// miss is not. One line names a sustained miss.
struct Miss { uint64_t nextTryMs = 0; uint32_t count = 0; };
std::unordered_map<uint32_t, Miss> g_miss;
constexpr uint64_t kMissRetryMs = 250;
constexpr uint32_t kMissSaidAt  = 40;     // about ten seconds of misses: one line

// An end edge whose prop has not arrived here yet -- a joiner mid-drag whose snapshot is still
// landing -- is kept until the prop resolves, so its copy still gets the rest pose the host's
// came to, or until it expires.
struct PendingEnd { coop::net::PropDriveEndPayload p; uint64_t expiresMs = 0; };
std::unordered_map<uint32_t, PendingEnd> g_pendingEnd;
constexpr uint64_t kPendingEndMs = 30000;

// The drained batch, kept across ticks for its capacity: the session swaps its buffer with this
// one, so the steady state of a drag allocates nothing on either thread. Game thread only.
std::vector<coop::net::PropPoseSnapshot> g_batch;

// Newer-than on the wrapping byte the stream carries.
bool GenAfter(uint8_t a, uint8_t b) { return static_cast<int8_t>(a - b) > 0; }

// A frozen or static prop keeps its physics off whatever the stream did: it is moved kinematically
// and left so, the stuck wall-attachable rule the held-prop receiver applies.
bool PhysicsStaysOff(void* actor) { return PR::IsFrozen(actor) || PR::IsStatic(actor); }

// By eid first -- the registry's O(1) row, which a joiner's keyed copy carries from the bind --
// then the key INDEX; never the cold object-array walk the general key resolver falls back to.
void* Resolve(uint32_t eid, const coop::net::WireKey& key) {
    if (void* a = coop::remote_prop::ResolveLiveActorByEid(eid)) return a;
    if (key.len == 0) return nullptr;
    return coop::prop_element_tracker::FindLiveActorByKey(coop::remote_prop::KeyToWString(key));
}

void GiveBackPhysics(Drive& dr, void* actor) {
    if (!dr.physicsParked || !actor || PhysicsStaysOff(actor)) return;
    coop::remote_prop::DriveSimulate(dr.d.mesh, true);
    dr.physicsParked = false;
}

// The end edge on a live prop: the final pose, then the host's physics flags through the same
// parity restore the join's converge applies after its teleport, then the velocity -- only into
// a body this module parked and the flags leave simulating, and only when it is not zero, since
// assigning a velocity wakes a body at rest.
void ApplyEnd(void* actor, const coop::net::PropDriveEndPayload& p, bool parkedHere) {
    E::SetActorLocation(actor, ue_wrap::FVector{p.x, p.y, p.z});
    E::SetActorRotation(actor, ue_wrap::FRotator{p.pitch, p.yaw, p.roll});
    if (PhysicsStaysOff(actor)) return;
    coop::prop_wire_parity::RestoreSpParityPhysicsAfterConverge(actor, p.physFlags);
    const float lin2 = p.linVelX * p.linVelX + p.linVelY * p.linVelY + p.linVelZ * p.linVelZ;
    if (parkedHere && coop::prop_wire_parity::SpParitySimulate(p.physFlags) && lin2 > 0.f) {
        void* mesh = PR::GetStaticMesh(actor);
        coop::remote_prop::DriveSetLinearVelocity(mesh, p.linVelX, p.linVelY, p.linVelZ);
        coop::remote_prop::DriveSetAngularVelocity(mesh, p.angVelX, p.angVelY, p.angVelZ);
    }
}

}  // namespace

void TickApplyAndDrive(coop::net::Session& s) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::TickApplyAndDrive");
    const uint64_t nowMs = AD::NowMs();
    g_batch.clear();
    if (s.TakeRemotePropDriveBatch(g_batch)) {
        for (const coop::net::PropPoseSnapshot& e : g_batch) {
            const uint32_t eid = e.elementId;
            if (eid == 0) continue;
            // The generation gate: a pose of a closed generation, or older than the live drive's,
            // is a datagram that outlived its stream.
            auto ended = g_endedGen.find(eid);
            if (ended != g_endedGen.end() && !GenAfter(e.ctx, ended->second)) continue;
            auto it = g_drives.find(eid);
            if (it != g_drives.end()) {
                if (GenAfter(it->second.gen, e.ctx)) continue;
                if (it->second.gen == e.ctx) {
                    // The steady state: the row already names the actor. No resolve, no string.
                    if (void* live = it->second.d.LiveActor()) {
                        if (!coop::remote_prop::IsActorUnderAnyDrive(live))   // a hand's stream wins
                            AD::BeginLerpToPose(it->second.d, ue_wrap::FVector{e.x, e.y, e.z},
                                                ue_wrap::FRotator{e.pitch, e.yaw, e.roll}, nowMs);
                        continue;
                    }
                }
            }
            // A first pose, a new generation, or a row whose actor died: resolve, on a throttle.
            auto miss = g_miss.find(eid);
            if (miss != g_miss.end() && nowMs < miss->second.nextTryMs) continue;
            void* actor = Resolve(eid, e.key);
            if (!actor) {
                Miss& m = g_miss[eid];
                m.nextTryMs = nowMs + kMissRetryMs;
                if (++m.count == kMissSaidAt)
                    UE_LOGI("[PROP-DRIVE] CLIENT eid=%u has not resolved in %u tries -- the host is "
                            "streaming a prop this peer does not have (a join still landing, or a "
                            "prop it never received)", eid, m.count);
                continue;
            }
            g_miss.erase(eid);
            if (!PR::IsDescendantOfProp(actor)) continue;
            // A hand's stream on the same prop wins: the holder is its syncer for as long as it
            // holds it, and the host closes this stream the moment it sees the hand.
            if (coop::remote_prop::IsActorUnderAnyDrive(actor)) continue;
            Drive& dr = g_drives[eid];
            if (dr.d.actor && dr.d.actor != actor) GiveBackPhysics(dr, dr.d.LiveActor());
            AD::ResetDriveState(dr.d);
            dr.d.actor    = actor;
            dr.d.actorIdx = R::InternalIndexOf(actor);
            dr.d.mesh     = PR::GetStaticMesh(actor);
            dr.d.isProxy  = true;    // the fixed-delay follow of a carried clump: freeze on a gap
            dr.d.lastEid  = eid;
            dr.gen        = e.ctx;
            dr.physicsParked = false;
            if (dr.d.mesh && !PhysicsStaysOff(actor)) {
                coop::remote_prop::DriveSimulate(dr.d.mesh, false);
                dr.physicsParked = true;
            }
            if (ended != g_endedGen.end()) g_endedGen.erase(ended);
            // A pose of a closed generation never reaches here, so a pending end for this prop is
            // of an OLDER generation than the one parking now: stale, and dropped before it can
            // land on the new stream.
            g_pendingEnd.erase(eid);
            UE_LOGI("[PROP-DRIVE] CLIENT park eid=%u gen=%u actor=%p (physics %s)",
                    eid, static_cast<unsigned>(e.ctx), actor,
                    dr.physicsParked ? "off" : "left as it was");
            AD::BeginLerpToPose(dr.d, ue_wrap::FVector{e.x, e.y, e.z},
                                ue_wrap::FRotator{e.pitch, e.yaw, e.roll}, nowMs);
        }
    }
    for (auto it = g_drives.begin(); it != g_drives.end();) {
        Drive& dr = it->second;
        void* actor = dr.d.LiveActor();
        if (!actor) { it = g_drives.erase(it); continue; }   // the destroy crossed on its own seam
        if (coop::remote_prop::IsActorUnderAnyDrive(actor)) {
            // A hand took it before the end edge landed: the holder's drive owns the actor from here,
            // and its release restores the physics. This drive yields for good rather than writing
            // a stale target back once the hand lets go.
            UE_LOGI("[PROP-DRIVE] CLIENT yield eid=%u -- a held-prop stream owns the actor", it->first);
            it = g_drives.erase(it);
            continue;
        }
        AD::AdvanceLerp(dr.d, nowMs);
        ++it;
    }
    // An end edge that arrived before its prop: applied the moment the prop resolves.
    for (auto it = g_pendingEnd.begin(); it != g_pendingEnd.end();) {
        if (nowMs >= it->second.expiresMs || g_drives.count(it->first)) {
            it = g_pendingEnd.erase(it);   // expired, or a newer stream parked the prop first
            continue;
        }
        void* actor = coop::remote_prop::ResolveLiveActorByEid(it->first);
        if (!actor) { ++it; continue; }
        if (PR::IsDescendantOfProp(actor) && !coop::remote_prop::IsActorUnderAnyDrive(actor)) {
            ApplyEnd(actor, it->second.p, /*parkedHere=*/false);
            UE_LOGI("[PROP-DRIVE] CLIENT END eid=%u applied late -> (%.1f,%.1f,%.1f) (the prop "
                    "arrived after its stream ended)", it->first, it->second.p.x, it->second.p.y,
                    it->second.p.z);
        }
        it = g_pendingEnd.erase(it);
    }
}

void OnEnd(const coop::net::PropDriveEndPayload& p) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::OnEnd");
    auto it = g_drives.find(p.eid);
    if (it == g_drives.end()) {
        // Never parked here. The generation still closes, so a pose of it in flight cannot park
        // the prop now; the rest pose is applied if the prop is here, kept for it if not, and
        // dropped only for a prop a hand holds -- that stream owns it.
        g_endedGen[p.eid] = p.gen;
        void* actor = coop::remote_prop::ResolveLiveActorByEid(p.eid);
        if (actor) {
            if (PR::IsDescendantOfProp(actor) && !coop::remote_prop::IsActorUnderAnyDrive(actor)) {
                ApplyEnd(actor, p, /*parkedHere=*/false);
                UE_LOGI("[PROP-DRIVE] CLIENT END eid=%u gen=%u -> (%.1f,%.1f,%.1f) (never parked here)",
                        p.eid, static_cast<unsigned>(p.gen), p.x, p.y, p.z);
            }
        } else {
            g_pendingEnd[p.eid] = PendingEnd{p, AD::NowMs() + kPendingEndMs};
        }
        return;
    }
    Drive& dr = it->second;
    if (GenAfter(dr.gen, p.gen)) return;   // an end for an older generation than the live drive
    if (void* actor = dr.d.LiveActor()) {
        if (coop::remote_prop::IsActorUnderAnyDrive(actor)) {
            // A hand's stream parked this prop after ours did, and its release will hand the
            // physics back; touching the pose or the physics here would undo that park.
            UE_LOGI("[PROP-DRIVE] CLIENT END eid=%u gen=%u -- a held-prop stream owns the actor; "
                    "row dropped, nothing touched", p.eid, static_cast<unsigned>(p.gen));
        } else {
            ApplyEnd(actor, p, dr.physicsParked);
            dr.physicsParked = false;
            UE_LOGI("[PROP-DRIVE] CLIENT END eid=%u gen=%u -> (%.1f,%.1f,%.1f) flags=0x%02x",
                    p.eid, static_cast<unsigned>(p.gen), p.x, p.y, p.z,
                    static_cast<unsigned>(p.physFlags));
        }
    }
    g_drives.erase(it);
    g_endedGen[p.eid] = p.gen;
}

void OnDisconnect() {
    for (auto& kv : g_drives) GiveBackPhysics(kv.second, kv.second.d.LiveActor());
    g_drives.clear();
    g_endedGen.clear();
    g_miss.clear();
    g_pendingEnd.clear();
    g_batch.clear();
}

}  // namespace coop::prop_drive_stream
