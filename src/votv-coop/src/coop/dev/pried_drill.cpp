// coop/dev/pried_drill.cpp -- see coop/dev/pried_drill.h.

#include "coop/dev/pried_drill.h"

#include "pried_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_ground.h"
#include "coop/dev/rest_compare.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_pry_host.h"
#include "coop/props/prop_pry_sync.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_events.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace coop::dev::pried_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace D  = coop::dev::pried_drill_detail;
namespace DC = coop::dev::drill_chat;
namespace DG = coop::dev::drill_ground;
namespace RC = coop::dev::rest_compare;
namespace RV = coop::rig_verdict;
using ue_wrap::FVector;
using Fixture = D::Fixture;

enum class Arm { Off, Host, Tv, Client, Far, Twice, Join, MidJoin };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::pried_drill);
        return v == "host" ? Arm::Host : v == "tv" ? Arm::Tv : v == "client" ? Arm::Client : v == "far" ? Arm::Far
             : v == "twice" ? Arm::Twice : v == "join" ? Arm::Join : v == "midjoin" ? Arm::MidJoin : Arm::Off;
    }();
    return a;
}

const char* Tag() {
    switch (ArmOf()) {
    case Arm::Host:    return "pry-host";
    case Arm::Tv:      return "pry-tv";
    case Arm::Client:  return "pry-client";
    case Arm::Far:     return "pry-far";
    case Arm::Twice:   return "pry-twice";
    case Arm::Join:    return "pry-join";
    case Arm::MidJoin: return "pry-midjoin";
    case Arm::Off:     break;
    }
    return "pry-off";
}

Fixture FixtureFor() {
    switch (ArmOf()) {
    case Arm::Tv:                          return Fixture::Tv;
    case Arm::Host: case Arm::Join: case Arm::MidJoin: return Fixture::Radiator;
    default:                               return Fixture::Plank;
    }
}

bool IsHost() { return coop::roster::LocalIsHost(); }
char Who() { return IsHost() ? 'H' : 'C'; }
// The join arms' second client life, which the rig tells by this variable (--rejoin-env).
bool SecondLife() { return coop::config::ReadEnv("VOTVCOOP_PRIED_LIFE") == "2"; }
bool JoinArm() { return ArmOf() == Arm::Join || ArmOf() == Arm::MidJoin; }

constexpr float    kAheadCm   = 300.f;   // the fixture's distance in front of the client's puppet
constexpr float    kFarCm     = 3000.f;  // the far arm's: past the judge's reach with its allowance
constexpr float    kTvLiftCm  = 200.f;   // the security TV's extra height, so its fall is one the drill can see
constexpr float    kMovedCm   = 5.f;     // a freed fixture must have ended further than this from where it hung
constexpr int      kMarkTicks = 90;      // still ticks before a rest is marked: the End has landed
constexpr uint8_t  kAllJoints = 0x0F;
constexpr uint64_t kBoundMs   = 360000;  // a wait for the other peer or a rest: INVALID only, never a verdict

const std::string kReadyPrefix  = "pried ready ";         // <token> <key> <x> <y> <z>
const std::string kDonePrefix   = "pried host done ";     // <mask hex 2> <pose>
const std::string kRestedPrefix = "pried client rested "; // <mask hex 2> <end 0|1> <pose>
const std::string kJoinerPrefix = "pried joiner rested "; // <mask hex 2> <pose>

enum class Step { WaitPeer, Bind, WaitGrant, WaitFar, WaitJoining, Watch, WaitClient, WaitJoiner,
                  WaitReady, Find, Pry, WaitDone, Settle, Done };

Step g_step = Step::WaitPeer;
bool g_armed = false;
bool g_over = false;
uint64_t g_stepMs = 0;
ue_wrap::CachedObjRef g_prop;
std::string g_key;
FVector g_spot{};
uint32_t g_eid = 0;
DG::Settle g_rest;
RC::Pose g_before;
RC::Pose g_hostRest;
uint8_t g_hostMask = 0;
bool g_hostDone = false;
bool g_wasReady[coop::net::kMaxPeers] = {};
uint64_t g_endMarkMs = 0;
coop::prop_pry_host::Counts g_baseCounts{};
coop::prop_pry_sync::Stats g_baseStats{};
char g_lastWait[2][160] = {};

