// coop/dev/container_intent_drill_throw_into.cpp -- the CLIENT's half of the container intent drill's throwinto arm: does a
// prop the client lets fall into a shared basket land as exactly one record on the host, with no live copy left on either
// peer and the client's mirror of the basket converged? Five sub-runs, each judged on its own; the arm MEASURES, it never
// works around a no.
//   n=1 thrown  -- a drive in the hotbar hand, thrown with throwHoldingProp from about kIntoStandCm (the director walks there).
//   n=2 control -- a NEW drive, grabbed and brought over the control's OWN basket (read 0) by a closed loop, let go with dropGrabObject.
//   n=3 held    -- the same grab lowered INTO the box and never let go: the host files the parked copy and the client's grab ends.
//   n=4 taken   -- a basket the fixture did not arm; the client takes its one record on the screen (the host's take arms the box),
//                  then throws as n=1 (container_intent_drill_throw_into_take.cpp).
//   n=5 observed -- the same with two clients: the first takes and watches, the second throws (container_intent_drill_throw_into_observer.cpp).
// A try whose prop rests outside the basket is a `miss`, not a verdict: the host says where it rested, the client moves its
// standpoint by the miss and tries again with a NEW drive, at most kTries tries per sub-run. The host judges
// (container_intent_drill_throw_into_host.cpp, _verdict.cpp) from both peers' reads taken in one window; the act and its end are in
// container_intent_drill_throw_into_release.cpp, the shared trace in container_intent_drill_throw.h.

#include "container_intent_drill_throw_into.h"
#include "container_intent_drill_throw_into_stand.h"
#include "container_route_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/container_route_census.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_act_request.h"
#include "coop/props/prop_drive_stream.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflected_offset.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace DR  = coop::director;
namespace DC  = coop::dev::drill_chat;
namespace RD  = coop::dev::container_route_drill::detail;
namespace AR  = coop::props::container_act_request;
using namespace into;

constexpr int      kWalkDeadlineS   = 150;
constexpr int      kRestChecks      = 4;
constexpr float    kIntoMinCm       = 40.f;    // a corrected thrown standpoint is kept in [this, kIntoMaxCm] from the basket
constexpr float    kIntoMaxCm       = 300.f;
constexpr float    kThroughBeyondCm = 150.f;   // the through try aims this far past the box's centre, on the line from the standpoint
constexpr uint64_t kHandBoundMs     = 10000;
constexpr uint64_t kGrabBoundMs     = 20000;
constexpr uint64_t kItemBoundMs     = 60000;   // the control's drive: the host's line, then its mirror at rest
constexpr uint64_t kMirrorArmBoundMs = 2000;   // the host's box armed to the mirror's box reading on (a sweep and the wire)
constexpr uint64_t kHeldBoundMs      = 60000;  // a held fixture failure: the host's measure tail begins within this, or the failure is said

Step     g_step = Step::WaitLine;
uint64_t g_stepMs = 0, g_nextMs = 0;
uint32_t g_itemEid = 0;
FVector  g_packAt{}, g_goal{}, g_lastPos{};
float    g_wantCm = 0.f;   // a retry's corrected distance from the basket; 0: kIntoStandCm
int      g_rest = 0, g_settled = 0;
bool     g_packSwitch = false;   // the control's own basket was named: its mirror is awaited before anything is aimed at it
bool     g_through = false;      // the try is the one aimed THROUGH the box and on (measured by m4, not judged)
bool     g_preDone = false;      // n=4 and n=5: the basket was taken from, and its box read armed
bool     g_isB = false;          // the second client of a two-client run: idle through n=1..4, the thrower of n=5
int      g_trigBefore = -1;      // the basket's trigger state before dingus()
bool     g_armThenTry = false;   // the armed basket goes on to the try (the first), not back to the control's drive wait
std::shared_ptr<DR::BackgroundWalk> g_walk;
std::unique_ptr<DR::AimedGrab> g_grab;
char     g_heldFail[300] = "";   // a fixture failure held until the arm's end: before n=5 (HoldEarly), or until the arming measure's tail is over

void WalkStop() {
    if (g_walk && g_walk->state.load() == 0) DR::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
}

}  // namespace

