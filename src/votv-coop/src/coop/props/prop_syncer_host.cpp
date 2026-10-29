// coop/props/prop_syncer_host.cpp -- see coop/props/prop_syncer_host.h. The claim judge, the admission of a
// syncing client's rows, the once-a-second pass that takes a row back, and the endings of a client's row.
// The template is the ATV's table (coop/interactables/atv_authority.cpp: a judged request, stream admission
// from the syncer only with a bucket and a late-syncer window, hand-back on silence or range); MTA files
// named below are reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp.

#include "coop/props/prop_syncer_host.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/prop_syncer_drill.h"        // IsEnabled: the drills' views are written only while one is on
#include "coop/dev/swinger_drill.h"            // IsEnabled: the swinger drill reads a dragged lid's parent count
#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/throw_bounds.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/active_drive.h"          // NowMs
#include "coop/props/prop_drive_host.h"       // InAHand, Coast
#include "coop/props/prop_drive_rules.h"      // kClientDriveSendMs
#include "coop/props/prop_drive_stream.h"     // FeedAdmitted, ReleaseHost
#include "coop/props/prop_element_tracker.h"  // GetPropElementIdForActor
#include "coop/props/prop_lifecycle.h"        // IsWireSuppressedPropClass
#include "coop/props/prop_throw.h"            // Mark: the thrown drop intent's election
#include "coop/props/prop_wire_parity.h"      // PhysFlagsOf: the flags an End carries
#include "coop/props/remote_prop.h"           // ResolveLiveActorByEid
#include "prop_drive_rows.h"                  // co-located private header (src tree, not include/)
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <vector>

