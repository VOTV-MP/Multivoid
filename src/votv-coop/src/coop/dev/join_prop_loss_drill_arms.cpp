// coop/dev/join_prop_loss_drill_arms.cpp -- the client's steps of the join prop-loss drill's two measure arms; see
// coop/dev/join_prop_loss_drill_arms.h.
//
//   swing   -- the client takes the axe, walks within 150 cm of the mirror of a host can and fires the game's fire
//              action at it, one swing every 600 ms, until its copy is gone or 10 swings.
//   throwat -- the client takes a beer can into its hand, walks within 300 cm of the mirror and throws it at the mirror
//              with the hand's throw, up to 3 times.
// Neither is judged on the client: the impact_damage_probe lines are the result, and the host reads whether its own can
// survived.

#include "join_prop_loss_drill_arms.h"

#include "coop/dev/director/director.h"
#include "coop/dev/director/input_stub.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/net_clock.h"
#include "coop/props/remote_prop.h"

#include "drive_drill_verbs.h"  // Throw: the hand's throw (co-located private header)

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::join_prop_loss_drill::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace DC = coop::dev::drill_chat;
namespace DR = coop::director;
namespace DV = coop::dev::drive_drill_verbs;
using ue_wrap::FVector;

constexpr const wchar_t* kAxeItem = L"axe";
constexpr const wchar_t* kAxeClass = L"prop_axe_C";
constexpr float    kSwingReachCm = 150.f;   // the walk ends this near the can
constexpr float    kThrowReachCm = 300.f;
constexpr int      kMaxSwings = 10;
constexpr int      kMaxThrows = 3;
constexpr uint64_t kSwingGapMs = 600;       // a player's swing rate
constexpr uint64_t kAimMs = 300;            // the camera follows the control rotation on the next frames
constexpr uint64_t kSettleMs = 5000;        // a thrown can rests, or this long passes
constexpr uint64_t kMinFlightMs = 400;      // no rest is read before the can has left the hand
constexpr int      kRestTicks = 3;
constexpr float    kRestCmS = 10.f;
constexpr float    kReachedCm = 40.f;       // a thrown can that came this near the target reached it
constexpr int      kWalkMinS = 90;
constexpr int      kWalkMaxS = 300;

enum class Step : uint8_t { Seek, Walk, Swing, Aim, Settle, WaitHost, Done };

Step                      g_step = Step::Seek;
uint64_t                  g_readyMs = 0;
uint64_t                  g_stepMs = 0;
uint64_t                  g_nextSwingMs = 0;
ue_wrap::CachedObjRef     g_target;
std::wstring              g_keyW;
std::string               g_key;
FVector                   g_targetAt{};
std::shared_ptr<DR::BackgroundWalk> g_walk;
int                       g_swings = 0;
int                       g_throws = 0;
std::vector<void*>        g_before;
ue_wrap::CachedObjRef     g_thrown;
float                     g_minDistCm = 1.0e9f;
uint64_t                  g_throwMs = 0;
int                       g_restTicks = 0;

// The fire action's press and release, the pair a left click makes.
DR::InputStub g_firePress{L"InpActEvt_fire_K2Node_InputActionEvent_58"};
DR::InputStub g_fireRelease{L"InpActEvt_fire_K2Node_InputActionEvent_59"};

bool InSet(const std::vector<void*>& set, void* a) {
    for (void* p : set)
        if (p == a) return true;
    return false;
}

bool TargetGone() { return !g_target.Alive() || !R::IsLive(g_target.Get()); }

void To(Step s, uint64_t now) {
    g_step = s;
    g_stepMs = now;
}

void Abort(const char* fmt, ...) {
    char buf[240];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Inconclusive("%s", buf);
    g_step = Step::Done;
    Finish();
}

// The target's place now: its own while it stands, else the last one read.
FVector TargetAt() {
    FVector at{};
    if (!TargetGone() && E::TryGetActorLocation(g_target.Get(), at)) g_targetAt = at;
    return g_targetAt;
}