// The client's state the release group (container_intent_drill_throw_into_release.cpp) reads and writes, and the three verbs it ends a
// step with.
namespace into {

ClientRun g_run;

void ClientNext(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
bool ClientExpired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }

namespace {

// A fixture failure before n=5 is held, the probe on or off: said to the host as the sub-run's verdict (`prefail`, with the reason), which
// holds it with the other lines and goes on to the next sub-run, while this peer waits for that sub-run's `run` line. The client's own FAIL
// is said at the arm's end (ClientEnd), so the rig's first-FAIL abort cannot end the run before n=5. n=5 is the last sub-run: its failure
// ends the arm at once.
bool HoldEarly(const char* reason) {
    if (g_run.r >= kLastRun || g_step == Step::Tail || g_step == Step::Held || g_step == Step::Done) return false;
    if (!g_heldFail[0]) std::snprintf(g_heldFail, sizeof(g_heldFail), "%s", reason);
    DC::Say(Ln("client", "prefail", g_run.r, g_run.t, true) + std::string(reason).substr(0, 120));
    UE_LOGI("[CI-DRILL] client %s n=%d try %d: the fixture stopped (%s); said to the host, the arm goes on", ArmName(), g_run.r, g_run.t, reason);
    HoldReset();
    ClientNext(Step::WaitNext);
    return true;
}

// The arm is over: a fixture failure held on the way (HoldEarly, or the arming measure's hold) is said now, else the client measured.
void ClientEnd() {
    if (g_heldFail[0]) {
        Fail("%s", g_heldFail);
    } else {
        UE_LOGI("[CI-DRILL] client %s MEASURED", ArmName());
        Finish();
    }
    ClientNext(Step::Done);
}

}  // namespace

// The client's end with a FAIL line when the host's answer is what failed, or the arm cannot go on: no hold.
void ClientFailNow(const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    WalkStop();
    g_grab.reset();
    DC::Say(kClientFailedLine);
    Fail("%s", g_heldFail[0] ? g_heldFail : buf);
    ClientNext(Step::Done);
}

// The client's end with the FAIL line. Before n=5 the failure is held (HoldEarly); at n=5 and after, with the arming measure on, it waits for
// the host's tail and is said after it. A failure already held is never said here: this one joins the wait, and the held line is said when
// the host's DONE is read (StepHeld, ClientEnd), never at a sub-run's start.
void ClientFail(const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    WalkStop();
    g_grab.reset();
    if (HoldEarly(buf)) return;
    DC::Say(kClientFailedLine);
    if ((ArmingOn() || g_heldFail[0]) && g_step != Step::Tail && g_step != Step::Held && g_step != Step::Done) {
        if (!g_heldFail[0]) std::snprintf(g_heldFail, sizeof(g_heldFail), "%s", buf);
        UE_LOGI("[CI-DRILL] client %s: the arm's fixture stopped (%s); waiting for the host's measure tail, the FAIL is said after it", ArmName(), buf);
        ClientNext(Step::Held);
        return;
    }
    Fail("%s", g_heldFail[0] ? g_heldFail : buf);
    ClientNext(Step::Done);
}

}  // namespace into

