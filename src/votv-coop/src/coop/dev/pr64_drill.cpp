// coop/dev/pr64_drill.cpp -- see coop/dev/pr64_drill.h. The drill's switch, its chat edges, its verdict lines and the
// coin arm: the host mints a coin through the gun's own sell verb, the client walks to it and presses E as a player
// does (a reflected call on either seam is the drill's own echo and would measure nothing).

#include "coop/dev/pr64_drill.h"
#include "pr64_drill_detail.h"

#include "coffee_drill_detail.h"  // co-located private header: Points

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_props.h"
#include "coop/dev/game_window.h"
#include "coop/element/registry.h"
#include "coop/items/coingun_sync.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::dev::pr64_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace DR = coop::director;
namespace DC = coop::dev::drill_chat;
namespace DP = coop::dev::drill_props;
namespace CD = coop::dev::coffee_drill::detail;
namespace EL = coop::element;
namespace D  = detail;
using ue_wrap::FVector;

bool g_done = false;
std::unordered_set<std::string> g_heard;
std::unordered_map<std::string, std::string> g_rests;

// One process is one role, so one step and one clock serve both: the clock is stamped when a step begins.
enum class Step : uint8_t {
    Idle,
    Pick, Eids, Rest, WaitPress, Watch, WaitFinal,              // the host
    Hear, Mirror, Walk, Aim, Observe, WaitHost                  // the client
};
struct State {
    Step step = Step::Idle;
    uint64_t stepMs = 0;
    uint32_t eid = 0;
    ue_wrap::FVector anchor{};   // the coin's place when the current still window began
    int stillTicks = 0;
    int b0 = -1, pointsSeen = -2, aliveSeen = -1;
    int32_t points0 = -1;
    int32_t coinPts = -1;                // the coin's points as the host minted it
    std::vector<ue_wrap::CachedObjRef> coins;
    ue_wrap::CachedObjRef coin;
    std::shared_ptr<DR::BackgroundWalk> walk;
};
State g_s;

void Go(Step s) {
    g_s.step = s;
    g_s.stepMs = D::NowMs();
}
bool Late(uint64_t boundMs) { return D::Expired(g_s.stepMs, boundMs); }
unsigned long long Since() { return static_cast<unsigned long long>(D::NowMs() - g_s.stepMs); }
void* LiveCoin(uint32_t eid) { return EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::WorldActor); }
int Lives(uint32_t eid) { return LiveCoin(eid) ? 1 : 0; }

// ---- the host: the coin ----

// A floor point on a ring about the host that a route reaches from the host and from the client's body.
void HostPick() {
    void* me = coop::players::Registry::Get().Local();
    void* pup = coop::puppet_drive::Puppet(1).GetActor();
    FVector mePos{}, pupPos{};
    if (!me || !pup || !R::IsLive(pup) || !E::TryGetActorLocation(me, mePos) || !E::TryGetActorLocation(pup, pupPos)) {
        if (Late(D::kPuppetBoundMs)) D::Fail("the client's body never appeared on the host");
        return;
    }
    const std::vector<FVector> ring = DR::ReachableStandpoints(me, mePos, D::kRingCm, D::kRingPoints, D::kStandCm);
    int floors = 0;
    for (const FVector& p : ring) {
        float fz = 0.f, len = 0.f;
        if (!DP::FloorAt(me, p.X, p.Y, p.Z + D::kFromUpCm, D::kDownCm, D::kMinColumnCm, &fz)) continue;
        ++floors;
        const FVector at{p.X, p.Y, fz};
        if (!DR::RouteInLegs(me, pupPos, at, D::kStandCm, &len) || len > D::kRouteMaxCm) continue;
        UE_LOGI("[PR64-DRILL] host fixture point (%.0f,%.0f,%.0f): a route of %.0f cm from the client's body", at.X,
                at.Y, at.Z, static_cast<double>(len));
        if (D::SpawnAndMint(at, g_s.coins)) Go(Step::Eids);
        return;
    }
    D::Fail("no floor point on a ring of %d is reached from the client's body (%zu ring points, %d with a floor)",
            D::kRingPoints, ring.size(), floors);
}

