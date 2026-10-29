// coop/dev/container_intent_drill_throw.cpp -- the CLIENT's half of the container intent drill's throw arms (throwhand,
// throwgrab); see container_intent_drill_throw.h for what they measure and coop/dev/container_intent_drill.h for the drill.
//   throwhand -- takes a drive into the hotbar hand (drill_hand), stands about kStandFromTargetCm before the target (the
//                director's walk, then it stops), turns the camera through the aim fan until its forward points at the
//                target's opening, and throws with the game's own throwHoldingProp.
//   throwgrab -- the same, but the drive is the host's, taken by the use key's physics grab (director::AimedGrab) and thrown
//                from the grab by the body of the fire key's grab branch (traceThrow, throwShit, thrown, dropGrabObject).
// A first throw that lands short or long of the target (and not in it) is followed, ONCE, by a second from a standpoint moved
// by the miss along the line to the target. Both throws are traced and reported. Lines are tagged [CI-DRILL].

#include "container_intent_drill_throw.h"
#include "container_route_drill_detail.h"
#include "drive_drill_verbs.h"  // Throw: the player's own throw (co-located private header)

#include "coop/dev/container_route_census.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drop_intent.h"
#include "coop/props/prop_throw.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace DV  = coop::dev::drive_drill_verbs;
namespace DR  = coop::director;
namespace DC  = coop::dev::drill_chat;
namespace RD  = coop::dev::container_route_drill::detail;
namespace cen = coop::dev::container_route_census;
using ue_wrap::FVector;

constexpr int      kR4Route          = 3;       // container_route_census::RouteVerbs: R1..R6 are 0..5
constexpr int      kWalkDeadlineS    = 150;
constexpr int      kRestChecks       = 4;
constexpr uint64_t kFixtureBoundMs   = 120000;  // the client's wait for the host's line, then for its mirrors
constexpr uint64_t kRestBoundMs      = 30000;
constexpr uint64_t kHandBoundMs      = 10000;
constexpr uint64_t kGrabBoundMs      = 20000;
constexpr uint64_t kItem2BoundMs     = 60000;   // the second standpoint's drive: the host's line, then its mirror
constexpr uint64_t kArmBoundMs       = 60000;   // the client's arm line to the host's armed line (drill chat leaves one line a second)
constexpr int      kAtCount          = 8;       // the instants after the press at which the thrower says where its copy was
constexpr uint64_t kAtEveryMs        = 250;
constexpr uint64_t kAtSlackMs        = 40;      // a sample this near an instant stands for it


enum class Step : uint8_t { WaitLine, WaitMirrors, Rest, WalkWait, Item2Wait, TakeHand, HandRead, Grab, ArmWait, Aim,
                            Flight };

Step     g_step = Step::WaitLine;
uint64_t g_stepMs = 0;      // the time the step was entered
uint64_t g_nextMs = 0;      // the steps that read the chat run four times a second; the aim, the grab and the flight every tick
int      g_n = 1;           // the throw in hand
uint32_t g_packEid = 0, g_itemEid = 0;
ue_wrap::CachedObjRef g_pack, g_item;
FVector  g_packAt{}, g_start{}, g_goal{}, g_stand{};
FVector  g_lastPos[2] = {};
int      g_rest = 0;
int      g_settled = 0;
std::shared_ptr<DR::BackgroundWalk> g_walk;
std::unique_ptr<DR::AimedGrab> g_grab;
Aimer    g_aimer;
HitJudge g_hit;

// The throw in flight.
Track    g_local, g_mirror;
uint32_t g_hostEid = 0;           // the host's element for the throw, once it says
bool     g_mirrorSame = false;    // the mirror by that eid is the very actor thrown
uint64_t g_pressMs = 0;           // the clock of the r4 lines at the press
uint64_t g_endAtMs = 0;
uint64_t g_ovSeen = 0;            // the census's overlap sequence read last
uint64_t g_ovAll0 = 0, g_adds0 = 0;
int      g_count0 = -1;
int      g_ovCount = 0;
const char* g_via = "";