namespace {

void* HeldActor(void* player) {
    E::MainPlayerGrabState hand{};
    return player && E::ReadMainPlayerGrabState(player, hand) ? hand.holdingActor : nullptr;
}

uint32_t WireEid(void* actor) {
    const uint32_t eid = RD::EidOf(actor);
    return eid == static_cast<uint32_t>(coop::element::kInvalidId) ? 0u : eid;
}

void StartWalk(bool carry, float reachCm) {
    g_walk = DR::StartBackgroundWalk(g_goal, reachCm, kWalkDeadlineS, carry);
    UE_LOGI("[CI-DRILL] client %s n=%d try %d walks%s to (%.0f,%.0f,%.0f), target at (%.0f,%.0f,%.0f)", ArmName(), g_run.r, g_run.t,
            carry ? " carrying" : "", static_cast<double>(g_goal.X), static_cast<double>(g_goal.Y), static_cast<double>(g_goal.Z),
            static_cast<double>(g_packAt.X), static_cast<double>(g_packAt.Y), static_cast<double>(g_packAt.Z));
    ClientNext(Step::WalkWait);
}

// The host is asked for a NEW drive beside `at`; its line and its mirror come next.
void SayNeed(const FVector& at) {
    char say[64];
    std::snprintf(say, sizeof(say), "at=%d,%d,%d", static_cast<int>(at.X), static_cast<int>(at.Y), static_cast<int>(at.Z));
    DC::Say(Ln("client", "need", g_run.r, g_run.t, true) + say);
    g_itemEid = 0;
    ClientNext(Step::ItemWait);
}

// A try begins: the thrown one walks to its standpoint; the control asks the host for a drive beside where it stands.
void BeginTry() {
    g_settled = g_rest = 0;
    if (ThrownRun(g_run.r) && (!TakenRun(g_run.r) || g_preDone)) {
        void* player = coop::players::Registry::Get().Local();
        const float want = g_wantCm > 0.f ? g_wantCm : kIntoStandCm;
        StandTally tally;
        if (!player || !FindStandWide(player, g_run.pack.Get(), g_run.r, g_run.t, want, kStandReachCm, g_goal, tally)) {
            char fan[160];
            TallyText(tally, fan, sizeof(fan));
            ClientFail("no standpoint %.0f..%.0f cm from the basket reachable by a route with a clear line to its mouth (%s)", static_cast<double>(want),
                  static_cast<double>(kIntoWiden2Cm), fan);
            return;
        }
        FVector now{};
        if (g_wantCm > 0.f && E::TryGetActorLocation(player, now)) {   // a moved standpoint: said how far the walk to it is, and whether it is a walk at all
            const float away = std::hypot(g_goal.X - now.X, g_goal.Y - now.Y);
            UE_LOGI("[CI-DRILL] client %s n=%d try %d: the moved standpoint (%.0f,%.0f,%.0f) is %.0f cm from where the player stands now, %s", ArmName(),
                    g_run.r, g_run.t, static_cast<double>(g_goal.X), static_cast<double>(g_goal.Y), static_cast<double>(g_goal.Z), static_cast<double>(away),
                    away <= kStandReachCm ? "inside the walk's reach, so the walk does not move the player" : "walked to");
        }
        g_wantCm = 0.f;
        StartWalk(/*carry=*/false, kStandReachCm);
        return;
    }
    void* player = coop::players::Registry::Get().Local();
    FVector at{};
    if (!player || !E::TryGetActorLocation(player, at)) {
        ClientFail("the player's place does not read");
        return;
    }
    if (TakenRun(g_run.r)) {   // n=4 and n=5: the basket first, then the take that arms it (the second client of n=5 only watches it)
        PreBegin(g_run.r, at, !g_isB);
        ClientNext(Step::Pre);
        return;
    }
    SayNeed(at);
}

// The basket's mirror is here and at rest: its trigger is the host's, read from the publication's flag (container_arming), never
// armed by this peer; the try begins once the mirror's box reads on. A fresh basket's `overlap` is NoCollision until the host arms its own.
void StartArm(bool thenTry) {
    void* pack = g_run.pack.Get();
    g_trigBefore = TriggerState(pack);
    ArmingTrack(pack);   // the arming measure: the mirror's box joins the hook's set
    g_armThenTry = thenTry;
    ClientNext(Step::Arming);
}

void StepArming() {
    const int after = TriggerState(g_run.pack.Get());
    if (after <= 0 && !UnarmedOn()) {   // the red container_throwinto_unarmed: the host armed nothing, so the box is not waited for
        if (ClientExpired(kMirrorArmBoundMs))
            ClientFail("the mirror's box did not arm from the host's flag (collision enabled %d -> %d within %llu ms)", g_trigBefore, after,
                  U(kMirrorArmBoundMs));
        return;
    }
    UE_LOGI("[CI-DRILL] client %s: the basket's mirror box read collision-enabled %d before the host's flag and %d after it (0 none, 1 query "
            "only, 2 physics only, 3 both)", ArmName(), g_trigBefore, after);
    if (g_armThenTry) BeginTry();
    else ClientNext(Step::ItemWait);
}

void StepWaitLine() {
    if (SecondClient()) {   // n=5's thrower: it idles through n=1..4
        g_isB = true;
        UE_LOGI("[CI-DRILL] client %s: this is the second client; it idles until n=5", ArmName());
        ClientNext(Step::Idle);
        return;
    }
    std::string rest;
    if (DC::SeenRestOf(kFixtureLine, &rest)) {
        int pack = 0, item = 0;
        if (!RD::IntAfter(rest, "pack", pack) || !RD::IntAfter(rest, "item", item) || pack <= 0 || item <= 0) {
            ClientFail("the host's fixture line does not read (%s)", rest.c_str());
            return;
        }
        g_run.packEid = static_cast<uint32_t>(pack);
        ClientNext(Step::WaitMirrors);
        return;
    }
    if (ClientExpired(kPeerBoundMs)) ClientFail("the host never said its fixture within %llu s", U(kPeerBoundMs / 1000));
}

void StepWaitMirrors() {
    void* pack = coop::element::LivePropActor(g_run.packEid);
    void* player = coop::players::Registry::Get().Local();
    if (pack && player && E::TryGetActorLocation(pack, g_packAt)) {
        g_run.pack.Set(pack);
        g_rest = 0;
        UE_LOGI("[CI-DRILL] client %s: the basket's mirror is here (eid=%u)", ArmName(), g_run.packEid);
        ClientNext(Step::Rest);
        return;
    }
    if (ClientExpired(kPeerBoundMs)) ClientFail("the basket's mirror never came (target %p, player %p)", pack, player);
}

// The host's drive of the fixture (it falls and settles) is over once the basket's mirror stays put for kRestChecks checks.
void StepRest() {
    FVector at{};
    if (!g_run.pack.Get() || !E::TryGetActorLocation(g_run.pack.Get(), at)) {
        ClientFail("the basket's mirror died or does not read");
        return;
    }
    const bool still = std::fabs(at.X - g_lastPos.X) + std::fabs(at.Y - g_lastPos.Y) + std::fabs(at.Z - g_lastPos.Z) < 1.f;
    g_lastPos = at;
    g_rest = still ? g_rest + 1 : 0;
    if (g_rest < kRestChecks) {
        if (ClientExpired(kPeerBoundMs)) ClientFail("the basket's mirror never came to rest");
        return;
    }
    g_packAt = at;
    StartArm(/*thenTry=*/true);
}

// The control's drive: the host's line, then its mirror, let go by the host's birth coast (not parked) and at rest.
void StepItemWait() {
    if (!g_itemEid) {
        std::string rest;
        int eid = 0;
        int pack = 0;
        if (DC::SeenRestOf(Ln("host", "item", g_run.r, g_run.t, true), &rest) && RD::IntAfter(rest, "eid", eid) && eid > 0) {
            g_itemEid = static_cast<uint32_t>(eid);
            g_packSwitch = RD::IntAfter(rest, "pack", pack) && pack > 0 && static_cast<uint32_t>(pack) != g_run.packEid;
            if (g_packSwitch) {
                g_run.packEid = static_cast<uint32_t>(pack);
                g_run.pack.Reset();
            }
            g_rest = 0;
            g_lastPos = FVector{};
        }
    }
    if (g_packSwitch) {   // the control's own basket: its mirror here and at rest before anything is aimed at it
        void* pack = coop::element::LivePropActor(g_run.packEid);
        FVector at{};
        if (pack && E::TryGetActorLocation(pack, at)) {
            const bool still = std::fabs(at.X - g_lastPos.X) + std::fabs(at.Y - g_lastPos.Y) + std::fabs(at.Z - g_lastPos.Z) < 1.f;
            g_lastPos = at;
            g_rest = still ? g_rest + 1 : 0;
            if (g_rest >= kRestChecks) {
                g_run.pack.Set(pack);
                g_packAt = at;
                g_packSwitch = false;
                g_rest = 0;
                g_lastPos = FVector{};
                UE_LOGI("[CI-DRILL] client %s n=%d: the control's own basket (eid=%u) is here and at rest", ArmName(), g_run.r, g_run.packEid);
                StartArm(/*thenTry=*/false);   // the new basket's trigger is armed before the drive is waited for
                return;
            }
        }
        if (g_packSwitch && ClientExpired(kItemBoundMs)) ClientFail("the control's basket never came to rest (eid %u)", g_run.packEid);
        return;
    }
    bool parked = false;
    if (g_itemEid) {
        void* item = coop::element::LivePropActor(g_itemEid);
        void* player = coop::players::Registry::Get().Local();
        FVector at{};
        if (item && player && E::TryGetActorLocation(item, at)) {
            parked = coop::prop_drive_stream::IsParked(item);
            const bool still = !parked && std::fabs(at.X - g_lastPos.X) + std::fabs(at.Y - g_lastPos.Y) + std::fabs(at.Z - g_lastPos.Z) < 1.f;
            g_lastPos = at;
            g_rest = still ? g_rest + 1 : 0;
            if (g_rest >= kRestChecks) {
                UE_LOGI("[CI-DRILL] client %s n=%d try %d: the host's drive (eid=%u) is unparked and at rest", ArmName(), g_run.r,
                        g_run.t, g_itemEid);
                g_run.item.Set(item);
                if (ThrownRun(g_run.r)) {   // the thrown try's drive is the host's, at rest beside the standpoint; the hand takes the hold
                    ClientNext(Step::TakeHand);
                    return;
                }
                g_grab = std::make_unique<DR::AimedGrab>(player, item);
                ClientNext(Step::Grab);
                return;
            }
        }
    }
    if (ClientExpired(kItemBoundMs))
        ClientFail("the control's drive never came to rest unparked (eid %u, %s)", g_itemEid, !g_itemEid ? "no host line" : parked ? "still parked" : "moving");
}

// The arm line: the drive in hand (or grabbed) is named to the host, which takes its baselines and answers armed.
void SayArm(void* held) {
    const char* dead = nullptr;
    if (cen::SeamsState(&dead) != cen::Seams::Live) {
        ClientFail("the census's watches are not all live (%s)", dead ? dead : "pending");
        return;
    }
    char say[32];
    std::snprintf(say, sizeof(say), "held=%u", WireEid(held));
    DC::Say(Ln("client", "arm", g_run.r, g_run.t, true) + say);
    ClientNext(Step::ArmWait);
}

void StepGrab() {
    if (!g_grab) {
        ClientFail("the grab is not running");
        return;
    }
    switch (g_grab->Tick()) {
    case DR::GrabState::Working:
        if (ClientExpired(kGrabBoundMs)) ClientFail("the grab never took within %llu s", U(kGrabBoundMs / 1000));
        return;
    case DR::GrabState::Failed:
        ClientFail("the grab did not take: %s, chain pre/use/post %d/%d/%d", g_grab->Why(), g_grab->ChainPre() ? 1 : 0,
              g_grab->ChainUse() ? 1 : 0, g_grab->ChainPost() ? 1 : 0);
        return;
    case DR::GrabState::Grabbed: {
        UE_LOGI("[CI-DRILL] client %s: the drive is grabbed after %d aim pose(s)", ArmName(), g_grab->AimPoses());
        g_grab.reset();
        // The hold point is the camera plus its forward times grabLen: with the camera turned at the point over the basket, the
        // standpoint is where the camera is kHoldDistCm from that point. StepHold sets grabLen to the distance it measures.
        FVector at{};
        void* player = coop::players::Registry::Get().Local();
        if (!player || !E::TryGetActorLocation(player, at) || !HoldReadPoint(g_run.pack.Get(), g_run.r == 3, g_run.item.Get())) {
            ClientFail("the carry's standpoint does not read");
            return;
        }
        const float standCm = HoldStandCm(kIntoMinCm);
        UE_LOGI("[CI-DRILL] client %s n=%d try %d: the standpoint is %.0f cm from the hold point", ArmName(), g_run.r, g_run.t,
                static_cast<double>(standCm));
        StandTally tally;
        if (!FindStandWide(player, g_run.pack.Get(), g_run.r, g_run.t, standCm, kStandReachCm, g_goal, tally)) {
            char fan[160];
            TallyText(tally, fan, sizeof(fan));
            ClientFail("no standpoint %.0f..%.0f cm from the basket reachable by a route with a clear line to its mouth (%s)", static_cast<double>(standCm),
                  static_cast<double>(kIntoWiden2Cm), fan);
            return;
        }
        StartWalk(/*carry=*/true, kStandReachCm);
        return;
    }
    }
}

void StepWalkWait() {
    const int st = g_walk ? g_walk->state.load() : 2;
    if (st == 0) return;
    g_walk.reset();
    void* player = coop::players::Registry::Get().Local();
    if (st != 1 || !player || !E::TryGetActorLocation(player, g_run.stand)) {
        ClientFail("the walk to the standpoint did not arrive");
        return;
    }
    UE_LOGI("[CI-DRILL] client %s n=%d try %d stands at (%.0f,%.0f,%.0f), %.0f cm from the target", ArmName(), g_run.r, g_run.t,
            static_cast<double>(g_run.stand.X), static_cast<double>(g_run.stand.Y), static_cast<double>(g_run.stand.Z),
            static_cast<double>(std::hypot(g_packAt.X - g_run.stand.X, g_packAt.Y - g_run.stand.Y)));
    if (ThrownRun(g_run.r)) {
        if (g_run.t > 1) SayNeed(g_run.stand);   // a retry has a NEW drive from the host, beside the new standpoint
        else ClientNext(Step::TakeHand);
        return;
    }
    if (DR::Grabbing(player) != g_run.item.Get()) {
        ClientFail("the grab was lost on the walk");
        return;
    }
    SayArm(g_run.item.Get());
}

void StepTakeHand() {
    if (!coop::dev::drill_hand::TakeIntoHandNow(L"drive", RD::kDriveClass)) {
        ClientFail("the drive could not be taken into the hand");
        return;
    }
    g_settled = 0;
    ClientNext(Step::HandRead);   // the hand's actor is read a tick later
}

void StepHandRead() {
    void* hand = HeldActor(coop::players::Registry::Get().Local());
    g_settled = hand && R::ClassNameOf(hand) == RD::kDriveClass ? g_settled + 1 : 0;
    if (g_settled < 2) {
        if (ClientExpired(kHandBoundMs)) ClientFail("the hand never held the drive");
        return;
    }
    SayArm(hand);
}

// n=4 and n=5: the basket is named, taken from and read armed (container_intent_drill_throw_into_take.cpp); the thrown try follows.
// The first client of n=5 does not throw: it watches the second one's tries (container_intent_drill_throw_into_observer.cpp).
void StepPre() {
    char why[200];
    PreOut out;
    switch (PreTick(why, sizeof(why), out)) {
    case PreState::Working: return;
    case PreState::Failed:  ClientFail("%s", why); return;
    case PreState::Done:
        g_run.pack.Set(out.pack);
        g_run.packEid = out.eid;
        g_packAt = out.at;
        g_preDone = true;
        if (g_run.r == 5 && !g_isB) {
            ObsBegin(g_run.packEid);
            ClientNext(Step::Obs);
            return;
        }
        BeginTry();
        return;
    }
}

void StepIdle() {
    switch (IdleTick()) {
    case IdleState::Working: return;
    case IdleState::Start:   g_run.r = 5; g_run.t = 1; g_preDone = false; BeginTry(); return;
    case IdleState::Done:    ClientEnd(); return;
    }
}

// A fixture failure held for the host's measure tail: the tail starts this peer's own (IntoClientTick); the failure is said when the host's DONE
// is read, or past the bound.
void StepHeld() {
    if (DC::SeenHas(kDoneLine)) {
        ClientEnd();
        return;
    }
    if (!ClientExpired(kHeldBoundMs)) return;
    Fail("%s (the host's measure tail did not begin within %llu s)", g_heldFail, U(kHeldBoundMs / 1000));
    ClientNext(Step::Done);
}

void StepObs() {
    char why[200];
    switch (ObsTick(why, sizeof(why))) {
    case ObsState::Working: return;
    case ObsState::Failed:  ClientFail("%s", why); return;
    case ObsState::Done:    ClientEnd(); return;
    }
}

void StepArmWait() {
    if (DC::SeenHas(Ln("host", "armed", g_run.r, g_run.t, false))) {
        if (ThrownRun(g_run.r)) {   // the camera is aimed DOWN at the trigger box's real world centre, so the toss enters by the mouth
            TriggerBox box;
            if (!ReadTriggerBox(g_run.pack.Get(), box)) {
                ClientFail("the basket's overlap box does not read");
                return;
            }
            FVector aim = box.loc;
            float ux = 0.f, uy = 0.f, len = 0.f;
            if (g_through && Unit(g_run.stand, box.loc, ux, uy, len)) aim = {box.loc.X + ux * kThroughBeyondCm, box.loc.Y + uy * kThroughBeyondCm, box.loc.Z};
            g_run.aim = aim;
            g_run.aimer.BeginAt(g_run.r, aim);
            ClientNext(Step::Aim);
            return;
        }
        // The act begins now: a held drive in an armed box is filed by the host before any release, so the baselines are taken first.
        MeterBegin(g_run.cm, "client", g_run.packEid);
        TrackSet(g_run.cm.track, g_run.item.Get());
        ArmingWatchCopy(g_run.item.Get());
        ReadTriggerBox(g_run.pack.Get(), g_run.box);
        g_run.seq = 0;
        g_run.filedHeld = false;
        if (!HoldBegin(g_run.pack.Get(), g_run.r, g_run.t, g_run.r == 3, g_run.item.Get())) {
            ClientFail("the basket's overlap box does not read");
            return;
        }
        ClientNext(Step::Hold);
        return;
    }
    if (ClientExpired(kArmBoundMs)) ClientFail("the host never armed n=%d try %d", g_run.r, g_run.t);
}

// The host's answer: another try (its rest says where the miss was), the control, or the end.
void StepWaitNext() {
    std::string rest;
    int at[3] = {};
    if (DC::SeenRestOf(Ln("host", "retry", g_run.r, g_run.t, true), &rest)) {
        if (!IntsAfter(rest, "rest=", at, 3)) {
            ClientFail("the host's miss does not read (%s)", rest.c_str());
            return;
        }
        const FVector miss{static_cast<float>(at[0]), static_cast<float>(at[1]), static_cast<float>(at[2])};
        if (ThrownRun(g_run.r)) {   // the thrown standpoint moves by the miss; the control's is measured again by its own loop
            if (!WantAfterMiss(g_run.stand, g_packAt, miss, kIntoMinCm, kIntoMaxCm, g_wantCm)) {
                ClientFail("the miss cannot move the standpoint");
                return;
            }
            UE_LOGI("[CI-DRILL] client %s n=%d try %d missed: the standpoint moves to %.0f cm from the basket", ArmName(), g_run.r, g_run.t,
                    static_cast<double>(g_wantCm));
        }
        ++g_run.t;
        BeginTry();
        return;
    }
    if (DC::SeenHas(Ln("host", "through", g_run.r, g_run.t, false))) {   // the arming measure's extra thrown try, aimed through the box and on
        ++g_run.t;
        g_through = true;
        g_wantCm = 0.f;
        BeginTry();
        return;
    }
    if (DC::SeenHas(Ln("host", "run", g_run.r, g_run.t, false))) {
        ++g_run.r;
        g_run.t = 1;
        g_through = false;
        g_wantCm = 0.f;
        g_preDone = g_run.filedHeld = false;
        BeginTry();
        return;
    }
    if (DC::SeenHas(kDoneLine)) {
        ClientEnd();
        return;
    }
    if (ClientExpired(kNextBoundMs)) ClientFailNow("the host gave no verdict on n=%d try %d within %llu s", g_run.r, g_run.t, U(kNextBoundMs / 1000));
}

}  // namespace

