// coop/dev/prop_syncer_drill.cpp -- see coop/dev/prop_syncer_drill.h. Three roles, each advancing on an edge another
// peer printed in the lobby chat or on a counter of its own, and ending on its own DONE PASS or FAIL: line.
//   host: prop_syncer_drill_host.cpp. client 1: says its ready line, then on the host's "go" walks to the nearest keyed
//   prop, claims, kicks, claims for kKickHoldMs, then says "kicked" and "rested <eid> <cm> <p y r x y z>".
//   client 2: the observer (prop_syncer_drill_observer.cpp).

#include "coop/dev/prop_syncer_drill.h"

#include "prop_syncer_drill_detail.h"  // co-located private header (src tree, not include/)
#include "hand_drop_census.h"          // the census the target is picked from

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/prop_syncer.h"
#include "coop/props/remote_prop.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_physics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace coop::dev::prop_syncer_drill {

namespace detail {

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(coop::config_registry::rows::prop_syncer_drill);
        return v == "kick" ? Arm::Kick : v == "forge" ? Arm::Forge : Arm::Off;
    }();
    return a;
}

uint64_t Now() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string Fmt(const char* f, ...) {
    char b[256];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof(b), f, ap);
    va_end(ap);
    return b;
}

void Pass(const char* role, bool& done, const std::string& numbers) {
    UE_LOGI("[PROP-SYNC] %s DONE PASS arm=%s %s", role, ArmOf() == Arm::Forge ? "forge" : "kick", numbers.c_str());
    done = true;
}

void Fail(const char* role, bool& done, const std::string& why) {
    UE_LOGW("[PROP-SYNC] %s FAIL: %s", role, why.c_str());
    done = true;
}

// `n` numbers separated by spaces; false when fewer are there.
bool ParseNums(const std::string& s, double* out, int n) {
    const char* p = s.c_str();
    char* end = nullptr;
    for (int i = 0; i < n; ++i, p = end) {
        out[i] = std::strtod(p, &end);
        if (end == p) return false;
    }
    return true;
}

bool ReadRested(Rested* r) {
    std::string rest;
    double n[8] = {};
    if (!coop::dev::drill_chat::LiveRestOf(kRestedPrefix, &rest) || !ParseNums(rest, n, 8)) return false;
    r->eid = static_cast<uint32_t>(n[0]);
    r->cm = static_cast<float>(n[1]);
    r->rot = ue_wrap::FRotator{static_cast<float>(n[2]), static_cast<float>(n[3]), static_cast<float>(n[4])};
    r->loc = ue_wrap::FVector{static_cast<float>(n[5]), static_cast<float>(n[6]), static_cast<float>(n[7])};
    return true;
}

bool ReadEid(const char* prefix, uint32_t* eid) {
    std::string rest;
    double n = 0;
    if (!coop::dev::drill_chat::LiveRestOf(prefix, &rest) || !ParseNums(rest, &n, 1)) return false;
    *eid = static_cast<uint32_t>(n);
    return true;
}

Verdict JudgeCopy(const Rested& r, const coop::prop_drive_stream::dev::DriveView& v, bool needTravel) {
    using ue_wrap::FRotator;
    using ue_wrap::FVector;
    const auto dist = [](const FVector& a, const FVector& b) {
        return std::sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));
    };
    const auto deg = [](float a, float b) { return std::fabs(ue_wrap::NormalizeAxis(a - b)); };
    Verdict out;
    void* actor = coop::remote_prop::ResolveLiveActorByEid(r.eid);
    FVector at{};
    FRotator rot{};
    if (!actor || !ue_wrap::engine::TryGetActorLocation(actor, at) || !ue_wrap::engine::TryGetActorRotation(actor, rot))
        return Verdict{"the copy does not read here"};
    if (needTravel) {   // at the last row applied before the End: the End's pose would satisfy any distance alone
        if (!v.startRead) return Verdict{"the copy's start place was never read"};
        out.travelled = dist(v.startLoc, v.lastRowLoc);
        if (out.travelled < kTravelFrac * r.cm) {
            out.why = Fmt("the stream took the copy %.1f cm of the client's %.1f", out.travelled, r.cm);
            return out;
        }
    }
    out.offCm = dist(at, r.loc);
    const float angle = (std::max)({deg(rot.Pitch, r.rot.Pitch), deg(rot.Yaw, r.rot.Yaw), deg(rot.Roll, r.rot.Roll)});
    if (out.offCm > kRestCm) out.why = Fmt("copy rests %.1f cm from the client's pose (want <= %.0f)", out.offCm, kRestCm);
    else if (angle > kRestDeg) out.why = Fmt("copy rests %.1f deg from the client's pose (want <= %.0f)", angle, kRestDeg);
    return out;
}

