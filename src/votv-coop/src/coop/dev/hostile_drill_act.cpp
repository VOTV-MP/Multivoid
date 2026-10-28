// coop/dev/hostile_drill_act.cpp -- see coop/dev/hostile_drill_act.h.

#include "coop/dev/hostile_drill_act.h"

#include "hostile_drill_act_detail.h"  // co-located private header (src tree, not include/)

#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_sync.h"
#include "coop/dev/announce_hold.h"
#include "coop/dev/drill_props.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/prop_act_host.h"
#include "coop/props/prop_act_request.h"
#include "coop/props/prop_hold.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace coop::dev::hostile_drill::act {
using namespace detail;
namespace {

using Clock  = std::chrono::steady_clock;
using Kind   = coop::net::ReliableKind;
using Reason = coop::net::PropActReason;
using Family = coop::net::PropActFamily;
using ue_wrap::FVector;
namespace EL  = coop::element;
namespace E   = ue_wrap::engine;
namespace PAH = coop::prop_act_host;
namespace PAR = coop::prop_act_request;
namespace DP  = coop::dev::drill_props;

// The flood: far over the queue's cap of 2, so a queue that bounds nothing holds them all.
constexpr uint32_t kFlood = 121;
// The judge's numbers, literals written from the lane's budget (no symbol of the change itself): the deepest the
// queue may be, and the requests of the flood the cap refuses outright before the sender is world-ready.
constexpr uint32_t kCapJudged = 2;
constexpr uint32_t kFloodRefusedUnready = kFlood - kCapJudged;
constexpr uint16_t kForgedCommitId = 40000;   // an id the client never asked with
constexpr uint16_t kRequestId = 1;
// The last host-band id: never allocated in a fresh world, so a request for it names no prop.
constexpr EL::ElementId kUnusedEid = EL::kHostRangeSize - 1;
constexpr auto kFailBound = std::chrono::seconds(180);   // a failure bound, never a wait

// The host's spawn: a crowbar on the floor beside the client's puppet (near) or 3000 uu away from it (far), the floor
// found by a trace of its own.
constexpr float kFarCm = 3000.f;
constexpr float kNearCm = 120.f;
constexpr float kDropCm = 40.f;          // above the floor, so the prop settles
constexpr float kNearUpCm = 150.f, kNearDownCm = 600.f;
constexpr float kFarUpCm = 1500.f, kFarDownCm = 4000.f;
constexpr float kMinColumnCm = 20.f;     // a column whose floor is nearer than this to its start began inside geometry

const char* kAbortedLine = "hostile-drill bad-commit aborted";

bool Unready(Arm a) { return a == Arm::ActNotYet || a == Arm::ActFloodEarly; }
bool DeadTarget(Arm a) { return a == Arm::ActGone || a == Arm::ActFlood; }
Reason WantedReason(Arm a) { return a == Arm::ActFar ? Reason::Reach : Reason::Gone; }

const char* ReasonName(size_t i) {
    static const char* const names[] = {"Granted", "Gone", "WrongFamily", "Held", "Reach", "QueueFull"};
    return i < sizeof(names) / sizeof(names[0]) ? names[i] : "?";
}

// The lines the two peers say to each other in the lobby chat. No save can hold them, as a prop class or a place could.
std::string ReadyHead(Arm a) { return std::string("hostile-drill ") + ArmName(a) + " host-ready eid="; }
std::string JudgedLine(Arm a) { return std::string("hostile-drill ") + ArmName(a) + " judged"; }

// ---- client (sender) ----
enum class CStep { Idle, AwaitReply, AwaitAborted, AwaitJudged, Done };
CStep g_cstep = CStep::Idle;
EL::ElementId g_ceid = EL::kInvalidId;
uint32_t g_staleBase = 0;
Clock::time_point g_cwait{};   // the first tick the current wait began
bool g_holdSet = false;

void ClientEnd(Arm a, const char* what, const char* why) {
    LogVerdict(a, what, why);
    g_cstep = CStep::Done;
}

// True once a wait has run past the failure bound: the wait itself is on a line or a count, never on this.
bool ClientWaitedTooLong() {
    if (g_cwait == Clock::time_point{}) g_cwait = Clock::now();
    return Clock::now() - g_cwait > kFailBound;
}

bool SendCommit(coop::net::Session* s, uint16_t id, uint8_t committed, EL::ElementId eid) {
    coop::net::PropActCommitPayload p{};
    p.id = id;
    p.committed = committed;
    p.eid = static_cast<uint32_t>(eid);
    return s->SendReliableToSlot(0, Kind::PropActCommit, &p, sizeof(p));
}

// The host's ready line carries the target's eid; false until the line is in the lobby chat.
bool HostSaidReady(Arm a, EL::ElementId* eid) {
    const std::string head = ReadyHead(a);
    std::string line;
    if (!ChatLine(head, &line)) return false;
    *eid = static_cast<EL::ElementId>(std::strtoul(line.c_str() + line.find(head) + head.size(), nullptr, 10));
    return true;
}

// The reply count a verdict reads: replies that matched no record, which every forged request's answer is.
uint32_t StaleReplies() { return PAR::dev::LastStaleReply().count; }

void ReadJudged(Arm a, uint32_t want) {
    if (!OwnJoinOver()) return;
    if (!PollDue()) return;
    if (!ChatLine(JudgedLine(a), nullptr)) {
        if (ClientWaitedTooLong()) ClientEnd(a, "FAIL", "the host never said it judged");
        return;
    }
    const uint32_t n = StaleReplies();
    if (n == want) {
        ClientEnd(a, "PASS", "the host's answers all arrived, once each, after the announce");
    } else if (n > want) {
        ClientEnd(a, "FAIL", "more replies came than requests were sent");
    } else if (ClientWaitedTooLong()) {
        char why[96];
        std::snprintf(why, sizeof(why), "%u of %u replies came", n, want);
        ClientEnd(a, "FAIL", why);
    }
}

// act_notyet and act_flood_early: the requests go out while the announce is held, then it is let go.
void ClientEarly(coop::net::Session* s, Arm a) {
    if (g_cstep == CStep::AwaitJudged) {
        ReadJudged(a, 1);
        return;
    }
    if (coop::net_pump::HasAnnouncedWorldReady()) {
        ClientEnd(a, "INCONCLUSIVE", "the announce went before the requests could be sent");
        return;
    }
    if (!coop::dev::announce_hold::Held()) return;   // Install sets the hold at the session's start
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (!coop::players::Registry::Get().Local() || !s->connected() || self == 0 ||
        self >= coop::players::kMaxPeers)
        return;   // the world or the slot is not up yet; the hold stays
    const uint32_t n = a == Arm::ActNotYet ? 1u : kFlood;
    bool sent = EL::Registry::Get().Get(kUnusedEid) == nullptr;
    for (uint32_t i = 0; sent && i < n; ++i) sent = SendRequest(s, static_cast<uint16_t>(i + 1), Family::Pocket, kUnusedEid);
    coop::dev::announce_hold::Set(false);
    if (!sent) {
        ClientEnd(a, "INCONCLUSIVE", "the requests were not all sent (or this client holds the unused id)");
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] %s sent %u PropActRequest for eid %u with the announce held, and let it go", ArmName(a),
            n, static_cast<unsigned>(kUnusedEid));
    g_cwait = {};
    g_cstep = a == Arm::ActNotYet ? CStep::AwaitJudged : CStep::Done;
}