void Next(Step s) {
    g_step = s;
    g_stepMs = ::GetTickCount64();
    DG::Restart(g_rest);
}
bool Expired() { return ::GetTickCount64() - g_stepMs > kBoundMs; }

void Wait(const char* what) {
    char* last = g_lastWait[IsHost() ? 0 : 1];
    if (std::strcmp(last, what) == 0) return;
    std::snprintf(last, sizeof(g_lastWait[0]), "%s", what);
    UE_LOGI("[PRIED-DRILL] [%c] waiting for %s", Who(), what);
}

void Invalid(const char* why) {
    if (g_over) return;
    g_over = true;
    UE_LOGW("[PRIED-DRILL] [%c] INVALID %s", Who(), why);
    RV::Say(Tag(), RV::Outcome::Invalid, "%s", why);
}

void InvalidWaiting(const char* what) {
    char why[200];
    std::snprintf(why, sizeof(why), "%s never came", what);
    Invalid(why);
}

void Fail(const char* fmt, ...) {
    if (g_over) return;
    g_over = true;
    char why[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    UE_LOGW("[PRIED-DRILL] [%c] FAIL %s", Who(), why);
    RV::Say(Tag(), RV::Outcome::Fail, "%s", why);
}

void Pass(const char* fmt, ...) {
    if (g_over) return;
    g_over = true;
    char why[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);
    UE_LOGI("[PRIED-DRILL] [%c] PASS %s", Who(), why);
    RV::Say(Tag(), RV::Outcome::Pass, "%s", why);
}

std::string Hex(uint8_t mask) {
    char b[8];
    std::snprintf(b, sizeof(b), "%02x", static_cast<unsigned>(mask));
    return b;
}
uint8_t ParseHex(const std::string& s) { return static_cast<uint8_t>(std::strtoul(s.substr(0, 2).c_str(), nullptr, 16)); }

// ---- the host ------------------------------------------------------------------------------------------------

bool SlotReady(coop::net::Session& s, int slot) {
    return s.IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot) &&
           !coop::join_window_baseline::IsLateWindowOpen(slot);
}

// The midjoin arm's moment: a joiner's world is up and the host is still draining its snapshot rows.
bool JoinerDraining(coop::net::Session& s) {
    for (int i = 1; i < coop::net::kMaxPeers; ++i)
        if (s.IsSlotWorldReady(i) && !coop::prop_snapshot::IsBracketClosed(i)) return true;
    return false;
}

void SayReady() {
    char tail[160];
    std::snprintf(tail, sizeof(tail), "%s %s %.0f %.0f %.0f", D::Token(FixtureFor()), g_key.c_str(), g_spot.X, g_spot.Y,
                  g_spot.Z);
    DC::Say(kReadyPrefix + tail);
}

void SayDone() { DC::Say(kDonePrefix + Hex(g_hostMask) + " " + RC::Encode(g_hostRest)); }

// The host's own pry, as its player's crowbar ends in it: the prop's own crowbarOpen, which runs through the lane's watch.
bool PryOwn(void* prop) {
    const bool ran = PR::CallCrowbarOpen(prop);
    UE_LOGI("[PRIED-DRILL] [H] the host pries '%s' (crowbarOpen ran=%d; static=%d)", g_key.c_str(), ran ? 1 : 0,
            PR::IsStatic(prop) ? 1 : 0);
    if (!ran) Invalid("the host's crowbarOpen did not dispatch");
    return ran;
}