bool StreamsRun() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle &&
           coop::net_pump::IsWorldSettled() && coop::join_membership_sweep::HasLoadTailQuiesced();
}

long LoadKb() {
    static const long kb = coop::config::ResolveInt(coop::config_registry::rows::prop_syncer_load_kb);
    return kb;
}

}  // namespace detail

using namespace detail;

namespace {
namespace DC  = coop::dev::drill_chat;
namespace PS  = coop::prop_syncer;
namespace PR  = ue_wrap::prop;
namespace E   = ue_wrap::engine;
using ue_wrap::FRotator;
using ue_wrap::FVector;

constexpr uint64_t kKickHoldMs = 1000;        // the drill's own interaction length: Claim every tick this long
constexpr float    kPickRadiusCm = 6000.f;    // the census around the client for its target
constexpr float    kReachCm = 150.f;          // the walk ends this close to the prop
constexpr int      kWalkDeadlineS = 120, kMaxTargets = 4;
constexpr float    kKickSpeedCmS = 500.f, kKickUpCmS = 150.f;
constexpr float    kMinTravelCm = 50.f;       // a kick that moved the prop less proves nothing
constexpr uint64_t kGrantBoundMs = 15000, kRestBoundMs = 30000;   // client: the claim to its answer; the hold to the rest End
const char* const kRefusalName[8] = {"None", "NotReady", "Unknown", "InHand", "HostAuthored", "OtherSyncer",
                                     "OutOfReach", "Paced"};

// ---- the acting client ----
enum class CPh : uint8_t { Go, Walk, Claim, Kick };

struct Target {
    ue_wrap::CachedObjRef actor;
    FVector               at{};
    uint32_t              eid = 0;
};

struct ClientState {
    bool                                            done = false, readySaid = false, goSeen = false;
    CPh                                             phase = CPh::Go;
    uint64_t                                        at = 0, phaseAt = 0, kickAt = 0;
    std::vector<Target>                             targets;
    size_t                                          next = 0;
    std::shared_ptr<coop::director::BackgroundWalk> walk;
    FVector                                         start{};
    uint32_t                                        eid = 0, maxRows = 0;
} g_c;

// The host's DevLoadFiller as this client sees it: how many messages arrived and when the first and the last did.
struct FillerState {
    uint32_t count = 0, total = 0, atGrant = 0;   // atGrant: the count when this client first saw its grant
    uint64_t firstAt = 0, lastAt = 0;
} g_f;

void WalkCancel() {
    if (g_c.walk && g_c.walk->state.load() == 0) coop::director::CancelBackgroundWalk(*g_c.walk);
    g_c.walk.reset();
}

void ClientFail(const std::string& why) {
    Fail("client", g_c.done, why);
    WalkCancel();
}

// The keyed props around the player, nearest first: each with a host element id, a prop_drive class, in no hand,
// not trash, and a body that simulates (a kick moves nothing else).
bool PickTargets(void* player) {
    FVector me{};
    if (!E::TryGetActorLocation(player, me)) return false;
    std::vector<coop::dev::hand_drop_census::PropRow> unread;
    auto rows = coop::dev::hand_drop_census::Census(me, kPickRadiusCm, unread);
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.distCm < b.distCm; });
    for (const auto& r : rows) {
        if (g_c.targets.size() >= static_cast<size_t>(kMaxTargets)) break;
        const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(r.actor);
        void* mesh = PR::GetStaticMesh(r.actor);
        FVector at{};
        if (!coop::element::Registry::IsAllowedHostAllocatedEid(eid) || PR::IsTrashActor(r.actor) ||
            coop::prop_lifecycle::IsWireSuppressedPropClass(r.cls) || coop::prop_drive_host::InAHand(r.actor) ||
            PR::IsFrozen(r.actor) || PR::IsStatic(r.actor) || !mesh || !E::IsComponentSimulatingPhysics(mesh) ||
            !E::TryGetActorLocation(r.actor, at))
            continue;
        g_c.targets.emplace_back();
        g_c.targets.back().actor.Set(r.actor);
        g_c.targets.back().at = at;
        g_c.targets.back().eid = static_cast<uint32_t>(eid);
    }
    return !g_c.targets.empty();
}