// The arms that name a real or lately real prop wait for the host's ready line, which carries its eid.
void ClientTarget(coop::net::Session* s, Arm a) {
    if (g_cstep == CStep::AwaitJudged) {
        ReadJudged(a, kFlood);
        return;
    }
    if (g_cstep == CStep::AwaitReply) {
        const PAR::dev::LastReply last = PAR::dev::LastStaleReply();
        if (last.count == g_staleBase) {
            if (ClientWaitedTooLong()) ClientEnd(a, "FAIL", "the host never answered the take");
            return;
        }
        if (last.id != kRequestId || last.granted != 1 || last.reason != 0) {
            ClientEnd(a, "FAIL", "the answer to the take was not a grant");
            return;
        }
        if (!SendCommit(s, kRequestId, 0, g_ceid)) {
            ClientEnd(a, "INCONCLUSIVE", "the abort was not sent");
            return;
        }
        UE_LOGI("[HOSTILE-DRILL] %s was granted the take and aborted it", ArmName(a));
        g_cwait = {};
        g_cstep = CStep::AwaitAborted;
        return;
    }
    if (g_cstep == CStep::AwaitAborted) {
        if (!PollDue()) return;
        if (!ChatLine(kAbortedLine, nullptr)) {
            if (ClientWaitedTooLong()) ClientEnd(a, "FAIL", "the host never said the abort was taken");
            return;
        }
        if (!SendCommit(s, kRequestId, 0, g_ceid)) {
            ClientEnd(a, "INCONCLUSIVE", "the second abort was not sent");
            return;
        }
        UE_LOGI("[HOSTILE-DRILL] %s sent the same abort again", ArmName(a));
        g_cstep = CStep::Done;
        return;
    }
    // Idle: the join over, then the host's line.
    if (!OwnJoinOver() || !PollDue()) return;
    if (!HostSaidReady(a, &g_ceid)) {
        if (ClientWaitedTooLong()) ClientEnd(a, "FAIL", "the host never said it was ready");
        return;
    }
    g_cwait = {};
    switch (a) {
    case Arm::ActBadCommit: {
        g_staleBase = StaleReplies();
        if (!SendCommit(s, kForgedCommitId, 1, g_ceid) || !SendRequest(s, kRequestId, Family::Pocket, g_ceid)) {
            ClientEnd(a, "INCONCLUSIVE", "the forged commit or the take was not sent");
            return;
        }
        UE_LOGI("[HOSTILE-DRILL] %s sent a commit for id %u it never asked with, then a take of eid %u", ArmName(a),
                static_cast<unsigned>(kForgedCommitId), static_cast<unsigned>(g_ceid));
        g_cstep = CStep::AwaitReply;
        return;
    }
    case Arm::ActFlood: {
        for (uint32_t i = 0; i < kFlood; ++i) {
            if (SendRequest(s, static_cast<uint16_t>(i + 1), Family::Pocket, g_ceid)) continue;
            ClientEnd(a, "INCONCLUSIVE", "a request of the flood was not sent");
            return;
        }
        UE_LOGI("[HOSTILE-DRILL] %s sent %u PropActRequest for eid %u", ArmName(a), kFlood,
                static_cast<unsigned>(g_ceid));
        g_cstep = CStep::AwaitJudged;
        return;
    }
    default:
        break;
    }
    if (!SendRequest(s, kRequestId, Family::Pocket, g_ceid)) {
        ClientEnd(a, "INCONCLUSIVE", "the request was not sent");
        return;
    }
    UE_LOGI("[HOSTILE-DRILL] %s sent one PropActRequest (family %u) for eid %u", ArmName(a),
            static_cast<unsigned>(Family::Pocket), static_cast<unsigned>(g_ceid));
    g_cstep = CStep::Done;
}

