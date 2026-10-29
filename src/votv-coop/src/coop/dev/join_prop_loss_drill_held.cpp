// coop/dev/join_prop_loss_drill_held.cpp -- the client's steps of the join prop-loss drill's heldbreak arm; see
// coop/dev/join_prop_loss_drill_arms.h.
//
//   heldbreak -- the client walks within 150 cm of the mirror of a host can, takes it into the hand through the director's
//                use-key physics grab, and calls the can component's impactDamage with the can as the hitting actor, as
//                break does. A prop the client holds is its own to break: its copy must go, and the host's can with it.

#include "join_prop_loss_drill_arms.h"

#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_chat.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>

namespace coop::dev::join_prop_loss_drill::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace DC = coop::dev::drill_chat;
namespace DR = coop::director;
using ue_wrap::FVector;

constexpr float    kGrabReachCm = 150.f;      // the walk ends this near the can
constexpr uint64_t kGrabBoundMs = 30000;      // the grab takes within this, else the arm is inconclusive
constexpr int      kWalkMinS = 90;
constexpr int      kWalkMaxS = 300;

enum class Step : uint8_t { Seek, Walk, Grab, Observe, WaitHost, Done };

Step                                g_step = Step::Seek;
uint64_t                            g_readyMs = 0;
uint64_t                            g_stepMs = 0;
ue_wrap::CachedObjRef               g_target;
std::string                         g_key;
FVector                             g_targetAt{};
std::shared_ptr<DR::BackgroundWalk> g_walk;
std::unique_ptr<DR::AimedGrab>      g_grab;

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
    const std::wstring keyW = ue_wrap::prop::GetKeyString(can);
    g_key = Narrow(keyW);
    g_target.Set(can);
    E::TryGetActorLocation(can, g_targetAt);
    UE_LOGI("[JPL-DRILL] client heldbreak targets %ls eid=%u at (%.1f,%.1f,%.1f)", keyW.c_str(),
            static_cast<unsigned>(coop::remote_prop::ResolveMirrorEidByActor(can, true)), g_targetAt.X, g_targetAt.Y,
            g_targetAt.Z);
    FVector mine{};
    float len = 0.f;
    if (!E::TryGetActorLocation(me, mine) || !DR::RouteInLegs(me, mine, g_targetAt, kGrabReachCm, &len))
        return Abort("no navigation route to within %.0f cm of the can", static_cast<double>(kGrabReachCm));
    const int deadline = std::clamp(static_cast<int>(len / 100.f) + 60, kWalkMinS, kWalkMaxS);
    g_walk = DR::StartBackgroundWalk(g_targetAt, kGrabReachCm, deadline);
    UE_LOGI("[JPL-DRILL] client heldbreak walks to within %.0f cm, a %.0f cm route, %d s budget",
            static_cast<double>(kGrabReachCm), static_cast<double>(len), deadline);
    To(Step::Walk, now);
}

void StepWalk(uint64_t now) {
    const int state = g_walk ? g_walk->state.load() : 2;
    if (state == 0) return;
    if (state == 2) return Abort("the director's walk to the can did not arrive");
    void* me = LocalPlayer();
    void* can = g_target.Alive() ? g_target.Get() : nullptr;
    if (!me || !can || !R::IsLive(can)) return Abort("the player or the can is gone");
    UE_LOGI("[JPL-DRILL] client heldbreak arrived; grabbing");
    g_grab = std::make_unique<DR::AimedGrab>(me, can);
    To(Step::Grab, now);
}

void StepGrab(uint64_t now) {
    switch (g_grab->Tick()) {
    case DR::GrabState::Working:
        if (now - g_stepMs >= kGrabBoundMs)
            Abort("the grab never took within %llu s", static_cast<unsigned long long>(kGrabBoundMs / 1000));
        return;
    case DR::GrabState::Failed:
        return Abort("the grab did not take: %s", g_grab->Why());
    case DR::GrabState::Grabbed:
        break;
    }
    g_grab.reset();
    void* me = LocalPlayer();
    void* can = g_target.Alive() ? g_target.Get() : nullptr;
    if (!me || !can || !R::IsLive(can)) return Abort("the player or the can is gone");
    UE_LOGI("[JPL-DRILL] client heldbreak grabbing: %d", E::IsMainPlayerGrabbing(me, can) ? 1 : 0);
    if (!CallImpactDamage(can, can)) return Abort("impactDamage did not run on the can's physicsImpact component");
    To(Step::Observe, now);
}

void StepObserve(uint64_t now) {
    const bool live = g_target.Alive() && R::IsLive(g_target.Get());
    UE_LOGI("[JPL-DRILL] client copy of %s live=%d", g_key.c_str(), live ? 1 : 0);
    if (!live) Pass();
    else Fail("the can the client holds did not break on its own damage");
    DC::Say(std::string(kBrokeLine) + g_key);
    To(Step::WaitHost, now);
}

}  // namespace

void HeldClientTick(uint64_t now) {
    switch (g_step) {
    case Step::Seek:     StepSeek(now); return;
    case Step::Walk:     StepWalk(now); return;
    case Step::Grab:     StepGrab(now); return;
    case Step::Observe:  StepObserve(now); return;
    case Step::WaitHost:
        if (EndOnHostJudged()) g_step = Step::Done;
        return;
    case Step::Done:     return;
    }
}

void HeldOnDisconnect() {
    if (g_walk && g_walk->state.load() == 0) DR::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
    g_grab.reset();
    g_step = Step::Seek;
    g_readyMs = g_stepMs = 0;
    g_target.Reset();
    g_key.clear();
    g_targetAt = FVector{};
}

}  // namespace coop::dev::join_prop_loss_drill::detail
