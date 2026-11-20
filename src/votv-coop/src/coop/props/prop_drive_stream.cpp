// coop/props/prop_drive_stream.cpp -- see coop/props/prop_drive_stream.h.

#include "coop/props/prop_drive_stream.h"

#include "coop/config/config.h"                // ResolveFlag: the coast drill's red
#include "coop/config/config_registry.h"
#include "coop/dev/end_trace.h"                 // OnEndApplied: the measure of what a slept copy does next
#include "coop/dev/prop_syncer_drill.h"        // IsEnabled: the drill's view is kept only while it is on
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/element/bind_wait.h"            // Hold / NoteNeverBound: an end whose prop has no bind yet
#include "coop/element/registry.h"           // IsCorrectionStale: the eid's bound actor
#include "coop/props/active_drive.h"
#include "coop/props/prop_drive_host.h"        // InAHand: who is a hand
#include "coop/props/prop_child_release.h"      // OnParentReleased: a jointed door waits for its parent
#include "coop/props/prop_key_index.h"  // Find: the index only, never the cold walk
#include "coop/props/prop_park.h"             // Park / Unpark: the parked prop, and a Character welded on it
#include "coop/props/prop_syncer.h"           // SkipsHostPose: the prop this peer authors
#include "coop/props/prop_wire_parity.h"      // the join converge's own physics restore
#include "remote_prop_drive.h"                // RemoteHandHolderOf (impl-private, src-local, NOT under include/)
#include "coop/player/local_streams.h"
#include "coop/player/hand_item.h"
#include "coop/props/remote_prop.h"           // ResolveLiveActorByEid, IsActorUnderAnyDrive
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"      // IsActorRootBodyAtRest: whether the sleep took
#include "ue_wrap/engine/world_identity.h"     // AddWorldChangeListener: a pending yield belongs to one world

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <mutex>
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
    uint32_t relatches = 0;       // the game switched the parked copy's simulation back on, this park
};

// A park whose copy the game switches back to simulating this many times is being fought every
// tick, not nudged once: said loudly, once, as the held-prop drive says it (remote_prop).
constexpr uint32_t kRelatchLoud = 60;
std::unordered_map<uint32_t, Drive>   g_drives;    // eid -> drive
std::unordered_map<uint32_t, uint8_t> g_endedGen;  // eid -> the generation its end edge closed
// eid -> the generation whose END pose ApplyEnd wrote, and onto which actor: captured with its slot
// serial, so a successor re-created in the same slot does not read as the actor the pose was written on.
struct EndOnActor { uint8_t gen = 0; ue_wrap::CachedObjRef actor; };
std::unordered_map<uint32_t, EndOnActor> g_endOnActor;
// eid -> a generation a hand took over before its end edge landed. Its end pose is older than
// whatever the hand did since, so a late end of it closes the generation and writes nothing -- except the client's
// first-pose yield to a remote hand, which waits for that hand's release (PendYield).
std::unordered_map<uint32_t, uint8_t> g_yieldedGen;

// A pose whose prop has not arrived here is retried on a throttle, never per tick: the host
// re-sends every tick the prop moves, and though the resolve is two O(1) lookups, a line per
// miss is not. One line names a sustained miss.
struct Miss { uint64_t nextTryMs = 0; uint32_t count = 0; };
std::unordered_map<uint32_t, Miss> g_miss;
constexpr uint64_t kMissRetryMs = 250;
constexpr uint32_t kMissSaidAt  = 40;     // about ten seconds of misses: one line

// The drained batch, kept across ticks for its capacity: the session swaps its buffer with this
// one, so the steady state of a drag allocates nothing on either thread. Game thread only.
std::vector<coop::net::PropDriveRow> g_batch;

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
    return coop::prop_key_index::Find(coop::remote_prop::KeyToWString(key));
}

void GiveBackPhysics(Drive& dr, void* actor) {
    if (!dr.physicsParked || !actor) return;
    const bool simulate = !PhysicsStaysOff(actor);
    coop::prop_park::Unpark(actor, dr.d.mesh, simulate);
    if (simulate) dr.physicsParked = false;
}