// ---- host (judge) ----
enum class HStep { WaitArm, Prep, Judge, Done };
HStep g_hstep = HStep::WaitArm;
int g_slot = -1;
uint16_t g_playerNo = 0;   // the judged slot's occupant at arm: the counters reset when it changes
Clock::time_point g_armAt{}, g_readyAt{};
ue_wrap::CachedObjRef g_target;
EL::ElementId g_eid = EL::kInvalidId;
bool g_saidAborted = false;
uint32_t g_answeredBase[coop::net::kPropActReasons] = {};
uint32_t g_totalBase = 0, g_waitsBase = 0, g_unreadyBase = 0, g_badBase = 0, g_abortBase = 0;

void Finish(Arm a, const char* what, const std::string& why) {
    LogVerdict(a, what, why);
    // The client that reads a verdict of its own reads it after this line, and only after a pass.
    if (std::string_view(what) == "PASS" && (a == Arm::ActNotYet || a == Arm::ActFlood)) Say(JudgedLine(a));
    g_hstep = HStep::Done;
}

bool Bounded(Arm a, const Clock::time_point& since, const char* what) {
    if (Clock::now() - since <= kFailBound) return false;
    Finish(a, "FAIL", what);
    return true;
}

void TakeBaseline() {
    for (size_t i = 0; i < coop::net::kPropActReasons; ++i)
        g_answeredBase[i] = PAH::AnsweredFor(g_slot, static_cast<Reason>(i));
    g_totalBase = PAH::AnsweredTotal(g_slot);
    g_waitsBase = PAH::WaitsOf(g_slot, PAH::WaitCause::NotReady);
    g_unreadyBase = PAH::TallyOf(PAH::Tally::QueueFullUnready);
    g_badBase = PAH::TallyOf(PAH::Tally::BadCommit);
    g_abortBase = PAH::CommitsOf(g_slot, PAH::internal::CommitStep::Abort);
    g_playerNo = coop::roster_ledger::Get(g_slot).playerNo;
}

