// coop/dev/cremator_drill_g2.cpp -- the G2 arms of the cremator drill: a client presses the machine's handle through the
// player's own seam, the press is refused on its copy and asked of the host (coop/interactables/cremator_intent), and the
// host judges and runs it. See coop/dev/cremator_drill.h for the arms and their lines.
//
// The client walks to the handle with the director, turns the camera until its own look-at trace sets the copy's
// lookingAt_handle, and presses as a player's E does: the entry verb actionOptionIndex(player, action 4) on the copy,
// which a door's press uses too (door::CallPress dispatches the instance's own override). The host counts how many
// times its own useLever ran (a drill-owned watch on the body), so a press the judge refused leaves the count where it
// was, and judges its copy's state; the client's copy is read back through peer_signal, as G1 does.

#include "cremator_drill_g2.h"  // co-located private header (src tree, not include/)

#include "harness/autotest/peer_signal.h"

#include "coop/dev/director/aim_fan.h"
#include "coop/dev/director/director.h"
#include "coop/interactables/cremator_intent.h"  // the judge's refusal counts
#include "coop/player/players_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/cremator.h"
#include "ue_wrap/devices/door.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_nav.h"
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::cremator_drill {

// The host's lever runs are counted at the body, on the host's own copy: the judge's call and a player's own pull both
// pass it, and a press the judge refused does not.
UE_FACT_FN(kRowUseLever, "dev.cremator_drill_g2", L"cremator_C", L"useLever", Declared);

namespace {

namespace C  = ue_wrap::cremator;
namespace D  = ue_wrap::door;
namespace E  = ue_wrap::engine;
namespace PS = harness::autotest::peer_signal;
namespace DR = coop::director;
namespace sg = ue_wrap::script_gate;
using namespace detail;
using PS::Await;
using PS::AwaitText;
using PS::Mark;

// The door's press and the cremator's handle press are one action value (4): the door's verb is the press used here.
static_assert(D::kUseAction == C::kPressAction, "the press verb's action is the handle's");

constexpr int kTagUseLever = 0x43443247;  // 'CD2G'

constexpr float kReachCm        = 120.f;   // the walk to the handle: the NavMesh route ends short of the machine's front
constexpr float kPaceCmPerSec   = 300.f;   // a walk's budget is twice a walk's pace over its route, plus a margin
constexpr int   kRouteMarginSec = 60;
constexpr int   kAimStepMs      = 30;      // the trace runs on the player's own tick: each pose is held across frames
constexpr int   kAimBoundSec    = 120;
constexpr int   kDoubleGapMs    = 200;     // g2-double: the second press follows the first by this
// g2-far: the host's reach is the sheet's 400 uu, the machine's bounds and a 600 uu pose-lag allowance. The walk-back
// point is picked at least kFarPickCm from the handle and the press is made only if the walker stands kFarMinCm or
// more from it, which is past that reach with the bounds' radius to spare.
constexpr float kFarPickCm      = 1800.f;
constexpr float kFarMinCm       = 1500.f;
constexpr float kFarRouteMaxCm  = 6000.f;
constexpr float kFarRingCm[]    = {1800.f, 2200.f, 2800.f};
constexpr int   kFarDirs        = 16;
constexpr int   kWalkWaitSec    = 900;     // the host waits out the client's walk legs, a fresh world's included

std::atomic<int> g_runs{0};

void OnUseLeverPost(const sg::Call& call) {
    if (call.object && C::IsCremator(call.object)) g_runs.fetch_add(1, std::memory_order_relaxed);
}

int Runs() { return g_runs.load(std::memory_order_relaxed); }

// The judge's refusals for `why` so far (host); its counters are the game thread's.
int DeniedSoFar(coop::cremator_intent::Denied why) {
    uint64_t n = 0;
    OnGameThread([&] { n = coop::cremator_intent::DeniedCount(why); });
    return static_cast<int>(n);
}

const char* DoneTag(const std::string& arm) { return arm == "g2" ? "G2" : arm == "g2-double" ? "G2-DOUBLE" : "G2-FAR"; }

float Between(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---- HOST ------------------------------------------------------------------------------------------------

// Registers the count of the host's lever runs and waits for it to be live. False (said) when the gate refused it.
bool WatchRuns() {
    bool registered = false;
    OnGameThread([&] { registered = sg::WatchName(kRowUseLever, kTagUseLever, nullptr, &OnUseLeverPost); });
    if (!registered) { UE_LOGW("cremator-drill: g2 the useLever watch did not register"); return false; }
    return PS::WaitFor("the useLever count's watch live", 60, [] { return sg::NameWatchLive(kRowUseLever, kTagUseLever); });
}

// The host's side of every G2 arm before the client's walk: the client is in, the handle is at rest with the run off,
// the door is shut (a start with the host's door open is refused) and the lever count is live. `key` names the machine.
bool HostSetup(std::wstring& key) {
    if (!Await("client_ready", kJoinSec)) return false;
    key = HostFindKey();
    if (key.empty()) return false;
    if (!PS::WaitFor("the host's handle at rest, run off", kHostRunSec, [&] { return CopyAt(key, 0); })) return false;
    bool ran = false, wasOpened = false;
    OnGameThread([&] {
        void* c = Resolve(key);
        ran = c && C::ShutDoor(c, wasOpened);
    });
    UE_LOGI("cremator-drill: HOST g2 shut the door ran=%d doorWasOpened=%d", ran ? 1 : 0, wasOpened ? 1 : 0);
    if (!ran) { Abort("the host's door did not shut"); return false; }
    if (!WatchRuns()) { Abort("the lever count is not live"); return false; }
    Mark("key", Narrow(key));
    return true;
}

// g2: a press asks ON, then a press asks OFF; the host's copy and the client's copy follow each, and the host's lever
// ran once for each.
bool HostG2(const std::wstring& key) {
    std::string pressed;
    if (!AwaitText("g2_on_pressed", pressed, kWalkWaitSec)) return false;
    const bool hostOn = PS::WaitFor("the host's run on", kHostRunSec, [&] { return CopyAt(key, 1); }, false);
    const int runsOn = Runs();
    Mark("g2_on");
    std::string onSeen;
    if (!AwaitText("g2_on_seen", onSeen, kSignalSec)) return false;
    const Reading on = Parse(onSeen);
    char what[420];
    if (!hostOn || runsOn != 1 || on.active != 1 || on.closed != 1) {
        Mark("g2_off_go", "stop");
        std::snprintf(what, sizeof(what),
                      "the ON press: the host's copy %s (on wanted), the host's lever ran %d time(s) (1 wanted), the client's copy '%s' (on, "
                      "closed wanted); the client said '%s'",
                      hostOn ? "reached on" : "never reached on", runsOn, onSeen.c_str(), pressed.c_str());
        Verdict(false, "g2", what);
        return true;
    }
    Mark("g2_off_go", "go");
    std::string offPressed;
    if (!AwaitText("g2_off_pressed", offPressed, kSignalSec)) return false;
    const bool hostOff = PS::WaitFor("the host's run off", kHostRunSec, [&] { return CopyAt(key, 0); }, false);
    const int runsOff = Runs();
    Mark("g2_off");
    std::string offSeen;
    if (!AwaitText("g2_off_seen", offSeen, kSignalSec)) return false;
    const Reading off = Parse(offSeen);
    std::snprintf(what, sizeof(what),
                  "ON: host copy on, lever runs %d (1 wanted), client copy '%s'; OFF: host copy %s, lever runs %d (2 wanted), client copy '%s'",
                  runsOn, onSeen.c_str(), hostOff ? "off" : "never off", runsOff, offSeen.c_str());
    Verdict(hostOff && runsOff == 2 && off.active == 0, "g2", what);
    return true;
}

// g2-double: two presses 0.2 s apart. The host's own handle is mid-travel when the second arrives, so the judge refuses
// it and the lever runs once. With the judge's handle check off (the red arm) the second press runs the lever again.
bool HostG2Double(const std::wstring& key) {
    std::string pressed;
    if (!AwaitText("g2d_pressed", pressed, kWalkWaitSec)) return false;
    const bool hostOn = PS::WaitFor("the host's run on", kHostRunSec, [&] { return CopyAt(key, 1); }, false);
    Mark("g2d_on");
    std::string seen;
    if (!AwaitText("g2d_on_seen", seen, kSignalSec)) return false;
    const Reading on = Parse(seen);
    const int runs = Runs();
    const int presses = FieldOf(pressed, "presses=");
    const int deniedMoving = DeniedSoFar(coop::cremator_intent::Denied::HandleBusyOrUnread);
    char what[500];
    std::snprintf(what, sizeof(what),
                  "the client made %d press(es) (2 wanted); the host's copy %s, its lever ran %d time(s) (1 wanted), the client's copy '%s'; "
                  "the judge refused %d press(es) for a moving or unread handle (1 or more wanted)",
                  presses, hostOn ? "reached on" : "never reached on", runs, seen.c_str(), deniedMoving);
    Verdict(presses == 2 && hostOn && runs == 1 && on.active == 1 && on.closed == 1 && deniedMoving >= 1, "g2-double", what);
    return true;
}

// g2-far: a press from beyond the host's reach, with the look-at forced (it measures the host's reach check, not the
// aim), must run nothing. The client then walks back, aims, presses again, and that press must run: the near press
// proves the lane is alive, so the far press's silence is the reach check and not a dead path.
bool HostG2Far(const std::wstring& key) {
    std::string farText;
    if (!AwaitText("g2f_far", farText, kWalkWaitSec)) return false;
    // The client walks back before it says it is ready: a run the far press made would show by now.
    if (!Await("g2f_near_ready", kWalkWaitSec)) return false;
    const int runsFar = Runs();
    const int deniedReach = DeniedSoFar(coop::cremator_intent::Denied::NotAuthorized);
    const Reading hostNow = [&] {
        Reading r;
        OnGameThread([&] { r = ReadCopy(key); });
        return r;
    }();
    char say[32];
    std::snprintf(say, sizeof(say), "runs=%d", runsFar);
    Mark("g2f_far_judged", say);
    char what[520];
    if (runsFar != 0) {
        std::snprintf(what, sizeof(what), "the far press ran the host's lever %d time(s) (0 wanted); the client said '%s'; the host's copy %s",
                      runsFar, farText.c_str(), Text(hostNow).c_str());
        Verdict(false, "g2-far", what);
        return true;
    }
    std::string nearPressed;
    if (!AwaitText("g2f_near_pressed", nearPressed, kSignalSec)) return false;
    const bool hostOn = PS::WaitFor("the host's run on", kHostRunSec, [&] { return CopyAt(key, 1); }, false);
    const int runsNear = Runs();
    Mark("g2f_on");
    std::string seen;
    if (!AwaitText("g2f_on_seen", seen, kSignalSec)) return false;
    const Reading on = Parse(seen);
    std::snprintf(what, sizeof(what),
                  "far press: %s, lever runs 0 (0 wanted), the authorisation refused %d press(es), reach among its reasons (1 or more wanted); near press: the host's "
                  "copy %s, lever runs %d (1 wanted), the client's copy '%s'",
                  farText.c_str(), deniedReach, hostOn ? "reached on" : "never reached on", runsNear, seen.c_str());
    Verdict(deniedReach >= 1 && hostOn && runsNear == 1 && on.active == 1 && on.closed == 1, "g2-far", what);
    return true;
}

// ---- CLIENT ----------------------------------------------------------------------------------------------

void* LocalPlayer() { return coop::players::Registry::Get().Local(); }

// A walk with the director to `to`, blocking this thread: its budget is twice a walk's pace over the longer of the route
// and the line, plus a margin for the stuck episodes. The director's stage places a walker that would cross the map at
// a known-good anchor first. False, with the ABORT line said, when it did not reach.
bool WalkTo(const ue_wrap::FVector& to, const char* what) {
    float route = 0.f, line = 0.f;
    OnGameThread([&] {
        void* me = LocalPlayer();
        ue_wrap::FVector from{};
        if (!me || !E::TryGetActorLocation(me, from)) return;
        std::vector<ue_wrap::FVector> pts;
        if (E::FindNavPath(me, from, to, pts))
            for (size_t i = 1; i < pts.size(); ++i) route += Between(pts[i - 1], pts[i]);
        line = Between(from, to);
    });
    const float way = (route > line) ? route : line;
    const int budget = static_cast<int>(way / kPaceCmPerSec * 2.f) + kRouteMarginSec;
    UE_LOGI("cremator-drill: CLIENT g2 walks to %hs (%.0f,%.0f,%.0f): line %.0f cm, route %.0f cm, %d s budget", what, to.X, to.Y,
            to.Z, line, route, budget);
    DR::DirectorGoal goal;
    goal.targetPos = to;
    goal.reachCm = kReachCm;
    goal.epoch = DR::WalkEpoch();
    DR::ControlManager mgr;
    DR::AddWalkToProcesses(mgr, goal);
    const bool reached = mgr.Run(goal, budget) && goal.reached;
    if (!reached) Abort("the client did not reach %s (%s)", what, goal.failReason);
    return reached;
}

// The press as a player's E: the entry verb on the copy with the handle's action, by the player. Game thread.
bool PressHandle(void* cremator, void* player) { return D::CallPress(cremator, player, D::kUseAction); }

// Turns the camera through the aim fan round the handle until the copy's look-at reads the handle, and presses in the
// same game-thread slot, so the press runs with the look-at it was aimed for. The fan's poses are held across frames
// (the trace runs on the player's own tick). False, with the ABORT line said, when no pose took the handle or the
// press did not dispatch. `text` is the log line's tail: what was struck and from how far.
bool AimAndPress(const std::wstring& key, std::string& text) {
    DR::AimFan fan;
    const auto since = std::chrono::steady_clock::now();
    for (;;) {
        enum : int { Working, Pressed, Lost, NoAim, NoPress } st = Working;
        float dist = -1.f;
        bool struck = false;
        OnGameThread([&] {
            void* me = LocalPlayer();
            void* c = Resolve(key);
            ue_wrap::FVector at{}, me3{};
            if (!me || !c || !C::HandleLocation(c, at) || !E::TryGetActorLocation(me, me3)) { st = Lost; return; }
            bool looking = false;
            C::ReadLookingAtHandle(c, looking);
            if (looking) {
                dist = Between(me3, at);
                struck = E::ReadMainPlayerHitComponent(me) == C::Handle(c);
                st = PressHandle(c, me) ? Pressed : NoPress;
                return;
            }
            if (fan.Tick(me, at, false) == DR::AimFan::State::Failed) st = NoAim;
        });
        if (st == Pressed) {
            char t[96];
            std::snprintf(t, sizeof(t), "dist=%.0f struck=%d poses=%d", dist, struck ? 1 : 0, fan.Poses());
            text = t;
            UE_LOGI("cremator-drill: CLIENT g2 pressed (lookingAt_handle=1) %s", t);
            return true;
        }
        if (st != Working) {
            Abort("the client's %s", st == Lost ? "player or machine is gone"
                  : st == NoAim ? "aim fan never made the look-at read the handle" : "press did not dispatch");
            return false;
        }
        if (std::chrono::steady_clock::now() - since > std::chrono::seconds(kAimBoundSec)) {
            Abort("the client's look-at never read the handle in %d s", kAimBoundSec);
            return false;
        }
        ::Sleep(kAimStepMs);
    }
}

// The handle's world location; game thread. False, said, when the machine or its handle does not read.
bool HandleAt(const std::wstring& key, ue_wrap::FVector& at) {
    bool ok = false;
    OnGameThread([&] {
        void* c = Resolve(key);
        ok = c && C::HandleLocation(c, at);
    });
    if (!ok) Abort("the cremator's handle component does not read");
    return ok;
}

// The NavMesh point at least kFarPickCm from the handle with the shortest route from where the walker stands, among a
// ring of candidates round the handle. Game thread. False when none has a route within kFarRouteMaxCm.
bool PickFarPoint(const ue_wrap::FVector& handle, ue_wrap::FVector& out, float& routeCm) {
    void* me = LocalPlayer();
    ue_wrap::FVector from{};
    if (!me || !E::TryGetActorLocation(me, from)) return false;
    float best = kFarRouteMaxCm + 1.f;
    for (const float ring : kFarRingCm) {
        for (int k = 0; k < kFarDirs; ++k) {
            const float a = 6.2831853f * static_cast<float>(k) / static_cast<float>(kFarDirs);
            const ue_wrap::FVector want{handle.X + ring * std::cos(a), handle.Y + ring * std::sin(a), handle.Z};
            ue_wrap::FVector on{};
            if (!E::ProjectToNav(me, want, ue_wrap::FVector{500.f, 500.f, 400.f}, &on)) continue;
            if (Between(on, handle) < kFarPickCm) continue;
            std::vector<ue_wrap::FVector> pts;
            if (!E::FindNavPath(me, from, on, pts) || pts.empty()) continue;
            float len = 0.f;
            for (size_t i = 1; i < pts.size(); ++i) len += Between(pts[i - 1], pts[i]);
            if (Between(pts.back(), on) > kReachCm || len >= best) continue;
            best = len;
            out = on;
            routeCm = len;
        }
    }
    return best <= kFarRouteMaxCm;
}

// Presses the handle with the look-at written true on the copy, from wherever the walker stands: the arm measures the
// host's reach check, not the aim. The write is cleared again after the press, so the later aim reads the trace's own.
// `text` is the walker's distance to the handle and whether the press dispatched. False when the walker stands nearer
// than kFarMinCm or the write or the press did not run (the ABORT line says which).
bool PressForced(const std::wstring& key, std::string& text) {
    float dist = -1.f;
    int wrote = 0, dispatched = 0;
    OnGameThread([&] {
        void* me = LocalPlayer();
        void* c = Resolve(key);
        ue_wrap::FVector at{}, me3{};
        if (!me || !c || !C::HandleLocation(c, at) || !E::TryGetActorLocation(me, me3)) return;
        dist = Between(me3, at);
        if (dist < kFarMinCm) return;
        wrote = C::WriteLookingAtHandle(c, true) ? 1 : 0;
        dispatched = wrote && PressHandle(c, me) ? 1 : 0;
        C::WriteLookingAtHandle(c, false);
    });
    char t[96];
    std::snprintf(t, sizeof(t), "dist=%.0f forced=%d dispatched=%d", dist, wrote, dispatched);
    text = t;
    if (!dispatched) {
        Abort("the forced far press did not run (%s; %.0f cm wanted at least)", t, kFarMinCm);
        return false;
    }
    UE_LOGI("cremator-drill: CLIENT g2 pressed from afar (lookingAt_handle forced to 1) %s", t);
    return true;
}

// g2-double's second press, kDoubleGapMs after the first, on the same aim. False when it did not dispatch.
bool PressAgain(const std::wstring& key) {
    ::Sleep(kDoubleGapMs);
    bool ran = false, looking = false;
    OnGameThread([&] {
        void* me = LocalPlayer();
        void* c = Resolve(key);
        if (!me || !c) return;
        C::ReadLookingAtHandle(c, looking);
        ran = PressHandle(c, me);
    });
    UE_LOGI("cremator-drill: CLIENT g2 pressed again %d ms later (lookingAt_handle=%d) dispatched=%d", kDoubleGapMs, looking ? 1 : 0,
            ran ? 1 : 0);
    return ran;
}

void ClientG2(const std::wstring& key) {
    ue_wrap::FVector handle{};
    if (!HandleAt(key, handle) || !WalkTo(handle, "the handle")) return;
    std::string text;
    if (!AimAndPress(key, text)) return;
    Mark("g2_on_pressed", text);
    if (!Await("g2_on", kSignalSec)) return;
    ReportCopy(key, 1, "g2_on_seen");
    std::string go;
    if (!AwaitText("g2_off_go", go, kSignalSec)) return;
    if (go == "go") {
        if (!AimAndPress(key, text)) return;
        Mark("g2_off_pressed", text);
        if (!Await("g2_off", kSignalSec)) return;
        ReportCopy(key, 0, "g2_off_seen");
    }
    UE_LOGI("cremator-drill: DONE on client");
}

void ClientG2Double(const std::wstring& key) {
    ue_wrap::FVector handle{};
    if (!HandleAt(key, handle) || !WalkTo(handle, "the handle")) return;
    std::string text;
    if (!AimAndPress(key, text)) return;
    text += PressAgain(key) ? " presses=2" : " presses=1";
    Mark("g2d_pressed", text);
    if (!Await("g2d_on", kSignalSec)) return;
    ReportCopy(key, 1, "g2d_on_seen");
    UE_LOGI("cremator-drill: DONE on client");
}

void ClientG2Far(const std::wstring& key) {
    ue_wrap::FVector handle{};
    if (!HandleAt(key, handle) || !WalkTo(handle, "the handle")) return;
    ue_wrap::FVector farAt{};
    float routeCm = 0.f;
    bool picked = false;
    OnGameThread([&] { picked = PickFarPoint(handle, farAt, routeCm); });
    if (!picked) {
        Abort("no NavMesh point %.0f cm or more from the handle within a %.0f cm route", kFarPickCm, kFarRouteMaxCm);
        return;
    }
    UE_LOGI("cremator-drill: CLIENT g2 far point (%.0f,%.0f,%.0f), route %.0f cm", farAt.X, farAt.Y, farAt.Z, routeCm);
    if (!WalkTo(farAt, "the far point")) return;
    std::string farText;
    if (!PressForced(key, farText)) return;
    Mark("g2f_far", farText);
    if (!WalkTo(handle, "the handle again")) return;
    Mark("g2f_near_ready");
    std::string judged;
    if (!AwaitText("g2f_far_judged", judged, kSignalSec)) return;
    if (FieldOf(judged, "runs=") != 0) {
        UE_LOGI("cremator-drill: CLIENT g2 the far press ran the host's lever (%s)", judged.c_str());
        UE_LOGI("cremator-drill: DONE on client");
        return;
    }
    std::string text;
    if (!AimAndPress(key, text)) return;
    Mark("g2f_near_pressed", text);
    if (!Await("g2f_on", kSignalSec)) return;
    ReportCopy(key, 1, "g2f_on_seen");
    UE_LOGI("cremator-drill: DONE on client");
}

}  // namespace

void RunHostG2(const std::string& arm) {
    std::wstring key;
    if (!HostSetup(key)) return;
    const bool judged = arm == "g2" ? HostG2(key) : arm == "g2-double" ? HostG2Double(key) : HostG2Far(key);
    if (judged) UE_LOGI("cremator-drill: %hs DONE fails=%d", DoneTag(arm), Fails());
}

void RunClientG2(const std::string& arm) {
    if (!HostPuppetUp()) return;
    Mark("client_ready");
    std::string keyText;
    if (!AwaitText("key", keyText, kSignalSec)) return;
    const std::wstring key = Widen(keyText);
    if (!CopyResolves(key)) return;
    if (arm == "g2") ClientG2(key);
    else if (arm == "g2-double") ClientG2Double(key);
    else ClientG2Far(key);
}

}  // namespace coop::dev::cremator_drill