// The one way a stream gives a prop up to a hand, from the tick, the first pose and the end edge
// alike: this player's own hand gets back the physics the park took, which no peer's release will
// restore, and the generation is closed as superseded, so neither a pose nor the end of it can
// write its stale pose once the hand lets go -- except the client's first-pose yield to a remote
// hand, which waits for that hand's release (PendYield).
bool IsOwnHand(void* actor) {
    return actor == coop::local_streams::LastHeldActor() || coop::hand_item::IsHandAxisActor(actor);
}
// Both records only ever move forward. The tick can yield an OLDER generation's row in the same
// pass that the first pose yielded a newer one, and writing the older one back reopened the newer
// generation: its end then found no yield and parked the stale rest pose after the hand let go.
void AdvanceGen(std::unordered_map<uint32_t, uint8_t>& m, uint32_t eid, uint8_t gen) {
    auto it = m.find(eid);
    if (it == m.end()) m.emplace(eid, gen);
    else if (GenAfter(gen, it->second)) it->second = gen;
}
void YieldToHand(uint32_t eid, uint8_t gen, Drive* dr, void* actor) {
    if (dr && actor && IsOwnHand(actor)) GiveBackPhysics(*dr, actor);
    AdvanceGen(g_endedGen, eid, gen);
    AdvanceGen(g_yieldedGen, eid, gen);
    if (actor) coop::prop_child_release::OnParentReleased(actor);   // the stream let go: a waiting door goes next
}
bool YieldedAtOrAfter(uint32_t eid, uint8_t gen) {
    auto y = g_yieldedGen.find(eid);
    return y != g_yieldedGen.end() && !GenAfter(gen, y->second);
}

// A first pose that finds the copy in a REMOTE hand yields the generation to that hand, and the row's let-go End is dropped
// with it. The hand's release and the drive row travel on two lanes (the pose stream, the reliable lane), so the pose can
// beat the release of the very hold it follows: the yield is then parked here with the marks it overwrote, and lifted when
// that hand's drive ends by a release (OnRemoteHandEnded), so the row parks the copy and its End applies. MTA applies a
// detach at once and gates an element's sync by its time context (CClientEntity.cpp:630-663, :179;
// CUnoccupiedVehicleSync.cpp:178-190) and needs no pending state, since its detach and the element's sync arrive in order
// on one channel; ours arrive on two, so the yield waits for its own release.
struct PendingYield {
    uint8_t  slot = 0;           // the remote hand that held the copy at the first pose
    uint16_t holdGen = 0;        // ... and its hold
    uint8_t  gen = 0;            // the drive generation that was yielded
    bool     hadEnded = false;   // g_endedGen / g_yieldedGen as the yield found them
    uint8_t  priorEnded = 0;
    bool     hadYielded = false;
    uint8_t  priorYielded = 0;
    ue_wrap::CachedObjRef actor;
};
using PendingMap = std::unordered_map<uint32_t, PendingYield>;
PendingMap g_pending;            // eid -> the one yield waiting on its hand's release

enum class Drop : uint8_t { End, Newer, NewHold, Closed, Stale, World, HandGone, Superseded, Knob, kCount };
constexpr const char* kDropName[] = {"its End came first", "a newer generation", "a new hold took the copy",
                                     "the generation was closed here", "the actor died", "world change",
                                     "the hand's drive ended without a release", "the marks moved on",
                                     "letgo_unyield_off"};
uint32_t g_pendingN = 0;         // yields kept pending, this session
uint32_t g_unyieldedN = 0;       // ... and lifted at their hand's release
uint32_t g_dropped[static_cast<size_t>(Drop::kCount)] = {};
bool SaidAt(uint32_t n) { return n <= 3 || (n & (n - 1)) == 0; }

PendingMap::iterator DropPending(PendingMap::iterator it, Drop why) {
    const uint32_t n = ++g_dropped[static_cast<size_t>(why)];
    if (SaidAt(n))
        UE_LOGI("[PROP-DRIVE] pending yield of eid=%u gen=%u dropped: %s (#%u of this reason)", it->first,
                static_cast<unsigned>(it->second.gen), kDropName[static_cast<size_t>(why)], n);
    return g_pending.erase(it);
}
void ErasePending(uint32_t eid, Drop why) {
    if (auto it = g_pending.find(eid); it != g_pending.end()) DropPending(it, why);
}
void OnWorldChange() noexcept {
    while (!g_pending.empty()) DropPending(g_pending.begin(), Drop::World);
}