void HostWaitPeer(coop::net::Session& s) {
    int slot = 0;
    for (int i = 1; i < coop::net::kMaxPeers && !slot; ++i)
        if (SlotReady(s, i)) slot = i;
    if (!slot) { Wait("a client slot that is world-ready with its join window closed"); return; }
    void* puppet = coop::puppet_drive::Puppet(slot).GetActor();
    FVector at{};
    if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at)) { Wait("the client's puppet to read"); return; }
    const FVector fwd = E::GetActorForwardVector(puppet);
    if (fwd.X * fwd.X + fwd.Y * fwd.Y < 0.25f) { Wait("the client's puppet to face somewhere"); return; }
    if (!D::WitnessesLive()) {
        Wait("the drill's witnesses (BreakConstraint's hook, the crowbarOpen watch) to go live");
        if (Expired()) InvalidWaiting("the witnesses' going live");
        return;
    }
    const float ahead = ArmOf() == Arm::Far ? kFarCm : kAheadCm;
    // A security TV stands tall: from a waist-high spot it rests within a few cm of where it hung, so it hangs higher.
    g_spot = {at.X + fwd.X * ahead, at.Y + fwd.Y * ahead, at.Z + (ArmOf() == Arm::Tv ? kTvLiftCm : 0.f)};
    void* prop = D::SpawnFixture(FixtureFor(), g_spot);
    if (!prop) { Invalid("the fixture did not spawn"); return; }
    g_prop.Set(prop);
    UE_LOGI("[PRIED-DRILL] [H] spawned a %s (actor %p) at (%.0f, %.0f, %.0f), %.0f cm ahead of slot %d's puppet",
            D::Token(FixtureFor()), prop, g_spot.X, g_spot.Y, g_spot.Z, ahead, slot);
    Next(Step::Bind);
}

void HostStartArm(void* prop) {
    g_baseCounts = coop::prop_pry_host::GetCounts();
    g_baseStats = coop::prop_pry_sync::GetStats();
    switch (ArmOf()) {
    case Arm::Host: case Arm::Tv: case Arm::Join:
        if (PryOwn(prop)) Next(Step::Watch);
        return;
    case Arm::Client: Next(Step::WaitGrant); return;
    case Arm::Far:    Next(Step::WaitFar); return;
    case Arm::MidJoin: Next(Step::WaitJoining); return;
    case Arm::Twice: {
        // The first pry runs and marks the plank pried; the second is the lane's to cancel, and its body must not run.
        if (!PryOwn(prop)) return;
        const uint32_t bodies = D::BodiesRun(prop);
        if (bodies != 1) { Invalid("the first crowbarOpen did not run its body once"); return; }
        const coop::prop_pry_sync::Stats before = coop::prop_pry_sync::GetStats();
        PryOwn(prop);
        const coop::prop_pry_sync::Stats after = coop::prop_pry_sync::GetStats();
        const uint32_t bodies2 = D::BodiesRun(prop);
        if (bodies2 >= 2) Fail("the second crowbarOpen of a pried plank ran its body (%u bodies in all)", bodies2);
        else if (after.twiceCancelled == before.twiceCancelled + 1)
            Pass("the second crowbarOpen of a pried plank was cancelled and its body ran once");
        else Invalid("the second crowbarOpen neither ran its body nor was counted cancelled");
        Next(Step::Done);
        return;
    }
    default: return;
    }
}

void HostBind() {
    void* prop = g_prop.Get();
    if (!prop) { Invalid("the fixture died before it was ready"); return; }
    const uint32_t eid = static_cast<uint32_t>(coop::prop_element_tracker::GetPropElementIdForActor(prop));
    const bool bound = eid != static_cast<uint32_t>(coop::element::kInvalidId);
    const std::wstring key = PR::GetInteractableKeyString(prop);
    const bool keyed = !key.empty() && key != L"None";
    FVector at{};
    const bool stable = E::TryGetActorLocation(prop, at) && DG::Stable(g_rest, at, DG::Quiet(prop));
    if (!(bound && keyed && stable) || coop::prop_drive_host::ContextOf(prop, eid).open) {
        Wait("the fixture's element id, key and rest");
        if (Expired()) InvalidWaiting("the fixture's element id and key");
        return;
    }
    if (!PR::IsStatic(prop) && !PR::IsFrozen(prop)) { Invalid("the fixture is not static: there is nothing to free"); return; }
    g_eid = eid;
    g_key.assign(key.begin(), key.end());
    RC::Read(prop, g_before);
    SayReady();
    UE_LOGI("[PRIED-DRILL] [H] READY: '%s' hangs at (%.1f, %.1f, %.1f)", g_key.c_str(), g_before.loc.X, g_before.loc.Y,
            g_before.loc.Z);
    HostStartArm(prop);
}