bool TargetAlive() {
    void* const t = g_target.Get();
    return t && EL::LivePropActor(g_eid) == t;
}

// Spawns the arm's crowbar beside (or 3000 uu from) the client's puppet. Null on success, else why not.
const char* SpawnTarget(Arm a, coop::RemotePlayer& rp) {
    void* p = nullptr;
    if (const char* why = SpawnOnFloor(rp, kCrowbarPath, a == Arm::ActFar, 0, nullptr, &p)) return why;
    g_target.Set(p);
    return nullptr;
}

// The arms that need the host's eye for the sender wait for a posed puppet, then spawn their prop.
void ArmTarget(coop::net::Session* s, Arm a) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && g_slot < 0; ++slot)
        if (s->IsSlotWorldReady(slot)) g_slot = slot;
    if (g_slot < 0) return;
    if (!coop::prop_snapshot::IsBracketClosed(g_slot) || coop::join_window_baseline::IsLateWindowOpen(g_slot)) return;
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(g_slot);
    if (!rp.valid() || !rp.HasPose()) return;
    if (const char* why = SpawnTarget(a, rp)) {
        Finish(a, "INCONCLUSIVE", why);
        return;
    }
    g_playerNo = coop::roster_ledger::Get(g_slot).playerNo;
    g_armAt = Clock::now();
    g_hstep = HStep::Prep;
}

// The spawned prop is announced when its eid has a row; the arms whose prop must be gone destroy it then, from drill code.
void PrepTarget(Arm a) {
    if (Bounded(a, g_armAt, "the target never became ready")) return;
    if (g_eid == EL::kInvalidId) {
        void* const actor = g_target.Get();
        if (!actor) {
            Finish(a, "INCONCLUSIVE", "the spawned prop was gone before it was announced");
            return;
        }
        const EL::ElementId eid = EL::Registry::Get().EidForActor(actor);
        if (eid == EL::kInvalidId) return;   // not announced yet
        g_eid = eid;
        if (DeadTarget(a)) E::DestroyActor(actor);
    }
    if (DeadTarget(a) && EL::LivePropActor(g_eid)) return;   // the destroy has not landed
    TakeBaseline();
    Say(ReadyHead(a) + std::to_string(static_cast<unsigned>(g_eid)));
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %d: eid %u said ready%s", ArmName(a), g_slot, static_cast<unsigned>(g_eid),
            DeadTarget(a) ? " (destroyed first)" : "");
    g_armAt = Clock::now();
    g_hstep = HStep::Judge;
}