// Called before YieldToHand, which overwrites the marks read here.
void PendYield(uint32_t eid, uint8_t gen, void* actor) {
    uint8_t slot = 0;
    uint16_t holdGen = 0;
    if (!coop::remote_prop::RemoteHandHolderOf(actor, slot, holdGen)) return;
    ErasePending(eid, Drop::Newer);
    PendingYield& p = g_pending[eid];
    p.slot = slot;
    p.holdGen = holdGen;
    p.gen = gen;
    if (auto e = g_endedGen.find(eid); e != g_endedGen.end()) { p.hadEnded = true; p.priorEnded = e->second; }
    if (auto y = g_yieldedGen.find(eid); y != g_yieldedGen.end()) { p.hadYielded = true; p.priorYielded = y->second; }
    p.actor.Set(actor);
    static bool s_listening = false;
    if (!s_listening) {
        s_listening = true;
        if (!ue_wrap::world_identity::AddWorldChangeListener(&OnWorldChange))
            UE_LOGE("[PROP-DRIVE] the world-change list is full -- a pending yield is dropped at the session's end only");
    }
    if (SaidAt(++g_pendingN))
        UE_LOGI("[PROP-DRIVE] eid=%u gen=%u yield kept pending on slot %u hold %u's release (#%u)", eid,
                static_cast<unsigned>(gen), static_cast<unsigned>(slot), static_cast<unsigned>(holdGen), g_pendingN);
}

// What the prop syncer drill's judge reads (prop_drive_stream.h, dev::ViewOf): per eid, the rows of the last
// generation, where the copy stood before its park, and whether that generation's End applied. Null (and nothing
// kept) unless the drill row is on.
std::unordered_map<uint32_t, dev::DriveView> g_devView;
dev::DriveView* DevViewOf(uint32_t eid, uint8_t gen) {
    if (!coop::dev::prop_syncer_drill::IsEnabled()) return nullptr;
    dev::DriveView& v = g_devView[eid];
    if (v.gen != gen) v = dev::DriveView{gen};
    return &v;
}

// Rows that reached the receive store per eid, counted on the net thread before the store keeps the newest per eid
// (dev::NoteMerged); against the view's rows (taken and applied) it tells coalescing from loss.
std::mutex g_mergedMu;
std::unordered_map<uint32_t, uint32_t> g_merged;

// The last stream ends applied here, a ring of kRecentEnds (prop_drive_stream.h).
AppliedEnd g_ends[kRecentEnds];
size_t g_endCount = 0;   // ends recorded since the session began; the newest is at (g_endCount - 1) % kRecentEnds

