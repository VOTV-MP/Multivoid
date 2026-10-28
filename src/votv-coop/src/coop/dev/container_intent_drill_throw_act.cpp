// coop/dev/container_intent_drill_throw_act.cpp -- the two acts of a throw that the throw arms share between the peer that
// throws (the client in throwhand and throwgrab, the host in throwhostgrab): the camera aimed at the target's opening, and
// a grabbed prop thrown by the body of the fire key's grab branch. See container_intent_drill_throw.h.

#include "container_intent_drill_throw.h"

#include "coop/dev/director/aim_fan.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"  // LookAt

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/engine/engine_physics.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace DR = coop::director;
using ue_wrap::FVector;

constexpr uint64_t kAimBoundMs = 40000;
constexpr float    kAimDeg     = 2.f;   // the camera's forward is aimed when within this of the direction to the opening

// The angle, in degrees, between the camera's forward vector and the direction from the camera to `point`. The game's throw
// (traceThrow) aims along the camera's forward, not along the use key's look trace, which reaches less far than the stand.
float CameraOffDeg(const FVector& point) {
    constexpr float kRad = 3.14159265358979f / 180.f;
    const FVector cam = E::GetCameraLocation();
    const ue_wrap::FRotator r = E::GetCameraRotation();
    const float cp = std::cos(r.Pitch * kRad);
    const float fx = cp * std::cos(r.Yaw * kRad), fy = cp * std::sin(r.Yaw * kRad), fz = std::sin(r.Pitch * kRad);
    const float dx = point.X - cam.X, dy = point.Y - cam.Y, dz = point.Z - cam.Z;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1.f) return 180.f;
    return std::acos(std::clamp((fx * dx + fy * dy + fz * dz) / len, -1.f, 1.f)) / kRad;
}

}  // namespace

void Aimer::Begin(int n) {
    fan_ = DR::AimFan();
    n_ = n;
    read_ = direct_ = false;
    startMs_ = ::GetTickCount64();
}

// The camera turned through the fan round the target's opening (its location plus its bounds' half-height, read once) until
// its forward is within kAimDeg of the direction to it; a fan that never gets there is followed by a direct look at the point.
Aimer::State Aimer::Tick(void* player, void* pack, const FVector& stand, char* why, size_t whyLen) {
    if (!pack || !player) {
        std::snprintf(why, whyLen, "the target's mirror or the player died");
        return State::Failed;
    }
    if (!read_) {
        FVector at{}, origin{}, extent{};
        if (!E::TryGetActorLocation(pack, at) || !E::GetActorBounds(pack, false, origin, extent)) {
            std::snprintf(why, whyLen, "the target's place or bounds do not read");
            return State::Failed;
        }
        point_ = {at.X, at.Y, at.Z + extent.Z};
        read_ = true;
        UE_LOGI("[CI-DRILL] %s %s throw %d aims at the opening point (%.0f,%.0f,%.0f), %.0f cm from the standpoint", Role(), ArmName(), n_,
                static_cast<double>(point_.X), static_cast<double>(point_.Y), static_cast<double>(point_.Z),
                static_cast<double>(std::hypot(at.X - stand.X, at.Y - stand.Y)));
    }
    off_ = CameraOffDeg(point_);
    const bool expired = ::GetTickCount64() - startMs_ > kAimBoundMs;
    if (!direct_) {
        switch (fan_.Tick(player, point_, off_ < kAimDeg)) {
        case DR::AimFan::State::Working:
            if (expired) {
                std::snprintf(why, whyLen, "the camera never came within %.0f deg of the opening point (%.2f deg off)",
                              static_cast<double>(kAimDeg), static_cast<double>(off_));
                return State::Failed;
            }
            return State::Working;
        case DR::AimFan::State::Failed:   // every pose of the fan was held: look at the point itself
            direct_ = true;
            startMs_ = ::GetTickCount64();   // the direct look has the whole bound
            E::SetControlRotation(E::GetController(player), DR::LookAt(E::GetCameraLocation(), point_));
            return State::Working;
        case DR::AimFan::State::Aimed: break;
        }
    } else if (off_ >= kAimDeg) {
        if (expired) {
            std::snprintf(why, whyLen, "the camera stays %.2f deg off the opening point after a direct look",
                          static_cast<double>(off_));
            return State::Failed;
        }
        E::SetControlRotation(E::GetController(player), DR::LookAt(E::GetCameraLocation(), point_));
        return State::Working;
    }
    UE_LOGI("[CI-DRILL] %s %s throw %d aimed after %d pose(s)%s, the camera %.2f deg off the opening point", Role(), ArmName(), n_,
            fan_.Poses(), direct_ ? " and a direct look" : "", static_cast<double>(off_));
    return State::Aimed;
}