void StartWalk() {
    const Target& t = g_c.targets[g_c.next];
    g_c.eid = t.eid;
    g_c.walk = coop::director::StartBackgroundWalk(t.at, kReachCm, kWalkDeadlineS);
    g_c.phase = CPh::Walk;
}

// The game's own physics on the prop's body: its velocity set along the line from the player, a little upward.
bool Kick(void* player, void* prop) {
    FVector from{}, to{};
    void* mesh = PR::GetStaticMesh(prop);
    if (!mesh || !E::TryGetActorLocation(player, from) || !E::TryGetActorLocation(prop, to)) return false;
    float dx = to.X - from.X, dy = to.Y - from.Y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.f) { dx = 1.f; dy = 0.f; } else { dx /= len; dy /= len; }
    return E::SetComponentLinearVelocity(mesh, dx * kKickSpeedCmS, dy * kKickSpeedCmS, kKickUpCmS);
}

void ClientGo(void* player) {
    if (!g_c.readySaid && StreamsRun()) {   // the host's go waits on this line and the observer's
        DC::Say(kClientReadyLine);
        g_c.readySaid = true;
    }
    if (g_c.readySaid && !g_c.goSeen && DC::PollDue()) g_c.goSeen = DC::ChatHasLive(kGoLine);   // latched: the row retires
    if (g_c.goSeen && StreamsRun()) {
        if (!PickTargets(player)) return ClientFail("no keyed prop with a host element id, simulating, near the player");
        return StartWalk();
    }
    if (Now() - g_c.at > kReadyBoundMs) ClientFail("the host's go line never came");
}

void ClientWalk() {
    const int state = g_c.walk->state.load();
    if (state == 0) return;
    g_c.walk.reset();
    if (state == 2)   // a target the walk did not reach: the next nearest
        return ++g_c.next >= g_c.targets.size() ? ClientFail("no candidate prop was reachable by the director") : StartWalk();
    g_c.phase = CPh::Claim;
    g_c.phaseAt = Now();
}

// A refusal of any kind ends the drill at the claim; Paced is the refuse red's.
bool ClaimRefused() {
    for (uint8_t why = 1; why < 8; ++why)
        if (PS::dev::RefusedBy(why) > 0) { ClientFail(Fmt("claim refused (%s)", kRefusalName[why])); return true; }
    return false;
}

void ClientClaim(void* player, void* prop) {
    PS::Claim(prop, coop::net::PropSyncReason::DevDrill);
    if (ClaimRefused()) return;
    if (PS::dev::EntryOf(g_c.eid).state == PS::dev::EntryState::Syncing) {
        g_f.atGrant = g_f.count;   // near 0 on the Transfer lane (the answer overtakes the filler), all of it on the Default lane (FIFO ahead of the answer)
        if (!E::TryGetActorLocation(prop, g_c.start)) return ClientFail("the target does not read");
        DC::Say(std::string(kKickedPrefix) + std::to_string(g_c.eid));
        if (!Kick(player, prop)) return ClientFail("the kick did not run");
        g_c.kickAt = Now();
        g_c.phase = CPh::Kick;
        return;
    }
    if (Now() - g_c.phaseAt > kGrantBoundMs) ClientFail("the host never answered the claim");
}