void IntoClientTick(const coop::net::Session& session) {
    if (!g_stepMs) {   // the first step's clock starts when the arm does, never at reset
        if (!cen::IsEnabled()) {
            ClientFailNow("the arm reads the census's overlap watch: run with container_route_census=1 on both peers");
            return;
        }
        HitWatchInstall();   // the thrown copy's first hit (container_intent_drill_throw_hit.cpp); the arm measures with or without it
        static const bool noCancel = coop::config::ResolveFlag(::coop::config_registry::rows::container_overlap_no_cancel);
        if (noCancel) AR::dev::NoOverlapCancel(true);   // the red: this client's overlap put on a shared mirror is not cancelled
        ArmingInstall(/*host=*/false);
        ClientNext(Step::WaitLine);
    }
    if (g_step == Step::Done) return;
    ArmingTick();
    const bool due = ::GetTickCount64() >= g_nextMs;
    if (due) {
        g_nextMs = ::GetTickCount64() + RD::kCheckEveryMs;
        if (DC::SeenHas(kHostFailedLine)) {
            ClientFailNow("the host ended the arm with a FAIL");
            return;
        }
        uint32_t tailEid = 0;
        if (ArmingOn() && g_step != Step::Tail && g_step != Step::Done && ArmingTailAsked(&tailEid)) {   // the arming measure's tail starts, whatever step this peer is in
            WalkStop();
            g_grab.reset();
            void* mirror = coop::element::LivePropActor(tailEid);
            ArmingTailBegin(/*host=*/false, mirror ? mirror : g_run.pack.Get(), tailEid ? tailEid : g_run.packEid);
            ClientNext(Step::Tail);
            return;
        }
    }
    void* player = coop::players::Registry::Get().Local();
    switch (g_step) {   // the grab, the aim and the flight are read every tick; the rest four times a second
    case Step::Aim:    if (player) StepAim(player); else ClientFail("the local player is gone"); return;
    case Step::Grab:   StepGrab(); return;
    case Step::Hold:   if (player) StepHold(player); else ClientFail("the local player is gone"); return;
    case Step::Flight: StepFlight(session); return;
    case Step::Tail: {   // the arming measure's tail
        char why[200];
        const TailState ts = ArmingTailTick(/*host=*/false, due, why, sizeof(why));
        if (ts == TailState::Failed) ClientFail("%s", why);
        else if (ts == TailState::Done) ClientEnd();   // a fixture failure held until the tail was over is said now
        return;
    }
    default:           break;
    }
    if (!due) return;
    switch (g_step) {
    case Step::WaitLine:    StepWaitLine(); return;
    case Step::Arming:      StepArming(); return;
    case Step::WaitMirrors: StepWaitMirrors(); return;
    case Step::Rest:        StepRest(); return;
    case Step::ItemWait:    StepItemWait(); return;
    case Step::WalkWait:    StepWalkWait(); return;
    case Step::TakeHand:    StepTakeHand(); return;
    case Step::HandRead:    StepHandRead(); return;
    case Step::Pre:         StepPre(); return;
    case Step::Idle:        StepIdle(); return;
    case Step::Obs:         StepObs(); return;
    case Step::ArmWait:     StepArmWait(); return;
    case Step::WaitRead:    StepWaitRead(); return;
    case Step::WaitNext:    StepWaitNext(); return;
    case Step::Held:        StepHeld(); return;
    default:                return;
    }
}

void IntoReset() {
    WalkStop();
    g_grab.reset();
    IntoHostReset();
    g_step = Step::WaitLine;
    g_stepMs = g_nextMs = 0;
    g_run.r = g_run.t = 1;
    g_run.packEid = g_itemEid = 0;
    g_run.pack.Reset();
    g_run.item.Reset();
    g_packAt = g_goal = g_run.stand = g_lastPos = FVector{};
    g_wantCm = 0.f;
    g_rest = g_settled = 0;
    g_packSwitch = false;
    g_trigBefore = -1;
    g_armThenTry = false;
    g_run.aimer = Aimer();
    HoldReset();
    g_through = false;
    g_preDone = g_isB = g_run.filedHeld = false;
    PreReset();
    ObsReset();
    g_run.cm = Meter{};
    g_run.hit = HitJudge();
    g_run.trigger = g_run.bOrigin = g_run.bExtent = FVector{};
    ArmingReset();
    g_heldFail[0] = 0;
    g_run.box = TriggerBox{};
    g_run.aim = g_run.v0 = FVector{};
    g_run.seq = 0;
}

}  // namespace coop::dev::container_intent_drill::detail