// Every new coin's element id (the host's enrol runs after the mint); the lowest is the one the client is sent to.
void HostEids() {
    size_t have = 0;
    uint32_t lowest = 0;
    for (ue_wrap::CachedObjRef& c : g_s.coins) {
        void* a = c.Get();
        const EL::ElementId e = a ? EL::Registry::Get().EidForActor(a) : EL::kInvalidId;
        if (e == EL::kInvalidId || e == 0u) continue;
        ++have;
        if (!lowest || e < lowest) lowest = static_cast<uint32_t>(e);
    }
    if (have < g_s.coins.size() && !Late(D::kEidBoundMs)) return;
    if (!have) {
        D::Fail("none of the %zu minted coins got an element id in %llu ms", g_s.coins.size(),
                static_cast<unsigned long long>(D::kEidBoundMs));
        return;
    }
    for (ue_wrap::CachedObjRef& c : g_s.coins) {
        void* a = c.Get();
        FVector at{};
        const unsigned e = a ? static_cast<unsigned>(EL::Registry::Get().EidForActor(a)) : 0u;
        if (a) E::TryGetActorLocation(a, at);
        if (a && e == lowest) {
            g_s.coin.Set(a);
            g_s.anchor = at;
            g_s.coinPts = coop::coingun_sync::ReadCoinPoints(a);
        }
        UE_LOGI("[PR64-DRILL] host coin eid=%u points=%d at (%.0f,%.0f,%.0f) mirrored=%d", e,
                a ? static_cast<int>(coop::coingun_sync::ReadCoinPoints(a)) : -1, at.X, at.Y, at.Z,
                a && coop::world_actor_sync::IsMirroredActor(a) ? 1 : 0);
    }
    g_s.eid = lowest;
    g_s.stillTicks = 0;
    Go(Step::Rest);
}

// The sell throws the coin up from a random point in the cup's bounds. It is sent where it lies: still when its place
// has stayed within kRestCm of one anchor for kRestTicks ticks.
void HostRest() {
    FVector at{};
    void* a = g_s.coin.Get();
    if (!a || !E::TryGetActorLocation(a, at)) {
        D::Fail("the coin was gone before it came to rest");
        return;
    }
    const float dx = at.X - g_s.anchor.X, dy = at.Y - g_s.anchor.Y, dz = at.Z - g_s.anchor.Z;
    if (dx * dx + dy * dy + dz * dz >= D::kRestCm * D::kRestCm) {
        g_s.anchor = at;
        g_s.stillTicks = 0;
    } else if (++g_s.stillTicks >= D::kRestTicks) {
        g_s.points0 = CD::Points();
        char line[96];
        std::snprintf(line, sizeof(line), "%s%u %.1f %.1f %.1f", D::kCoinReady, g_s.eid, at.X, at.Y, at.Z);
        D::Say(line);
        UE_LOGI("[PR64-DRILL] host coin-ready eid=%u at rest (%.0f,%.0f,%.0f) after %llu ms, host points=%d", g_s.eid,
                at.X, at.Y, at.Z, Since(), static_cast<int>(g_s.points0));
        Go(Step::WaitPress);
        return;
    }
    if (Late(D::kRestBoundMs)) D::Fail("the coin never came to rest (%llu ms)", Since());
}

void HostWaitPress() {
    std::string rest;
    double n[1];
    if (D::Rest(D::kCoinPressed, &rest) && D::Numbers(rest, 3, n, 1)) {   // "B0 <balance>"
        g_s.b0 = static_cast<int>(n[0]);
        UE_LOGI("[PR64-DRILL] host heard the press: client B0=%d, host points=%d, coin lives=%d", g_s.b0,
                static_cast<int>(CD::Points()), Lives(g_s.eid));
        Go(Step::Watch);
    } else if (Late(D::kPressBoundMs)) {
        D::Fail("the client never said its press");
    }
}

