// coop/dev/container_intent_drill_throw_into_host.cpp -- the HOST's half of the container intent drill's throwinto arm; see
// container_intent_drill_throw_into.cpp for the arm. It spawns the basket ahead of the client's puppet and a drive beside
// the first standpoint, says their element ids on drill chat, and for the control a second basket beside the client (its own
// start count; n=3, n=4 and n=5 each have their own as well, the n=4 and n=5 ones unarmed and holding one record) and, per try, a drive beside the standpoint the
// client names, takes its baselines at the client's arm line, traces its own element of the prop, and judges once the client
// has said what it read. A miss is answered with another try; each sub-run's line is held until all are judged:
//   [CI-DRILL] host throwinto n=<k> <thrown|control|held|taken> seq=<s> elected=<0|1> host <a>-><b> client <a>-><b> stray host=<n>
//   client=<n> addObject host=<n>@<ms> client=<n>@<ms> -> PASS | FAIL: <why>
// <a> and <b> are the basket's record count on each peer before the act and after; stray the live drives left beyond those the
// act accounts for; addObject the overlap route's verbs and the clock (UnixMs) of the first.


#include "container_intent_drill_throw_into_host.h"
#include "container_intent_drill_throw_into_spawn.h"
#include "container_route_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/container_route_census.h"
#include "coop/dev/drill_chat.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/container_act_host.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace ci  = ue_wrap::container_inventory;
namespace DC  = coop::dev::drill_chat;
namespace RD  = coop::dev::container_route_drill::detail;
using namespace into;

constexpr float    kTargetAheadCm   = 600.f;   // the basket, before the client's puppet
// A taken run's basket, to the side of the client's place: 100 cm flat from a standing eye is within the view's 200 uu reach (the eye about
// 140 above the floor), so the client opens it where it stands.
constexpr float    kTakenSideCm     = 100.f;
constexpr uint64_t kBindBoundMs     = 15000;
constexpr uint64_t kDingusBoundMs   = 2000;    // dingus() to the box reading on
constexpr uint64_t kSettleBoundMs   = 30000;
constexpr int      kSettleChecks    = 4;       // the target stayed put this many checks: it has settled

}  // namespace

// ---- the host -----------------------------------------------------------------------------------------------------------

namespace into {

HostRun g_host;

void HNext(HStep s) { g_host.step = s; g_host.ms = ::GetTickCount64(); }
bool HExpired(uint64_t bound) { return ::GetTickCount64() - g_host.ms > bound; }

namespace {

// With the arming measure on, a fixture failure waits for the tail (the fill, the client's take, m3, the roomba) and its FAIL is said
// after it; true when held. Not held without a bound target to run the tail on, nor once the tail runs.
bool HHold(const char* text) {
    if (!ArmingOn() || g_host.step == HStep::Tail || g_host.step == HStep::Done || !g_host.target.Get() || !g_host.targetEid || !g_host.heldFixture.empty())
        return false;
    g_host.heldFixture = text;
    UE_LOGI("[CI-DRILL] host %s: the arm's fixture stopped (%s); the measure tail runs first and the FAIL is said after it", ArmName(), text);
    ArmingTailBegin(/*host=*/true, g_host.target.Get(), g_host.targetEid);
    HNext(HStep::Tail);
    return true;
}

}  // namespace

// The host's end with the FAIL line; the client is told so it does not wait out its bounds.
void HFail(const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (HHold(buf)) return;
    DC::Say(kHostFailedLine);
    FlushHeld();
    if (!g_host.heldFixture.empty()) UE_LOGW("[CI-DRILL] host %s FAIL: %s", ArmName(), g_host.heldFixture.c_str());
    Fail("%s", buf);
    HNext(HStep::Done);
}

}  // namespace into