void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }

unsigned long long U(uint64_t v) { return static_cast<unsigned long long>(v); }

void WalkStop() {
    if (g_walk && g_walk->state.load() == 0) DR::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
}

// The client's end with the FAIL line; the host is told so it does not wait out its bounds.
void CFail(const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    WalkStop();
    g_grab.reset();
    DC::Say(kThrowClientFailedLine);
    Fail("%s", buf);
}

void* HeldActor(void* player) {
    E::MainPlayerGrabState hand{};
    return player && E::ReadMainPlayerGrabState(player, hand) ? hand.holdingActor : nullptr;
}

uint32_t WireEid(void* actor) {
    const uint32_t eid = RD::EidOf(actor);
    return eid == static_cast<uint32_t>(coop::element::kInvalidId) ? 0u : eid;
}

bool HorizUnit(const FVector& from, const FVector& to, float& ux, float& uy, float& len) {
    ux = to.X - from.X;
    uy = to.Y - from.Y;
    len = std::hypot(ux, uy);
    if (len < 1.f) return false;
    ux /= len;
    uy /= len;
    return true;
}

// ---- the walk and the preparation ---------------------------------------------------------------------------------------

void StartWalk() {
    g_walk = DR::StartBackgroundWalk(g_goal, kWalkReachCm, kWalkDeadlineS, /*carry=*/false);
    UE_LOGI("[CI-DRILL] client %s walks for throw %d to (%.0f,%.0f,%.0f), target at (%.0f,%.0f,%.0f)", ArmName(), g_n,
            static_cast<double>(g_goal.X), static_cast<double>(g_goal.Y), static_cast<double>(g_goal.Z),
            static_cast<double>(g_packAt.X), static_cast<double>(g_packAt.Y), static_cast<double>(g_packAt.Z));
    Next(Step::WalkWait);
}

void StepWaitLine() {
    std::string rest;
    if (DC::LiveRestOf(kThrowFixtureLine, &rest)) {
        int pack = 0, item = 0;
        if (!RD::IntAfter(rest, "pack", pack) || !RD::IntAfter(rest, "item", item) || pack <= 0 || item <= 0) {
            CFail("the host's fixture line does not read (%s)", rest.c_str());
            return;
        }
        g_packEid = static_cast<uint32_t>(pack);
        g_itemEid = static_cast<uint32_t>(item);
        Next(Step::WaitMirrors);
        return;
    }
    if (Expired(kFixtureBoundMs)) CFail("the host never said its fixture within %llu s", U(kFixtureBoundMs / 1000));
}

void StepWaitMirrors() {
    void* pack = coop::element::LivePropActor(g_packEid);
    void* item = coop::element::LivePropActor(g_itemEid);
    void* player = coop::players::Registry::Get().Local();
    if (pack && item && player && E::TryGetActorLocation(pack, g_packAt) && E::TryGetActorLocation(player, g_start)) {
        g_pack.Set(pack);
        g_item.Set(item);
        g_rest = 0;
        UE_LOGI("[CI-DRILL] client %s: the fixture's mirrors are here (target eid=%u, drive eid=%u)", ArmName(), g_packEid,
                g_itemEid);
        Next(Step::Rest);
        return;
    }
    if (Expired(kFixtureBoundMs)) CFail("the fixture's mirrors never came (target %p, drive %p, player %p)", pack, item, player);
}