// The client's pry is granted, or refused: the host's counters are the events.
void HostWaitGrant() {
    const coop::prop_pry_host::Counts c = coop::prop_pry_host::GetCounts();
    if (c.refused > g_baseCounts.refused) { Fail("the host refused the client's pry"); Next(Step::Done); return; }
    if (c.granted > g_baseCounts.granted) {
        UE_LOGI("[PRIED-DRILL] [H] the host granted the client's pry");
        Next(Step::Watch);
        return;
    }
    Wait("the client's pry ask");
    if (Expired()) InvalidWaiting("the client's pry ask");
}

// The raw ask out of reach: refused as far, with the plank still stuck, or granted (the judge's red).
void HostWaitFar() {
    void* prop = g_prop.Get();
    const coop::prop_pry_host::Counts c = coop::prop_pry_host::GetCounts();
    if (c.granted > g_baseCounts.granted) { Fail("the out-of-reach ask was granted"); Next(Step::Done); return; }
    if (c.farRefused > g_baseCounts.farRefused) {
        if (prop && (PR::IsStatic(prop) || PR::IsFrozen(prop)))
            Pass("the out-of-reach ask was refused as out of reach and the plank stayed stuck");
        else
            Fail("the out-of-reach ask was refused but the plank is no longer stuck");
        Next(Step::Done);
        return;
    }
    if (c.refused > g_baseCounts.refused) { Invalid("the ask was refused for a reason other than reach"); Next(Step::Done); return; }
    Wait("the client's raw ask");
    if (Expired()) InvalidWaiting("the client's raw ask");
}

// The host's copy is freed, its joints broken (a radiator, a TV), and at rest with its drive's End sent.
void HostWatch() {
    void* prop = g_prop.Get();
    FVector at{};
    if (!prop || !E::TryGetActorLocation(prop, at)) { Invalid("the fixture died before its rest"); return; }
    const bool hanging = FixtureFor() != Fixture::Plank;
    if (hanging && D::JointMask(prop) != kAllJoints) {
        DG::Restart(g_rest);
        Wait("the radiator's four joints to break");
        if (Expired()) InvalidWaiting("the fourth joint's break");
        return;
    }
    if (!hanging && (PR::IsStatic(prop) || PR::IsFrozen(prop))) {
        DG::Restart(g_rest);
        Wait("the plank to come free");
        if (Expired()) InvalidWaiting("the plank's freeing");
        return;
    }
    const bool open = coop::prop_drive_host::ContextOf(prop, g_eid).open;
    if (!DG::Stable(g_rest, at, DG::Quiet(prop), kMarkTicks) || open) {
        Wait("the host's copy to rest and its drive to end");
        if (Expired()) InvalidWaiting("the host's copy's rest");
        return;
    }
    RC::Read(prop, g_hostRest);
    g_hostMask = D::JointMask(prop);
    g_hostDone = true;
    UE_LOGI("[PRIED-DRILL] [H] the host's copy RESTED at (%.1f, %.1f, %.1f), %.1f cm from where it hung; joints %02x",
            g_hostRest.loc.X, g_hostRest.loc.Y, g_hostRest.loc.Z, RC::Cm(g_before, g_hostRest),
            static_cast<unsigned>(g_hostMask));
    if (RC::Cm(g_before, g_hostRest) <= kMovedCm) { Invalid("the fixture did not move from where it hung: nothing was freed"); return; }
    SayDone();
    Next(JoinArm() ? Step::WaitJoiner : Step::WaitClient);
}

