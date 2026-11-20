// coop/dev/vent_probe.cpp -- see coop/dev/vent_probe.h.

#include "coop/dev/vent_probe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_footsteps.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::vent_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace DC = coop::dev::drill_chat;
namespace RV = coop::rig_verdict;
using ue_wrap::FVector;

constexpr const char* kTag = "vent-crawl";

bool IsHost() { return coop::roster::LocalIsHost(); }
char Who() { return IsHost() ? 'H' : 'C'; }

// The crawl's path: a route of kMinRouteCm..kMaxRouteCm to the nearest of eight compass points kProbeCm out, taken in a
// fixed order; the host walks it through the director's walk.
constexpr float kProbeCm      = 2500.f;
constexpr float kMinRouteCm   = 2000.f;
constexpr float kMaxRouteCm   = 5000.f;
constexpr float kNavExtentCm  = 300.f;
constexpr float kReachCm      = 150.f;
constexpr int   kWalkDeadlineS = 240;
// A frame's step this long is a warp, not walking (the stride's own guard): it is not counted into the distance.
constexpr float kMaxSampleCm  = coop::puppet_footsteps::Stride::kMaxSampleCm;
// Fewest steps the client must have counted before it judges, and the bands: within 20% of the crouched stride (PASS) or of
// the standing one (FAIL).
constexpr uint32_t kMinSteps  = 8;
constexpr float kCrouchedCm   = coop::puppet_footsteps::Stride::kStrideCm / coop::puppet_footsteps::Stride::kCrouchFactor;
constexpr float kStandingCm   = coop::puppet_footsteps::Stride::kStrideCm;
constexpr float kBandFrac     = 0.20f;
// The puppet is at the host's end point within this, and its step count has stood this many ticks: a count of ticks that
// the steps are all in (a settle), not a clock that decides a verdict.
constexpr float kNearCm       = 100.f;
constexpr int   kQuietTicks   = 60;
// A wait for the other peer or a state ends on its event; this bound only says INVALID.
constexpr uint64_t kBoundMs   = 120000;
// The client waits out the host's whole walk: the walk's own deadline and a minute (INVALID only).
constexpr uint64_t kWalkBoundMs = (kWalkDeadlineS + 60) * 1000ull;

const std::string kCrouchedLine = "vent crouched";
const std::string kArmedLine    = "vent armed";
const std::string kDonePrefix   = "vent done ";     // "<cm travelled> <x> <y> <z>"
const std::string kResultPrefix = "vent result ";   // "PASS|FAIL|INVALID <text>"
const std::string kInvalidLine  = "vent invalid";

enum class Step { WaitPeer, Crouch, WaitCrouched, WaitArmed, Walk, WaitResult, WaitHostCrouch, WaitDone, Settle, Done };

Step g_step = Step::WaitPeer;
bool g_armed = false;
bool g_over = false;
uint64_t g_stepMs = 0;
bool g_crouchCalled = false;
std::shared_ptr<coop::director::BackgroundWalk> g_walk;
float g_hostCm = 0.f;                  // the host's travelled distance on the crawl
FVector g_hostLast{};
bool g_haveLast = false;
uint32_t g_stepsAtArm = 0;             // the client's: the puppet's steps when it said armed
float g_hostCmSaid = 0.f;              // the client's: the distance the host said
FVector g_hostEnd{};                   // ... and where it stopped
uint32_t g_lastSteps = 0;
int g_quiet = 0;
char g_lastWait[2][160] = {};

void Next(Step s) {
    g_step = s;
    g_stepMs = ::GetTickCount64();
    g_quiet = 0;
}
bool Expired(uint64_t boundMs = kBoundMs) { return ::GetTickCount64() - g_stepMs > boundMs; }

void Wait(const char* what) {
    char* last = g_lastWait[IsHost() ? 0 : 1];
    if (std::strcmp(last, what) == 0) return;
    std::snprintf(last, sizeof(g_lastWait[0]), "%s", what);
    UE_LOGI("[VENT-DRILL] [%c] waiting for %s", Who(), what);
}

