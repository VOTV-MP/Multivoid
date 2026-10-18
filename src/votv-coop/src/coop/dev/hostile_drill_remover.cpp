// coop/dev/hostile_drill_remover.cpp -- see coop/dev/hostile_drill_remover.h.

#include "coop/dev/hostile_drill_remover.h"

#include "coop/creatures/kerfur_entity.h"
#include "coop/comms/chat_feed.h"
#include "coop/comms/chat_sync.h"
#include "coop/dispatch/event_feed.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/remover_request.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::hostile_drill::remover {
namespace {

using Clock  = std::chrono::steady_clock;
using Kind   = coop::net::ReliableKind;
using Reason = coop::net::RemoverReason;
using ue_wrap::FVector;
namespace EL = coop::element;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace RR = coop::remover_request;

// The flood: far over the lane's cap and over what its pace pays in the span of a burst, so a lane that bounds
// nothing runs them all.
constexpr uint32_t kFlood = 121;

// The judge's numbers, literals written from the lane's budget (no symbol of the change itself): the pace's burst
// and per-second refill, and the cap of requests the lane queues per sender.
constexpr uint32_t kCap = 8;
constexpr float    kBurst = 4.f;
constexpr float    kPerSecond = 2.f;
constexpr auto     kFailBound = std::chrono::seconds(180);  // a failure bound, never a wait

constexpr float kFarCm       = 2000.f;  // far's origin from the camera: past the lane's 600 uu staleness bound
constexpr float kSeenMaxCm   = 2500.f;  // hidden: only props this near the camera are tried
constexpr float kSightSlack  = 25.f;
constexpr float kBlockedMarginCm = 100.f;  // hidden: the blocker stands this far short of the lane's own slack
constexpr size_t kReasons = static_cast<size_t>(Reason::Count);

// ---- client (sender) ----
bool g_sent = false;
Clock::time_point g_waitSince{};   // the first tick far or hidden waited for the host's ready line

bool OwnJoinOver() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct Candidate { void* actor; EL::ElementId id; FVector at; float cm; };

// The host's plain props this client holds, nearest `from` first: a Prop element in the host's range whose actor is
// live and placed, and not a trash pile or clump or a kerfur (those the lane answers Unsupported).
std::vector<Candidate> HostProps(const FVector& from) {
    std::vector<EL::Registry::ActorIdPair> rows;
    EL::Registry::Get().SnapshotActorsByType(EL::ElementType::Prop, rows);
    std::vector<Candidate> out;
    for (const auto& r : rows) {
        FVector at{};
        if (!EL::Registry::IsAllowedHostAllocatedEid(r.id) || !r.actor || !R::IsLiveByIndex(r.actor, r.internalIdx) ||
            !E::TryGetActorLocation(r.actor, at))
            continue;
        if (ue_wrap::prop::IsTrashActor(r.actor) || ue_wrap::prop::IsTrashBitsPile(r.actor) ||
            coop::kerfur_entity::IsKerfurPropClass(R::ClassOf(r.actor)))
            continue;
        out.push_back({r.actor, r.id, at, Dist(at, from)});
    }
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.cm < b.cm; });
    return out;
}

bool Send(coop::net::Session* s, EL::ElementId id, const FVector& origin, const FVector& hit) {
    coop::net::RemoverRequestPayload p{};
    p.elementId = static_cast<uint32_t>(id);
    p.origin[0] = origin.X; p.origin[1] = origin.Y; p.origin[2] = origin.Z;
    p.hit[0] = hit.X;       p.hit[1] = hit.Y;       p.hit[2] = hit.Z;
    return s->SendReliableToSlot(0, Kind::RemoverRequest, &p, sizeof(p));
}

void Inconclusive(Arm a, const char* why) { UE_LOGW("[HOSTILE-DRILL] %s INCONCLUSIVE: %s", ArmName(a), why); }

// The readiness signal far and hidden wait for: a line the host says in the lobby chat once it holds a posed puppet for
// the sender, so a request is only sent when the host can place the sender's eye. No save can hold it, as a prop class
// or a position could.
std::string ReadyLine(Arm a) { return std::string("hostile-drill ") + ArmName(a) + " host-ready"; }

bool HostSaidReady(Arm a) {
    const std::string line = ReadyLine(a);
    bool found = false;
    coop::chat_feed::ForEachRow([&](const coop::chat_feed::RowView& r) {
        if (r.line.find(line) != std::string_view::npos) found = true;
    });
    return found;
}