// The client's rested line: "<mask> <end> <pose>".
void HostJudgeClient(const std::string& rest) {
    if (rest.size() < 6 || rest[2] != ' ' || (rest[3] != '0' && rest[3] != '1') || rest[4] != ' ') {
        Invalid("the client's rested line could not be read");
        return;
    }
    const uint8_t mask = ParseHex(rest);
    RC::Pose client;
    if (!RC::Decode(rest.substr(5), client)) { Invalid("the client's rested pose could not be read"); return; }
    if (mask != g_hostMask) { Fail("the client's broken joints %02x differ from the host's %02x", mask, g_hostMask); return; }
    if (!RC::Judge(Tag(), "the pried fixture", g_hostRest, client)) { g_over = true; return; }
    if (rest[3] != '1') { Fail("no drive End after the pry reached the client's copy"); return; }
    Pass("the client's joints %02x equal the host's, its drive End arrived, and the copies rest within %.0f cm and %.0f deg",
         mask, RC::kBoundCm, RC::kBoundDeg);
}

// The joiner's rested line: "<mask> <pose>".
void HostJudgeJoiner(const std::string& rest) {
    if (rest.size() < 4 || rest[2] != ' ') { Invalid("the joiner's rested line could not be read"); return; }
    const uint8_t mask = ParseHex(rest);
    RC::Pose joiner;
    if (!RC::Decode(rest.substr(3), joiner)) { Invalid("the joiner's rested pose could not be read"); return; }
    if (mask != g_hostMask) { Fail("the joiner's broken joints %02x differ from the host's %02x", mask, g_hostMask); return; }
    if (!RC::Judge(Tag(), "the pried fixture at the joiner", g_hostRest, joiner)) { g_over = true; return; }
    Pass("the joiner's joints %02x equal the host's and its copy rests within %.0f cm and %.0f deg", mask, RC::kBoundCm,
         RC::kBoundDeg);
}

// A joiner's world coming up is said to again: the first life's lines are not its.
void HostEmitOnJoin(coop::net::Session& s) {
    bool rising = false;
    for (int i = 1; i < coop::net::kMaxPeers; ++i) {
        const bool r = SlotReady(s, i);
        if (r && !g_wasReady[i]) rising = true;
        g_wasReady[i] = r;
    }
    if (!rising || g_step <= Step::Bind || !g_prop.Get()) return;
    SayReady();
    if (g_hostDone) SayDone();
    UE_LOGI("[PRIED-DRILL] [H] a client's world is up: ready%s said again", g_hostDone ? " and done" : "");
}

void HostTick(coop::net::Session& s) {
    HostEmitOnJoin(s);
    switch (g_step) {
    case Step::WaitPeer: HostWaitPeer(s); return;
    case Step::Bind: HostBind(); return;
    case Step::WaitGrant: HostWaitGrant(); return;
    case Step::WaitFar: HostWaitFar(); return;
    case Step::WaitJoining: {  // the midjoin arm: the pry starts as the joiner's snapshot rows drain
        if (!JoinerDraining(s)) { Wait("a joiner's world up while its snapshot rows drain"); if (Expired()) InvalidWaiting("a joiner's draining"); return; }
        void* prop = g_prop.Get();
        if (!prop) { Invalid("the fixture died before the joiner came"); return; }
        UE_LOGI("[PRIED-DRILL] [H] a joiner's rows are draining");
        if (PryOwn(prop)) Next(Step::Watch);
        return;
    }
    case Step::Watch: HostWatch(); return;
    case Step::WaitClient: {
        std::string rest;
        if (!DC::SeenRestOf(kRestedPrefix, &rest)) { Wait("the client's rested line"); if (Expired()) InvalidWaiting("the client's rested line"); return; }
        HostJudgeClient(rest);
        Next(Step::Done);
        return;
    }
    case Step::WaitJoiner: {
        std::string rest;
        if (!DC::SeenRestOf(kJoinerPrefix, &rest)) { Wait("the joiner's rested line"); if (Expired()) InvalidWaiting("the joiner's rested line"); return; }
        HostJudgeJoiner(rest);
        Next(Step::Done);
        return;
    }
    default: return;
    }
}

// ---- the client ----------------------------------------------------------------------------------------------

// A drive End applied to this copy at or after `sinceMs`: the prop's own spawn fall may have one of its own.
bool EndSeen(void* prop, uint64_t sinceMs) {
    const uint32_t eid = static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(prop));
    coop::prop_drive_stream::AppliedEnd ends[coop::prop_drive_stream::kRecentEnds];
    const size_t n = coop::prop_drive_stream::RecentEnds(ends, coop::prop_drive_stream::kRecentEnds);
    for (size_t i = 0; i < n; ++i)
        if (ends[i].eid == eid && ends[i].atMs >= sinceMs) return true;
    return false;
}