// Says the verdict once; the host's INVALID reaches the client as a line so it does not idle to its bound.
void Finish(RV::Outcome o, const char* why) {
    if (g_over) return;
    g_over = true;
    UE_LOGI("[VENT-DRILL] [%c] %s %s", Who(), o == RV::Outcome::Pass ? "PASS" : o == RV::Outcome::Fail ? "FAIL" : "INVALID",
            why);
    if (IsHost() && o == RV::Outcome::Invalid) DC::Say(kInvalidLine);
    RV::Say(kTag, o, "%s", why);
    UE_LOGI("[VENT-DRILL] [%c] DONE", Who());
}

void Invalid(const char* why) { Finish(RV::Outcome::Invalid, why); }
void InvalidWaiting(const char* what) {
    char why[200];
    std::snprintf(why, sizeof(why), "%s never came", what);
    Invalid(why);
}

float Planar(const FVector& a, const FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

// The route's length along the NavMesh points, from `start`.
float RouteCm(const FVector& start, const std::vector<FVector>& pts) {
    float len = 0.f;
    FVector at = start;
    for (const FVector& p : pts) {
        len += std::sqrt((p.X - at.X) * (p.X - at.X) + (p.Y - at.Y) * (p.Y - at.Y) + (p.Z - at.Z) * (p.Z - at.Z));
        at = p;
    }
    return len;
}

// The first compass point (+x, -x, +y, -y, then the four diagonals) whose NavMesh route from `me` is within the bounds.
bool ChooseGoal(void* me, FVector& goal) {
    FVector start{};
    if (!E::TryGetActorLocation(me, start)) return false;
    constexpr float kD = 0.70710678f;
    const float dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {kD, kD}, {kD, -kD}, {-kD, kD}, {-kD, -kD}};
    for (const auto& d : dirs) {
        FVector onNav{};
        if (!E::ProjectToNav(me, {start.X + d[0] * kProbeCm, start.Y + d[1] * kProbeCm, start.Z},
                             {kNavExtentCm, kNavExtentCm, kNavExtentCm}, &onNav))
            continue;
        std::vector<FVector> pts;
        if (!E::FindNavPath(me, start, onNav, pts)) continue;
        const float len = RouteCm(start, pts);
        if (len < kMinRouteCm || len > kMaxRouteCm) continue;
        goal = onNav;
        UE_LOGI("[VENT-DRILL] [H] the crawl's goal (%.0f,%.0f,%.0f), a route of %.0f cm", goal.X, goal.Y, goal.Z, len);
        return true;
    }
    return false;
}

bool CrouchNow(void* me) {
    ue_wrap::ParamFrame cf(R::FindDispatchFunctionCached(R::ClassOf(me), L"Crouch"));
    return cf.valid() && cf.Set<bool>(L"bClientSimulation", false) && ue_wrap::Call(me, cf);
}

// ---- The host: crouches, tells the client, walks the path, says the distance ---------------------------------------------

