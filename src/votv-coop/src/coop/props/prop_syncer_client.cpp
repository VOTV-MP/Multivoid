// coop/props/prop_syncer_client.cpp -- see coop/props/prop_syncer.h. The client table: one entry per prop this
// client has claimed, Pending until the host answers and Syncing while the host admits its rows. MTA's
// unoccupied-vehicle push (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientGame.cpp:4712-4733), with
// the claim sent once per episode and the stop decided by the client's own rest.

#include "coop/props/prop_syncer.h"

#include "coop/dev/prop_syncer_drill.h"     // IsEnabled: the last-grant record is kept only while the drill is on
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/active_drive.h"        // NowMs
#include "coop/props/prop_drive_host.h"     // Claim (the host's), InAHand
#include "coop/props/prop_drive_rules.h"    // the epsilons, the rest rule, the send pace
#include "coop/props/prop_drive_stream.h"   // CloseGen
#include "coop/props/remote_prop.h"         // ResolveLiveActorByEid
#include "coop/session/net_pump.h"          // HasAnnouncedWorldReady
#include "prop_drive_rows.h"                // co-located private header (src tree, not include/)
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace coop::prop_syncer {
namespace {

namespace E     = ue_wrap::engine;
namespace rules = coop::prop_drive_rules;
using coop::net::PropSyncRefusal;
using coop::net::PropSyncResult;
using dev::Count;

// MTA's MIN_PUSH_ANTISPAM_RATE (CUnoccupiedVehicleSync.h:15), here per client and eid: after a refusal or a
// revoke the same prop is not claimed again sooner.
constexpr uint64_t kReclaimBackoffMs = 1500;
// A syncing client that is standing still re-sends its pose this often, well inside the host's kSyncerSilenceMs, and
// reads the prop at most every kRestProbeMs between beats. MTA sends a syncer's rows every 400 ms by default with the
// move and turn speed in each, on change only and with no beat (CMainConfig.cpp:1513, and the client's
// CUnoccupiedVehicleSync.cpp:19,66): rows here are pose-only at kClientDriveSendMs, a lerp-only receiver needing the
// rate that MTA's velocity extrapolation buys, and the beat answers the host's silence rule, which MTA lacks.
constexpr uint64_t kSyncerBeatMs = 1000;
// A claim the host has not answered in this long is lost (an answer can only be lost to a session change): the entry
// goes Idle with the backoff, so nothing skips the host's poses for a prop for good. D7: kPendingMaxMs = 3000.
constexpr uint64_t kPendingMaxMs = 3000;
// An interaction in progress keeps the sync across a still moment: the rest End waits this long after the
// feeder's last Claim.
constexpr uint64_t kInteractionHoldMs = 250;

enum class Role : uint8_t { Unknown, Host, Client };
enum class State : uint8_t { Pending, Syncing };

struct Entry {
    ue_wrap::CachedObjRef ref;
    State                 state = State::Pending;
    uint16_t              seq = 0;
    uint8_t               gen = 0;                 // Syncing only
    coop::net::WireKey    key{};
    bool                  everSent = false;        // a row of this episode went out; sentLoc/sentRot hold the last
    ue_wrap::FVector      sentLoc{};
    ue_wrap::FRotator     sentRot{};
    uint32_t              rowsSent = 0;
    uint64_t              lastSendMs = 0;
    uint64_t              lastMoveMs = 0;          // the copy last moved past the epsilons (or the grant)
    uint64_t              claimSentMs = 0;         // Pending: when the claim went out
    uint64_t              nextProbeMs = 0;         // Syncing: the copy is not read before this
    uint64_t              lastInteractMs = 0;      // the feeder's last Claim
};

// A re-claim of the eid is held off until `untilMs`, and for an InHand refusal while a hand still holds it.
struct Backoff { uint64_t untilMs = 0; bool inHand = false; };

Role                                  g_role = Role::Unknown;
coop::net::Session*                   g_session = nullptr;
uint16_t                              g_seq = 0;
std::unordered_map<uint32_t, Entry>   g_table;
std::unordered_map<uint32_t, Backoff> g_backoff;
std::unordered_map<uint32_t, uint8_t> g_lastGen;     // eid -> the generation of its last grant, for the drill
std::unordered_map<uint32_t, uint64_t> g_lastGrantMs; // eid -> ms from the claim to its last grant, for the drill
uint32_t                              g_count[static_cast<unsigned>(Count::kCount)] = {};
uint32_t                              g_refused[8] = {};

const char* const kRefusalName[8] = {"none", "not_ready", "unknown", "in_hand", "host_authored", "other_syncer",
                                     "out_of_reach", "paced"};

uint32_t& CountRef(Count c) { return g_count[static_cast<unsigned>(c)]; }

// Each outcome is counted and said at the first and at every 100th, so a feeder's every-tick call costs the log nothing.
void Tally(Count c, const char* what, uint32_t eid) {
    const uint32_t n = ++CountRef(c);
    if (n == 1 || (n % 100) == 0) UE_LOGI("[PROP-SYNC] CLIENT %s eid=%u (#%u)", what, eid, n);
}

const char* ReasonName(coop::net::PropSyncReason r) {
    switch (r) {
        case coop::net::PropSyncReason::DevDrill:       return "dev drill";
        case coop::net::PropSyncReason::SwingerParent:  return "swinger parent";
    }
    return "client claim";
}

void SetBackoff(uint32_t eid, uint64_t nowMs, bool inHand) {
    g_backoff[eid] = Backoff{nowMs + kReclaimBackoffMs, inHand};
}

// An episode that ends with a Revoked, a refusal or the host's End closes nothing in the stream's gate: the
// host's End of that generation applies when it arrives. Only this client's own End closes its generation.
void ClientClaim(void* actor, coop::net::PropSyncReason reason) {
    if (!g_session) { Tally(Count::FeederRefused, "claim refused: no session yet", 0); return; }
    // The host answers a slot that is not world-ready on a path its pre-world gate drops, so a claim sent earlier
    // would stay Pending with no answer coming.
    if (!coop::net_pump::HasAnnouncedWorldReady()) {
        Tally(Count::FeederRefused, "claim refused: this client's world is not ready", 0);
        return;
    }
    const coop::element::ElementId id = coop::element::Registry::Get().EidForActor(actor);
    // The host answers a claim only for a host-allocated id and drops any other as malformed, so an unanswered
    // claim would leave the entry Pending (skipping the host's poses) for good.
    if (!coop::element::Registry::IsAllowedHostAllocatedEid(id)) {
        Tally(Count::FeederRefused, "claim refused: the prop has no host element id", id == coop::element::kInvalidId ? 0u : id);
        return;
    }
    const uint32_t eid = static_cast<uint32_t>(id);
    const uint64_t now = coop::active_drive::NowMs();
    if (const auto open = g_table.find(eid); open != g_table.end()) {
        open->second.lastInteractMs = now;   // an open episode: the interaction goes on, no send and no token
        return;
    }
    if (const auto b = g_backoff.find(eid); b != g_backoff.end()) {
        if ((b->second.inHand && coop::prop_drive_host::InAHand(actor)) || now < b->second.untilMs) {
            Tally(Count::HeldOff, "claim held off after a refusal or a revoke", eid);
            return;
        }
        g_backoff.erase(b);
    }
    coop::net::PropSyncClaimPayload p{};
    const std::wstring keyW = ue_wrap::prop::GetInteractableKeyString(actor);
    for (size_t i = 0; i < keyW.size() && i < sizeof(p.key.data); ++i) p.key.data[p.key.len++] = static_cast<char>(keyW[i]);
    p.eid    = eid;
    p.seq    = ++g_seq;
    p.reason = static_cast<uint8_t>(reason);
    if (!g_session->SendReliable(coop::net::ReliableKind::PropSyncClaim, &p, sizeof(p))) {
        Tally(Count::SendFailed, "claim not sent: the reliable lane refused it", eid);
        return;
    }
    Entry& e = g_table[eid];
    e.ref.Set(actor);
    e.state = State::Pending;
    e.seq = p.seq;
    e.key = p.key;
    e.lastInteractMs = now;
    e.claimSentMs = now;
    const uint32_t n = ++CountRef(Count::ClaimsSent);
    if (n == 1 || (n % 100) == 0)
        UE_LOGI("[PROP-SYNC] CLIENT claim sent eid=%u seq=%u reason=%u (%s) (#%u)", eid, static_cast<unsigned>(p.seq),
                static_cast<unsigned>(p.reason), ReasonName(reason), n);
}

// The End of a generation this client no longer holds (its entry ended while the claim was out): the copy's
// pose now, so the host's row ends at once instead of after the silence, and the generation closed here.
void EndUnheldGrant(const coop::net::PropSyncAnswerPayload& p) {
    ++CountRef(Count::GrantedNoEntry);
    void* actor = g_session ? coop::remote_prop::ResolveLiveActorByEid(p.eid) : nullptr;
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (actor && E::TryGetActorLocation(actor, loc) && E::TryGetActorRotation(actor, rot)) {
        const coop::net::PropDriveEndPayload end = coop::prop_drive_host::EndFromBody(actor, p.eid, p.gen, loc, rot);
        if (!g_session->SendReliable(coop::net::ReliableKind::PropDriveEnd, &end, sizeof(end)))
            Tally(Count::SendFailed, "End of an unheld grant not sent: the reliable lane refused it", p.eid);
        UE_LOGI("[PROP-SYNC] CLIENT granted without an entry eid=%u gen=%u -- End sent at (%.1f,%.1f,%.1f), "
                "generation closed (#%u)", p.eid, static_cast<unsigned>(p.gen), loc.X, loc.Y, loc.Z,
                CountRef(Count::GrantedNoEntry));
    } else {
        UE_LOGW("[PROP-SYNC] CLIENT granted without an entry eid=%u gen=%u -- the prop does not read here, no End "
                "sent (the host takes the row back on silence), generation closed (#%u)", p.eid,
                static_cast<unsigned>(p.gen), CountRef(Count::GrantedNoEntry));
    }
    coop::prop_drive_stream::CloseGen(p.eid, p.gen);
}

void OnGranted(const coop::net::PropSyncAnswerPayload& p) {
    const auto it = g_table.find(p.eid);
    if (it == g_table.end() || it->second.state != State::Pending || it->second.seq != p.seq) {
        EndUnheldGrant(p);
        return;
    }
    // The grant carries no state and the copy keeps simulating, where MTA's StartSync resets the syncer to the
    // server's place and speed (the client's CUnoccupiedVehicleSync.cpp:102-134): a pusher's own copy is the contact
    // truth, and a snap to the host's pre-kick pose would undo the kick.
    Entry& e = it->second;
    e.state = State::Syncing;
    e.gen = p.gen;
    e.everSent = false;
    e.rowsSent = 0;
    const uint64_t now = coop::active_drive::NowMs();
    e.lastMoveMs = now;
    e.nextProbeMs = 0;
    const uint64_t ms = now - e.claimSentMs;   // the claim's send to this grant, on this client's clock
    if (coop::dev::prop_syncer_drill::IsEnabled()) {
        g_lastGen[p.eid] = p.gen;
        g_lastGrantMs[p.eid] = ms;
    }
    ++CountRef(Count::Granted);
    UE_LOGI("[PROP-SYNC] CLIENT granted eid=%u gen=%u seq=%u ms=%llu (#%u)", p.eid, static_cast<unsigned>(p.gen),
            static_cast<unsigned>(p.seq), static_cast<unsigned long long>(ms), CountRef(Count::Granted));
}

void OnRefused(const coop::net::PropSyncAnswerPayload& p) {
    const auto it = g_table.find(p.eid);
    if (it == g_table.end() || it->second.state != State::Pending || it->second.seq != p.seq) {
        Tally(Count::AnswersDropped, "answer dropped: no claim is open for it", p.eid);
        return;
    }
    const uint64_t now = coop::active_drive::NowMs();
    const uint64_t ms = now - it->second.claimSentMs;   // the claim's send to this refusal
    g_table.erase(it);
    SetBackoff(p.eid, now, p.refusal == static_cast<uint8_t>(PropSyncRefusal::InHand));
    ++CountRef(Count::Refused);
    uint32_t& n = g_refused[p.refusal & 7u];
    if (++n == 1 || (n % 100) == 0)
        UE_LOGI("[PROP-SYNC] CLIENT refused eid=%u: %s ms=%llu (#%u)", p.eid, kRefusalName[p.refusal & 7u],
                static_cast<unsigned long long>(ms), n);
}

void OnRevoked(const coop::net::PropSyncAnswerPayload& p) {
    const auto it = g_table.find(p.eid);
    if (it == g_table.end() || it->second.state != State::Syncing || it->second.gen != p.gen) {
        Tally(Count::AnswersDropped, "answer dropped: no entry of that generation", p.eid);
        return;
    }
    g_table.erase(it);
    SetBackoff(p.eid, coop::active_drive::NowMs(), false);
    ++CountRef(Count::Revoked);
    UE_LOGI("[PROP-SYNC] CLIENT revoked eid=%u gen=%u -- the host took the prop back (#%u)", p.eid,
            static_cast<unsigned>(p.gen), CountRef(Count::Revoked));
}

// One Syncing entry's turn: a row when the copy moved (or the beat is due), the rest End when it rested and the
// interaction is over. True when the entry ended.
bool StepSyncing(coop::net::Session& s, uint32_t eid, Entry& e, void* actor, uint64_t now) {
    // The engine is read (two dispatches) only when a row or the rest End may be due, in the host rows' order: the
    // probe cadence and the send pace first, then the queue's turn. A moving copy is read at the pace, a still one at
    // kRestProbeMs, so a feeder calling every tick costs nothing between turns.
    if (now < e.nextProbeMs) return false;
    if (e.everSent && now - e.lastSendMs < rules::kClientDriveSendMs) return false;
    if (s.PropDrivePoseTurn(eid) == coop::net::PoseTurn::Wait) return false;   // read again on a later tick
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(actor, loc) || !E::TryGetActorRotation(actor, rot)) return false;
    const bool moved = !e.everSent || rules::MovedPastEps(loc, rot, e.sentLoc, e.sentRot);
    if (moved) e.lastMoveMs = now;
    if (now - e.lastMoveMs >= rules::kRestMs && now - e.lastInteractMs >= kInteractionHoldMs) {
        const coop::net::PropDriveEndPayload end = coop::prop_drive_host::EndFromBody(actor, eid, e.gen, loc, rot);
        if (!s.SendReliable(coop::net::ReliableKind::PropDriveEnd, &end, sizeof(end))) {
            Tally(Count::SendFailed, "rest End not sent: the reliable lane refused it", eid);
            return false;   // the next tick tries again; the entry stays
        }
        coop::prop_drive_stream::CloseGen(eid, e.gen);   // the host's echo of this End and any late pose are dropped
        ++CountRef(Count::RestEnds);
        UE_LOGI("[PROP-SYNC] CLIENT rest End eid=%u gen=%u -> (%.1f,%.1f,%.1f) after %u rows (#%u)", eid,
                static_cast<unsigned>(e.gen), loc.X, loc.Y, loc.Z, e.rowsSent, CountRef(Count::RestEnds));
        return true;
    }
    const bool due = moved || now - e.lastSendMs >= kSyncerBeatMs;
    if (!due) {
        e.nextProbeMs = now + (now - e.lastMoveMs >= rules::kRestMs ? rules::kRestProbeMs : rules::kClientDriveSendMs);
        return false;
    }
    coop::net::PropPoseSnapshot pp{};
    pp.key       = e.key;
    pp.elementId = eid;
    pp.ctx       = e.gen;
    pp.x = loc.X; pp.y = loc.Y; pp.z = loc.Z;
    pp.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
    pp.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
    pp.roll  = ue_wrap::NormalizeAxis(rot.Roll);
    s.PublishPropDrivePose(pp);   // queued: it goes out in its turn, refreshed until then
    e.everSent   = true;
    e.sentLoc    = loc;
    e.sentRot    = rot;
    e.lastSendMs = now;
    e.nextProbeMs = 0;   // the send pace governs the next read
    ++e.rowsSent;
    Tally(Count::RowsSent, "row sent", eid);
    return false;
}

}  // namespace

