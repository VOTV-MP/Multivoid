// coop/props/container_view_close.cpp -- see the header.
#include "coop/props/container_view_close.h"

#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_write_policy.h"
#include "ue_wrap/actors/container_openers.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cmath>
#include <string>

namespace coop::props::container_view_close {
namespace {

namespace CV = ue_wrap::container_view;
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
using ue_wrap::FVector;

// The game's reach is its camera trace, mainPlayer::arm, armLength long; the host's arbitration of a
// container slice owns the same number.
constexpr float kReachUU = coop::props::container_write_policy::kReachUU;

// Some point of the actor's reach sphere is within the arm's reach of `eye`. An actor whose place
// cannot be read is not in reach, as the host answers it.
bool InReach(const FVector& eye, void* actor) {
    FVector c{};
    float r = 0.f;
    if (!E::ActorReachSphere(actor, c, r)) return false;
    const float dx = c.X - eye.X, dy = c.Y - eye.Y, dz = c.Z - eye.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) <= kReachUU + r;
}

}  // namespace

void Tick() {
    void* const container = CV::Viewed();
    if (!container || container == CV::OwnContainer()) return;
    FVector eye{};
    if (!E::ReadMainPlayerCameraLocation(coop::players::Registry::Get().Local(), eye)) return;
    // A container destroyed under the view is out of reach too.
    const bool live = R::IsLive(container);
    if (live && InReach(eye, container)) return;
    struct Ctx {
        const FVector* eye;
        bool reached;
    } ctx{&eye, false};
    if (live) {
        ue_wrap::container_openers::ForEach(container, [](void* c, void* opener) {
            Ctx& x = *static_cast<Ctx*>(c);
            x.reached = InReach(*x.eye, opener);
            return !x.reached;
        }, &ctx);
    }
    if (ctx.reached) return;
    const std::wstring cls = live ? R::ClassNameOf(container) : std::wstring(L"(destroyed)");
    const auto eid = live ? coop::element::Registry::Get().EidForActor(container) : 0;
    if (CV::Close()) {
        UE_LOGI("container_view_close: the view into %ls eid=%u closed -- neither it nor an actor that opens "
                "it is within reach", cls.c_str(), static_cast<unsigned>(eid));
        return;
    }
    static bool s_warned = false;
    if (!s_warned) {
        s_warned = true;
        UE_LOGW("container_view_close: the view into %ls eid=%u is out of reach and the screen's exit() did "
                "not dispatch -- the view stays open", cls.c_str(), static_cast<unsigned>(eid));
    }
}

}  // namespace coop::props::container_view_close