void HostTick(coop::net::Session& s) {
    void* me = coop::players::Registry::Get().Local();
    if (!me || !R::IsLive(me)) { Wait("the host's pawn"); return; }
    switch (g_step) {
    case Step::WaitPeer: {
        bool ready = false;
        for (int i = 1; i < coop::net::kMaxPeers && !ready; ++i) ready = s.IsSlotWorldReady(i);
        if (!ready) { Wait("a client slot that is world-ready"); return; }
        Next(Step::Crouch);
        return;
    }
    case Step::Crouch:
        if (!g_crouchCalled) {
            g_crouchCalled = true;
            UE_LOGI("[VENT-DRILL] [H] crouch ran=%d", CrouchNow(me) ? 1 : 0);
        }
        Next(Step::WaitCrouched);
        return;
    case Step::WaitCrouched:
        if (!ue_wrap::puppet::ReadCharacterIsCrouched(me)) {
            Wait("the host's pawn to read crouched");
            if (Expired()) InvalidWaiting("the host's pawn reading crouched");
            return;
        }
        DC::Say(kCrouchedLine);
        Next(Step::WaitArmed);
        return;
    case Step::WaitArmed: {
        if (!DC::SeenHas(kArmedLine)) {
            Wait("the client's armed line");
            if (Expired()) InvalidWaiting("the client's armed line");
            return;
        }
        FVector goal{};
        if (!ChooseGoal(me, goal)) { Invalid("no point of the NavMesh within the route bounds to crawl to"); return; }
        g_walk = coop::director::StartBackgroundWalk(goal, kReachCm, kWalkDeadlineS);
        g_haveLast = false;
        g_hostCm = 0.f;
        Next(Step::Walk);
        return;
    }
    case Step::Walk: {
        FVector at{};
        if (E::TryGetActorLocation(me, at)) {
            if (g_haveLast) {
                const float d = std::sqrt((at.X - g_hostLast.X) * (at.X - g_hostLast.X) + (at.Y - g_hostLast.Y) * (at.Y - g_hostLast.Y) +
                                          (at.Z - g_hostLast.Z) * (at.Z - g_hostLast.Z));
                if (d <= kMaxSampleCm) g_hostCm += d;
            }
            g_hostLast = at;
            g_haveLast = true;
        }
        const int state = g_walk ? g_walk->state.load() : 2;
        if (state == 0) { Wait("the director's walk to end"); return; }
        if (state == 2) { Invalid("the director's walk of the crawl did not arrive"); return; }
        if (!ue_wrap::puppet::ReadCharacterIsCrouched(me)) { Invalid("the host stood up during the crawl"); return; }
        if (g_hostCm < kMinRouteCm - kReachCm) {
            Invalid("the host travelled too short a distance to count steps over");
            return;
        }
        char line[96];
        std::snprintf(line, sizeof(line), "%.0f %.0f %.0f %.0f", g_hostCm, g_hostLast.X, g_hostLast.Y, g_hostLast.Z);
        DC::Say(kDonePrefix + line);
        UE_LOGI("[VENT-DRILL] [H] the crawl ended: %.0f cm travelled, crouched", g_hostCm);
        Next(Step::WaitResult);
        return;
    }
    case Step::WaitResult: {
        std::string rest;
        if (!DC::SeenRestOf(kResultPrefix, &rest)) {
            Wait("the client's result line");
            if (Expired()) InvalidWaiting("the client's result line");
            return;
        }
        // "<PASS|FAIL|INVALID> <text>"
        const RV::Outcome o = rest.rfind("PASS", 0) == 0 ? RV::Outcome::Pass : rest.rfind("FAIL", 0) == 0 ? RV::Outcome::Fail
                                                                                                           : RV::Outcome::Invalid;
        const size_t sp = rest.find(' ');
        Finish(o, sp == std::string::npos ? rest.c_str() : rest.c_str() + sp + 1);
        Next(Step::Done);
        return;
    }
    default: return;
    }
}

// ---- The client: counts the host's puppet's steps over the distance the host crawled ------------------------------------

coop::RemotePlayer* HostPuppet() {
    coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
    return p && p->valid() && p->actor() ? p : nullptr;
}