namespace {

// The open basket at `where`, held as the host's target (container_intent_drill_throw_into_spawn.cpp). A basket that does not load is
// the arm's FAIL: sub-run n's line is held like the others, and the arm ends.
bool SpawnTarget(const FVector& where, const FVector& faceTo) {
    void* target = SpawnBasket(where, faceTo);
    if (!target) {
        char line[96];
        std::snprintf(line, sizeof(line), "[CI-DRILL] host throwinto n=%d FAIL: the basket did not load", g_host.r);
        g_host.held[g_host.r - 1] = Held{true, true, line};
        HFail("the basket did not load");
        return false;
    }
    g_host.target.Set(target);
    g_host.packAt = where;
    return true;
}

// A spot for a basket with a routed ring of navmesh round it (routes from the client's place `from`), in any direction round `at`; the arm
// fails when there is none.
bool HostRingSpot(void* ctx, const FVector& from, const FVector& at, const FVector& fwd, float ahead, float side, bool stepSide, int minValid, FVector& out) {
    char why[400];
    if (RingSpotFan(ctx, from, at, fwd, ahead, side, stepSide, minValid, out, why, sizeof(why))) return true;
    HFail("%s", why);
    return false;
}

// The basket about 600 cm ahead of the client's puppet, on a spot with a ring of navmesh round it (moved along the facing when it has
// none), and a drive beside the first standpoint.
bool SpawnFixture(void* puppet, const FVector& at, const FVector& fwd) {
    void* itemCls = R::FindClass(RD::kDriveClass);
    if (!itemCls) {
        HFail("the drive's class does not load");
        return false;
    }
    FVector spot{};
    if (!HostRingSpot(puppet, at, at, fwd, kTargetAheadCm, 0.f, /*stepSide=*/false, kRingMin, spot) || !SpawnTarget(spot, at)) return false;
    // The drive goes beside the first standpoint, on the line from the client to the spot (the heading, unless the fan turned it).
    float ux = fwd.X, uy = fwd.Y, ahead = 0.f;
    if (!Unit(at, spot, ux, uy, ahead)) {
        ux = fwd.X;
        uy = fwd.Y;
    }
    void* item = E::SpawnActor(itemCls, Place(at, FVector{ux, uy, 0.f}, ahead - kIntoStandCm, 80.f));
    if (!item) {
        HFail("the drive did not spawn");
        return false;
    }
    g_host.item.Set(item);
    return true;
}

// Client A is the lowest connected client slot, whatever loads first: the clients' own rule (Put2IsJoiner, container_intent_drill_put2.cpp),
// which makes B the one with a connected peer of a lower slot. The fixture is built for A once it is world-ready.
void StepHWaitPeer(const coop::net::Session& s) {
    int slot = 0;
    for (int i = 1; i < coop::net::kMaxPeers && !slot; ++i)
        if (s.IsSlotConnected(i)) slot = i;
    if (!slot || !s.IsSlotWorldReady(slot)) {
        if (HExpired(kPeerBoundMs)) HFail("no world-ready client within %llu s", U(kPeerBoundMs / 1000));
        return;
    }
    void* puppet = coop::puppet_drive::Puppet(slot).GetActor();   // spawned once the peer's pose arrives
    FVector at{};
    if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at)) return;
    FVector fwd = E::GetActorForwardVector(puppet);
    const float len = std::hypot(fwd.X, fwd.Y);
    fwd = len < 0.01f ? FVector{1.f, 0.f, 0.f} : FVector{fwd.X / len, fwd.Y / len, 0.f};
    g_host.slot = slot;
    if (SpawnFixture(puppet, at, fwd)) HNext(HStep::Bind);
}

// The fixture's lines, said once the target is armed.
void SayFixture() {
    void* target = g_host.target.Get();
    char say[64];
    std::snprintf(say, sizeof(say), "%spack=%u item=%u", kFixtureLine, g_host.targetEid, g_host.itemEid);
    DC::Say(say);
    UE_LOGI("[CI-DRILL] host %s fixture: target eid=%u %ls holds %d record(s), drive eid=%u", ArmName(), g_host.targetEid,
            target ? R::ClassNameOf(target).c_str() : L"?", CountOf(g_host.targetEid), g_host.itemEid);
    HNext(HStep::WaitArm);
}

void SayItem() {
    char say[48];
    std::snprintf(say, sizeof(say), "eid=%u pack=%u", g_host.itemEid, g_host.targetEid);
    DC::Say(Ln("host", "item", g_host.r, g_host.t, true) + say);
    UE_LOGI("[CI-DRILL] host %s n=%d try %d: the drive is eid=%u", ArmName(), g_host.r, g_host.t, g_host.itemEid);
    HNext(HStep::WaitArm);
}

// The target is bound: its trigger is armed the game's own way (a fresh container's `overlap` box is NoCollision until the
// container's dingus() runs), and the clients are told only once it is on.
void StartDingus(bool forItem) {
    void* target = g_host.target.Get();
    g_host.trigBefore = TriggerState(target);
    ArmingTrack(target);   // the arming measure: this basket's box joins the hook's set, and the call below is its "ourcall" route
    ArmingRoute("ourcall");
    if (UnarmedOn()) {   // the red container_throwinto_unarmed: the fixture arms nothing and the box stays off
        UE_LOGI("[CI-DRILL] host %s: the red container_throwinto_unarmed is on, the target's box stays unarmed (collision-enabled %d)", ArmName(),
                g_host.trigBefore);
        if (forItem) SayItem();
        else SayFixture();
        return;
    }
    if (!CallDingus(target)) {
        HFail("dingus() did not run on the target (trigger collision-enabled %d)", g_host.trigBefore);
        return;
    }
    g_host.dingusItem = forItem;
    HNext(HStep::Dingus);
}