// The host's drive of the fixture (it falls and settles) is over once the mirrors stay put for kRestChecks checks; the
// standpoint is then fixed: kStandFromTargetCm before the target, on the line from where the client stands.
void StepRest() {
    void* actors[2] = {g_pack.Get(), g_item.Get()};
    bool still = true;
    for (int i = 0; i < 2; ++i) {
        FVector at{};
        if (!actors[i] || !E::TryGetActorLocation(actors[i], at)) {
            CFail("a fixture's mirror died or does not read");
            return;
        }
        still = still && std::fabs(at.X - g_lastPos[i].X) + std::fabs(at.Y - g_lastPos[i].Y) + std::fabs(at.Z - g_lastPos[i].Z) < 1.f;
        g_lastPos[i] = at;
    }
    g_rest = still ? g_rest + 1 : 0;
    if (g_rest < kRestChecks) {
        if (Expired(kRestBoundMs)) CFail("the fixture's mirrors never came to rest");
        return;
    }
    E::TryGetActorLocation(g_pack.Get(), g_packAt);
    float ux = 0.f, uy = 0.f, len = 0.f;
    if (!HorizUnit(g_start, g_packAt, ux, uy, len)) {
        CFail("the client stands on the target");
        return;
    }
    g_goal = {g_packAt.X - ux * kStandFromTargetCm, g_packAt.Y - uy * kStandFromTargetCm, g_start.Z};
    UE_LOGI("[CI-DRILL] client %s: the target is %.0f cm from where the client stands; the standpoint is %.0f cm before it", ArmName(),
            static_cast<double>(len), static_cast<double>(kStandFromTargetCm));
    StartWalk();
}

void StepWalkWait() {
    const int st = g_walk ? g_walk->state.load() : 2;
    if (st == 0) return;
    if (st != 1) {
        CFail("the walk to the standpoint did not arrive");
        return;
    }
    g_walk.reset();
    void* player = coop::players::Registry::Get().Local();
    if (!player || !E::TryGetActorLocation(player, g_stand)) {
        CFail("the player's place does not read");
        return;
    }
    UE_LOGI("[CI-DRILL] client %s: throw %d stands at (%.0f,%.0f,%.0f), %.0f cm from the target", ArmName(), g_n,
            static_cast<double>(g_stand.X), static_cast<double>(g_stand.Y), static_cast<double>(g_stand.Z),
            static_cast<double>(std::hypot(g_packAt.X - g_stand.X, g_packAt.Y - g_stand.Y)));
    if (IsHandThrow(ArmOf())) {
        Next(Step::TakeHand);
        return;
    }
    if (g_n == 1) {
        void* item = g_item.Get();
        if (!item) {
            CFail("the drive's mirror died");
            return;
        }
        g_grab = std::make_unique<DR::AimedGrab>(player, item);
        Next(Step::Grab);
        return;
    }
    Next(Step::Item2Wait);   // the host spawns the second drive beside the second standpoint once told it
}

// throwgrab, throw 2: the host's second drive (its eid on a line), and its mirror.
void StepItem2Wait() {
    std::string rest;
    int eid = 0;
    if (DC::LiveRestOf(kThrowItem2Line, &rest) && RD::IntAfter(rest, "eid", eid) && eid > 0) {
        void* item = coop::element::LivePropActor(static_cast<uint32_t>(eid));
        void* player = coop::players::Registry::Get().Local();
        if (item && player) {
            g_itemEid = static_cast<uint32_t>(eid);
            g_item.Set(item);
            g_grab = std::make_unique<DR::AimedGrab>(player, item);
            Next(Step::Grab);
            return;
        }
    }
    if (Expired(kItem2BoundMs)) CFail("the second drive never came (host line %s)", rest.empty() ? "absent" : rest.c_str());
}

void StepTakeHand() {
    if (!coop::dev::drill_hand::TakeIntoHandNow(L"drive", RD::kDriveClass)) {
        CFail("the drive could not be taken into the hand");
        return;
    }
    g_settled = 0;
    Next(Step::HandRead);   // the hand's actor is read a tick later
}

// The arm line: the drive in hand (or grabbed) is named to the host, which takes its set of drives and answers armed.
void SayArm(void* held) {
    const char* dead = nullptr;
    if (cen::SeamsState(&dead) != cen::Seams::Live) {
        CFail("the census's watches are not all live (%s)", dead ? dead : "pending");
        return;
    }
    if (!HitWatchLive()) {
        CFail("the hit watch on the drive's component-hit handler is not live");
        return;
    }
    char say[64];
    std::snprintf(say, sizeof(say), "%sheld=%u", ThrowLine("client", "arm", g_n, true).c_str(), WireEid(held));
    DC::Say(say);
    Next(Step::ArmWait);
}

