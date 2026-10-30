// coop/props/container_view_close.cpp -- see the header.
#include "coop/props/container_view_close.h"

#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
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

constexpr float kUnread = -1.0e9f;

// The room left between `eye` and the actor's reach sphere: `arm` plus its radius minus the distance to its
// centre. Some point of the sphere is within `arm` of `eye` when it is not negative. An actor whose place
// cannot be read has none, as the host answers it.
float SphereMargin(const FVector& eye, float arm, void* actor) {
    FVector c{};
    float r = 0.f;
    if (!E::ActorReachSphere(actor, c, r)) return kUnread;
    const float dx = c.X - eye.X, dy = c.Y - eye.Y, dz = c.Z - eye.Z;
    return arm + r - std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The best margin over the container and the actors that open it. A container destroyed under the view is out
// of reach too. With `stopAtReach` the search ends at the first sphere that holds.
float BestMargin(const FVector& eye, float arm, void* container, bool stopAtReach) {
    if (!R::IsLive(container)) return kUnread;
    const float own = SphereMargin(eye, arm, container);
    if (stopAtReach && own >= 0.f) return own;
    struct Ctx {
        const FVector* eye;
        float arm;
        bool stop;
        float best;
    } ctx{&eye, arm, stopAtReach, own};
    ue_wrap::container_openers::ForEach(container, [](void* c, void* opener) {
        Ctx& x = *static_cast<Ctx*>(c);
        const float m = SphereMargin(*x.eye, x.arm, opener);
        if (m > x.best) x.best = m;
        return !(x.stop && x.best >= 0.f);
    }, &ctx);
    return ctx.best;
}

// The camera and the arm; false when either does not read, since the player's reach is then unknown.
bool ReadReach(void* player, FVector& eye, float& arm) {
    if (!E::ReadMainPlayerCameraLocation(player, eye)) return false;
    if (!E::ReadMainPlayerArmLength(player, arm)) {
        static bool s_armWarned = false;
        if (!s_armWarned) {
            s_armWarned = true;
            UE_LOGW("container_view_close: mainPlayer_C.armLength does not resolve -- no view is closed, since "
                    "the player's reach is unknown");
        }
        return false;
    }
    return true;
}

}  // namespace

bool ViewInReach(void* player, void* container) {
    FVector eye{};
    float arm = 0.f;
    if (!ReadReach(player, eye, arm)) return true;
    return BestMargin(eye, arm, container, /*stopAtReach=*/true) >= 0.f;
}

float ViewReachMargin(void* player, void* container) {
    FVector eye{};
    float arm = 0.f;
    if (!ReadReach(player, eye, arm)) return kUnread;
    return BestMargin(eye, arm, container, /*stopAtReach=*/false);
}

void Tick() {
    if (coop::roster::LocalIsHost()) return;
    void* const container = CV::Viewed();
    if (!container || CV::IsOwnInventory(container)) return;
    // A screen whose close does not resolve is not measured at all: there is nothing to do with the answer.
    if (!CV::CanClose()) {
        static bool s_noClose = false;
        if (!s_noClose) {
            s_noClose = true;
            UE_LOGW("container_view_close: the inventory screen's exit() does not resolve -- no view is closed");
        }
        return;
    }
    void* const player = coop::players::Registry::Get().Local();
    if (ViewInReach(player, container)) return;
    float arm = 0.f;
    E::ReadMainPlayerArmLength(player, arm);
    // Named before the close, while the screen still holds it.
    const bool live = R::IsLive(container);
    const std::wstring cls = live ? R::ClassNameOf(container) : std::wstring(L"(destroyed)");
    const auto eid = live ? coop::element::Registry::Get().EidForActor(container) : 0;
    if (CV::Close()) {
        UE_LOGI("container_view_close: the view into %ls eid=%u closed -- neither it nor an actor that opens "
                "it is within the arm's %.0f uu", cls.c_str(), static_cast<unsigned>(eid), arm);
        return;
    }
    // Asked again next tick, and said once: a close that resolves but does not run is a fault to see.
    static bool s_closeFailed = false;
    if (!s_closeFailed) {
        s_closeFailed = true;
        UE_LOGW("container_view_close: the inventory screen's close did not run for the view into %ls eid=%u "
                "-- it is tried again each tick", cls.c_str(), static_cast<unsigned>(eid));
    }
}

}  // namespace coop::props::container_view_close