// far: the nearest plain host prop, asked for from a point 2000 uu off the camera.
void SendFar(coop::net::Session* s, Arm a, const FVector& camera) {
    const std::vector<Candidate> props = HostProps(camera);
    if (props.empty()) { Inconclusive(a, "this client holds no plain host prop"); return; }
    const FVector far{camera.X + kFarCm, camera.Y, camera.Z};
    if (!Send(s, props[0].id, far, props[0].at)) { Inconclusive(a, "the request was not sent"); return; }
    UE_LOGI("[HOSTILE-DRILL] %s sent one request for element %u from %.0f uu off the camera", ArmName(a),
            static_cast<unsigned>(props[0].id), kFarCm);
}

// hidden: the first prop the camera cannot see, asked for from the real camera. Traced as the host traces: the shot's
// four object types, from the camera, ignoring every player body and the prop, and the blocker well short of the prop.
// The host's verdict checks that what stopped the segment is a wall and not a player body.
void SendHidden(coop::net::Session* s, Arm a, void* player, const FVector& camera) {
    const uint8_t types[4] = {0, 1, 2, 3};
    void* ignore[coop::players::kMaxPeers + 1];
    const int nBodies = coop::players::Registry::Get().LiveBodies(ignore, coop::players::kMaxPeers);
    for (const Candidate& c : HostProps(camera)) {
        if (c.cm > kSeenMaxCm) break;
        ignore[nBodies] = c.actor;
        ue_wrap::trace::Hit blocker;
        if (!ue_wrap::trace::LineHitForObjects(player, camera, c.at, types, 4, ignore, nBodies + 1, &blocker)) continue;
        if (!blocker.blocked || Dist(camera, blocker.point) >= c.cm - kSightSlack - kBlockedMarginCm) continue;
        if (!Send(s, c.id, camera, c.at)) { Inconclusive(a, "the request was not sent"); return; }
        UE_LOGI("[HOSTILE-DRILL] %s sent one request for element %u, %.0f uu away behind a blocker %.0f uu away",
                ArmName(a), static_cast<unsigned>(c.id), c.cm, Dist(camera, blocker.point));
        return;
    }
    Inconclusive(a, "this client has no host prop within 2500 uu that its camera cannot see");
}

void SendFlood(coop::net::Session* s, Arm a) {
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    if (self == 0 || self >= coop::players::kMaxPeers) { g_sent = false; return; }  // no slot yet; wait
    // kFlood distinct ids from the top of the host's range, none held on this client.
    EL::ElementId ids[kFlood];
    uint32_t have = 0;
    for (EL::ElementId id = EL::kHostRangeSize - 1; have < kFlood && EL::Registry::IsAllowedHostAllocatedEid(id); --id)
        if (!EL::Registry::Get().Get(id)) ids[have++] = id;
    if (have < kFlood) { Inconclusive(a, "too few unheld host-range ids"); return; }
    const FVector none{};
    for (uint32_t n = 0; n < kFlood; ++n) {
        if (!Send(s, ids[n], none, none)) { Inconclusive(a, "a request of the flood was not sent"); return; }
    }
    UE_LOGI("[HOSTILE-DRILL] %s sent %u RemoverRequest for slot %u (ids %u down to %u)", ArmName(a), kFlood,
            static_cast<unsigned>(self), static_cast<unsigned>(ids[0]), static_cast<unsigned>(ids[kFlood - 1]));
}

// ---- host (judge) ----
enum class Step { WaitArm, Wait, Done };
Step g_step = Step::WaitArm;
int  g_slot = -1;
uint16_t g_playerNo = 0;  // the judged slot's occupant at arm: the counters reset when it changes
bool g_readySaid = false;
Clock::time_point g_armAt{};
uint32_t g_answeredBase[kReasons] = {};
uint32_t g_runsBase = 0, g_refusedBase = 0, g_drainedBase = 0;
// The flood's reads.
uint32_t g_runs = 0, g_refused = 0;
Clock::time_point g_t0{}, g_tLast{};
std::string g_paceFail, g_capFail;