void StepHandRead() {
    void* hand = HeldActor(coop::players::Registry::Get().Local());
    g_settled = hand && R::ClassNameOf(hand) == RD::kDriveClass ? g_settled + 1 : 0;
    if (g_settled < 2) {
        if (Expired(kHandBoundMs)) CFail("the hand never held the drive");
        return;
    }
    SayArm(hand);
}

void StepGrab() {
    if (!g_grab) {
        CFail("the grab is not running");
        return;
    }
    switch (g_grab->Tick()) {
    case DR::GrabState::Working:
        if (Expired(kGrabBoundMs)) CFail("the grab never took within %llu s", U(kGrabBoundMs / 1000));
        return;
    case DR::GrabState::Failed:
        CFail("the grab did not take: %s, chain pre/use/post %d/%d/%d", g_grab->Why(), g_grab->ChainPre() ? 1 : 0,
              g_grab->ChainUse() ? 1 : 0, g_grab->ChainPost() ? 1 : 0);
        return;
    case DR::GrabState::Grabbed:
        UE_LOGI("[CI-DRILL] client %s: the drive is grabbed after %d aim pose(s)", ArmName(), g_grab->AimPoses());
        g_grab.reset();
        SayArm(DR::Grabbing(coop::players::Registry::Get().Local()));
        return;
    }
}

void StepArmWait() {
    if (DC::ChatHasLive(ThrowLine("host", "armed", g_n, false))) {
        g_aimer.Begin(g_n);
        Next(Step::Aim);
        return;
    }
    if (Expired(kArmBoundMs)) CFail("the host never armed throw %d", g_n);
}

// ---- the throw ----------------------------------------------------------------------------------------------------------

// What the throw is judged against, taken just before it.
void Baseline() {
    g_pressMs = cen::UnixMs();
    g_endAtMs = 0;
    g_ovSeen = cen::LastR4Overlap().seq;
    g_ovAll0 = cen::R4Overlaps();
    g_adds0 = cen::RouteVerbs(kR4Route, /*take=*/false);
    g_count0 = CountOf(g_packEid);
    g_ovCount = 0;
    g_hostEid = 0;
    g_mirrorSame = false;
    TrackBegin(g_mirror, "client", "mirror");
}

void BeginFlight(void* actor, const char* via) {
    // The throw is numbered on this copy (prop_throw), on both throw paths: a throw with no sequence would be a flight the
    // host never elects.
    const unsigned seq = coop::prop_throw::SeqFor(actor);
    UE_LOGI("[CI-DRILL] client %s throw %d: throw seq %u (minted %u, puppet throwShit ignored n=%u)", ArmName(), g_n, seq,
            coop::prop_throw::dev::Minted(), coop::prop_throw::dev::PuppetIgnored());
    if (seq == 0) {
        CFail("the game's throw left no sequence on the thrown copy (the throwShit watch)");
        return;
    }
    g_via = via;
    TrackBegin(g_local, "client", "local");
    TrackSet(g_local, actor);
    g_hit.Begin(/*releaseKnown=*/true);
    UE_LOGI("[CI-DRILL] client %s throw %d press t=%llu via=%s target holds %d record(s)", ArmName(), g_n, U(g_pressMs), via,
            g_count0);
    Next(Step::Flight);
}

void ThrowTheHeld(void* player) {
    Baseline();
    void* const before = coop::prop_drop_intent::dev::LastClientPlaceSpawn();
    if (!DV::Throw(player)) {
        CFail("throwHoldingProp did not run");
        return;
    }
    void* const placed = coop::prop_drop_intent::dev::LastClientPlaceSpawn();
    if (!placed || placed == before) {
        CFail("the throw spawned no world actor the place hook caught (run with container_route_census=1)");
        return;
    }
    HitWatchBegin(placed);
    BeginFlight(placed, "throwHoldingProp");
}