// The end edge on a live prop: the final pose, then the host's physics flags through the same
// parity restore the join's converge applies after its teleport, then the velocity -- only into
// a body this module parked and the flags leave simulating, and only when it is not zero, since
// assigning a velocity wakes a body at rest. A body whose End carries no velocity is put to sleep at the pose.
void ApplyEnd(void* actor, const coop::net::PropDriveEndPayload& p, bool parkedHere) {
    coop::prop_wire_parity::NoteCorrection(actor, ue_wrap::FVector{p.x, p.y, p.z},
                                           parkedHere ? "drive-end" : "drive-end-unparked");
    E::SetActorLocation(actor, ue_wrap::FVector{p.x, p.y, p.z});
    if (dev::DriveView* v = DevViewOf(p.eid, p.gen)) v->endApplied = true;
    EndOnActor& on = g_endOnActor[p.eid];
    on.gen = p.gen;
    on.actor.Set(actor);
    g_ends[g_endCount++ % kRecentEnds] = AppliedEnd{p.eid, ue_wrap::FVector{p.x, p.y, p.z}, ::GetTickCount64()};
    E::SetActorRotation(actor, ue_wrap::FRotator{p.pitch, p.yaw, p.roll});
    // The host's static, frozen and sleep at the end first: a hook that bites a frozen prop unfreezes it on
    // the host only (hook_C's attach_a runs setPropProps), so a copy still frozen here takes the
    // host's flags before the physics decision reads them. Static is learned too: a tool press on a prop
    // this copy was held for waits for the hold's end (prop_tool_flags), and this edge carries the host's.
    coop::prop_wire_parity::ConvergeStatic(actor, p.physFlags);
    coop::prop_wire_parity::ConvergeFrozenSleep(actor, p.physFlags);
    bool sleepAtPose = false;
    if (!PhysicsStaysOff(actor)) {
        coop::prop_wire_parity::RestoreSpParityPhysicsAfterConverge(actor, p.physFlags);
        const float lin2 = p.linVelX * p.linVelX + p.linVelY * p.linVelY + p.linVelZ * p.linVelZ;
        const bool simulates = coop::prop_wire_parity::SpParitySimulate(p.physFlags);
        if (parkedHere && simulates && lin2 > 0.f) {
            void* mesh = PR::GetStaticMesh(actor);
            ue_wrap::engine::SetComponentLinearVelocity(mesh, p.linVelX, p.linVelY, p.linVelZ);
            ue_wrap::engine::SetComponentAngularVelocity(mesh, p.angVelX, p.angVelY, p.angVelZ);
        } else if (simulates && lin2 == 0.f) {
            sleepAtPose = true;   // below: after the park is over, the last thing that touches the body
        }
    }
    // The park is over, whatever the flags left the body: a Character it stopped moves again.
    coop::prop_park::Unpark(actor, /*mesh=*/nullptr, /*rootSimulates=*/false);
    coop::prop_child_release::OnParentReleased(actor);   // a door jointed to it was waiting for this
    if (sleepAtPose) {
        // The body the End describes is at rest at this pose: this copy rests here too, asleep, and no second simulation settles
        // it somewhere else (a body switched on awake at a rest pose came to rest 1.1 cm and 1.1 degrees away). It is put to
        // sleep after its simulation is on and after the park is over, so nothing the End does later wakes it; touched by another
        // body it wakes, as the host's would.
        const bool ran = ue_wrap::engine::PutComponentToSleep(PR::GetStaticMesh(actor));
        // Said for the first 32 and at each doubling: whether the call ran and the body reads at rest after it.
        static uint32_t s_slept = 0;
        const uint32_t n = ++s_slept;
        if (n <= 32 || (n & (n - 1)) == 0)
            UE_LOGI("[PROP-DRIVE] End eid=%u gen=%u: the copy put to sleep at the End's pose (call ran %d, body at rest after "
                    "%d) (#%u)", p.eid, static_cast<unsigned>(p.gen), ran ? 1 : 0,
                    ue_wrap::engine::IsActorRootBodyAtRest(actor) ? 1 : 0, n);
        coop::dev::end_trace::OnEndApplied(actor, p.eid, p.gen, ue_wrap::FVector{p.x, p.y, p.z}, ran);
    }
}