void Claim(void* actor, coop::net::PropSyncReason reason) {
    UE_ASSERT_GAME_THREAD("prop_syncer::Claim");
    if (!actor) return;
    if (g_role == Role::Host) { coop::prop_drive_host::Claim(actor, ReasonName(reason)); return; }
    if (g_role == Role::Client) { ClientClaim(actor, reason); return; }
    Tally(Count::FeederRefused, "claim refused: no role yet", 0);
}

void Tick(coop::net::Session& s, bool isHost) {
    UE_ASSERT_GAME_THREAD("prop_syncer::Tick");
    g_role = isHost ? Role::Host : Role::Client;
    g_session = &s;
    if (isHost || g_table.empty()) return;
    const uint64_t now = coop::active_drive::NowMs();
    for (auto it = g_table.begin(); it != g_table.end();) {
        Entry& e = it->second;
        void* actor = e.ref.Get();
        if (!actor) {   // the destroy crossed on its own seam; the host's row drops with its copy
            Tally(Count::Died, "entry dropped: the prop died here", it->first);
            it = g_table.erase(it);
            continue;
        }
        if (coop::prop_drive_host::InAHand(actor)) {   // the held stream owns it; the host closes the row
            Tally(Count::HandEnds, "entry dropped: a hand took the prop here", it->first);
            SetBackoff(it->first, now, /*inHand=*/true);   // the feeder's next Claim waits for the hand to let go
            it = g_table.erase(it);
            continue;
        }
        if (e.state == State::Pending && now - e.claimSentMs >= kPendingMaxMs) {
            Tally(Count::PendingExpired, "claim dropped: the host never answered it", it->first);
            SetBackoff(it->first, now, false);
            it = g_table.erase(it);
            continue;
        }
        if (e.state == State::Syncing && StepSyncing(s, it->first, e, actor, now)) it = g_table.erase(it);
        else ++it;
    }
}