namespace coop::prop_syncer_host {
namespace {

namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;
using coop::net::IntentBudget;
using coop::net::PropSyncRefusal;
using coop::net::PropSyncResult;
using coop::prop_drive_host::Driven;
namespace tb = coop::net::throw_bounds;

// The ATV's numbers (atv_authority.cpp): the claim pace is per sender, where MTA's is per vehicle, one push per
// 1500 ms (CUnoccupiedVehicleSync.cpp:490); the stream bucket is the row's, its burst absorbing a syncer's send jitter.
// MTA polices no rate on the syncer's stream (CUnoccupiedVehicleSync.cpp:263-474 has no bucket): the bucket is ours,
// because rows here are sent at 40 Hz and each one reaches the host's physics and every other peer.
constexpr IntentBudget kClaimBudget{2.f, 1.f};
constexpr IntentBudget kStreamBudget{16.f, 40.f};
// Twice mainPlayer.armLength, the helper adding the prop's bounds. MTA's contact radius is 30 m by default
// (CMainConfig.cpp:1528 vehicle_contact_sync_radius): a kick is within arm's reach, and the radius there is slack for
// the lag between a player's contact and the server's last position of it.
constexpr float kReachUU = 400.f;
constexpr uint64_t kSyncerSilenceMs = 5000;      // no admitted row this long: the host takes the prop
constexpr float kSyncerDistanceCm = 13000.f;     // MTA's default unoccupied syncer distance, 130 m
constexpr float kSyncerMaxSpeedCmS = 5000.f;     // provisional: a faster step between admitted rows is logged only
constexpr uint64_t kPassMs = 1000;

// The host's session, cached by the entry points: a row's reader that ends it (Tick, Claim, Coast) has none of its own.
coop::net::Session* g_session = nullptr;
coop::roster_ledger::PerSlotState<coop::net::IntentBucket> g_claimBuckets;
// The slot a row was last handed back from, and when: the ATV's late-syncer record, kept after the row is gone.
struct HandBack { uint8_t slot = 0; uint64_t ms = 0; };
std::unordered_map<uint32_t, HandBack> g_handBack;
uint64_t g_nextPassMs = 0;
// Per client slot, kept capacity: the drain allocates nothing in steady state.
std::vector<coop::net::PropPoseSnapshot> g_take[coop::net::kMaxPeers];

// Each outcome is counted and said at the first and at every 100th, so a client's flood costs the log nothing.
uint32_t g_granted = 0, g_admitted = 0, g_notSyncer = 0, g_lateSyncer = 0, g_staleGen = 0, g_paced = 0, g_steps = 0;
uint32_t g_endsAdmitted = 0, g_endsNotSyncer = 0, g_endsStale = 0, g_endsOutOfBounds = 0, g_endsClamped = 0;
uint32_t g_nonSyncerAdmitted = 0;
uint32_t g_elected = 0, g_electRefused = 0, g_rowsRefused = 0, g_thrownAdmitted = 0, g_jitRows = 0;
int64_t  g_jitMax = 0;   // the largest (sender dt - host arrival dt) of an elected row's later rows, ms
std::unordered_map<uint32_t, dev::Admitted> g_admittedBy;   // eid -> rows admitted, for the drill's judge
uint32_t g_refused[8] = {};
uint32_t g_ended[static_cast<unsigned>(Ending::kCount)] = {};

const char* const kRefusalName[8] = {"none", "not_ready", "unknown", "in_hand", "host_authored", "other_syncer",
                                     "out_of_reach", "paced"};
const char* const kEndingName[static_cast<unsigned>(Ending::kCount)] = {
    "hand", "host verb", "died", "silent", "out of range", "slot left", "flight bound", "rows refused", "grant timeout"};

void Tally(uint32_t& n, const char* what, uint32_t eid, unsigned slot) {
    if (++n == 1 || (n % 100) == 0)
        UE_LOGI("[PROP-SYNC] HOST %s eid=%u slot %u (#%u)", what, eid, slot, n);
}

void Answer(coop::net::Session& s, uint8_t slot, uint32_t eid, uint16_t seq, uint8_t gen, PropSyncResult result,
            PropSyncRefusal why) {
    coop::net::PropSyncAnswerPayload a{};
    a.eid     = eid;
    a.seq     = seq;
    a.gen     = gen;
    a.result  = static_cast<uint8_t>(result);
    a.refusal = static_cast<uint8_t>(why);
    s.SendReliableToSlot(slot, coop::net::ReliableKind::PropSyncAnswer, &a, sizeof(a));
}

// A refusal: counted, answered, and, for an out-of-reach claim on a prop nothing streams, its pose sent to the
// refused client by a host coast (one pose, then the rested End), so the client parks onto where the prop is. One
// coast per claim token, so the claim bucket bounds them. A Paced refusal costs the host nothing past the answer:
// MTA's refused push does nothing at all (CUnoccupiedVehicleSync.cpp:490-507), and a flood must not buy rows.
void Refuse(coop::net::Session& s, const coop::net::PropSyncClaimPayload& p, uint8_t slot, PropSyncRefusal why,
            void* actor = nullptr) {
    Tally(g_refused[static_cast<unsigned>(why) & 7u], kRefusalName[static_cast<unsigned>(why) & 7u], p.eid, slot);
    Answer(s, slot, p.eid, p.seq, 0, PropSyncResult::Refused, why);
    if (why == PropSyncRefusal::OutOfReach && !coop::prop_drive_host::FindByEid(p.eid)) {
        if (!actor) actor = coop::remote_prop::ResolveLiveActorByEid(p.eid);
        if (actor) coop::prop_drive_host::Coast(actor, "refused claim");
    }
}

// The live prop a claim names: an Aprop_C the host minted the element id of (a client-spawned prop is a mirror
// here and reads another id), of a class the wire carries.
void* ResolveProp(uint32_t eid) {
    void* a = coop::remote_prop::ResolveLiveActorByEid(eid);
    if (!a || !PR::IsDescendantOfProp(a)) return nullptr;
    if (static_cast<uint32_t>(coop::prop_element_tracker::GetPropElementIdForActor(a)) != eid) return nullptr;
    if (coop::prop_lifecycle::IsWireSuppressedPropClass(R::ClassNameOf(a))) return nullptr;
    return a;
}

void RecordHandBack(uint32_t eid, uint8_t slot, uint64_t nowMs) {
    g_handBack[eid] = HandBack{slot, nowMs};
}

// The honest race: the row of the syncer a hand-back just replaced, sent before it knew (the ATV's IsLateSyncer).
bool IsLateSyncer(uint32_t eid, uint8_t slot, uint64_t nowMs) {
    const auto it = g_handBack.find(eid);
    if (it == g_handBack.end() || it->second.slot != slot) return false;
    const int rtt = g_session ? g_session->rttMsForSlot(slot) : -1;
    const uint64_t window = static_cast<uint64_t>(rtt > 0 ? rtt : 0) + 2 * coop::prop_drive_rules::kClientDriveSendMs;
    return nowMs - it->second.ms <= window;
}

// The End of a row ended by the host: the last admitted pose and linear velocity (zero when the syncer never sent one;
// the rows carry no spin), and the released copy's flags. MTA's receivers keep the last speed after a sync stops.
coop::net::PropDriveEndPayload EndOf(const Driven& d, void* actor) {
    coop::net::PropDriveEndPayload p{};
    p.eid       = d.eid;
    p.gen       = d.gen;
    p.physFlags = coop::prop_wire_parity::PhysFlagsOf(actor);
    p.x = d.admitLoc.X; p.y = d.admitLoc.Y; p.z = d.admitLoc.Z;
    p.pitch = ue_wrap::NormalizeAxis(d.admitRot.Pitch);
    p.yaw   = ue_wrap::NormalizeAxis(d.admitRot.Yaw);
    p.roll  = ue_wrap::NormalizeAxis(d.admitRot.Roll);
    p.linVelX = d.admitVel.X; p.linVelY = d.admitVel.Y; p.linVelZ = d.admitVel.Z;
    return p;
}

// An elected row's test (coop/net/throw_bounds.h): its velocity under the flight ceiling and its step agreeing with
// it. The first row is measured from the release (its pose, and its arrival on this clock), the rest from the last
// admitted row (its stamp on the sender's clock, its arrival on this one); the host never compares clocks across
// peers, only deltas. A refused row is counted against the run that ends the row (RowRefused).
bool ElectedRowAdmitted(Driven& row, const coop::net::PropPoseSnapshot& e, uint8_t slot, uint64_t nowMs) {
    const bool first = !row.admittedEver;
    const ue_wrap::FVector from = first ? row.startLoc : row.admitLoc;
    const float dx = e.x - from.X, dy = e.y - from.Y, dz = e.z - from.Z;
    const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
    const int64_t dtRow = first ? static_cast<int64_t>(e.sendTimeMs)
                                : static_cast<int64_t>(e.sendTimeMs) - static_cast<int64_t>(row.admitSendMs);
    const int64_t arrival = static_cast<int64_t>(nowMs - (first ? row.startMs : row.admitMs));
    if (!first) {
        if (dtRow - arrival > g_jitMax || g_jitRows == 0) g_jitMax = dtRow - arrival;
        ++g_jitRows;
    }
    if (tb::RowAdmitted(e.vel, step, dtRow, arrival)) {
        row.refusedRun = 0;
        return true;
    }
    if (row.refusedRun < 255) ++row.refusedRun;
    Tally(g_rowsRefused, "row refused: its velocity or step does not agree with the throw", e.elementId, slot);
    return false;
}

// One row from slot `slot`, judged against the row that names it: the sender, the generation, the pace.
void AdmitRow(const coop::net::PropPoseSnapshot& e, uint8_t slot, uint64_t nowMs) {
    // The developer red arm of the prop syncer drill: the sender test is skipped, so a forged row of the open
    // generation is admitted and the drill's forge arm must fail. Resolved once.
    static const bool s_redNotSyncer =
        coop::config::ResolveEnum(coop::config_registry::rows::prop_syncer_red) == "not_syncer";
    Driven* row = coop::prop_drive_host::FindByEid(e.elementId);
    if (!row || (row->syncer != slot && !s_redNotSyncer)) {
        if (IsLateSyncer(e.elementId, slot, nowMs)) Tally(g_lateSyncer, "late row of a handed-back syncer", e.elementId, slot);
        else                                        Tally(g_notSyncer, "row refused: sender is no syncer", e.elementId, slot);
        return;
    }
    // Strict equality, where MTA's CanUpdateSync also accepts a zero stamp from either side (CElement.cpp:1304): gen 0
    // is "none" here, never issued, so a row that names no generation is stale by construction.
    if (e.ctx != row->gen) { Tally(g_staleGen, "row refused: stale generation", e.elementId, slot); return; }
    if (!row->streamBucket.Take(kStreamBudget, nowMs)) { Tally(g_paced, "row refused: paced", e.elementId, slot); return; }
    // An elected row meets the throw's bounds (a refusal is counted and, after a run of them, ends the row). Any other
    // row keeps the old rule: a movement discontinuity is logged, never refused (the host may cheat and the relay
    // holds), and its first row after a grant is not tested, the claim's reach test having bounded where it started.
    if (row->elected) {
        if (!ElectedRowAdmitted(*row, e, slot, nowMs)) {
            if (row->refusedRun >= tb::kMaxRefusedRows) EndClientRow(*row, Ending::RowRefused);
            return;
        }
    } else if (row->admittedEver) {
        const float dx = e.x - row->admitLoc.X, dy = e.y - row->admitLoc.Y, dz = e.z - row->admitLoc.Z;
        const float dtMs = static_cast<float>(std::max<uint64_t>(nowMs - row->admitMs, 1));
        if (std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.f / dtMs > kSyncerMaxSpeedCmS)
            Tally(g_steps, "row steps faster than a prop moves (admitted)", e.elementId, slot);
    }
    row->admitLoc = ue_wrap::FVector{e.x, e.y, e.z};
    row->admitRot = ue_wrap::FRotator{e.pitch, e.yaw, e.roll};
    if (row->elected && !row->admittedEver) {   // the first row: where the host copy stood against where the thrower's copy was
        ue_wrap::FVector cur{};
        void* a = row->ref.Get();
        if (a && E::TryGetActorLocation(a, cur))
            UE_LOGI("[PROP-SYNC] HOST first row eid=%u slot %u: the host copy is %.1f cm from the row, |v|=%.0f cm/s, release-to-row %llu ms",
                    e.elementId, static_cast<unsigned>(slot), std::sqrt((cur.X - e.x) * (cur.X - e.x) + (cur.Y - e.y) * (cur.Y - e.y) + (cur.Z - e.z) * (cur.Z - e.z)),
                    std::sqrt(e.vel[0] * e.vel[0] + e.vel[1] * e.vel[1] + e.vel[2] * e.vel[2]), static_cast<unsigned long long>(nowMs - row->startMs));
    }
    if (row->elected) ++g_thrownAdmitted;
    row->admitVel = ue_wrap::FVector{e.vel[0], e.vel[1], e.vel[2]};
    row->admitSendMs = e.sendTimeMs;
    row->admitMs = nowMs;
    row->admittedEver = true;
    row->freshAdmit = true;
    row->nextProbeMs = 0;
    // The pose is the sender's, the key the row's: a forged key must not name another prop at the apply's fallback.
    coop::net::PropPoseSnapshot fed = e;
    fed.key = row->key;
    coop::prop_drive_stream::FeedAdmitted(fed);
    if (row->syncer != slot) ++g_nonSyncerAdmitted;
    if (coop::dev::prop_syncer_drill::IsEnabled() || coop::dev::swinger_drill::IsEnabled()) {   // the drills' judges read it
        dev::Admitted& seen = g_admittedBy[e.elementId];
        if (seen.gen != row->gen) seen = dev::Admitted{row->gen, 0};
        ++seen.rows;
    }
    Tally(g_admitted, "row admitted", e.elementId, slot);
}

// Once a second (the ATV's RunPass rules 1 and 3): no admitted row for kSyncerSilenceMs, or the syncer's puppet
// beyond kSyncerDistanceCm; and for a thrown prop no admitted row kGrantTimeoutMs after the election, or a flight past
// kMaxThrowFlightMs. MTA re-elects the nearest player (:133-146); a prop here belongs to no author at rest and the
// host's world is always loaded (the ATV's reason, atv_authority.cpp:366-368).
void Pass(uint64_t nowMs) {
    for (Driven& d : coop::prop_drive_host::Rows()) {
        if (d.dead || d.syncer == 0) continue;
        // A thrown prop is the host's again when no row was admitted in time, or when its flight outlasts any throw.
        if (d.elected && !d.admittedEver && nowMs - d.startMs >= tb::kGrantTimeoutMs) { EndClientRow(d, Ending::GrantTimeout); continue; }
        if (d.elected && nowMs - d.startMs >= tb::kMaxThrowFlightMs) { EndClientRow(d, Ending::FlightBound); continue; }
        if (nowMs - d.admitMs >= kSyncerSilenceMs) { EndClientRow(d, Ending::Silence); continue; }
        void* actor = d.ref.Get();
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(d.syncer);
        ue_wrap::FVector at{}, prop{};
        // A puppet or a prop whose place does not read is not out of range: silence and departure cover a gone client.
        if (!actor || !rp || !rp->TryGetLocation(at) || !E::TryGetActorLocation(actor, prop)) continue;
        const float dx = at.X - prop.X, dy = at.Y - prop.Y, dz = at.Z - prop.Z;
        if (!(std::sqrt(dx * dx + dy * dy + dz * dz) <= kSyncerDistanceCm)) EndClientRow(d, Ending::Range);
    }
    for (auto it = g_handBack.begin(); it != g_handBack.end();)   // a record outlives any late window (rtt + 50 ms)
        it = (nowMs - it->second.ms > kSyncerSilenceMs) ? g_handBack.erase(it) : std::next(it);
}

}  // namespace

void OnClaim(coop::net::Session& s, const coop::net::PropSyncClaimPayload& p, uint8_t slot) {
    UE_ASSERT_GAME_THREAD("prop_syncer_host::OnClaim");
    g_session = &s;
    // The tests, in the order of the design: the sender's tokens go before any other test runs (MTA tests its
    // anti-spam first, per vehicle), and a row on the prop is judged before the reach is measured.
    // The developer red arm of the prop syncer drill: every claim is refused as paced, so the drill's kick arm
    // must fail at its claim. Resolved once.
    static const bool s_redRefuse =
        coop::config::ResolveEnum(coop::config_registry::rows::prop_syncer_red) == "refuse";
    // The client sends no claim before its own world-ready (prop_syncer_client), so this is the host's backstop: the
    // pre-world gate drops a PropSyncAnswer to a slot that is not world-ready, and the answer here may not arrive.
    if (!s.IsSlotWorldReady(slot)) return Refuse(s, p, slot, PropSyncRefusal::NotReady);
    if (s_redRefuse || !g_claimBuckets[slot].Take(kClaimBudget, coop::active_drive::NowMs()))
        return Refuse(s, p, slot, PropSyncRefusal::Paced);
    void* actor = ResolveProp(p.eid);
    if (!actor) return Refuse(s, p, slot, PropSyncRefusal::Unknown);
    if (coop::prop_drive_host::InAHand(actor)) return Refuse(s, p, slot, PropSyncRefusal::InHand, actor);
    // A host verb or push in flight wins (MTA's persistent syncer); another client's row is not stolen (MTA steals
    // from a non-persistent syncer, :498-501, which would ping-pong two pushers here). The sender's own row is
    // refused the same way: the client never re-claims while it waits or syncs.
    if (const Driven* row = coop::prop_drive_host::FindByEid(p.eid))
        return Refuse(s, p, slot, row->syncer == 0 ? PropSyncRefusal::HostAuthored : PropSyncRefusal::OtherSyncer, actor);
    // MTA tests the dimension here too (CUnoccupiedVehicleSync.cpp:492); VOTV has one level, so the reach stands in for
    // it. A body or a target the helper cannot read (a puppet that is dead, ragdolled or not yet posed) is Unknown, the
    // nearest analogue of a wrong-dimension player, and only a measured distance beyond reach is OutOfReach.
    switch (EL::IntentTarget::ForClientIntent(s, slot, kReachUU).Authorize(actor).outcome) {
        case EL::IntentOutcome::Ok:         break;
        case EL::IntentOutcome::OutOfReach: return Refuse(s, p, slot, PropSyncRefusal::OutOfReach, actor);
        default:                            return Refuse(s, p, slot, PropSyncRefusal::Unknown, actor);
    }
    Driven* row = coop::prop_drive_host::OpenClientRow(actor, slot, "client claim");
    if (!row) return Refuse(s, p, slot, PropSyncRefusal::Unknown, actor);   // a hand or a class took it since the tests
    row->claimSeq = p.seq;
    ++g_granted;
    UE_LOGI("[PROP-SYNC] HOST granted eid=%u gen=%u to slot %u (reason %u, seq %u) (#%u)", p.eid,
            static_cast<unsigned>(row->gen), static_cast<unsigned>(slot), static_cast<unsigned>(p.reason),
            static_cast<unsigned>(p.seq), g_granted);
    coop::dev::prop_syncer_drill::BeforeGrant(s, slot);   // the load arm's filler goes out ahead of the answer
    Answer(s, slot, p.eid, p.seq, row->gen, PropSyncResult::Granted, PropSyncRefusal::None);
}

bool ElectSyncer(void* actor, uint8_t slot, uint16_t throwSeq, const float releaseLoc[3], uint64_t releaseMs) {
    UE_ASSERT_GAME_THREAD("prop_syncer_host::ElectSyncer");
    static const bool s_noElect = coop::config::ResolveFlag(coop::config_registry::rows::container_throw_no_elect);
    if (s_noElect) return false;   // the throw arms' red: the host keeps its own simulation of the throw
    if (!g_session || !actor || slot == 0 || throwSeq == 0) return false;
    const uint32_t eid = static_cast<uint32_t>(coop::prop_element_tracker::GetPropElementIdForActor(actor));
    // The grant is answered to the thrower alone, on a lane a slot that is not world-ready drops.
    if (!g_session->IsSlotWorldReady(slot)) {
        Tally(g_electRefused, "election refused: the thrower is not world-ready", eid, slot);
        return false;
    }
    int held = 0;
    for (const Driven& d : coop::prop_drive_host::Rows())
        if (!d.dead && d.syncer == slot && d.elected) ++held;
    if (held >= tb::kMaxThrowRowsPerSlot) {
        Tally(g_electRefused, "election refused: the slot syncs too many thrown props", eid, slot);
        return false;
    }
    Driven* row = coop::prop_drive_host::ElectClientRow(actor, slot, "throw");
    if (!row) {
        Tally(g_electRefused, "election refused: the prop cannot open a row", eid, slot);
        return false;
    }
    row->elected    = true;
    row->throwSeq   = throwSeq;
    row->startLoc   = ue_wrap::FVector{releaseLoc[0], releaseLoc[1], releaseLoc[2]};
    row->startMs    = releaseMs;
    row->refusedRun = 0;
    row->claimSeq   = throwSeq;   // the grant, and a Revoked after it, echo the throw
    UE_LOGI("[PROP-SYNC] HOST ELECT eid=%u gen=%u slot %u throw seq %u (#%u)", row->eid, static_cast<unsigned>(row->gen),
            static_cast<unsigned>(slot), static_cast<unsigned>(throwSeq), ++g_elected);
    Answer(*g_session, slot, row->eid, throwSeq, row->gen, PropSyncResult::Granted, PropSyncRefusal::None);
    return true;
}

bool ElectFromMark(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_syncer_host::ElectFromMark");
    if (!g_session || !actor || !coop::prop_throw::HasMarks()) return false;
    coop::prop_throw::Mark m;
    if (!coop::prop_throw::TakeMark(PR::GetInteractableKeyString(actor), &m)) return false;
    return ElectSyncer(actor, m.slot, m.seq, m.loc, m.ms);
}

void OnClientEnd(coop::net::Session& s, const coop::net::PropDriveEndPayload& sent, uint8_t slot) {
    coop::net::PropDriveEndPayload p = sent;   // clamped below before it reaches the host's body or any relay
    UE_ASSERT_GAME_THREAD("prop_syncer_host::OnClientEnd");
    g_session = &s;
    const uint64_t nowMs = coop::active_drive::NowMs();
    Driven* row = coop::prop_drive_host::FindByEid(p.eid);
    if (!row || row->syncer != slot) {
        Tally(IsLateSyncer(p.eid, slot, nowMs) ? g_lateSyncer : g_endsNotSyncer, "End refused: sender is no syncer", p.eid, slot);
        return;
    }
    if (p.gen != row->gen) { Tally(g_endsStale, "End refused: stale generation", p.eid, slot); return; }
    // The End reaches the host's body: its pose meets the bounds a client's held pose meets (a refused End leaves the
    // row, which the silence pass takes back) and its velocity the throw bounds, clamped, before the host copy or any relay.
    if (!coop::net::PropDriveEndInBounds(p)) {
        Tally(g_endsOutOfBounds, "End refused: pose out of bounds", p.eid, slot);
        return;
    }
    {
        float lin[3] = {p.linVelX, p.linVelY, p.linVelZ};
        float ang[3] = {p.angVelX, p.angVelY, p.angVelZ};
        if (tb::ClampPair(lin, tb::kMaxFlightSpeedCmS, ang, tb::kMaxLaunchAngDegS) == tb::Clamp::Clamped) {
            p.linVelX = lin[0]; p.linVelY = lin[1]; p.linVelZ = lin[2];
            p.angVelX = ang[0]; p.angVelY = ang[1]; p.angVelZ = ang[2];
            Tally(g_endsClamped, "End velocity clamped", p.eid, slot);
        }
    }
    // The client's rest End is always admitted when syncer, generation and bounds match, admitted pose or not: it
    // carries the client's truth. The host's copy takes it, every other client gets it as received (the syncer
    // itself is skipped, MTA's pSkip: it ended the drive), and the row goes with no Revoked, since the client ended it.
    coop::prop_drive_stream::ReleaseHost(p);
    s.SendReliable(coop::net::ReliableKind::PropDriveEnd, &p, sizeof(p), slot);
    RecordHandBack(p.eid, slot, nowMs);
    coop::prop_drive_host::EraseRow(*row);
    Tally(g_endsAdmitted, "End admitted (rest)", p.eid, slot);
    if (row->elected)
        UE_LOGI("[PROP-SYNC] HOST thrown eid=%u gen=%u slot %u rested: release-to-End %llu ms, rows admitted %u, jitter (sender dt - arrival dt) max %lld ms over %u rows",
                p.eid, static_cast<unsigned>(p.gen), static_cast<unsigned>(slot), static_cast<unsigned long long>(nowMs - row->startMs), g_thrownAdmitted, static_cast<long long>(g_jitMax), g_jitRows);
}

void Admit(coop::net::Session& s) {
    UE_ASSERT_GAME_THREAD("prop_syncer_host::Admit");
    g_session = &s;
    const uint64_t nowMs = coop::active_drive::NowMs();
    for (auto& v : g_take) v.clear();
    const uint32_t got = s.TakeRemotePropDriveBatches(g_take);   // every origin's merge, under one lock
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot)
        if (got & (1u << slot))
            for (const coop::net::PropPoseSnapshot& e : g_take[slot]) AdmitRow(e, static_cast<uint8_t>(slot), nowMs);
    if (nowMs >= g_nextPassMs) {
        g_nextPassMs = nowMs + kPassMs;
        Pass(nowMs);
    }
}