// One pose of a driven prop: parks the copy on a new generation's first, a lerp target after. The client's
// apply of the host's batch, and the host's of a pose it admitted from a prop's syncer (FeedAdmitted); `who`
// names the role in the log; `client` is the client's apply, the only one whose hand-yield can wait on a release.
void ApplyPose(const coop::net::PropDriveRow& e, uint64_t nowMs, const char* who, bool client) {
    const uint32_t eid = e.elementId;
    if (eid == 0) return;
    // The generation gate: a pose of a closed generation, or older than the live drive's,
    // is a datagram that outlived its stream.
    auto ended = g_endedGen.find(eid);
    if (ended != g_endedGen.end() && !GenAfter(e.ctx, ended->second)) return;
    auto it = g_drives.find(eid);
    if (it != g_drives.end()) {
        if (GenAfter(it->second.gen, e.ctx)) return;
        if (it->second.gen == e.ctx) {
            // The steady state: the row already names the actor. No resolve, no string.
            if (void* live = it->second.d.LiveActor()) {
                if (!coop::prop_drive_host::InAHand(live)) {   // a hand's stream wins
                    if (dev::DriveView* v = DevViewOf(eid, e.ctx)) {
                        ++v->rows;
                        v->lastRowLoc = ue_wrap::FVector{e.x, e.y, e.z};
                    }
                    AD::BeginLerpToPose(it->second.d, ue_wrap::FVector{e.x, e.y, e.z},
                                        ue_wrap::FRotator{e.pitch, e.yaw, e.roll}, nowMs);
                }
                return;
            }
        }
    }
    // A first pose, a new generation, or a row whose actor died: resolve, on a throttle.
    auto miss = g_miss.find(eid);
    if (miss != g_miss.end() && nowMs < miss->second.nextTryMs) return;
    void* actor = Resolve(eid, e.key);
    if (!actor) {
        Miss& m = g_miss[eid];
        m.nextTryMs = nowMs + kMissRetryMs;
        if (++m.count == kMissSaidAt)
            UE_LOGI("[PROP-DRIVE] %s eid=%u has not resolved in %u tries -- the host is "
                    "streaming a prop this peer does not have (a join still landing, or a "
                    "prop it never received)", who, eid, m.count);
        return;
    }
    g_miss.erase(eid);
    if (!PR::IsDescendantOfProp(actor)) return;
    // A hand's stream on the same prop wins: the holder is its syncer for as long as it
    // holds it, and the host closes this stream the moment it sees the hand.
    if (coop::prop_drive_host::InAHand(actor)) {
        // The first pose found the prop already in a hand: the generation is the hand's. The Tick's yield and the End
        // row's mean the local hand or a newer grab, which no remote release lifts: they keep nothing pending.
        if (client) PendYield(eid, e.ctx, actor);
        YieldToHand(eid, e.ctx, nullptr, actor);
        UE_LOGI("[PROP-DRIVE] %s eid=%u gen=%u -- a hand holds it at the first pose; "
                "generation yielded", who, eid, static_cast<unsigned>(e.ctx));
        return;
    }
    if (dev::DriveView* seen = DevViewOf(eid, e.ctx)) {
        seen->startRead = E::TryGetActorLocation(actor, seen->startLoc);
        ++seen->rows;
        seen->lastRowLoc = ue_wrap::FVector{e.x, e.y, e.z};
    }
    Drive& dr = g_drives[eid];
    if (dr.d.actor && dr.d.actor != actor) GiveBackPhysics(dr, dr.d.LiveActor());
    AD::ResetDriveState(dr.d);
    dr.d.actor    = actor;
    dr.d.actorIdx = R::InternalIndexOf(actor);
    dr.d.mesh     = PR::GetStaticMesh(actor);
    dr.d.isTrashMirror = true;  // the fixed-delay follow of a carried clump: freeze on a gap
    dr.d.lastEid  = eid;
    dr.gen        = e.ctx;
    dr.physicsParked = false;
    dr.relatches  = 0;
    if (dr.d.mesh && !PhysicsStaysOff(actor)) {
        coop::prop_park::Park(actor, dr.d.mesh);
        dr.physicsParked = true;
    }
    if (ended != g_endedGen.end()) g_endedGen.erase(ended);
    g_yieldedGen.erase(eid);
    ErasePending(eid, Drop::Newer);
    // The parked newer generation supersedes the END pose a correction was judged against.
    g_endOnActor.erase(eid);
    // A pose of a closed generation never reaches here, so an end held for this prop (bind_wait) is of
    // an OLDER generation than the one parking now: its replay finds the drive parked and drops it as stale.
    UE_LOGI("[PROP-DRIVE] %s park eid=%u gen=%u actor=%p (physics %s)",
            who, eid, static_cast<unsigned>(e.ctx), actor,
            dr.physicsParked ? "off" : "left as it was");
    AD::BeginLerpToPose(dr.d, ue_wrap::FVector{e.x, e.y, e.z},
                        ue_wrap::FRotator{e.pitch, e.yaw, e.roll}, nowMs);
}

}  // namespace

size_t RecentEnds(AppliedEnd* out, size_t max) {
    const size_t have = g_endCount < kRecentEnds ? g_endCount : kRecentEnds;
    size_t n = 0;
    for (; n < have && n < max; ++n) out[n] = g_ends[(g_endCount - 1 - n) % kRecentEnds];
    return n;
}

void FeedAdmitted(const coop::net::PropDriveRow& e) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::FeedAdmitted");
    ApplyPose(e, AD::NowMs(), "HOST", /*client=*/false);
}

void ReleaseHost(const coop::net::PropDriveEndPayload& end) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::ReleaseHost");
    auto it = g_drives.find(end.eid);
    if (it != g_drives.end()) {
        Drive& dr = it->second;
        if (void* actor = dr.d.LiveActor()) {
            if (coop::prop_drive_host::InAHand(actor)) {
                // The same yield as the client's: a hand owns the copy, and this player's own gets its physics back.
                YieldToHand(end.eid, end.gen, &dr, actor);
            } else {
                ApplyEnd(actor, end, dr.physicsParked);
                dr.physicsParked = false;
            }
        }
        g_drives.erase(it);
    } else if (void* actor = coop::remote_prop::ResolveLiveActorByEid(end.eid)) {
        // Nothing was admitted, so the copy was never parked: it takes the End as a receiver that never saw the gen.
        if (PR::IsDescendantOfProp(actor) && !coop::prop_drive_host::InAHand(actor))
            ApplyEnd(actor, end, /*parkedHere=*/false);
    }
    UE_LOGI("[PROP-DRIVE] HOST release eid=%u gen=%u -> (%.1f,%.1f,%.1f) flags=0x%02x", end.eid,
            static_cast<unsigned>(end.gen), end.x, end.y, end.z, static_cast<unsigned>(end.physFlags));
}

