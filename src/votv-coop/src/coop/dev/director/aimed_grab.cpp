// coop/dev/director/aimed_grab.cpp -- see coop/dev/director/aimed_grab.h.

#include "coop/dev/director/aimed_grab.h"

#include "coop/dev/director/director.h"  // LookAt

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace coop::director {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr int   kAimTicksPerPose = 6;   // the trace runs on the player's own tick: hold each pose
constexpr int   kAimFanHalf      = 5;   // an 11 x 11 fan of 3 degrees round the prop
constexpr float kAimFanStepDeg   = 3.f;
constexpr int   kGrabVerifyTicks = 30;

// The aim fan, nearest pose first: the prop's centre, then offsets of growing size.
const std::vector<std::pair<int, int>>& AimFan() {
    static const std::vector<std::pair<int, int>> fan = [] {
        std::vector<std::pair<int, int>> v;
        for (int p = -kAimFanHalf; p <= kAimFanHalf; ++p)
            for (int y = -kAimFanHalf; y <= kAimFanHalf; ++y) v.emplace_back(p, y);
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
        });
        return v;
    }();
    return fan;
}

}  // namespace

void* Grabbing(void* player) {
    E::MainPlayerGrabState gs{};
    return E::ReadMainPlayerGrabState(player, gs) ? gs.grabbingActor : nullptr;
}

bool CallWithPlayer(void* obj, const wchar_t* fnName, void* player) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(obj), fnName);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && ue_wrap::Call(obj, f);
}

bool CallOnPlayer(void* player, const wchar_t* fnName) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(player), fnName);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(player, f);
}

GrabState AimedGrab::Tick() {
    void* player = player_.Get();
    void* prop = prop_.Get();
    if (!player || !prop) {
        why_ = "the player or the prop died before the grab";
        return GrabState::Failed;
    }
    ++ticks_;
    if (!aimed_) {
        if (E::ReadMainPlayerHitActor(player) == prop) {
            aimed_ = true;
            return GrabState::Working;
        }
        if (ticks_ % kAimTicksPerPose != 1) return GrabState::Working;
        const auto& fan = AimFan();
        if (poses_ >= static_cast<int>(fan.size())) {
            why_ = "no aim the player's trace would take";
            noAim_ = true;
            // Said once, so the cause is read and not guessed: where the camera is, where it aimed, and what the trace struck
            // last (its actor and component) when it was not the prop.
            ue_wrap::FVector cam = E::GetCameraLocation(), centre{}, extent{};
            E::GetActorBounds(prop, /*onlyColliding=*/true, centre, extent);
            void* const hitActor = E::ReadMainPlayerHitActor(player);
            void* const hitComp = E::ReadMainPlayerHitComponent(player);
            const float dx = centre.X - cam.X, dy = centre.Y - cam.Y, dz = centre.Z - cam.Z;
            UE_LOGW("aimed_grab: no aim took the prop after %d poses -- camera (%.0f, %.0f, %.0f), the prop's bounds centre "
                    "(%.0f, %.0f, %.0f) extent (%.0f, %.0f, %.0f) %.0f uu away, the last trace hit actor %ls (%p) component "
                    "%ls (%p)",
                    poses_, cam.X, cam.Y, cam.Z, centre.X, centre.Y, centre.Z, extent.X, extent.Y, extent.Z,
                    std::sqrt(dx * dx + dy * dy + dz * dz),
                    hitActor ? R::ClassNameOf(hitActor).c_str() : L"none", hitActor,
                    hitComp ? R::ClassNameOf(hitComp).c_str() : L"none", hitComp);
            return GrabState::Failed;
        }
        // The bounds' centre: a lying prop's origin can sit at the floor's surface.
        ue_wrap::FVector centre{}, extent{};
        if (!E::GetActorBounds(prop, /*onlyColliding=*/true, centre, extent) && !E::TryGetActorLocation(prop, centre)) {
            why_ = "the prop has neither bounds nor a location to aim at";
            return GrabState::Failed;
        }
        ue_wrap::FRotator r = LookAt(E::GetCameraLocation(), centre);
        r.Pitch += kAimFanStepDeg * static_cast<float>(fan[poses_].first);
        r.Yaw   += kAimFanStepDeg * static_cast<float>(fan[poses_].second);
        E::SetControlRotation(E::GetController(player), r);
        ++poses_;
        return GrabState::Working;
    }
    if (holdAtAim_) return GrabState::Working;
    if (grabTicks_++ == 0) {
        // The use key's release on an aimed prop with an empty hand, in its order.
        chainPre_ = CallWithPlayer(prop, L"playerGrabbed_pre", player);
        if (void* useFn = R::FindDispatchFunctionCached(R::ClassOf(player), L"useAction")) {
            ue_wrap::ParamFrame f(useFn);
            chainUse_ = f.valid() && f.Set<bool>(L"sec", false) && ue_wrap::Call(player, f);
        }
        chainPost_ = CallWithPlayer(prop, L"playerGrabbed", player);
        return GrabState::Working;
    }
    if (Grabbing(player) == prop) return GrabState::Grabbed;
    if (grabTicks_ > kGrabVerifyTicks) {
        why_ = "the grab chain did not put the prop in the hand";
        return GrabState::Failed;
    }
    return GrabState::Working;
}

}  // namespace coop::director
