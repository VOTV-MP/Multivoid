#include "coop/remote_player.h"

#include "coop/nameplate.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/puppet.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <cmath>
#include <utility>

namespace coop {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace Pup = ue_wrap::puppet;

bool RemotePlayer::Spawn() {
    // The local player gives us BOTH the spawn anchor and the skin to copy.
    void* local = R::FindObjectByClass(P::name::MainPlayerClass);
    if (!local) {
        UE_LOGW("RemotePlayer::Spawn: no local mainPlayer_C (not in gameplay yet)");
        return false;
    }

    // The pawn actor origin is the capsule CENTRE (~half a body above the feet);
    // a puppet's mesh root sits at its actor origin, so copying the centre Z makes
    // it float. Subtract the capsule half-height to land the feet on the ground.
    ue_wrap::FVector loc = E::GetActorLocation(local);
    const float halfH = Pup::GetCapsuleHalfHeight(local);
    loc.Z -= halfH;
    UE_LOGI("RemotePlayer::Spawn: local centreZ=%.1f capsuleHalfH=%.1f -> feetZ=%.1f",
            loc.Z + halfH, halfH, loc.Z);

    // Place the puppet a couple metres in FRONT of the local player so it's
    // immediately in view, then face it back toward the player (so its face --
    // not its back -- is what the user sees). Independent of where P1 looks.
    ue_wrap::FVector fwd = E::GetActorForwardVector(local);
    loc.X += fwd.X * 250.f;
    loc.Y += fwd.Y * 250.f;

    void* skin = Pup::GetMeshPlayerVisibleAsset(local);
    if (!skin) {
        UE_LOGE("RemotePlayer::Spawn: could not read local skin asset");
        return false;
    }
    void* animClass = Pup::GetMeshPlayerVisibleAnimClass(local);

    actor_ = Pup::SpawnPuppet(loc, skin, animClass);
    if (!actor_) {
        UE_LOGE("RemotePlayer::Spawn: SpawnPuppet failed");
        return false;
    }

    // Face the puppet toward the local player (yaw = direction from puppet to
    // player = -forward). atan2 in degrees.
    const float yaw = std::atan2(-fwd.Y, -fwd.X) * 57.29578f;
    SetFacing(yaw);

    // Show the floating nameplate above this body (lazily hooks the HUD draw).
    nameplate::Register(this);

    UE_LOGI("RemotePlayer::Spawn: puppet=%p at (%.0f,%.0f,%.0f) yaw=%.0f nick='%ls'",
            actor_, loc.X, loc.Y, loc.Z, yaw, nickname_.c_str());
    return true;
}

bool RemotePlayer::valid() const { return actor_ != nullptr && R::IsLive(actor_); }

void RemotePlayer::Drive(const ue_wrap::FVector& location, float yaw, float speed) {
    if (!valid()) { actor_ = nullptr; return; }
    E::SetActorLocation(actor_, location);
    E::SetActorRotation(actor_, ue_wrap::FRotator{0.f, yaw, 0.f});
    Pup::DriveAnimBP(actor_, speed);
}

bool RemotePlayer::SetLocation(const ue_wrap::FVector& location) {
    if (!actor_) return false;
    return E::SetActorLocation(actor_, location);
}

bool RemotePlayer::SetFacing(float yaw) {
    if (!actor_) return false;
    return E::SetActorRotation(actor_, ue_wrap::FRotator{0.f, yaw, 0.f});
}

ue_wrap::FVector RemotePlayer::GetLocation() const {
    if (!actor_) return {};
    return E::GetActorLocation(actor_);
}

ue_wrap::FVector RemotePlayer::GetHeadPosition() const {
    ue_wrap::FVector p = GetLocation();
    p.Z += 200.f;  // ~head height + a little, so the tag floats just above
    return p;
}

void RemotePlayer::SetNickname(std::wstring name) { nickname_ = std::move(name); }

}  // namespace coop