void ClientKick(void* prop) {
    const uint64_t now = Now();
    const PS::dev::EntryView e = PS::dev::EntryOf(g_c.eid);
    g_c.maxRows = (std::max)(g_c.maxRows, e.rowsSent);
    if (e.state == PS::dev::EntryState::None) {
        if (PS::dev::CountOf(PS::dev::Count::RestEnds) < 1)
            return ClientFail(Fmt("the claim ended without a rest End (revoked %u, host ends %u)",
                                  PS::dev::CountOf(PS::dev::Count::Revoked), PS::dev::CountOf(PS::dev::Count::HostEnds)));
        FVector at{};
        FRotator rot{};
        if (!E::TryGetActorLocation(prop, at) || !E::TryGetActorRotation(prop, rot)) return ClientFail("the copy does not read at rest");
        const float cm = std::sqrt((at.X - g_c.start.X) * (at.X - g_c.start.X) + (at.Y - g_c.start.Y) * (at.Y - g_c.start.Y) +
                                   (at.Z - g_c.start.Z) * (at.Z - g_c.start.Z));
        if (cm < kMinTravelCm) return ClientFail(Fmt("the kick moved the prop only %.1f cm (want >= %.0f)", cm, kMinTravelCm));
        DC::Say(Fmt("%s%u %.1f %.1f %.1f %.1f %.1f %.1f %.1f", kRestedPrefix, g_c.eid, cm, ue_wrap::NormalizeAxis(rot.Pitch),
                    ue_wrap::NormalizeAxis(rot.Yaw), ue_wrap::NormalizeAxis(rot.Roll), at.X, at.Y, at.Z));
        std::string load;
        if (LoadKb() > 0)
            load = Fmt(" grant_ms=%llu load_msgs_at_grant=%u load_total=%u load_ms=%llu",
                       static_cast<unsigned long long>(PS::dev::LastGrantMs(g_c.eid)), g_f.atGrant, g_f.total,
                       static_cast<unsigned long long>(g_f.count == g_f.total ? g_f.lastAt - g_f.firstAt : 0));
        return Pass("client", g_c.done, Fmt("eid=%u gen=%u rows_sent=%u travelled=%.1f cm", g_c.eid,
                    static_cast<unsigned>(PS::dev::LastGrantedGen(g_c.eid)), g_c.maxRows, cm) + load);
    }
    if (now - g_c.kickAt < kKickHoldMs) PS::Claim(prop, coop::net::PropSyncReason::DevDrill);
    else if (now - g_c.kickAt > kKickHoldMs + kRestBoundMs) ClientFail("the copy never rested");
}

void ClientTick() {
    ClientState& c = g_c;
    if (c.done) return;
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    void* player = coop::players::Registry::Get().Local();
    if (!coop::net_pump::HasAnnouncedWorldReady() || !player || self != kActorSlot) return;   // another slot idles
    if (c.at == 0) c.at = Now();
    if (c.phase == CPh::Go) return ClientGo(player);
    if (c.phase == CPh::Walk) return ClientWalk();
    void* prop = c.targets[c.next].actor.Get();
    if (!prop) return ClientFail("the target is gone");
    if (c.phase == CPh::Claim) ClientClaim(player, prop);
    else ClientKick(prop);
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void OnFiller(uint32_t seq, uint32_t total) {
    if (ArmOf() == Arm::Off) return;
    FillerState& f = g_f;
    const uint64_t now = Now();
    if (f.count == 0) {
        f.firstAt = now;
        f.total = total;
        UE_LOGI("[PROP-SYNC] client load first filler seq=%u total=%u", seq, total);
    }
    if (++f.count == f.total) {
        f.lastAt = now;
        UE_LOGI("[PROP-SYNC] client load done: %u msgs in %llu ms", f.count,
                static_cast<unsigned long long>(f.lastAt - f.firstAt));
    }
}

void Tick(coop::net::Session* s) {
    const Arm a = ArmOf();
    if (a == Arm::Off || !s) return;
    if (s->role() == coop::net::Role::Host) return HostTick(a);
    if (!s->connected()) return;
    if (coop::players::Registry::Get().LocalPeerId() == kObserverSlot && coop::net_pump::HasAnnouncedWorldReady())
        ObserverTick(s, a);
    else
        ClientTick();
}

void OnDisconnect() {
    WalkCancel();
    HostReset();
    g_c = ClientState{};
    g_f = FillerState{};
    ObserverReset();
}

}  // namespace coop::dev::prop_syncer_drill
