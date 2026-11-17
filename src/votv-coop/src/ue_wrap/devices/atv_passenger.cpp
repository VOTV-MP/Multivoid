// ue_wrap/devices/atv_passenger.cpp -- see ue_wrap/devices/atv_passenger.h.

#include "ue_wrap/devices/atv_passenger.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_nav.h"

#include <cmath>
#include <cstdint>

namespace ue_wrap::atv_passenger {
namespace {

namespace R = reflection;
namespace E = engine;

// The seats, in the ATV root's local space (X forward, Y right, Z up, cm): the rider's actor, a capsule centre. Measured on
// the game's ATV by the passenger drill's `geom` lines: behind the driver's seat (its top at z 71 under x -40 to -60, the
// driver's own centre at 90, so a seated body's centre rides 19 above what it sits on) the rig has a flat rear deck at z 56
// to 62 from x -40 to -130 and past y +-60, 53 at its back edge. Three riders sit abreast across it, 19 above its surface.
constexpr FVector kSeatOffsets[kSeats] = {
    {-80.f,   0.f, 75.f},   // 0: behind the driver, on the middle of the deck (surface z 56)
    {-80.f, -45.f, 80.f},   // 1: the deck's left (surface z 61)
    {-80.f,  45.f, 80.f},   // 2: the deck's right (surface z 61)
};

// UCharacterMovementComponent::SetMovementMode's byte: MOVE_None stops every input, MOVE_Walking is the ground default.
constexpr uint8_t kMoveNone = 0;
constexpr uint8_t kMoveWalking = 1;
constexpr float kExitSideCm = 230.f;    // beside the rig, past the wheels
constexpr float kExitRearCm = -300.f;   // behind the rack
constexpr float kExitUpCm = 220.f;      // the trace down starts this far above the candidate's rig height
constexpr float kExitDownCm = 520.f;    // and ends this far below it
constexpr float kExitMaxDropCm = 260.f; // ground farther under the rig than this is a ledge, not a floor
constexpr float kExitMinNormalZ = 0.6f; // and a slope steeper than this is not one to stand on
constexpr float kRestLiftCm = 4.f;      // a capsule centre rests this far above the ground it was measured on
constexpr float kFallLimitCms = 800.f;  // the sit verb's own fall-velocity gate (|fallVeloc.Z| < 800)

void* g_attachFn = nullptr;      // Actor::K2_AttachToComponent
void* g_setRelLocFn = nullptr;   // Actor::K2_SetActorRelativeLocation
void* g_setMoveFn = nullptr;     // CharacterMovementComponent::SetMovementMode

void* ActorFn(void** cache, const wchar_t* name) {
    if (!*cache) {
        if (void* c = R::FindClass(L"Actor")) *cache = R::FindFunction(c, name);
    }
    return *cache;
}

FVector Rotate(const FRotator& r, const FVector& v) {
    constexpr float kRad = 0.01745329252f;
    const float sp = std::sin(r.Pitch * kRad), cp = std::cos(r.Pitch * kRad);
    const float sy = std::sin(r.Yaw * kRad),   cy = std::cos(r.Yaw * kRad);
    const float sr = std::sin(r.Roll * kRad),  cr = std::cos(r.Roll * kRad);
    // FRotationMatrix's three axes, forward then right then up.
    const FVector ax{cp * cy, cp * sy, sp};
    const FVector ay{sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
    const FVector az{-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
    return {ax.X * v.X + ay.X * v.Y + az.X * v.Z,
            ax.Y * v.X + ay.Y * v.Y + az.Y * v.Z,
            ax.Z * v.X + ay.Z * v.Y + az.Z * v.Z};
}

bool SetMovementMode(void* character, uint8_t mode) {
    void* cmc = E::GetCharacterMovementComponent(character);
    if (!cmc) return false;
    if (!g_setMoveFn) {
        g_setMoveFn = R::FindFunction(R::ClassOf(cmc), L"SetMovementMode");
        if (!g_setMoveFn) {
            if (void* c = R::FindClass(L"CharacterMovementComponent")) g_setMoveFn = R::FindFunction(c, L"SetMovementMode");
        }
    }
    if (!g_setMoveFn) return false;
    ParamFrame f(g_setMoveFn);
    if (!f.valid()) return false;
    f.Set<uint8_t>(L"NewMovementMode", mode);
    f.Set<uint8_t>(L"NewCustomMode", uint8_t{0});
    return Call(cmc, f);
}

bool SetRelativeLocation(void* actor, const FVector& rel) {
    void* fn = ActorFn(&g_setRelLocFn, L"K2_SetActorRelativeLocation");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.SetRaw(L"NewRelativeLocation", &rel, sizeof(rel));
    f.Set<bool>(L"bSweep", false);
    f.Set<bool>(L"bTeleport", true);
    return Call(actor, f);
}

// K2_AttachToComponent to `atv`'s root body: the location snapped to it, the rider's own facing and scale kept.
bool AttachToRoot(void* rider, void* atv) {
    void* root = E::RootComponentOf(atv);
    void* fn = ActorFn(&g_attachFn, L"K2_AttachToComponent");
    if (!root || !fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    R::FName none{};
    f.Set<void*>(L"Parent", root);
    f.SetRaw(L"SocketName", &none, sizeof(none));
    f.Set<uint8_t>(L"LocationRule", uint8_t{2});   // EAttachmentRule::SnapToTarget
    f.Set<uint8_t>(L"RotationRule", uint8_t{1});   // KeepWorld: the rider keeps its own facing
    f.Set<uint8_t>(L"ScaleRule", uint8_t{1});      // KeepWorld
    f.Set<bool>(L"bWeldSimulatedBodies", false);
    return Call(rider, f);
}

// The first hit of a trace down from `top` to `bottom`, or false.
bool Ground(void* ctx, const FVector& top, const FVector& bottom, trace::Hit& hit) {
    return trace::LineHitStatDyn(ctx, top, bottom, &hit) && hit.blocked && hit.normal.Z >= kExitMinNormalZ;
}

}  // namespace

FVector SeatOffset(int seat) {
    return seat >= 0 && seat < kSeats ? kSeatOffsets[seat] : FVector{};
}

bool SeatWorldLocation(void* atv, int seat, FVector& out) {
    if (seat < 0 || seat >= kSeats) return false;
    FVector loc{};
    FRotator rot{};
    if (!atv::GetRootTransform(atv, loc, rot)) return false;
    const FVector d = Rotate(rot, kSeatOffsets[seat]);
    out = {loc.X + d.X, loc.Y + d.Y, loc.Z + d.Z};
    return true;
}

bool SeatRider(void* rider, void* atv, int seat, bool local) {
    if (!rider || !atv || !R::IsLive(rider) || !R::IsLive(atv) || seat < 0 || seat >= kSeats) return false;
    // Collision off first, so the attach's own snap cannot shove the rig; a local player's movement stops with it.
    E::SetActorEnableCollision(rider, false);
    if (local) SetMovementMode(rider, kMoveNone);
    if (!AttachToRoot(rider, atv) || !SetRelativeLocation(rider, kSeatOffsets[seat])) {
        E::DetachActorFromParent(rider);
        E::SetActorEnableCollision(rider, true);
        if (local) SetMovementMode(rider, kMoveWalking);
        return false;
    }
    return true;
}

bool MoveRider(void* rider, void* atv, int seat) {
    if (!rider || !atv || !R::IsLive(rider) || !R::IsLive(atv) || seat < 0 || seat >= kSeats) return false;
    return SetRelativeLocation(rider, kSeatOffsets[seat]);
}

bool ReleaseRider(void* rider, bool local, const FVector* placeAt) {
    if (!rider || !R::IsLive(rider)) return false;
    E::DetachActorFromParent(rider);
    if (placeAt) {
        FRotator rot{};
        E::TryGetActorRotation(rider, rot);
        E::TeleportTo(rider, *placeAt, FRotator{0.f, rot.Yaw, 0.f});
    }
    E::SetActorEnableCollision(rider, true);
    if (local) SetMovementMode(rider, kMoveWalking);
    return true;
}

bool IsRiderOf(void* rider, void* atv) {
    void* parent = nullptr;
    return rider && atv && E::GetAttachParentActor(rider, parent) && parent == atv;
}

bool FindGroundBelow(void* worldCtx, const FVector& from, float halfHeight, FVector& out) {
    trace::Hit hit;
    if (!worldCtx || !Ground(worldCtx, {from.X, from.Y, from.Z + 100.f}, {from.X, from.Y, from.Z - kExitDownCm}, hit))
        return false;
    out = {from.X, from.Y, hit.point.Z + halfHeight + kRestLiftCm};
    return true;
}

bool FindExitSpot(void* atv, int seat, float halfHeight, FVector& out) {
    FVector at{};
    FRotator rot{};
    if (!atv || !atv::GetRootTransform(atv, at, rot)) return false;
    // Beside the rig on the seat's own side first, then the other side, then the rear and the corners.
    const float s = seat >= 0 && seat < kSeats && kSeatOffsets[seat].Y < 0.f ? -1.f : 1.f;
    const FVector cand[] = {
        {0.f, s * kExitSideCm, 0.f}, {0.f, -s * kExitSideCm, 0.f}, {kExitRearCm, 0.f, 0.f},
        {-110.f, s * kExitSideCm, 0.f}, {-110.f, -s * kExitSideCm, 0.f}, {140.f, s * kExitSideCm, 0.f},
    };
    // The rig's own height; the rotation's yaw alone, so a tipped rig does not lift a candidate off the ground.
    const FRotator yawOnly{0.f, rot.Yaw, 0.f};
    for (const FVector& c : cand) {
        const FVector d = Rotate(yawOnly, c);
        const FVector p{at.X + d.X, at.Y + d.Y, at.Z};
        // A wall between the rig and the spot, at chest height.
        if (trace::LineBlockedStatDyn(atv, {at.X, at.Y, at.Z + 60.f}, {p.X, p.Y, at.Z + 60.f}) != 0) continue;
        trace::Hit hit;
        if (!Ground(atv, {p.X, p.Y, at.Z + kExitUpCm}, {p.X, p.Y, at.Z - kExitDownCm}, hit)) continue;
        if (at.Z - hit.point.Z > kExitMaxDropCm) continue;
        const FVector spot{p.X, p.Y, hit.point.Z + halfHeight + kRestLiftCm};
        // Headroom for the whole capsule.
        if (trace::LineBlockedStatDyn(atv, {p.X, p.Y, hit.point.Z + 10.f}, {p.X, p.Y, spot.Z + halfHeight}) != 0) continue;
        out = spot;
        return true;
    }
    FVector nav{};
    if (E::ProjectToNav(atv, at, {600.f, 600.f, 300.f}, &nav)) {
        out = {nav.X, nav.Y, nav.Z + halfHeight + kRestLiftCm};
        return true;
    }
    return false;
}

bool MayBoard(void* player, const char** why) {
    auto refuse = [&](const char* w) { if (why) *why = w; return false; };
    if (!player || !R::IsLive(player)) return refuse("no player");
    void* pcls = R::ClassOf(player);
    if (!pcls) return refuse("no player class");
    const int32_t off = R::FindPropertyOffset(pcls, L"fallVeloc");
    if (off >= 0) {
        const FVector v = *reinterpret_cast<FVector*>(reinterpret_cast<uint8_t*>(player) + off);
        if (std::fabs(v.Z) >= kFallLimitCms) return refuse("falling");
    }
    // Empty hands: nothing held in the hand and nothing held by the grav hand.
    E::MainPlayerGrabState gs{};
    if (E::ReadMainPlayerGrabState(player, gs) && (gs.holdingActor || gs.grabbingActor)) return refuse("hands are full");
    return true;
}

}  // namespace ue_wrap::atv_passenger