void OnSlotLeft(uint8_t slot) {
    UE_ASSERT_GAME_THREAD("prop_syncer_host::OnSlotLeft");
    for (Driven& d : coop::prop_drive_host::Rows())
        if (!d.dead && d.syncer == slot) EndClientRow(d, Ending::SlotLeft);
    for (auto it = g_handBack.begin(); it != g_handBack.end();)
        it = (it->second.slot == slot) ? g_handBack.erase(it) : std::next(it);
}

void EndClientRow(Driven& d, Ending why) {
    UE_ASSERT_GAME_THREAD("prop_syncer_host::EndClientRow");
    const uint64_t nowMs = coop::active_drive::NowMs();
    const uint8_t syncer = d.syncer;
    const uint8_t gen = d.gen;
    void* actor = d.ref.Get();
    // A host verb, and the three endings that are the host's own judgement of a thrown prop, hand the prop to the host:
    // its copy goes on from the last admitted row and its stream takes over under a new generation.
    const bool toHost = why == Ending::HostVerb || why == Ending::FlightBound || why == Ending::RowRefused ||
                        why == Ending::GrantTimeout;
    // (1) the host's copy is released at the last admitted pose and velocity, (2) the End is built from it. With
    // nothing admitted no receiver saw this generation and the copy was never parked, and a dead prop has no copy:
    // no release, no End.
    if (why != Ending::Died && d.admittedEver && actor) {
        coop::net::PropDriveEndPayload end = EndOf(d, actor);
        coop::prop_drive_stream::ReleaseHost(end);
        // A hand-over sends no End: its new generation's poses supersede this one on every receiver.
        if (!toHost && g_session) {
            end.physFlags = coop::prop_wire_parity::PhysFlagsOf(actor);
            g_session->SendReliable(coop::net::ReliableKind::PropDriveEnd, &end, sizeof(end));
        }
    }
    // (3) the syncer is told, with the generation it ends (before any bump) and the seq of its claim or the sequence of
    // its throw; a slot that left is told nothing.
    if (why != Ending::SlotLeft && g_session)
        Answer(*g_session, syncer, d.eid, d.claimSeq, gen, PropSyncResult::Revoked, PropSyncRefusal::None);
    RecordHandBack(d.eid, syncer, nowMs);
    Tally(g_ended[static_cast<unsigned>(why)], kEndingName[static_cast<unsigned>(why)], d.eid, syncer);
    if (why == Ending::FlightBound || why == Ending::RowRefused || why == Ending::GrantTimeout)
        UE_LOGI("[PROP-SYNC] HOST took eid=%u back from slot %u (%s) -- %s", d.eid, static_cast<unsigned>(syncer),
                kEndingName[static_cast<unsigned>(why)],
                d.admittedEver ? "from its last admitted row" : "from its own copy, no row was admitted");
    // (4) the row goes, or for a hand-over it becomes the host's: a new generation, nothing sent yet, the stream
    // continuing from the released copy. A verb's claim stays its own; the host's judgement coasts to rest.
    if (!toHost) { coop::prop_drive_host::EraseRow(d); return; }
    d.syncer       = 0;
    d.gen          = coop::prop_drive_host::BumpGen(d.eid);
    d.claimed      = why == Ending::HostVerb;
    d.everSent     = false;
    d.resend       = false;
    d.unreadSaid   = false;
    d.openMs       = nowMs;
    d.lastMoveMs   = nowMs;
    d.nextProbeMs  = 0;
    d.admittedEver = false;
    d.freshAdmit   = false;
    d.admitMs      = 0;
    d.admitVel     = ue_wrap::FVector{};
    d.admitSendMs  = 0;
    d.claimSeq     = 0;
    d.elected      = false;
    d.throwSeq     = 0;
    d.refusedRun   = 0;
    d.streamBucket.Reset();
}