// The host's balance and the coin's life across the forwarded collect, printed as they change.
void HostWatch() {
    const int pts = static_cast<int>(CD::Points()), alive = Lives(g_s.eid);
    if (pts != g_s.pointsSeen || alive != g_s.aliveSeen) {
        g_s.pointsSeen = pts;
        g_s.aliveSeen = alive;
        UE_LOGI("[PR64-DRILL] host +%llums points=%d coin lives=%d", Since(), pts, alive);
    }
    // The collect has settled when the coin is gone and the balance rose by the coin's points: that marker, not a clock,
    // ends the watch. A balance that never gets there fails at the bound, with the numbers it stood at.
    if (alive != 0 || pts != g_s.points0 + g_s.coinPts) {
        if (Late(D::kFinalBoundMs))
            D::Fail("the host's collect never settled in %llu ms: points %d -> %d (a collect of the %d-point coin makes "
                    "%d), coin lives=%d", Since(), static_cast<int>(g_s.points0), pts, static_cast<int>(g_s.coinPts),
                    static_cast<int>(g_s.points0 + g_s.coinPts), alive);
        return;
    }
    char line[96];
    std::snprintf(line, sizeof(line), "%s%d %d %d", D::kCoinHost, static_cast<int>(g_s.points0), pts, alive);
    D::Say(line);
    UE_LOGI("[PR64-DRILL] host after the collect: points %d -> %d, coin lives=%d, settled after %llu ms",
            static_cast<int>(g_s.points0), pts, alive, Since());
    Go(Step::WaitFinal);
}

// The host's verdict once the client has said its own: one collect rose its balance by the coin's points and the coin
// is gone. Read again here, after the client's line (which follows the client's whole observe window), so a second
// credit after the watch's marker still fails it, and a failing host does not tell the client to fail before it reads.
// A red client's FAIL line races this one in the chat, so the rig reads the client's line.
void HostWaitFinal() {
    std::string rest;
    if (D::Rest(D::kCoinFinal, &rest)) {
        UE_LOGI("[PR64-DRILL] host client's final numbers: %s", rest.c_str());
        const int pts = static_cast<int>(CD::Points());
        if (Lives(g_s.eid) != 0)
            D::Fail("the coin still lives on the host after the collect");
        else if (pts != g_s.points0 + g_s.coinPts)
            D::Fail("the host's balance went %d -> %d, one collect of the %d-point coin makes it %d",
                    static_cast<int>(g_s.points0), pts, static_cast<int>(g_s.coinPts),
                    static_cast<int>(g_s.points0 + g_s.coinPts));
        else
            D::Pass();
    } else if (Late(D::kFinalBoundMs)) {
        D::Fail("the client never said its final numbers");
    }
}

void HostTick(coop::net::Session* s) {
    if (!s->AnyWorldReadyPeer()) return;
    if (g_s.step == Step::Idle) Go(Step::Pick);
    if (D::Heard(D::kClientFail)) {
        D::Fail("the client failed");
        return;
    }
    switch (g_s.step) {
    case Step::Pick:      HostPick(); return;
    case Step::Eids:      HostEids(); return;
    case Step::Rest:      HostRest(); return;
    case Step::WaitPress: HostWaitPress(); return;
    case Step::Watch:     HostWatch(); return;
    case Step::WaitFinal: HostWaitFinal(); return;
    default:              return;
    }
}

// ---- the client: walk, aim, press, read ----

void ClientHear() {
    // The count of the client's own credits starts being watched before anything can press (idempotent).
    if (!D::CreditWatchStart()) {
        D::Fail("the script gate refused the watch of lib_C::addPoints");
        return;
    }
    std::string rest;
    double n[4];
    if (D::Rest(D::kCoinReady, &rest) && D::Numbers(rest, 0, n, 4)) {   // "<eid> <x> <y> <z>"
        g_s.eid = static_cast<uint32_t>(n[0]);
        UE_LOGI("[PR64-DRILL] client heard coin-ready eid=%u at (%.0f,%.0f,%.0f); mirror here=%d", g_s.eid, n[1], n[2],
                n[3], Lives(g_s.eid));
        Go(Step::Mirror);
    } else if (Late(D::kHearBoundMs)) {
        D::Fail("the host never said coin-ready");
    }
}

