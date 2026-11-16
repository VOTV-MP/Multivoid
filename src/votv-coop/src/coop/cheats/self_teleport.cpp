// coop/cheats/self_teleport.cpp -- see coop/cheats/self_teleport.h.

#include "coop/cheats/self_teleport.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/teleport_client.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>

namespace coop::cheats::self_teleport {
namespace {

namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;

constexpr float kBehindCm = 100.f;
constexpr float kLiftCm = 50.f;
constexpr float kUnstuckCm = 150.f;

void MoveToPlayer(int slot) {
    if (slot < 0 || slot >= coop::players::kMaxPeers) return;
    auto& reg = coop::players::Registry::Get();
    coop::RemotePlayer* const puppet = reg.Puppet(static_cast<uint8_t>(slot));
    ue_wrap::FVector at{};
    if (!puppet || !puppet->valid() || !puppet->HasPose() || !puppet->TryGetLocation(at)) {
        UE_LOGW("cheats: cannot teleport to slot %d -- that player has no place here yet", slot);
        return;
    }
    const ue_wrap::FVector aim = puppet->GetSyncedAimDirection();
    const float len = std::sqrt(aim.X * aim.X + aim.Y * aim.Y);
    const float fx = len > 0.001f ? aim.X / len : 1.f, fy = len > 0.001f ? aim.Y / len : 0.f;
    const float yawDeg = std::atan2(fy, fx) * 57.2957795f;
    coop::teleport_client::ApplyArgs args{};
    args.locX = at.X - fx * kBehindCm;
    args.locY = at.Y - fy * kBehindCm;
    args.locZ = at.Z + kLiftCm;
    args.rotYaw = yawDeg;
    coop::teleport_client::ApplyLocally(args);
}

void Lift() {
    void* const local = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!local || !E::TryGetActorLocation(local, at)) {
        UE_LOGW("cheats: cannot unstick -- this player's place does not read");
        return;
    }
    float yaw = 0.f;
    if (void* const ctrl = E::GetController(local)) yaw = E::GetControlRotation(ctrl).Yaw;
    coop::teleport_client::ApplyArgs args{};
    args.locX = at.X;
    args.locY = at.Y;
    args.locZ = at.Z + kUnstuckCm;
    args.rotYaw = yaw;
    coop::teleport_client::ApplyLocally(args);
}

}  // namespace

void ToPlayer(int slot) { GT::Post([slot] { MoveToPlayer(slot); }); }

void Unstuck() { GT::Post([] { Lift(); }); }

}  // namespace coop::cheats::self_teleport