// The camera turned to the target. The pose stream carries the aim, so the host reads a swing made facing the can.
void FaceTarget(void* me) {
    void* ctrl = E::GetController(me);
    if (ctrl) E::SetControlRotation(ctrl, DR::LookAt(E::GetCameraLocation(), TargetAt()));
}

// The hand takes what the arm uses: the axe for a swing, a beer can for a throw.
bool TakeIntoHand() {
    return IsThrowAt() ? coop::dev::drill_hand::TakeIntoHandNow(kCanRow, kCanClass)
                       : coop::dev::drill_hand::TakeIntoHandNow(kAxeItem, kAxeClass);
}

void Report(uint64_t now) {
    const bool gone = TargetGone();
    if (IsThrowAt()) UE_LOGI("[JPL-DRILL] client throwat: copy %s after %d throws", gone ? "gone" : "stands", g_throws);
    else if (gone) UE_LOGI("[JPL-DRILL] client swing: copy gone after %d swings", g_swings);
    else UE_LOGI("[JPL-DRILL] client swing: copy stands after %d swings", g_swings);
    coop::dev::drill_chat::Say(std::string(gone || !IsThrowAt() ? kBrokeLine : kStoodLine) + g_key);
    To(Step::WaitHost, now);
}

void StepSeek(uint64_t now) {
    if (!JoinedAndQuiet()) return;
    void* me = LocalPlayer();
    if (!me) return;
    if (!g_readyMs) g_readyMs = now;
    void* can = NearestMirror(me);
    if (!can) {
        if (now - g_readyMs >= kMirrorBudgetMs)
            Abort("no beer can with a wire mirror within %.0f cm of the player after %llu ms",
                  static_cast<double>(kMirrorRangeCm), static_cast<unsigned long long>(kMirrorBudgetMs));
        return;
    }
    g_keyW = ue_wrap::prop::GetKeyString(can);
    g_key = Narrow(g_keyW);
    g_target.Set(can);
    E::TryGetActorLocation(can, g_targetAt);
    UE_LOGI("[JPL-DRILL] client %s targets %ls eid=%u at (%.1f,%.1f,%.1f)", ArmName(), g_keyW.c_str(),
            static_cast<unsigned>(coop::remote_prop::ResolveMirrorEidByActor(can, true)), g_targetAt.X, g_targetAt.Y,
            g_targetAt.Z);
    if (!TakeIntoHand()) return Abort("the %s could not be taken into the hand", IsThrowAt() ? "beer can" : "axe");
    const float reach = IsThrowAt() ? kThrowReachCm : kSwingReachCm;
    FVector mine{};
    float len = 0.f;
    if (!E::TryGetActorLocation(me, mine) || !DR::RouteInLegs(me, mine, g_targetAt, reach, &len))
        return Abort("no navigation route to within %.0f cm of the can", static_cast<double>(reach));
    const int deadline = std::clamp(static_cast<int>(len / 100.f) + 60, kWalkMinS, kWalkMaxS);
    g_walk = DR::StartBackgroundWalk(g_targetAt, reach, deadline);
    UE_LOGI("[JPL-DRILL] client %s walks to within %.0f cm, a %.0f cm route, %d s budget", ArmName(),
            static_cast<double>(reach), static_cast<double>(len), deadline);
    To(Step::Walk, now);
}

void StepWalk(uint64_t now) {
    const int state = g_walk ? g_walk->state.load() : 2;
    if (state == 0) return;
    if (state == 2) return Abort("the director's walk to the can did not arrive");
    UE_LOGI("[JPL-DRILL] client %s arrived", ArmName());
    if (IsThrowAt()) To(Step::Aim, now);
    else To(Step::Swing, now);
}