void ThrowTheGrab(void* player) {
    Baseline();
    void* item = DR::Grabbing(player);
    if (!item || item != g_item.Get()) {
        CFail("the grab was lost before the throw");
        return;
    }
    HitWatchBegin(item);   // before the release, so a hit at the release point is recorded
    char why[160];
    if (!ThrowGrabBranch(player, item, g_n, why, sizeof(why))) {
        CFail("%s", why);
        return;
    }
    BeginFlight(item, "traceThrow+throwShit+dropGrabObject");
}

void StepAim(void* player) {
    char why[160];
    switch (g_aimer.Tick(player, g_pack.Get(), g_stand, why, sizeof(why))) {
    case Aimer::State::Working: return;
    case Aimer::State::Failed:  CFail("%s", why); return;
    case Aimer::State::Aimed:
        if (IsHandThrow(ArmOf())) ThrowTheHeld(player);
        else ThrowTheGrab(player);
        return;
    }
}

// ---- the flight and its end ---------------------------------------------------------------------------------------------

void PollMirror() {
    if (g_hostEid) return;
    std::string rest;
    int eid = 0;
    if (!DC::LiveRestOf(ThrowLine("host", "thrown", g_n, true), &rest) || !RD::IntAfter(rest, "eid", eid) || eid <= 0) return;
    g_hostEid = static_cast<uint32_t>(eid);
    void* actor = coop::element::LivePropActor(g_hostEid);
    g_mirrorSame = actor && actor == g_local.ref.Raw();
    UE_LOGI("[CI-DRILL] client %s throw %d: the host's element is eid=%u, %s the actor thrown", ArmName(), g_n, g_hostEid,
            g_mirrorSame ? "the same as" : "another from");
    if (actor && !g_mirrorSame) {
        TrackSet(g_mirror, actor);
        HitWatchAdd(actor);
    }
}

// An overlap of the target by the thrown actor, whether or not addObject then ran (the census counts the verbs).
void PollOverlap() {
    const cen::R4Overlap o = cen::LastR4Overlap();
    if (o.seq == g_ovSeen) return;
    g_ovSeen = o.seq;
    if (o.container != g_packEid || !o.actor || (o.actor != g_local.ref.Raw() && o.actor != g_mirror.ref.Raw())) return;
    ++g_ovCount;
    UE_LOGI("[CI-DRILL] throw client overlap eid=%u sim=%d t=%llu", o.eid, o.sim ? 1 : 0, U(o.t));
}

// The miss of a rest from the target: along the line from the standpoint to it (negative: short) and across it.
void MissOf(const FVector& rest, float& along, float& side) {
    float ux = 0.f, uy = 0.f, len = 0.f;
    along = side = 0.f;
    if (!HorizUnit(g_stand, g_packAt, ux, uy, len)) return;
    const float dx = rest.X - g_packAt.X, dy = rest.Y - g_packAt.Y;
    along = dx * ux + dy * uy;
    side = dx * uy - dy * ux;
}

// The second throw's standpoint: the first moved by the miss along the line, kept off the target and within reach of a walk.
bool SecondStand(float along, FVector& out) {
    float ux = 0.f, uy = 0.f, len = 0.f;
    if (!HorizUnit(g_stand, g_packAt, ux, uy, len)) return false;
    const float dist = std::clamp(len + along, kMinStandCm, kMaxStandCm);   // the new distance to the target
    out = {g_packAt.X - ux * dist, g_packAt.Y - uy * dist, g_stand.Z};
    return true;
}