// The grabbed prop thrown by the body of the fire key's grab branch (mainPlayer's Hot Objects, input_fire with a
// grabbing_actor), called as its own functions in its order: traceThrow from the grab's centre of mass for the velocity,
// throwShit on the grab's component, the prop's thrown, and dropGrabObject, which releases the grab. The fire key's input
// event stub does not run that body (a zeroed frame), and throwHoldingProp reads the hand's item, not the grab. The two
// arrays and the hit results traceThrow fills in its frame are left to the engine's heap: a dev arm throws twice.
bool ThrowGrabBranch(void* player, void* item, int n, char* why, size_t whyLen) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(player), L"grabbing_component");
    void* comp = nullptr;
    if (off >= 0) comp = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(player) + off);
    FVector com{};
    if (!comp || !R::IsLive(comp) || !E::GetComponentCenterOfMass(comp, com)) {
        std::snprintf(why, whyLen, "the grab's component or its centre of mass does not read");
        return false;
    }
    void* traceFn = R::FindDispatchFunctionCached(R::ClassOf(player), L"traceThrow");
    ue_wrap::ParamFrame trace(traceFn);
    FVector vel{};
    if (!traceFn || !trace.valid() || !trace.Set<bool>(L"debug", false) || !trace.Set<FVector>(L"weaponLocation", com) ||
        !trace.Set<FVector>(L"cameraLocation", com) || !ue_wrap::Call(player, trace) ||
        !trace.GetRaw(L"velocity_fromCamera", &vel, sizeof(vel))) {
        std::snprintf(why, whyLen, "traceThrow did not run or its velocity_fromCamera does not read");
        return false;
    }
    void* shitFn = R::FindDispatchFunctionCached(R::ClassOf(player), L"throwShit");
    ue_wrap::ParamFrame shit(shitFn);
    if (!shitFn || !shit.valid() || !shit.Set<void*>(L"InputPin", comp) || !shit.Set<FVector>(L"NewVel", vel) ||
        !ue_wrap::Call(player, shit)) {
        std::snprintf(why, whyLen, "throwShit did not run");
        return false;
    }
    const bool thrown = DR::CallWithPlayer(item, L"thrown", player);
    if (!DR::CallOnPlayer(player, L"dropGrabObject")) {
        std::snprintf(why, whyLen, "dropGrabObject did not run");
        return false;
    }
    UE_LOGI("[CI-DRILL] %s %s throw %d: traceThrow velocity (%.0f,%.0f,%.0f) from the grab's centre (%.0f,%.0f,%.0f), thrown "
            "called=%d, after dropGrabObject the grab is %s", Role(), ArmName(), n, static_cast<double>(vel.X), static_cast<double>(vel.Y),
            static_cast<double>(vel.Z), static_cast<double>(com.X), static_cast<double>(com.Y), static_cast<double>(com.Z),
            thrown ? 1 : 0, DR::Grabbing(player) == item ? "still held" : "released");
    return true;
}

}  // namespace coop::dev::container_intent_drill::detail