void ClientFind() {
    void* prop = coop::prop_key_index::Find(std::wstring(g_key.begin(), g_key.end()));
    if (!prop || !R::IsLive(prop)) {
        Wait("this client's copy of the fixture to arrive");
        if (Expired()) InvalidWaiting("this client's copy of the fixture");
        return;
    }
    if (!D::WitnessesLive()) { Wait("the drill's witnesses to go live"); if (Expired()) InvalidWaiting("the witnesses' going live"); return; }
    g_prop.Set(prop);
    RC::Read(prop, g_before);
    g_endMarkMs = ::GetTickCount64();
    g_baseStats = coop::prop_pry_sync::GetStats();
    UE_LOGI("[PRIED-DRILL] [C] found its copy of '%s' at (%.1f, %.1f, %.1f); joints %02x already", g_key.c_str(),
            g_before.loc.X, g_before.loc.Y, g_before.loc.Z, static_cast<unsigned>(D::JointMask(prop)));
    switch (ArmOf()) {
    case Arm::Client: Next(Step::Pry); return;
    case Arm::Twice:  Next(Step::Done); return;   // the host alone acts and judges
    case Arm::MidJoin:
        if (!SecondLife()) {
            UE_LOGI("[PRIED-DRILL] [C] pried-drill: REJOIN-NOW the host's fixture hangs and this life has found its copy");
            Next(Step::Done);
            return;
        }
        Next(Step::WaitDone);
        return;
    default: Next(Step::WaitDone); return;
    }
}

// The client's pry, by the game's own route; the cancel and the ask are what it is judged on at the End's edge.
void ClientPry(void* me) {
    void* prop = g_prop.Get();
    const D::PryState st = D::StepRealPry(me, prop);
    if (st == D::PryState::Working) return;
    if (st == D::PryState::Failed) { Invalid(D::RealPryWhy()); return; }
    const coop::prop_pry_sync::Stats now = coop::prop_pry_sync::GetStats();
    const bool asked = now.asked == g_baseStats.asked + 1;
    const bool stuck = prop && (PR::IsStatic(prop) || PR::IsFrozen(prop));
    UE_LOGI("[PRIED-DRILL] [C] the pry's End ran: asked %llu, the copy is %s",
            static_cast<unsigned long long>(now.asked - g_baseStats.asked), stuck ? "still stuck" : "free");
    if (!asked || !stuck) {
        Fail("the client's own crowbarOpen ran on its copy instead of being asked of the host (asked %llu, copy %s)",
             static_cast<unsigned long long>(now.asked - g_baseStats.asked), stuck ? "still stuck" : "free");
        Next(Step::Done);
        return;
    }
    Next(Step::WaitDone);
}