void Wrap(const coop::net::Session& session) {
    g_hit.Finish(g_local);
    HitWatchEnd();
    const int count1 = CountOf(g_packEid);
    const uint64_t adds = cen::RouteVerbs(kR4Route, false) - g_adds0;
    const FVector rest = g_local.last;
    float along = 0.f, side = 0.f;
    if (!g_local.samples.empty()) MissOf(rest, along, side);
    const bool landed = !g_local.samples.empty() && !g_local.gone;
    const bool hit = count1 > g_count0 || (landed && std::hypot(rest.X - g_packAt.X, rest.Y - g_packAt.Y) <= kHitCm);
    UE_LOGI("[CI-DRILL] client %s throw %d end: via=%s arc=%s gone=%d rest=(%.1f,%.1f,%.1f) %s %.0f cm side %.0f cm of the target, "
            "overlaps=%d (census all %llu) addObject=%llu target %d -> %d peak %.0f cm/s at t=%llu rtt=%d", ArmName(), g_n, g_via,
            ArcOf(g_local), g_local.gone ? 1 : 0, static_cast<double>(rest.X), static_cast<double>(rest.Y),
            static_cast<double>(rest.Z), along < 0.f ? "short" : "long", static_cast<double>(std::fabs(along)),
            static_cast<double>(side), g_ovCount, U(cen::R4Overlaps() - g_ovAll0), U(adds), g_count0, count1,
            static_cast<double>(g_local.peakSpeed), U(g_local.peakT), session.rttMsForSlot(0));
    char say[160];
    std::snprintf(say, sizeof(say), "%st=%llu v=%d p=%d,%d,%d", ThrowLine("client", "peak", g_n, true).c_str(), U(g_local.peakT),
                  static_cast<int>(g_local.peakSpeed), static_cast<int>(g_local.peakPos.X), static_cast<int>(g_local.peakPos.Y),
                  static_cast<int>(g_local.peakPos.Z));
    DC::Say(say);
    // Where the thrower's copy was at fixed instants after the press: the host sets its own copy against them at the same
    // wall-clock instants. Said before the end line, which is what the host's verdict waits for.
    for (int k = 1; k <= kAtCount; ++k) {
        const uint64_t want = g_pressMs + static_cast<uint64_t>(k) * kAtEveryMs;
        const Sample* best = nullptr;
        for (const Sample& s : g_local.samples) {
            const uint64_t d = s.t > want ? s.t - want : want - s.t;
            if (d <= kAtSlackMs && (!best || d < (best->t > want ? best->t - want : want - best->t))) best = &s;
        }
        if (!best) continue;
        std::snprintf(say, sizeof(say), "%sk=%d t=%llu p=%d,%d,%d", ThrowLine("client", "at", g_n, true).c_str(), k, U(best->t),
                      static_cast<int>(best->pos.X), static_cast<int>(best->pos.Y), static_cast<int>(best->pos.Z));
        DC::Say(say);
    }
    std::snprintf(say, sizeof(say), "%sarc=%s gone=%d ov=%d all=%llu mir=%d rest=%d,%d,%d park=%d tmax=%llu",
                  ThrowLine("client", "end", g_n, true).c_str(), ArcOf(g_local), g_local.gone ? 1 : 0, g_ovCount,
                  U(cen::R4Overlaps() - g_ovAll0), count1 - g_count0, static_cast<int>(rest.X), static_cast<int>(rest.Y),
                  static_cast<int>(rest.Z), g_local.notSimMoving, U(g_local.atRest && g_local.restMs > g_pressMs ? g_local.restMs - g_pressMs : 0));
    DC::Say(say);
    FVector next{};
    if (g_n == 1 && landed && !hit && std::fabs(along) >= kMoveMinCm && SecondStand(along, next)) {
        g_n = 2;
        g_goal = next;
        std::snprintf(say, sizeof(say), "%sat=%d,%d,%d", kThrowSecondLine, static_cast<int>(next.X), static_cast<int>(next.Y),
                      static_cast<int>(next.Z));
        DC::Say(say);
        UE_LOGI("[CI-DRILL] client %s throw 1 missed by %.0f cm along the line: the standpoint moves to (%.0f,%.0f,%.0f)", ArmName(),
                static_cast<double>(along), static_cast<double>(next.X), static_cast<double>(next.Y), static_cast<double>(next.Z));
        StartWalk();
        return;
    }
    UE_LOGI("[CI-DRILL] client %s: no further throw (%s)", ArmName(),
            g_n == 2 ? "both thrown" : hit ? "the first throw hit" : !landed ? "the first copy is gone or unread" : "the miss is across the line");
    DC::Say(kThrowDoneLine);
    UE_LOGI("[CI-DRILL] client %s MEASURED", ArmName());
    Finish();
}