void ClientMirror() {
    void* coin = LiveCoin(g_s.eid);
    void* me = coop::players::Registry::Get().Local();
    FVector at{}, mine{};
    if (!coin) {
        if (Late(D::kMirrorBoundMs)) D::Fail("no mirror for coin eid=%u after %llu ms", g_s.eid, Since());
        return;
    }
    if (!me || !E::TryGetActorLocation(coin, at) || !E::TryGetActorLocation(me, mine)) {
        D::Fail("the coin's or the player's place cannot be read");
        return;
    }
    float len = 0.f;
    const bool routed = DR::RouteInLegs(me, mine, at, D::kStandCm, &len);
    UE_LOGI("[PR64-DRILL] client mirror of eid=%u after %llu ms: points=%d at (%.0f,%.0f,%.0f), mirrored=%d; route %d "
            "of %.0f cm from (%.0f,%.0f,%.0f)", g_s.eid, Since(), static_cast<int>(coop::coingun_sync::ReadCoinPoints(coin)),
            at.X, at.Y, at.Z, coop::world_actor_sync::IsMirroredActor(coin) ? 1 : 0, routed ? 1 : 0,
            static_cast<double>(len), mine.X, mine.Y, mine.Z);
    if (!routed) {
        D::Fail("no nav route from the client to the coin");
        return;
    }
    g_s.coin.Set(coin);
    g_s.walk = DR::StartBackgroundWalk(at, D::kStandCm, D::kWalkDeadlineS);
    Go(Step::Walk);
}

void ClientWalk() {
    const int st = g_s.walk ? g_s.walk->state.load() : 2;
    if (st == 0) return;
    g_s.walk.reset();
    if (st == 1) Go(Step::Aim);
    else D::Fail("the director's walk to the coin failed or ran out");
}

// Aimed each tick until the player's own interaction trace takes the coin, then the key as the OS delivers it.
void ClientAim() {
    // The watch is live before the press, so a credit it never reports was never made.
    const int watch = D::CreditWatchState();
    if (watch < 0) {
        D::Fail("the lib_C::addPoints watch is dead (the gate settled it without a body to watch)");
        return;
    }
    if (watch == 0) {
        if (Late(D::kAimBoundMs)) D::Fail("the lib_C::addPoints watch never went live");
        return;
    }
    void* me = coop::players::Registry::Get().Local();
    void* coin = g_s.coin.Get();
    FVector at{};
    if (!coin || !E::TryGetActorLocation(coin, at)) {
        D::Fail("the coin was gone before the press (balance %d): collected on the walk?", static_cast<int>(CD::Points()));
        return;
    }
    if (!D::Aim(me, at)) {
        D::Fail("the player has no controller to aim with");
        return;
    }
    void* hit = E::ReadMainPlayerHitActor(me);
    if (hit != coin) {
        if (Late(D::kAimBoundMs)) D::Fail("the interaction trace never took the coin (it took %ls)",
                                          hit ? R::ClassNameOf(hit).c_str() : L"nothing");
        return;
    }
    g_s.b0 = static_cast<int>(CD::Points());
    D::CreditWatchArm();
    const bool posted = coop::dev::game_window::PostKeyPress(D::kUseKey);
    UE_LOGI("[PR64-DRILL] client pressed E=%d after %llu ms of aim: B0=%d coin lives=1 at (%.0f,%.0f,%.0f)",
            posted ? 1 : 0, Since(), g_s.b0, at.X, at.Y, at.Z);
    if (!posted) {
        D::Fail("the key could not be posted to the game window");
        return;
    }
    char line[64];
    std::snprintf(line, sizeof(line), "%sB0 %d", D::kCoinPressed, g_s.b0);
    D::Say(line);
    Go(Step::Observe);
}

// A fixed window on purpose: it is the negative half. "No local credit" is the absence of a call, which no marker can
// announce, so the client reads for kObserveMs after its press before it takes the host's word.
void ClientObserve() {
    UE_LOGI("[PR64-DRILL] client +%llums points=%d coin lives=%d", Since(), static_cast<int>(CD::Points()), Lives(g_s.eid));
    if (Late(D::kObserveMs)) Go(Step::WaitHost);
}

void ClientWaitHost() {
    std::string rest;
    double host[3];   // the host's "<before> <after> <alive>"
    if (D::Rest(D::kCoinHost, &rest) && D::Numbers(rest, 0, host, 3)) {
        const int pts = static_cast<int>(CD::Points()), alive = Lives(g_s.eid), credits = D::CreditWatchCalls();
        UE_LOGI("[PR64-DRILL] client final: B0=%d points=%d coin lives=%d addPoints calls=%d; host said before/after/alive: %s",
                g_s.b0, pts, alive, credits, rest.c_str());
        char line[64];
        std::snprintf(line, sizeof(line), "%s%d %d", D::kCoinFinal, pts, alive);
        D::Say(line);
        // A credit the client made itself after its press happened before the host's word: the host's balance reaches
        // a client as a write that calls no addPoints, so the count is the verdict.
        if (credits > 0)
            D::Fail("local credit before the host (addPoints x%d)", credits);
        else if (alive != 0 || static_cast<int>(host[2]) != 0)
            D::Fail("the coin is not gone (client lives=%d, host lives=%d)", alive, static_cast<int>(host[2]));
        else if (pts != static_cast<int>(host[1]))
            D::Fail("the client ended with %d points, the host's balance is %d", pts, static_cast<int>(host[1]));
        else
            D::Pass();
    } else if (Late(D::kHostWordBoundMs)) {
        D::Fail("the host never said its numbers");
    }
}