// n=4 and n=5: the basket is the run's own and the fixture arms nothing; it holds one record for the client to take, and the take arms it.
void PrefillAndSay() {
    void* target = g_host.target.Get();
    if (!target || !FillBasket(target)) {
        HFail("the taken run's basket did not take its record");
        return;
    }
    char say[24];
    std::snprintf(say, sizeof(say), "pack=%u", g_host.targetEid);
    DC::Say(Ln("host", "basket", g_host.r, g_host.t, true) + say);
    UE_LOGI("[CI-DRILL] host %s n=%d: the basket eid=%u holds %d record(s) and its box reads collision-enabled %d (unarmed by the fixture)",
            ArmName(), g_host.r, g_host.targetEid, CountOf(g_host.targetEid), TriggerState(target));
    HNext(HStep::WaitTake);
}

// The container turns its box on after its own 0.1 s delay: read each tick until it is on, to the bound the client's twin wait uses
// (kMirrorArmBoundMs in container_intent_drill_throw_into.cpp).
void StepHDingus() {
    const int after = TriggerState(g_host.target.Get());
    if (after <= 0) {
        if (HExpired(kDingusBoundMs))
            HFail("the basket's box did not arm (collision enabled %d -> %d within %llu ms)", g_host.trigBefore, after, U(kDingusBoundMs));
        return;
    }
    UE_LOGI("[CI-DRILL] host %s: the target's trigger collision-enabled before dingus() %d, %llu ms after it %d (0 none, 1 query only, "
            "2 physics only, 3 both)", ArmName(), g_host.trigBefore, U(::GetTickCount64() - g_host.ms), after);
    if (g_host.dingusItem) SayItem();
    else SayFixture();
}

// The target is bound: it is waited on until it stops moving, its settled rotation (the actor's and its overlap box's) is read
// and printed, and its trigger is armed. The basket's resting pose is not asserted: its mesh is authored on its side (the actor
// rests at roll 90), so no upright test can be defined from data.
void StartSettle(bool forItem) {
    g_host.settleItem = forItem;
    g_host.settleStill = 0;
    g_host.settleLast = FVector{};
    HNext(HStep::Settle);
}

void StepHSettle() {
    void* target = g_host.target.Get();
    FVector at{};
    if (!target || !E::TryGetActorLocation(target, at)) {
        HFail("the target died or does not read before it settled");
        return;
    }
    const bool still = std::fabs(at.X - g_host.settleLast.X) + std::fabs(at.Y - g_host.settleLast.Y) + std::fabs(at.Z - g_host.settleLast.Z) < 1.f;
    g_host.settleLast = at;
    g_host.settleStill = still ? g_host.settleStill + 1 : 0;
    if (g_host.settleStill < kSettleChecks) {
        if (HExpired(kSettleBoundMs)) HFail("the target never came to rest within %llu s", U(kSettleBoundMs / 1000));
        return;
    }
    TriggerBox box;
    ue_wrap::FRotator actorRot{};
    if (!ReadTriggerBox(target, box) || !E::TryGetActorRotation(target, actorRot)) {
        HFail("the target's overlap box or rotation does not read once it settled");
        return;
    }
    UE_LOGI("[CI-DRILL] host %s: the target settled with its overlap box at (%.1f,%.1f,%.1f), box rotation pitch %.1f yaw %.1f roll %.1f "
            "(authored relative to the basket), world-Z reach %.1f; the ACTOR is at pitch %.1f yaw %.1f roll %.1f",
            ArmName(), static_cast<double>(box.loc.X), static_cast<double>(box.loc.Y), static_cast<double>(box.loc.Z),
            static_cast<double>(box.rot.Pitch), static_cast<double>(box.rot.Yaw), static_cast<double>(box.rot.Roll),
            static_cast<double>(box.zReach), static_cast<double>(actorRot.Pitch), static_cast<double>(actorRot.Yaw),
            static_cast<double>(actorRot.Roll));
    if (TakenRun(g_host.r)) PrefillAndSay();
    else StartDingus(g_host.settleItem);
}