void StepFlight(const coop::net::Session& session) {
    const int rtt = session.rttMsForSlot(0);
    TrackTick(g_local, rtt);
    if (g_mirror.on) TrackTick(g_mirror, rtt);
    PollOverlap();
    g_hit.Tick(g_local);
    const uint64_t now = cen::UnixMs();
    if (!g_endAtMs) {
        if (g_local.atRest) g_endAtMs = g_local.restMs + kAfterRestMs;
        else if (g_local.gone) g_endAtMs = g_local.goneMs + kAfterRestMs;
        if (g_endAtMs && g_endAtMs < g_pressMs + kMinTraceMs) g_endAtMs = g_pressMs + kMinTraceMs;
    }
    if ((g_endAtMs && now >= g_endAtMs) || now >= g_pressMs + kFlightBoundMs) Wrap(session);
}

}  // namespace

std::string ThrowLine(const char* who, const char* what, int n, bool fields) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "ci-throw %s %s n=%d%s", who, what, n, fields ? " " : "");
    return buf;
}

void ThrowClientTick(const coop::net::Session& session) {
    if (ArmOf() == Arm::ThrowHostGrab) {
        HostGrabClientTick(session);
        return;
    }
    if (!g_stepMs) {   // the first step's clock starts when the arm does, never at reset
        if (!cen::IsEnabled()) {
            CFail("the arm reads the census's overlap watch: run with container_route_census=1 on both peers");
            return;
        }
        HitWatchInstall();
        Next(Step::WaitLine);
    }
    // The chat is read four times a second; the aim, the grab and the flight are read every tick.
    const bool due = ::GetTickCount64() >= g_nextMs;
    if (due) {
        g_nextMs = ::GetTickCount64() + RD::kCheckEveryMs;
        if (DC::ChatHasLive(kThrowHostFailedLine)) {
            CFail("the host ended the arm with a FAIL");
            return;
        }
        if (g_step == Step::Flight) PollMirror();
    }
    void* player = coop::players::Registry::Get().Local();
    switch (g_step) {
    case Step::Aim:        if (player) StepAim(player); else CFail("the local player is gone"); return;
    case Step::Grab:       StepGrab(); return;
    case Step::Flight:     StepFlight(session); return;
    default:               break;
    }
    if (!due) return;
    switch (g_step) {
    case Step::WaitLine:    StepWaitLine(); return;
    case Step::WaitMirrors: StepWaitMirrors(); return;
    case Step::Rest:        StepRest(); return;
    case Step::WalkWait:    StepWalkWait(); return;
    case Step::Item2Wait:   StepItem2Wait(); return;
    case Step::TakeHand:    StepTakeHand(); return;
    case Step::HandRead:    StepHandRead(); return;
    case Step::ArmWait:     StepArmWait(); return;
    default:                return;
    }
}

void ThrowReset() {
    WalkStop();
    g_grab.reset();
    ThrowHostReset();
    HostGrabReset();
    g_step = Step::WaitLine;
    g_stepMs = g_nextMs = 0;
    g_n = 1;
    g_packEid = g_itemEid = 0;
    g_pack.Reset();
    g_item.Reset();
    g_packAt = g_start = g_goal = g_stand = FVector{};
    g_lastPos[0] = g_lastPos[1] = FVector{};
    g_rest = g_settled = 0;
    g_aimer = Aimer();
    g_hit = HitJudge();
    g_local = Track{};
    g_mirror = Track{};
    g_hostEid = 0;
    g_mirrorSame = false;
    g_pressMs = g_endAtMs = g_ovSeen = g_ovAll0 = g_adds0 = 0;
    g_count0 = -1;
    g_ovCount = 0;
    g_via = "";
}

}  // namespace coop::dev::container_intent_drill::detail