void ClientTick() {
    void* me = coop::players::Registry::Get().Local();
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle || !me || !R::IsLive(me)) {
        Wait("this client's join to end and its pawn");
        return;
    }
    // The host's INVALID is told here, so this client does not idle to its bound.
    if (DC::SeenHas(kInvalidLine)) { Invalid("the host could not run its crawl"); return; }
    switch (g_step) {
    case Step::WaitPeer: Next(Step::WaitHostCrouch); return;
    case Step::WaitHostCrouch: {
        if (!DC::SeenHas(kCrouchedLine)) {
            Wait("the host's crouched line");
            if (Expired()) InvalidWaiting("the host's crouched line");
            return;
        }
        coop::RemotePlayer* p = HostPuppet();
        if (!p) { Wait("the host's puppet"); if (Expired()) InvalidWaiting("the host's puppet"); return; }
        g_stepsAtArm = p->StepsPlayed();
        DC::Say(kArmedLine);
        UE_LOGI("[VENT-DRILL] [C] armed: the host's puppet has stepped %u times", g_stepsAtArm);
        Next(Step::WaitDone);
        return;
    }
    case Step::WaitDone: {
        std::string rest;
        if (!DC::SeenRestOf(kDonePrefix, &rest)) {
            Wait("the host's done line");
            if (Expired(kWalkBoundMs)) InvalidWaiting("the host's done line");
            return;
        }
        float x = 0.f, y = 0.f, z = 0.f;
        if (sscanf_s(rest.c_str(), "%f %f %f %f", &g_hostCmSaid, &x, &y, &z) != 4) { Invalid("the host's done line could not be read"); return; }
        g_hostEnd = {x, y, z};
        UE_LOGI("[VENT-DRILL] [C] the host crawled %.0f cm and stopped at (%.0f,%.0f,%.0f)", g_hostCmSaid, x, y, z);
        Next(Step::Settle);
        return;
    }
    case Step::Settle: {
        coop::RemotePlayer* p = HostPuppet();
        FVector at{};
        if (!p || !E::TryGetActorLocation(p->actor(), at)) { Invalid("the host's puppet could not be read"); return; }
        // The puppet is where the host stopped and its step count has stood: the crawl's steps are all counted.
        const uint32_t steps = p->StepsPlayed();
        g_quiet = (steps == g_lastSteps) ? g_quiet + 1 : 0;
        g_lastSteps = steps;
        if (Planar(at, g_hostEnd) > kNearCm || g_quiet < kQuietTicks) {
            Wait("the host's puppet to arrive and stop stepping");
            if (Expired()) InvalidWaiting("the host's puppet to arrive and stop stepping");
            return;
        }
        const uint32_t counted = steps - g_stepsAtArm;
        char text[200];
        if (counted < kMinSteps) {
            std::snprintf(text, sizeof(text), "the puppet stepped %u times over %.0f cm, too few to judge", counted, g_hostCmSaid);
            DC::Say(kResultPrefix + "INVALID " + text);
            Finish(RV::Outcome::Invalid, text);
            Next(Step::Done);
            return;
        }
        const float spacing = g_hostCmSaid / static_cast<float>(counted);
        const char* verdict = std::fabs(spacing - kCrouchedCm) <= kBandFrac * kCrouchedCm ? "PASS"
                            : std::fabs(spacing - kStandingCm) <= kBandFrac * kStandingCm ? "FAIL" : "INVALID";
        std::snprintf(text, sizeof(text), "%u steps over %.0f cm: one every %.0f cm (crouched %.0f cm, standing %.0f cm)", counted,
                      g_hostCmSaid, spacing, kCrouchedCm, kStandingCm);
        DC::Say(kResultPrefix + verdict + " " + text);
        Finish(verdict[0] == 'P' ? RV::Outcome::Pass : verdict[0] == 'F' ? RV::Outcome::Fail : RV::Outcome::Invalid, text);
        Next(Step::Done);
        return;
    }
    default: return;
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::vent_crawl_drill);
    if (!on || !session || !session->connected() || g_over || g_step == Step::Done) return;
    if (!g_armed) {
        g_armed = true;
        g_stepMs = ::GetTickCount64();
        DC::SeenHas(kCrouchedLine);   // the first Seen* read starts the chat history
        UE_LOGI("[VENT-DRILL] [%c] started: reading the chat history", Who());
    }
    if (IsHost()) HostTick(*session);
    else ClientTick();
}

void OnDisconnect() {
    g_step = Step::WaitPeer;
    g_armed = false;
    g_over = false;
    g_crouchCalled = false;
    g_walk.reset();
    g_hostCm = 0.f;
    g_haveLast = false;
    g_lastSteps = 0;
    g_quiet = 0;
    g_lastWait[0][0] = g_lastWait[1][0] = '\0';
}

}  // namespace coop::dev::vent_probe