void OnAnswer(const coop::net::PropSyncAnswerPayload& p) {
    UE_ASSERT_GAME_THREAD("prop_syncer::OnAnswer");
    if (g_role != Role::Client) { Tally(Count::AnswersDropped, "answer dropped: not a client", p.eid); return; }
    switch (static_cast<PropSyncResult>(p.result)) {
        case PropSyncResult::Granted: OnGranted(p); break;
        case PropSyncResult::Refused: OnRefused(p); break;
        case PropSyncResult::Revoked: OnRevoked(p); break;
        default: Tally(Count::AnswersDropped, "answer dropped: unknown result", p.eid); break;
    }
}

void OnHostEnd(uint32_t eid, uint8_t gen) {
    UE_ASSERT_GAME_THREAD("prop_syncer::OnHostEnd");
    if (g_table.empty()) return;
    const auto it = g_table.find(eid);
    if (it == g_table.end() || it->second.state != State::Syncing || it->second.gen != gen) return;
    g_table.erase(it);
    SetBackoff(eid, coop::active_drive::NowMs(), false);
    ++CountRef(Count::HostEnds);
    UE_LOGI("[PROP-SYNC] CLIENT host End eid=%u gen=%u -- the host ended this client's row (#%u)", eid,
            static_cast<unsigned>(gen), CountRef(Count::HostEnds));
}