void StepHBind() {
    void* target = g_host.target.Get();
    void* item = g_host.item.Get();
    if (!target || !item) {
        HFail("a fixture actor died before the client took it");
        return;
    }
    const uint32_t te = RD::EidOf(target), ie = RD::EidOf(item);
    const int count = EidBound(te) && EidBound(ie) ? CountOf(te) : -1;
    if (count < 0) {
        if (HExpired(kBindBoundMs)) HFail("the fixture was never bound and published (target eid %u, drive eid %u)", te, ie);
        return;
    }
    g_host.targetEid = te;
    g_host.itemEid = ie;
    E::TryGetActorLocation(target, g_host.packAt);
    StartSettle(/*forItem=*/false);
}

// The control's drive, beside the standpoint the client names.
void StepHNeed() {
    std::string rest;
    int at[3] = {};
    if (!DC::SeenRestOf(Ln("client", "need", g_host.r, g_host.t, true), &rest)) {
        if (HExpired(kArmBoundMs)) HFail("the client never asked for the control's drive within %llu s", U(kArmBoundMs / 1000));
        return;
    }
    if (!IntsAfter(rest, "at=", at, 3)) {
        HFail("the client's standpoint does not read (%s)", rest.c_str());
        return;
    }
    if (void* t = g_host.target.Get()) E::TryGetActorLocation(t, g_host.packAt);   // where the basket came to rest
    void* cls = R::FindClass(RD::kDriveClass);
    const FVector stand{static_cast<float>(at[0]), static_cast<float>(at[1]), static_cast<float>(at[2])};
    float ux = 0.f, uy = 0.f, len = 0.f;
    if (!cls || !Unit(stand, g_host.packAt, ux, uy, len)) {
        HFail("the control's drive has no place (class %p)", cls);
        return;
    }
    // Every sub-run after n=1 has its OWN basket, spawned once beside the client (300 cm to its side, a taken run's kTakenSideCm): a
    // divergence of an earlier one cannot reach its start count. The drive is then placed toward it.
    if (g_host.r >= 2 && !g_host.ownBasket) {
        FVector spot{};
        void* ctx = coop::players::Registry::Get().Local();
        if (!ctx) {
            HFail("the host's player is gone");
            return;
        }
        // To the client's side, moved further along that side when the spot has no ring of navmesh.
        if (!HostRingSpot(ctx, stand, stand, FVector{ux, uy, 0.f}, 0.f, TakenRun(g_host.r) ? kTakenSideCm : 300.f, /*stepSide=*/true,
                           TakenRun(g_host.r) ? kFan : kRingMin, spot) || !SpawnTarget(spot, stand)) return;
        g_host.newTarget = true;
        g_host.ownBasket = true;
        if (!Unit(stand, g_host.packAt, ux, uy, len)) {
            HFail("the control's basket has no direction");
            return;
        }
        if (TakenRun(g_host.r)) {   // n=4 and n=5: the basket alone; its record and the take come before any drive
            g_host.item.Reset();
            HNext(HStep::ItemBind);
            return;
        }
    }
    void* item = E::SpawnActor(cls, Place(stand, FVector{ux, uy, 0.f}, 0.f, 80.f));
    if (!item) {
        HFail("the control's drive did not spawn");
        return;
    }
    g_host.item.Set(item);
    HNext(HStep::ItemBind);
}

void StepHItemBind() {
    void* item = g_host.item.Get();
    if (!item && !TakenRun(g_host.r)) {
        HFail("the control's drive died before the client took it");
        return;
    }
    const uint32_t ie = item ? RD::EidOf(item) : 0u;
    if (item && !EidBound(ie)) {
        if (HExpired(kBindBoundMs)) HFail("the control's drive was never bound");
        return;
    }
    if (g_host.newTarget) {   // the control's own basket: bound and published, read 0 as n=1's was
        void* target = g_host.target.Get();
        const uint32_t te = target ? RD::EidOf(target) : 0u;
        const int count = target && EidBound(te) ? CountOf(te) : -1;
        if (count < 0) {
            if (HExpired(kBindBoundMs)) HFail("the control's basket was never bound and published (eid %u)", te);
            return;
        }
        g_host.targetEid = te;
        g_host.newTarget = false;
        UE_LOGI("[CI-DRILL] host %s n=%d: the control's own target eid=%u holds %d record(s)", ArmName(), g_host.r, te, count);
        g_host.itemEid = ie;
        StartSettle(/*forItem=*/true);   // the item line follows once the new target has settled and its trigger is armed
        return;
    }
    g_host.itemEid = ie;
    SayItem();
}

}  // namespace