void TickApplyAndDrive(coop::net::Session& s) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::TickApplyAndDrive");
    const uint64_t nowMs = AD::NowMs();
    g_batch.clear();
    // The host's own batch slot is never written (a client's rows go to the origin stores), so only a client drains it.
    if (s.role() != coop::net::Role::Host && s.TakeRemotePropDriveBatch(g_batch)) {
        for (const coop::net::PropDriveRow& e : g_batch) {
            // The one skip: a prop this peer authors (a claim pending or granted) is moved by this peer's own
            // physics, and the host's poses for it are its own rows relayed, or a coast it parks onto once a
            // claim is refused. Skipping closes no generation, and an End is never skipped (OnEnd).
            if (coop::prop_syncer::SkipsHostPose(e.elementId)) continue;
            ApplyPose(e, nowMs, "CLIENT", /*client=*/true);
        }
    }
    for (auto it = g_drives.begin(); it != g_drives.end();) {
        Drive& dr = it->second;
        void* actor = dr.d.LiveActor();
        if (!actor) {   // the destroy crossed on its own seam
            // The generation closes with the drive: a held End of an older one then reads stale on replay.
            AdvanceGen(g_endedGen, it->first, dr.gen);
            it = g_drives.erase(it);
            continue;
        }
        if (coop::prop_drive_host::InAHand(actor)) {
            // A hand took it before the end edge landed: the holder's drive owns the actor from here,
            // and its release restores the physics. This drive yields for good rather than writing
            // a stale target back once the hand lets go.
            const bool ownHand = IsOwnHand(actor);
            YieldToHand(it->first, dr.gen, &dr, actor);
            UE_LOGI("[PROP-DRIVE] CLIENT yield eid=%u -- %s owns the actor", it->first,
                    ownHand ? "this player's hand" : "a held-prop stream");
            it = g_drives.erase(it);
            continue;
        }
        // The physics receiver's re-latch, as the held-prop drive does it: a parked copy stays
        // kinematic when the game here switches its simulation back on. That includes a copy parked
        // frozen, which the park left as it was, and which an unstick or an unfreeze here has since
        // freed: it is parked kinematic from then on, and the end edge hands its physics back.
        if (dr.d.mesh && ue_wrap::engine::IsComponentSimulatingPhysics(dr.d.mesh)) {
            coop::prop_park::Park(actor, dr.d.mesh);
            dr.physicsParked = true;
            if (++dr.relatches == 1) {
                UE_LOGI("[PROP-DRIVE] CLIENT eid=%u -- the game turned simulation back on under the park; "
                        "re-latched kinematic", it->first);
            } else if (dr.relatches == kRelatchLoud) {
                UE_LOGW("[PROP-DRIVE] CLIENT eid=%u -- re-latched %u times: something here keeps switching the "
                        "parked copy's simulation back on", it->first, static_cast<unsigned>(dr.relatches));
            }
        }
        AD::AdvanceLerp(dr.d, nowMs);
        ++it;
    }
    for (auto it = g_pending.begin(); it != g_pending.end();)   // empty in the steady state
        it = it->second.actor.Alive() ? std::next(it) : DropPending(it, Drop::Stale);
    coop::prop_child_release::Tick(nowMs);   // a waiting door's liveness and its failure bound
    coop::prop_wire_parity::FlushCorrectionSummary(nowMs);   // the 30 s tally of every path that moved a copy
}

namespace {
constexpr uint16_t kEndKind = static_cast<uint16_t>(coop::net::ReliableKind::PropDriveEnd);

// An end held in bind_wait runs again once its prop is bound: the whole of OnEnd, so a drive that parked
// meanwhile takes the parked branch and a hand that took the prop since is yielded to as on a first arrival.
void ReplayEnd(const uint8_t* bytes, size_t n, int /*senderSlot*/) {
    coop::net::PropDriveEndPayload p{};
    if (n != sizeof p) return;
    std::memcpy(&p, bytes, sizeof p);
    OnEnd(p);
}
}  // namespace

