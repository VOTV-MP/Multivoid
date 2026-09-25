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

// Some point of the actor's reach sphere is within `arm` of `eye`. An actor whose place cannot be read is
// not in reach, as the host answers it.
bool InReach(const FVector& eye, float arm, void* actor) {
    FVector c{};
    float r = 0.f;
    if (!E::ActorReachSphere(actor, c, r)) return false;
    const float dx = c.X - eye.X, dy = c.Y - eye.Y, dz = c.Z - eye.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) <= arm + r;
}

}  // namespace

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
    FVector eye{};
    float arm = 0.f;
    if (!E::ReadMainPlayerCameraLocation(player, eye)) return;
    if (!E::ReadMainPlayerArmLength(player, arm)) {
        static bool s_armWarned = false;
        if (!s_armWarned) {
            s_armWarned = true;
            UE_LOGW("container_view_close: mainPlayer_C.armLength does not resolve -- no view is closed, since "
                    "the player's reach is unknown");
        }
        return;
    }
    // A container destroyed under the view is out of reach too.
    const bool live = R::IsLive(container);
    if (live && InReach(eye, arm, container)) return;
    struct Ctx {
        const FVector* eye;
        float arm;
        bool reached;
    } ctx{&eye, arm, false};
    if (live) {
        ue_wrap::container_openers::ForEach(container, [](void* c, void* opener) {
            Ctx& x = *static_cast<Ctx*>(c);
            x.reached = InReach(*x.eye, x.arm, opener);
            return !x.reached;
        }, &ctx);
    }
    if (ctx.reached) return;
    // Named before the close, while the screen still holds it.
    const std::wstring cls = live ? R::ClassNameOf(container) : std::wstring(L"(destroyed)");
    const auto eid = live ? coop::element::Registry::Get().EidForActor(container) : 0;
    if (CV::Close())
        UE_LOGI("container_view_close: the view into %ls eid=%u closed -- neither it nor an actor that opens "
                "it is within the arm's %.0f uu", cls.c_str(), static_cast<unsigned>(eid), arm);
}

}  // namespace coop::props::container_view_close