void ClientTick() {
    if (!D::ClientReady()) return;
    if (g_s.step == Step::Idle) Go(Step::Hear);
    if (D::Heard(D::kHostFail)) {
        D::Fail("the host failed");
        return;
    }
    switch (g_s.step) {
    case Step::Hear:     ClientHear(); return;
    case Step::Mirror:   ClientMirror(); return;
    case Step::Walk:     ClientWalk(); return;
    case Step::Aim:      ClientAim(); return;
    case Step::Observe:  ClientObserve(); return;
    case Step::WaitHost: ClientWaitHost(); return;
    default:             return;
    }
}

void CancelWalk() {
    if (g_s.walk && g_s.walk->state.load() == 0) DR::CancelBackgroundWalk(*g_s.walk);
    g_s.walk.reset();
}

}  // namespace

namespace detail {

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::pr64_drill);
        return v == "coin" ? Arm::Coin : v == "lid" ? Arm::Lid : Arm::Off;
    }();
    return a;
}

const char* ArmName() { return ArmOf() == Arm::Coin ? "coin" : ArmOf() == Arm::Lid ? "lid" : "off"; }
const char* Role() { return coop::roster::LocalIsHost() ? "host" : "client"; }
uint64_t NowMs() { return coop::net::NowMs(); }
bool ClientReady() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

void Say(const std::string& line) { DC::Say(line); }
bool Heard(const char* line) { return g_heard.count(line) != 0; }

bool Rest(const char* prefix, std::string* rest) {
    const auto it = g_rests.find(prefix);
    if (it == g_rests.end()) return false;
    *rest = it->second;
    return true;
}

static void Finish() {
    g_done = true;
    CancelWalk();
    UE_LOGI("[PR64-DRILL] %s DONE", Role());
}

void Pass(const char* note) {
    UE_LOGI("[PR64-DRILL] %s %s PASS%s%s", Role(), ArmName(), note ? " -- " : "", note ? note : "");
    Finish();
}

void Fail(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGW("[PR64-DRILL] %s %s FAIL: %s", Role(), ArmName(), buf);
    Say(coop::roster::LocalIsHost() ? kHostFail : kClientFail);
    Finish();
}

bool Aim(void* player, const FVector& target) {
    void* c = player ? E::GetController(player) : nullptr;
    if (!c) return false;
    E::SetControlRotation(c, DR::LookAt(E::GetCameraLocation(), target));
    return true;
}

}  // namespace detail

bool IsEnabled() { return D::ArmOf() != D::Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || g_done || !session || !session->connected()) return;
    // Every line a role waits for is latched on sight: the lobby chat keeps few live rows, and a walking peer would lose one.
    if (DC::PollDue()) {
        for (const char* line : {D::kHostFail, D::kClientFail})
            if (!g_heard.count(line) && DC::ChatHasLive(line)) g_heard.insert(line);
        for (const char* prefix : {D::kCoinReady, D::kCoinPressed, D::kCoinHost, D::kCoinFinal, D::kLidReady,
                                   D::kLidFound, D::kLidHost, D::kLidClient}) {
            std::string rest;
            if (DC::LiveRestOf(prefix, &rest)) g_rests[prefix] = rest;
        }
    }
    const bool host = coop::roster::LocalIsHost();
    if (D::ArmOf() == D::Arm::Lid) {
        if (host) D::LidHostTick(session);
        else D::LidClientTick();
    } else if (host) HostTick(session);
    else ClientTick();
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    g_heard.clear();
    g_rests.clear();
    CancelWalk();
    g_s = State{};
    D::LidReset();
    D::CreditWatchStop();
}

}  // namespace coop::dev::pr64_drill