// act_notyet and act_flood_early: armed while the slot is connected and not yet world-ready.
void ArmUnready(coop::net::Session* s, Arm a) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && g_slot < 0; ++slot)
        if (s->HasPeerConn(slot) && coop::roster_ledger::Occupied(slot) && !s->IsSlotWorldReady(slot)) g_slot = slot;
    if (g_slot < 0) return;
    if (EL::LivePropActor(kUnusedEid)) {
        Finish(a, "FAIL", "eid is live: the unused id names a prop on this host");
        return;
    }
    TakeBaseline();
    g_armAt = Clock::now();
    g_hstep = HStep::Judge;
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %d (connected, not world-ready)", ArmName(a), g_slot);
}

// act_far and act_gone: one request, one answer, and it is the arm's reason.
void JudgeOne(Arm a) {
    const uint32_t total = Since(PAH::AnsweredTotal(g_slot), g_totalBase);
    if (total == 0) {
        Bounded(a, g_armAt, "no request was answered");
        return;
    }
    const size_t want = static_cast<size_t>(WantedReason(a));
    std::string seen;
    for (size_t i = 0; i < coop::net::kPropActReasons; ++i) {
        const uint32_t n = Since(PAH::AnsweredFor(g_slot, static_cast<Reason>(i)), g_answeredBase[i]);
        if (n) seen += std::string(" ") + ReasonName(i) + "=" + std::to_string(n);
    }
    // The prop stands, except for the arm whose prop was destroyed first (it must still be gone).
    const bool stateOk = a == Arm::ActGone ? EL::LivePropActor(g_eid) == nullptr : TargetAlive();
    const uint32_t gotWanted = Since(PAH::AnsweredFor(g_slot, static_cast<Reason>(want)), g_answeredBase[want]);
    if (gotWanted == 1 && total == 1 && stateOk)
        Finish(a, "PASS", std::string("the host answered ") + ReasonName(want) + " once and nothing else");
    else
        Finish(a, "FAIL", "the host answered" + seen + " (wanted " + ReasonName(want) + " once)" +
                              (stateOk ? "" : "; the prop's state is not what the arm leaves it in"));
}

void JudgeBadCommit(Arm a) {
    if (!TargetAlive()) {
        Finish(a, "FAIL", "the crowbar was destroyed by a commit that never matched a take");
        return;
    }
    // The abort is taken once the slot's reservation is back to 0; the client waits on this line to send it again.
    if (!g_saidAborted && Since(PAH::CommitsOf(g_slot, PAH::internal::CommitStep::Abort), g_abortBase) >= 1 &&
        coop::prop_hold::ReservationCount(g_slot) == 0) {
        Say(kAbortedLine);
        g_saidAborted = true;
    }
    const uint32_t bad = Since(PAH::TallyOf(PAH::Tally::BadCommit), g_badBase);
    if (bad > 2) {
        Finish(a, "FAIL", "more than two bad commits were counted: " + std::to_string(bad));
    } else if (bad == 2) {
        if (g_saidAborted) Finish(a, "PASS", "two bad commits tallied, the abort released the take, the crowbar stands");
        else Finish(a, "FAIL", "two bad commits tallied before any abort was taken");
    } else {
        Bounded(a, g_armAt, "the two bad commits never arrived");
    }
}

void JudgeFlood(Arm a) {
    const uint32_t depth = PAH::MaxDepthOf(g_slot);
    const uint32_t total = Since(PAH::AnsweredTotal(g_slot), g_totalBase);
    const uint32_t full = Since(PAH::AnsweredFor(g_slot, Reason::QueueFull),
                                g_answeredBase[static_cast<size_t>(Reason::QueueFull)]);
    char sum[120];
    std::snprintf(sum, sizeof(sum), "answered %u of %u, queue full %u, deepest queue %u", total, kFlood, full, depth);
    if (depth > kCapJudged) {
        Finish(a, "FAIL", std::string("cap (the queue held more than ") + std::to_string(kCapJudged) + "); " + sum);
    } else if (total < kFlood) {
        Bounded(a, g_armAt, sum);
    } else if (total != kFlood) {
        Finish(a, "FAIL", std::string("more answers than requests; ") + sum);
    } else if (full < 1) {
        Finish(a, "FAIL", std::string("no request was refused QueueFull; ") + sum);
    } else {
        Finish(a, "PASS", sum);
    }
}