double SecondsBetween(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

void Finish(Arm a, const char* what, const std::string& why) {
    if (std::string(what) == "PASS") UE_LOGI("[HOSTILE-DRILL] %s PASS -- %s", ArmName(a), why.c_str());
    else UE_LOGW("[HOSTILE-DRILL] %s %s -- %s", ArmName(a), what, why.c_str());
    g_step = Step::Done;
}

void ArmHost(coop::net::Session* s, Arm a) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && g_slot < 0; ++slot)
        if (s->IsSlotWorldReady(slot)) g_slot = slot;
    if (g_slot < 0) return;
    // far and hidden judge a request that needs the host's eye for the sender, which exists once the sender's puppet has
    // had its first pose: the host says so in the lobby chat then, and the sender sends only after it reads that line. The
    // flood is judged at the lane's cost path and needs no body.
    if (a != Arm::RemoverFlood && !g_readySaid) {
        if (!coop::prop_snapshot::IsBracketClosed(g_slot) || coop::join_window_baseline::IsLateWindowOpen(g_slot)) return;
        coop::RemotePlayer& rp = coop::puppet_drive::Puppet(g_slot);
        if (!rp.valid() || !rp.HasPose()) return;
        coop::chat_sync::QueueSend(ReadyLine(a));
        g_readySaid = true;
        return;   // the baseline is taken next tick, after the line is out
    }
    for (size_t i = 0; i < kReasons; ++i) g_answeredBase[i] = RR::AnsweredFor(g_slot, static_cast<Reason>(i));
    g_runsBase = RR::RunsFor(g_slot);
    g_refusedBase = RR::RefusedFor(g_slot);
    g_drainedBase = coop::event_feed::DrainedFrom(g_slot, Kind::RemoverRequest);
    g_runs = g_refused = 0;
    g_t0 = g_tLast = {};
    g_paceFail.clear();
    g_capFail.clear();
    g_playerNo = coop::roster_ledger::Get(g_slot).playerNo;
    g_armAt = Clock::now();
    g_step = Step::Wait;
    UE_LOGI("[HOSTILE-DRILL] %s armed on slot %d (the host holds a posed puppet for it)", ArmName(a), g_slot);
}

// far and hidden: one request, one answer, and it is OutOfSight.
void JudgeOne(Arm a) {
    uint32_t now[kReasons];
    uint32_t total = 0;
    std::string seen;
    for (size_t i = 0; i < kReasons; ++i) {
        now[i] = RR::AnsweredFor(g_slot, static_cast<Reason>(i)) - g_answeredBase[i];
        total += now[i];
        if (now[i]) seen += " reason" + std::to_string(i) + "=" + std::to_string(now[i]);
    }
    if (total == 0) {
        if (Clock::now() - g_armAt > kFailBound)
            Finish(a, "INCONCLUSIVE", "no request was answered (the client found nothing to ask for, or the lane never served)");
        return;
    }
    // OutOfSight once and nothing else, by the branch the arm exists to reach: far by the origin bound, so a blocked
    // segment cannot pass it; hidden by the blocked segment.
    const char* want = a == Arm::RemoverFar ? "origin far from the eye" : "segment blocked before the hit point";
    const char* got = RR::LastWhyFor(g_slot);
    // hidden must be stopped by geometry: the blocker is a class, and not a player body (mainPlayer_C).
    const std::wstring blockerCls = RR::LastBlockerClass();
    const bool wall = a != Arm::RemoverHidden || (!blockerCls.empty() && blockerCls != L"(no actor)" && blockerCls != L"mainPlayer_C");
    if (now[static_cast<size_t>(Reason::OutOfSight)] == 1 && total == 1 && got && std::string(got) == want && wall)
        Finish(a, "PASS", std::string("the host answered OutOfSight once and answered nothing else: ") + want);
    else
        Finish(a, "FAIL", "the host answered" + seen + ", last refusal '" + (got ? got : "none") + "' (wanted OutOfSight once, '" + want + "')" + (wall ? "" : "; the blocker was a player body or had no class, not a wall"));
}

void SampleFlood(Clock::time_point now) {
    const uint32_t prev = g_runs;
    g_runs = RR::RunsFor(g_slot) - g_runsBase;
    g_refused = RR::RefusedFor(g_slot) - g_refusedBase;
    // What the lane holds unserved and unrefused, at every read: handed over, neither run nor refused.
    const int64_t depth = static_cast<int64_t>(coop::event_feed::DrainedFrom(g_slot, Kind::RemoverRequest) -
                                               g_drainedBase) - g_runs - g_refused;
    if (depth > static_cast<int64_t>(kCap) && g_capFail.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "cap (depth %lld > cap %u)", static_cast<long long>(depth), kCap);
        g_capFail = buf;
    }
    if (g_runs > prev) {
        if (prev == 0) g_t0 = now;
        g_tLast = now;
    }
    if (g_runs == 0 || !g_paceFail.empty()) return;
    // From the first run at t0: the burst, what the refill paid since, and one for a refill inside the frame the read
    // did not see.
    const double most = kBurst + kPerSecond * SecondsBetween(g_t0, now) + 1.0;
    if (g_runs > most) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "pace (%u runs %.0f ms after the first, at most %.1f)", g_runs,
                      SecondsBetween(g_t0, now) * 1000.0, most);
        g_paceFail = buf;
    }
}