void IntoHostTick(const coop::net::Session& session) {
    g_host.session = &session;
    if (!g_host.ms) {   // the first step's clock starts when the arm does, never at reset
        if (!cen::IsEnabled()) {
            HFail("the arm reads the census's overlap watch: run with container_route_census=1 on both peers");
            return;
        }
        static const bool reachRed = coop::config::ResolveFlag(::coop::config_registry::rows::container_overlap_reach_red);
        if (reachRed) coop::props::container_act_host::overlap::dev::ReachRed(true);   // the red: the reach term of a held driver reads out of reach
        ArmingInstall(/*host=*/true);
        HNext(HStep::WaitPeer);
    }
    if (g_host.step == HStep::Done) return;
    ArmingTick();
    const uint64_t now = ::GetTickCount64();
    const bool due = now >= g_host.nextMs;
    if (due) {
        g_host.nextMs = now + RD::kCheckEveryMs;
        if (g_host.step != HStep::WaitPeer && g_host.step != HStep::Tail && DC::SeenHas(kClientFailedLine)) {
            if (HHold("the client ended the arm with a FAIL")) return;
            DC::Say(kHostFailedLine);   // the other client is told too, as every other end of the arm does
            FlushHeld();
            Fail("the client ended the arm with a FAIL");
            HNext(HStep::Done);
            return;
        }
        // No fixture to give up before the first one exists, none to leave in the tail's run, none while n=5's second client is awaited.
        if (g_host.step != HStep::WaitPeer && g_host.step != HStep::Tail && g_host.step != HStep::WaitSecond && HClientPrefail()) return;
    }
    if (g_host.step == HStep::Tail) {   // the arming measure's tail
        char why[200];
        switch (ArmingTailTick(/*host=*/true, due, why, sizeof(why))) {
        case TailState::Failed: HFail("%s", why); return;
        case TailState::Done:
            FlushHeld();   // every verdict line is held until the tail is over, whatever the verdicts were
            DC::Say(kDoneLine);
            if (!g_host.heldFixture.empty()) {   // the fixture failure held until the tail was over is said now
                Fail("%s", g_host.heldFixture.c_str());
                HNext(HStep::Done);
                return;
            }
            UE_LOGI("[CI-DRILL] host %s MEASURED", ArmName());
            Finish();
            HNext(HStep::Done);
            return;
        default: return;
        }
    }
    if (g_host.step == HStep::Late) {
        StepHLate(due);
        return;
    }
    if (g_host.step == HStep::Trace) {
        StepHTrace(session, due);
        return;
    }
    if (!due) return;
    switch (g_host.step) {
    case HStep::WaitPeer: StepHWaitPeer(session); return;
    case HStep::Bind:     StepHBind(); return;
    case HStep::Need:     StepHNeed(); return;
    case HStep::ItemBind: StepHItemBind(); return;
    case HStep::Settle:   StepHSettle(); return;
    case HStep::Dingus:   StepHDingus(); return;
    case HStep::WaitTake: StepHWaitTake(); return;
    case HStep::WaitArm:  StepHWaitArm(); return;
    case HStep::WaitSecond: StepHWaitSecond(); return;
    default:              return;
    }
}

void IntoHostReset() {
    ArmingReset();
    g_host.step = HStep::WaitPeer;
    g_host.ms = g_host.nextMs = g_host.findMs = g_host.endSeenMs = 0;
    g_host.r = g_host.t = 1;
    g_host.slot = 0;
    g_host.target.Reset();
    g_host.item.Reset();
    g_host.targetEid = g_host.itemEid = g_host.elemEid = 0;
    g_host.packAt = FVector{};
    g_host.m = Meter{};
    g_host.set.clear();
    g_host.live.clear();
    g_host.armRead = false;
    g_host.armRest.clear();
    g_host.endRest.clear();
    g_host.int0 = g_host.intThrown0 = g_host.relThrown0 = g_host.elected0 = 0;
    g_host.f = Frozen{};
    g_host.newTarget = false;
    g_host.ownBasket = false;
    g_host.session = nullptr;
    g_host.trigBefore = -1;
    g_host.dingusItem = false;
    g_host.through = false;
    g_host.heldFixture.clear();
    g_host.settleLast = FVector{};
    g_host.settleStill = 0;
    g_host.settleItem = false;
    g_host.end = ClientEnd{};
    for (Held& h : g_host.held) h = Held{};
}

}  // namespace coop::dev::container_intent_drill::detail