void StepSwing(uint64_t now) {
    if (TargetGone() || (g_swings >= kMaxSwings && now >= g_nextSwingMs)) return Report(now);
    if (g_swings >= kMaxSwings || now < g_nextSwingMs) return;
    void* me = LocalPlayer();
    if (!me) return Abort("the local player is gone");
    FaceTarget(me);
    if (!DR::PressInputEvent(me, g_firePress) || !DR::PressInputEvent(me, g_fireRelease))
        return Abort("the fire input event did not run");
    ++g_swings;
    g_nextSwingMs = now + kSwingGapMs;
    UE_LOGI("[JPL-DRILL] client swing %d of %d", g_swings, kMaxSwings);
}

// The throw: the camera is on the can for kAimMs, then the hand's throw verb sends a new can from the hand.
void StepAim(uint64_t now) {
    void* me = LocalPlayer();
    if (!me) return Abort("the local player is gone");
    FaceTarget(me);
    if (now - g_stepMs < kAimMs) return;
    Cans(g_before);
    if (!DV::Throw(me)) return Abort("throwHoldingProp did not run");
    ++g_throws;
    g_thrown.Reset();
    g_minDistCm = 1.0e9f;
    g_restTicks = 0;
    g_throwMs = now;
    UE_LOGI("[JPL-DRILL] client throwat throw %d of %d", g_throws, kMaxThrows);
    To(Step::Settle, now);
}

// Until the thrown can rests or kSettleMs: its nearest approach to the target is kept.
void StepSettle(uint64_t now) {
    if (!g_thrown.Alive()) {
        std::vector<void*> cans;
        Cans(cans);
        for (void* a : cans) {
            if (InSet(g_before, a)) continue;
            g_thrown.Set(a);
            break;
        }
    }
    bool rested = false;
    if (void* t = g_thrown.Get(); t && R::IsLive(t)) {
        FVector at{};
        if (E::TryGetActorLocation(t, at)) g_minDistCm = (std::min)(g_minDistCm, Dist(at, TargetAt()));
        const FVector v = E::GetActorVelocity(t);
        const float speed = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
        g_restTicks = speed < kRestCmS ? g_restTicks + 1 : 0;
        rested = now - g_throwMs >= kMinFlightMs && g_restTicks >= kRestTicks;
    }
    if (!rested && now - g_throwMs < kSettleMs && !TargetGone()) return;
    UE_LOGI("[JPL-DRILL] client throwat throw %d: thrown can %s, closest to the target %.1f cm, copy %s", g_throws,
            g_thrown.Alive() ? (rested ? "rested" : "still moving") : "not seen",
            static_cast<double>(g_minDistCm), TargetGone() ? "gone" : "stands");
    if (!TargetGone() && g_throws < kMaxThrows) {
        // The hand emptied with the throw.
        if (!TakeIntoHand()) return Abort("the beer can could not be taken into the hand");
        To(Step::Aim, now);
        return;
    }
    if (!TargetGone() && g_minDistCm > kReachedCm)
        return Abort("no throw reached the can (closest %.0f cm in %d throws)", static_cast<double>(g_minDistCm),
                     g_throws);
    Report(now);
}

}  // namespace

void MeasureClientTick(uint64_t now) {
    switch (g_step) {
    case Step::Seek:     StepSeek(now); return;
    case Step::Walk:     StepWalk(now); return;
    case Step::Swing:    StepSwing(now); return;
    case Step::Aim:      StepAim(now); return;
    case Step::Settle:   StepSettle(now); return;
    case Step::WaitHost:
        if (DC::SeenHas(kHostJudgedLine)) {
            g_step = Step::Done;
            Finish();
        }
        return;
    case Step::Done:     return;
    }
}

void MeasureOnDisconnect() {
    if (g_walk && g_walk->state.load() == 0) DR::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
    g_step = Step::Seek;
    g_readyMs = g_stepMs = g_nextSwingMs = g_throwMs = 0;
    g_target.Reset();
    g_thrown.Reset();
    g_keyW.clear();
    g_key.clear();
    g_targetAt = FVector{};
    g_swings = g_throws = g_restTicks = 0;
    g_before.clear();
    g_minDistCm = 1.0e9f;
}

}  // namespace coop::dev::join_prop_loss_drill::detail