// act_notyet and act_flood_early: judged after the slot is world-ready.
void JudgeUnready(coop::net::Session* s, Arm a) {
    const bool ready = s->IsSlotWorldReady(g_slot);
    const uint32_t total = Since(PAH::AnsweredTotal(g_slot), g_totalBase);
    if (a == Arm::ActNotYet && !ready) {
        if (total) Finish(a, "FAIL", "answered before the slot was world-ready: " + std::to_string(total));
        return;
    }
    if (!ready) return;
    if (g_readyAt == Clock::time_point{}) g_readyAt = Clock::now();
    char sum[120];
    if (a == Arm::ActNotYet) {
        if (total == 0) {
            Bounded(a, g_readyAt, "no answer after the slot was world-ready");
            return;
        }
        const uint32_t waits = Since(PAH::WaitsOf(g_slot, PAH::WaitCause::NotReady), g_waitsBase);
        std::snprintf(sum, sizeof(sum), "answered %u, waited not-ready %u", total, waits);
        if (total == 1 && waits > 0) Finish(a, "PASS", std::string("the request waited and was answered once, after the announce; ") + sum);
        else Finish(a, "FAIL", std::string("wanted one answer after at least one not-ready wait; ") + sum);
        return;
    }
    if (total < kCapJudged) {
        Bounded(a, g_readyAt, "the queued requests were not answered after the slot was world-ready");
        return;
    }
    const uint32_t unready = Since(PAH::TallyOf(PAH::Tally::QueueFullUnready), g_unreadyBase);
    std::snprintf(sum, sizeof(sum), "answered %u, counted unanswered %u", total, unready);
    if (total == kCapJudged && unready == kFloodRefusedUnready)
        Finish(a, "PASS", std::string("the requests past the cap were counted and not answered; ") + sum);
    else
        Finish(a, "FAIL", std::string("wanted two answers and 119 counted; ") + sum);
}

void Reset() {
    g_cstep = CStep::Idle;
    g_ceid = EL::kInvalidId;
    g_staleBase = 0;
    g_cwait = {};
    g_hstep = HStep::WaitArm;
    g_slot = -1;
    g_playerNo = 0;
    g_armAt = g_readyAt = {};
    g_target.Reset();
    g_eid = EL::kInvalidId;
    g_saidAborted = false;
    g_totalBase = g_waitsBase = g_unreadyBase = g_badBase = g_abortBase = 0;
    for (uint32_t& n : g_answeredBase) n = 0;
}

}  // namespace

namespace detail {

void Say(const std::string& line) { coop::chat_sync::QueueSend(line); }

bool ChatLine(const std::string& needle, std::string* whole) {
    bool found = false;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        if (found || r.line.find(needle) == std::string_view::npos) return;
        found = true;
        if (whole) *whole = std::string(r.line);
    });
    return found;
}

bool PollDue() {
    static uint32_t sTicks = 0;
    return sTicks++ % 15 == 0;
}

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

bool SendRequest(coop::net::Session* s, uint16_t id, Family family, EL::ElementId eid) {
    coop::net::PropActRequestPayload p{};
    p.id = id;
    p.family = static_cast<uint8_t>(family);
    p.eid = static_cast<uint32_t>(eid);
    return s->SendReliableToSlot(0, Kind::PropActRequest, &p, sizeof(p));
}

void LogVerdict(Arm a, const char* what, const std::string& why) {
    if (std::string_view(what) == "PASS") UE_LOGI("[HOSTILE-DRILL] %s PASS -- %s", ArmName(a), why.c_str());
    else UE_LOGW("[HOSTILE-DRILL] %s %s -- %s", ArmName(a), what, why.c_str());
}