void OnEnd(const coop::net::PropDriveEndPayload& p) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::OnEnd");
    // An End of the pending generation, or a newer one, that comes before its hand's release is the host's "taken by a
    // hand" End (prop_drive_host): applying it at the release would write a pose older than the hand's. The entry goes
    // and the yield stands; the let-go row's own End comes after the release.
    if (auto pe = g_pending.find(p.eid); pe != g_pending.end() && !GenAfter(pe->second.gen, p.gen))
        DropPending(pe, p.gen == pe->second.gen ? Drop::End : Drop::Newer);
    auto it = g_drives.find(p.eid);
    if (it == g_drives.end()) {
        // Never parked here. The generation still closes, so a pose of it in flight cannot park
        // the prop now; the rest pose is applied if the prop is here, held in bind_wait for its bind if the
        // bind is deferred (a joiner mid-drag whose snapshot is still landing), and
        // dropped for a prop a hand holds -- that stream owns it -- or one a hand took this generation
        // over from: the hand moved it since, so this pose is older than where it lies. The same for a
        // generation this peer closed itself (CloseGen). An end older than the generation already closed
        // here is stale outright and touches nothing.
        if (auto closed = g_endedGen.find(p.eid); closed != g_endedGen.end() && GenAfter(closed->second, p.gen))
            return;
        if (YieldedAtOrAfter(p.eid, p.gen)) {
            AdvanceGen(g_endedGen, p.eid, p.gen);
            UE_LOGI("[PROP-DRIVE] CLIENT END eid=%u gen=%u -- this generation was yielded or closed here (a "
                    "hand took it over, or this peer ended it); pose not applied", p.eid,
                    static_cast<unsigned>(p.gen));
            return;
        }
        AdvanceGen(g_endedGen, p.eid, p.gen);
        void* actor = coop::remote_prop::ResolveLiveActorByEid(p.eid);
        if (actor) {
            if (PR::IsDescendantOfProp(actor) && !coop::prop_drive_host::InAHand(actor)) {
                ApplyEnd(actor, p, /*parkedHere=*/false);
                UE_LOGI("[PROP-DRIVE] CLIENT END eid=%u gen=%u -> (%.1f,%.1f,%.1f) (never parked here)",
                        p.eid, static_cast<unsigned>(p.gen), p.x, p.y, p.z);
            }
        } else if (!coop::element::bind_wait::Hold(p.eid, kEndKind, /*senderSlot=*/0, &ReplayEnd, &p, sizeof p,
                                                   /*coalesce=*/true)) {
            // Not deferred: no bind is coming for this eid, so there is nothing to wait for.
            coop::element::bind_wait::NoteNeverBound(kEndKind, p.eid);
        }
        return;
    }
    Drive& dr = it->second;
    if (GenAfter(dr.gen, p.gen)) return;   // an end for an older generation than the live drive
    if (void* actor = dr.d.LiveActor()) {
        if (coop::prop_drive_host::InAHand(actor)) {
            // A hand took it before the tick yielded: the same yield, so this player's own hand gets
            // the physics back. A peer's held-prop stream parked it after ours did and its release
            // hands the physics back; the pose is left alone either way.
            YieldToHand(p.eid, p.gen, &dr, actor);
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
    AdvanceGen(g_endedGen, p.eid, p.gen);
}

void CloseGen(uint32_t eid, uint8_t gen) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::CloseGen");
    AdvanceGen(g_endedGen, eid, gen);
    AdvanceGen(g_yieldedGen, eid, gen);
    ErasePending(eid, Drop::Closed);
}

void OnRemoteHandStarted(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::OnRemoteHandStarted");
    if (g_pending.empty()) return;
    auto it = g_pending.find(static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(actor)));
    // Any hand drive starting on the copy ends the wait: every end of the awaited hold's drive has already dropped or lifted
    // its entry (remote_prop_drive::EndSlotDrive), so an entry still here belongs to an older hold.
    if (it != g_pending.end()) DropPending(it, Drop::NewHold);
}