void JudgeFlood(Arm a) {
    const Clock::time_point now = Clock::now();
    SampleFlood(now);
    char sum[120];
    std::snprintf(sum, sizeof(sum), "runs %u refused %u span %.0f ms", g_runs, g_refused,
                  g_runs ? SecondsBetween(g_t0, g_tLast) * 1000.0 : 0.0);
    if (!g_capFail.empty()) { Finish(a, "FAIL", g_capFail + "; " + sum); return; }
    if (g_runs + g_refused != kFlood) {
        if (now - g_armAt <= kFailBound) return;
        if (g_runs == 0) Finish(a, "INCONCLUSIVE", std::string("never-ran (the lane never became ready); ") + sum);
        else Finish(a, "FAIL", std::string("timeout (not all ") + std::to_string(kFlood) + " accounted for); " + sum);
        return;
    }
    if (g_runs == 0) { Finish(a, "INCONCLUSIVE", std::string("never-ran (refused before it could run); ") + sum); return; }
    if (g_runs < kCap) {
        Finish(a, "FAIL", std::string("under-cap (") + std::to_string(g_runs) + " < " + std::to_string(kCap) + "); " + sum);
        return;
    }
    if (!g_paceFail.empty()) { Finish(a, "FAIL", g_paceFail + "; " + sum); return; }
    Finish(a, "PASS", sum);
}

}  // namespace

const char* ArmName(Arm a) {
    return a == Arm::RemoverFar ? "remover_far" : a == Arm::RemoverHidden ? "remover_hidden"
         : a == Arm::RemoverFlood ? "remover_flood" : "off";
}

void TickClient(coop::net::Session* s, Arm a) {
    if (g_sent || !OwnJoinOver()) return;
    if (a == Arm::RemoverFlood) { g_sent = true; SendFlood(s, a); return; }
    void* player = coop::players::Registry::Get().Local();
    FVector camera{};
    if (!player || !E::ReadMainPlayerCameraLocation(player, camera)) return;  // not up yet; wait
    // far and hidden wait for the host's ready line: it says it only once it holds a posed puppet for this client.
    static int sTicks = 0;
    if (g_waitSince != Clock::time_point{} && ++sTicks % 15 != 0) return;   // a feed walk: a few times a second
    if (!HostSaidReady(a)) {
        if (g_waitSince == Clock::time_point{}) g_waitSince = Clock::now();
        if (Clock::now() - g_waitSince > kFailBound) {
            g_sent = true;
            Inconclusive(a, "the host never said it was ready (it has no posed puppet for this client)");
        }
        return;
    }
    g_sent = true;
    if (a == Arm::RemoverFar) SendFar(s, a, camera);
    else SendHidden(s, a, player, camera);
}

void TickHostJudge(coop::net::Session* s, Arm a) {
    if (g_step == Step::WaitArm) { ArmHost(s, a); return; }
    if (g_step == Step::Done) return;
    if (!s->HasPeerConn(g_slot)) {
        Finish(a, "INCONCLUSIVE", "the judged slot left before the verdict");
        return;
    }
    // The per-slot counters reset when the slot's occupant changes, so a replaced slot is not judged on them.
    if (coop::roster_ledger::Get(g_slot).playerNo != g_playerNo) {
        Finish(a, "INCONCLUSIVE", "slot-replaced (the judged slot's occupant changed)");
        return;
    }
    if (a == Arm::RemoverFlood) JudgeFlood(a);
    else JudgeOne(a);
}

void OnDisconnect() {
    g_waitSince = {};
    g_readySaid = false;
    g_sent = false;
    g_step = Step::WaitArm;
    g_slot = -1;
    g_playerNo = 0;
    g_armAt = {};
    g_runs = g_refused = 0;
    g_t0 = g_tLast = {};
    g_paceFail.clear();
    g_capFail.clear();
}

}  // namespace coop::dev::hostile_drill::remover