void OnDisconnect() {
    g_session = nullptr;
    for (int slot = 0; slot < g_claimBuckets.size(); ++slot) g_claimBuckets[slot].Reset();
    g_handBack.clear();
    for (auto& v : g_take) v.clear();
    g_nextPassMs = 0;
    g_granted = g_admitted = g_notSyncer = g_lateSyncer = g_staleGen = g_paced = g_steps = 0;
    g_endsAdmitted = g_endsNotSyncer = g_endsStale = g_endsOutOfBounds = g_endsClamped = 0;
    g_nonSyncerAdmitted = 0;
    g_elected = g_electRefused = g_rowsRefused = g_thrownAdmitted = g_jitRows = 0;
    g_jitMax = 0;
    g_admittedBy.clear();
    std::fill(std::begin(g_refused), std::end(g_refused), 0u);
    std::fill(std::begin(g_ended), std::end(g_ended), 0u);
}

namespace dev {

Admitted AdmittedOf(uint32_t eid) {
    const auto it = g_admittedBy.find(eid);
    return it == g_admittedBy.end() ? Admitted{} : it->second;
}

uint32_t NotSyncerCount() { return g_notSyncer; }

uint32_t NonSyncerAdmitted() { return g_nonSyncerAdmitted; }

}  // namespace dev

}  // namespace coop::prop_syncer_host