bool SkipsHostPose(uint32_t eid) {
    if (g_table.empty() || g_table.find(eid) == g_table.end()) return false;
    Tally(Count::EchoSkipped, "host pose skipped: this client authors the drive", eid);
    return true;
}

void OnDisconnect() {
    g_role = Role::Unknown;
    g_session = nullptr;
    g_seq = 0;
    g_table.clear();
    g_backoff.clear();
    g_lastGen.clear();
    g_lastGrantMs.clear();
    for (uint32_t& n : g_count) n = 0;
    for (uint32_t& n : g_refused) n = 0;
}

namespace dev {

EntryView EntryOf(uint32_t eid) {
    EntryView v;
    const auto it = g_table.find(eid);
    if (it == g_table.end()) return v;
    v.state    = it->second.state == State::Pending ? EntryState::Pending : EntryState::Syncing;
    v.gen      = it->second.gen;
    v.seq      = it->second.seq;
    v.rowsSent = it->second.rowsSent;
    return v;
}

uint8_t LastGrantedGen(uint32_t eid) {
    const auto it = g_lastGen.find(eid);
    return it == g_lastGen.end() ? 0 : it->second;
}

uint64_t LastGrantMs(uint32_t eid) {
    const auto it = g_lastGrantMs.find(eid);
    return it == g_lastGrantMs.end() ? 0 : it->second;
}

uint32_t CountOf(Count c) {
    return c < Count::kCount ? g_count[static_cast<unsigned>(c)] : 0;
}

uint32_t RefusedBy(uint8_t refusal) {
    return refusal < 8 ? g_refused[refusal] : 0;
}

}  // namespace dev

}  // namespace coop::prop_syncer