void OnRemoteHandEnded(uint8_t slot, void* actor, uint16_t holdGen, bool released) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::OnRemoteHandEnded");
    if (g_pending.empty()) return;
    // By the eid the registry knows the copy under: the payload's is 0 for a keyed prop's release.
    auto it = g_pending.find(static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(actor)));
    if (it == g_pending.end() || it->second.slot != slot || it->second.holdGen != holdGen) return;
    if (!released) {   // a refusal or a take-from-hand: the hand did not let go of a copy the stream may park
        DropPending(it, Drop::HandGone);
        return;
    }
    // The coast drill's red: the entry is written and then dropped at the release, so the yield stands as it did.
    static const bool s_unyieldOff = coop::config::ResolveFlag(coop::config_registry::rows::letgo_unyield_off);
    if (s_unyieldOff) {
        DropPending(it, Drop::Knob);
        return;
    }
    // Lifted only where the yield's own marks still stand: a newer generation's, or one this peer closed itself
    // (CloseGen erases the entry), are never lowered.
    const uint32_t eid = it->first;
    const PendingYield p = it->second;
    bool lifted = false;
    if (auto e = g_endedGen.find(eid); e != g_endedGen.end() && e->second == p.gen) {
        if (p.hadEnded) e->second = p.priorEnded;
        else g_endedGen.erase(e);
        lifted = true;
    }
    if (auto y = g_yieldedGen.find(eid); y != g_yieldedGen.end() && y->second == p.gen) {
        if (p.hadYielded) y->second = p.priorYielded;
        else g_yieldedGen.erase(y);
        lifted = true;
    }
    if (!lifted) {
        DropPending(it, Drop::Superseded);
        return;
    }
    g_pending.erase(it);
    UE_LOGI("prop_drive_stream: eid=%u gen=%u un-yielded at slot %u's release", eid, static_cast<unsigned>(p.gen),
            static_cast<unsigned>(slot));
    if (SaidAt(++g_unyieldedN))
        UE_LOGI("[PROP-DRIVE] pending yields lifted at their hand's release: %u of %u kept", g_unyieldedN, g_pendingN);
}

void PendingYieldCounts(uint32_t& pending, uint32_t& unyielded) {
    pending = g_pendingN;
    unyielded = g_unyieldedN;
}

bool IsParked(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::IsParked");
    if (!actor) return false;
    // The live actor, not the cached pointer: a row whose actor died is erased only on the next tick,
    // and a new actor at the freed address is not parked.
    for (const auto& kv : g_drives)
        if (kv.second.d.LiveActor() == actor) return true;
    return false;
}

bool IsCorrectionStale(uint32_t eid, uint8_t gen, bool open) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::IsCorrectionStale");
    if (const auto d = g_drives.find(eid); d != g_drives.end()) return GenAfter(d->second.gen, gen);
    const auto e = g_endOnActor.find(eid);
    if (e == g_endOnActor.end()) return false;
    // The END pose died with its actor (a load-tail re-create, a purge) unless the bound actor is
    // still the one it was written onto: the same pointer, its slot still carrying the serial taken then.
    coop::element::Element* el = coop::element::Registry::Get().Get(eid);
    void* actor = el ? el->GetActor() : nullptr;
    if (!actor || !R::IsLiveByIndex(actor, el->GetInternalIdx()) || !e->second.actor.Is(actor))
        return false;
    return GenAfter(e->second.gen, gen) || (e->second.gen == gen && open);
}

void OnDisconnect() {
    for (auto& kv : g_drives) GiveBackPhysics(kv.second, kv.second.d.LiveActor());
    coop::prop_child_release::OnDisconnect();   // the waiting doors are given back with their parents
    g_drives.clear();
    g_endedGen.clear();
    g_endOnActor.clear();
    g_yieldedGen.clear();
    g_pending.clear();
    g_pendingN = g_unyieldedN = 0;
    std::memset(g_dropped, 0, sizeof g_dropped);
    g_miss.clear();
    g_devView.clear();
    {
        std::lock_guard<std::mutex> lk(g_mergedMu);
        g_merged.clear();
    }
    g_endCount = 0;
    g_batch.clear();
}

namespace dev {

DriveView ViewOf(uint32_t eid) {
    const auto it = g_devView.find(eid);
    return it == g_devView.end() ? DriveView{} : it->second;
}

void NoteMerged(uint32_t eid) {
    if (!coop::dev::prop_syncer_drill::IsEnabled()) return;
    std::lock_guard<std::mutex> lk(g_mergedMu);
    ++g_merged[eid];
}

uint32_t MergedOf(uint32_t eid) {
    std::lock_guard<std::mutex> lk(g_mergedMu);
    const auto it = g_merged.find(eid);
    return it == g_merged.end() ? 0u : it->second;
}

bool ClosedGen(uint32_t eid, uint8_t& gen) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::dev::ClosedGen");
    const auto it = g_endedGen.find(eid);
    if (it == g_endedGen.end()) return false;
    gen = it->second;
    return true;
}

}  // namespace dev

}  // namespace coop::prop_drive_stream
