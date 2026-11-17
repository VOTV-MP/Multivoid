// coop/cheats/self_teleport.cpp -- see coop/cheats/self_teleport.h.

#include "coop/cheats/self_teleport.h"

#include "coop/comms/chat_feed.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/teleport_client.h"
#include "l10n/l10n.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_nav.h"

#include <atomic>
#include <cmath>

namespace coop::cheats::self_teleport {
namespace {

namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;

constexpr float kBehindCm = 100.f;
// How far the NavMesh may be from the wanted point: a point beside a wall slides to the floor on its own side.
constexpr ue_wrap::FVector kNavExtent{150.f, 150.f, 250.f};

std::atomic<unsigned> g_landed{0};    // the moves that landed, and the ones that came to nothing
std::atomic<unsigned> g_refused{0};
std::atomic<float> g_targetX{0.f}, g_targetY{0.f}, g_targetZ{0.f};   // where the last landing put the body's centre

void NoSafePlace() {
    coop::chat_feed::Push(std::string(l10n::T("No safe place to land there.")), coop::chat_feed::Keep::Transient);
}

// Teleport this machine's body onto the NavMesh point nearest `want`, facing `yaw`, keeping the view's pitch and roll.
// The NavMesh is the floor the game lets a walker stand on, so the body never lands in a wall or under the ground (a
// body put inside geometry is hidden by the game's out-of-bounds check). False, with the player told and nothing moved,
// when no NavMesh point lies near `want`.
bool LandNear(void* local, const ue_wrap::FVector& want, float yaw) {
    ue_wrap::FVector floor{};
    const float halfHeight = E::GetActorCharacterHalfHeight(local);
    if (!E::ProjectToNav(local, want, kNavExtent, &floor) || !(halfHeight > 0.f)) {
        UE_LOGW("cheats: no NavMesh within reach of (%.0f, %.0f, %.0f) -- the player stays where it is", want.X, want.Y,
                want.Z);
        NoSafePlace();
        g_refused.fetch_add(1);
        return false;
    }
    ue_wrap::FRotator view{};
    if (void* const ctrl = E::GetController(local)) view = E::GetControlRotation(ctrl);
    coop::teleport_client::ApplyArgs args{};
    args.locX = floor.X;
    args.locY = floor.Y;
    args.locZ = floor.Z + halfHeight;   // the capsule's centre stands one half-height above the floor
    args.rotPitch = view.Pitch;
    args.rotYaw = yaw;
    args.rotRoll = view.Roll;
    // The call reports whether a teleport was dispatched, not where the body ended: a landing counts only when it did.
    if (!coop::teleport_client::ApplyLocally(args)) {
        UE_LOGW("cheats: the game's teleport to (%.0f, %.0f, %.0f) reported failure -- the player stays where it is",
                args.locX, args.locY, args.locZ);
        g_refused.fetch_add(1);
        return false;
    }
    g_targetX.store(args.locX);
    g_targetY.store(args.locY);
    g_targetZ.store(args.locZ);
    g_landed.fetch_add(1);
    return true;
}

void MoveToPlayer(int slot) {
    if (slot < 0 || slot >= coop::players::kMaxPeers) return;
    auto& reg = coop::players::Registry::Get();
    void* const local = reg.Local();
    coop::RemotePlayer* const puppet = reg.Puppet(static_cast<uint8_t>(slot));
    ue_wrap::FVector at{};
    if (!local || !puppet || !puppet->valid() || !puppet->HasPose() || !puppet->TryGetLocation(at)) {
        UE_LOGW("cheats: cannot teleport to slot %d -- that player has no place here yet", slot);
        return;
    }
    const ue_wrap::FVector aim = puppet->GetSyncedAimDirection();
    const float len = std::sqrt(aim.X * aim.X + aim.Y * aim.Y);
    const float fx = len > 0.001f ? aim.X / len : 1.f, fy = len > 0.001f ? aim.Y / len : 0.f;
    const float yawDeg = std::atan2(fy, fx) * 57.2957795f;
    LandNear(local, ue_wrap::FVector{at.X - fx * kBehindCm, at.Y - fy * kBehindCm, at.Z}, yawDeg);
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
    LandNear(local, at, yaw);
}

}  // namespace

Landings Outcomes() {
    return {g_landed.load(), g_refused.load(), ue_wrap::FVector{g_targetX.load(), g_targetY.load(), g_targetZ.load()}};
}

void ToPlayer(int slot) { GT::Post([slot] { MoveToPlayer(slot); }); }

void Unstuck() { GT::Post([] { Lift(); }); }

}  // namespace coop::cheats::self_teleport