const char* SpawnOnFloor(coop::RemotePlayer& rp, const wchar_t* path, bool far, int firstDir, int* dirUsed, void** out) {
    void* const cls = ue_wrap::asset_load::LoadObjectByPath(path);
    if (!cls) return "the target's class did not load";
    return SpawnClassOnFloor(rp, cls, far ? kFarCm : kNearCm, far, firstDir, dirUsed, out);
}

const char* SpawnClassOnFloor(coop::RemotePlayer& rp, void* cls, float outCm, bool far, int firstDir, int* dirUsed,
                              void** out) {
    FVector at{};
    if (!E::TryGetActorLocation(rp.GetActor(), at)) return "the puppet's place could not be read";
    for (int i = firstDir; i < kFan; ++i) {
        const float yaw = 6.2831853f * static_cast<float>(i) / static_cast<float>(kFan);
        const float x = at.X + std::cos(yaw) * outCm;
        const float y = at.Y + std::sin(yaw) * outCm;
        const DP::Drop d = DP::DropOnFloor(rp.GetActor(), cls, nullptr, x, y, at.Z + (far ? kFarUpCm : kNearUpCm),
                                           far ? kFarDownCm : kNearDownCm, kMinColumnCm, kDropCm, out);
        if (d == DP::Drop::NoFloor) continue;
        if (d == DP::Drop::SpawnFailed) return "the target did not spawn";
        if (dirUsed) *dirUsed = i;
        return nullptr;
    }
    return "no floor was found in any direction";
}

}  // namespace detail

const char* ArmName(Arm a) {
    switch (a) {
    case Arm::ActFar:         return "act_far";
    case Arm::ActGone:        return "act_gone";
    case Arm::ActWrongFamily: return "act_wrongfamily";
    case Arm::ActBadCommit:   return "act_bad_commit";
    case Arm::ActNotYet:      return "act_notyet";
    case Arm::ActFlood:       return "act_flood";
    case Arm::ActFloodEarly:  return "act_flood_early";
    default:                  return "off";
    }
}

void Install(coop::net::Session* s, Arm a) {
    if (!Unready(a) || g_holdSet || !s || s->role() != coop::net::Role::Client) return;
    g_holdSet = true;
    coop::dev::announce_hold::Set(true);
    UE_LOGI("[HOSTILE-DRILL] %s holds this client's announce until its requests are sent", ArmName(a));
}

void TickClient(coop::net::Session* s, Arm a) {
    if (a == Arm::ActWrongFamily) {
        TickFamilyClient(s);
        return;
    }
    if (g_cstep == CStep::Done) return;
    if (Unready(a)) ClientEarly(s, a);
    else ClientTarget(s, a);
}

void TickHostJudge(coop::net::Session* s, Arm a) {
    if (a == Arm::ActWrongFamily) {
        TickFamilyHost(s);
        return;
    }
    if (g_hstep == HStep::Done) return;
    if (g_hstep == HStep::WaitArm) {
        if (Unready(a)) ArmUnready(s, a);
        else ArmTarget(s, a);
        return;
    }
    if (!s->HasPeerConn(g_slot)) {
        Finish(a, "INCONCLUSIVE", "the judged slot left before the verdict");
        return;
    }
    // The per-slot counters reset when the slot's occupant changes, so a replaced slot is not judged on them.
    if (coop::roster_ledger::Get(g_slot).playerNo != g_playerNo) {
        Finish(a, "INCONCLUSIVE", "slot-replaced (the judged slot's occupant changed)");
        return;
    }
    if (g_hstep == HStep::Prep) {
        PrepTarget(a);
        return;
    }
    switch (a) {
    case Arm::ActBadCommit:   JudgeBadCommit(a); break;
    case Arm::ActFlood:       JudgeFlood(a); break;
    case Arm::ActNotYet:
    case Arm::ActFloodEarly:  JudgeUnready(s, a); break;
    default:                  JudgeOne(a); break;
    }
}

void OnDisconnect() {
    Reset();
    ResetFamily();
    g_holdSet = false;
    coop::dev::announce_hold::Set(false);
}

}  // namespace coop::dev::hostile_drill::act