void ClientTick(coop::net::Session& s) {
    void* player = coop::players::Registry::Get().Local();
    D::WitnessesLive();  // installed early: they must be live before the host's pry
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle || !player || !R::IsLive(player)) {
        Wait("this client's join to end and its pawn");
        return;
    }
    switch (g_step) {
    case Step::WaitPeer: Next(Step::WaitReady); return;
    case Step::WaitReady: {
        std::string rest;
        if (!DC::SeenRestOf(kReadyPrefix, &rest)) { Wait("the host's ready line"); if (Expired()) InvalidWaiting("the host's ready line"); return; }
        char token[16] = {}, key[64] = {};
        float x = 0.f, y = 0.f, z = 0.f;
        Fixture f;
        if (sscanf_s(rest.c_str(), "%15s %63s %f %f %f", token, static_cast<unsigned>(sizeof(token)), key,
                     static_cast<unsigned>(sizeof(key)), &x, &y, &z) != 5 ||
            !D::FixtureOfToken(token, f) || f != FixtureFor()) {
            Invalid("the host's ready line could not be read");
            return;
        }
        g_key = key;
        g_spot = {x, y, z};
        UE_LOGI("[PRIED-DRILL] [C] the host's %s '%s' hangs over (%.0f, %.0f, %.0f)", token, key, x, y, z);
        if (ArmOf() == Arm::Far) {
            // A raw ask for a plank no hand of this client's could reach: a crowbar in the hand, so reach alone refuses it.
            if (!D::TakeCrowbar()) { Invalid("the crowbar did not come into the hand"); return; }
            coop::net::PryAskPayload p{};
            coop::net::WireKeyFromString(std::wstring(g_key.begin(), g_key.end()), p.key);
            if (!s.SendReliable(coop::net::ReliableKind::PryAsk, &p, sizeof(p))) { Invalid("the raw ask was not sent"); return; }
            UE_LOGI("[PRIED-DRILL] [C] sent a raw PryAsk for '%s', %.0f cm from nobody it could reach", key, kFarCm);
            Next(Step::Done);
            return;
        }
        Next(Step::Find);
        return;
    }
    case Step::Find: ClientFind(); return;
    case Step::Pry: ClientPry(player); return;
    case Step::WaitDone: {
        std::string rest;
        if (!DC::SeenRestOf(kDonePrefix, &rest)) { Wait("the host's done line"); if (Expired()) InvalidWaiting("the host's done line"); return; }
        if (ArmOf() == Arm::Join && !SecondLife()) {
            UE_LOGI("[PRIED-DRILL] [C] pried-drill: REJOIN-NOW the host's radiator rests pried");
            Next(Step::Done);
            return;
        }
        Next(Step::Settle);
        return;
    }
    case Step::Settle: {
        void* prop = g_prop.Get();
        FVector at{};
        if (!prop || !E::TryGetActorLocation(prop, at)) { Invalid("this client's copy died before its rest"); return; }
        if (!DG::Stable(g_rest, at, DG::Quiet(prop), kMarkTicks)) {
            Wait("this client's copy to rest");
            if (Expired()) InvalidWaiting("this client's copy's rest");
            return;
        }
        RC::Pose pose;
        if (!RC::Read(prop, pose)) { Invalid("this client's copy's pose could not be read"); return; }
        const uint8_t mask = D::JointMask(prop);
        const bool endSeen = EndSeen(prop, g_endMarkMs);
        UE_LOGI("[PRIED-DRILL] [C] this client's copy RESTED at (%.1f, %.1f, %.1f); joints %02x; a drive End reached it: %d",
                pose.loc.X, pose.loc.Y, pose.loc.Z, static_cast<unsigned>(mask), endSeen ? 1 : 0);
        if (JoinArm()) DC::Say(kJoinerPrefix + Hex(mask) + " " + RC::Encode(pose));
        else DC::Say(kRestedPrefix + Hex(mask) + (endSeen ? " 1 " : " 0 ") + RC::Encode(pose));
        Next(Step::Done);
        return;
    }
    default: return;
    }
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || !session || !session->connected() || g_over || g_step == Step::Done) return;
    if (!g_armed) {
        // The first Seen* read starts the chat history: lines reach a peer while its join window is still open, and the feed
        // retires a row after about ten seconds.
        g_armed = true;
        g_stepMs = ::GetTickCount64();  // the first step's bound counts from here, not from an unset clock
        DC::SeenHas(kReadyPrefix);
        UE_LOGI("[PRIED-DRILL] [%c] started (arm %s): reading the chat history", Who(), Tag());
    }
    if (IsHost()) HostTick(*session);
    else ClientTick(*session);
}

void OnDisconnect() {
    // The join arms' host keeps its fixture across the first client's departure: the rejoin is the arm.
    if (IsHost() && JoinArm() && (g_step == Step::WaitJoiner || g_step == Step::WaitJoining)) return;
    g_step = Step::WaitPeer;
    g_armed = false;
    g_over = false;
    g_prop.Reset();
    g_key.clear();
    g_eid = 0;
    g_hostDone = false;
    g_hostMask = 0;
    g_endMarkMs = 0;
    for (bool& w : g_wasReady) w = false;
    DG::Restart(g_rest);
    D::Reset();
    g_lastWait[0][0] = g_lastWait[1][0] = '\0';
}

}  // namespace coop::dev::pried_drill
