// coop/cheats/item_spawn.cpp -- see coop/cheats/item_spawn.h.

#include "coop/cheats/item_spawn.h"

#include "coop/cheats/item_catalog.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <string>

namespace coop::cheats::item_spawn {
namespace {

namespace E = ue_wrap::engine;

constexpr float kFrontCm = 200.f;    // ahead of the player, for "in front"
constexpr float kBesideCm = 80.f;    // ahead of the player, for "at the player": clear of the body
constexpr float kLiftCm = 40.f;      // above the player's pivot, so a prop falls onto the floor and not into it
constexpr float kStackCm = 35.f;     // each further prop of one request, higher

// Where the player in `slot` stands and which way they face (a unit vector in the ground plane). False when the body
// has no place to read.
bool BodyOf(int slot, ue_wrap::FVector& at, float& fx, float& fy) {
    auto& reg = coop::players::Registry::Get();
    if (slot == 0) {
        void* const local = reg.Local();
        if (!local || !E::TryGetActorLocation(local, at)) return false;
        float yawDeg = 0.f;
        if (void* const ctrl = E::GetController(local)) yawDeg = E::GetControlRotation(ctrl).Yaw;
        const float rad = yawDeg * 0.0174532925f;
        fx = std::cos(rad);
        fy = std::sin(rad);
        return true;
    }
    if (slot < 0 || slot >= coop::players::kMaxPeers) return false;
    coop::RemotePlayer* const puppet = reg.Puppet(static_cast<uint8_t>(slot));
    if (!puppet || !puppet->valid() || !puppet->HasPose() || !puppet->TryGetLocation(at)) return false;
    const ue_wrap::FVector aim = puppet->GetSyncedAimDirection();
    const float len = std::sqrt(aim.X * aim.X + aim.Y * aim.Y);
    if (!(len > 0.001f)) return false;
    fx = aim.X / len;
    fy = aim.Y / len;
    return true;
}

}  // namespace

Result Spawn(std::string_view name, int count, int slot, bool inFront, int* spawned) {
    UE_ASSERT_GAME_THREAD("item_spawn::Spawn");
    if (spawned) *spawned = 0;
    std::wstring canonical;
    if (!coop::cheats::item_catalog::Resolve(name, canonical)) return Result::UnknownItem;
    const std::wstring path = L"/Game/objects/prop_" + canonical + L".prop_" + canonical + L"_C";
    void* const cls = ue_wrap::asset_load::LoadObjectByPath(path.c_str());
    if (!cls) {
        UE_LOGI("cheats: no class at %ls", path.c_str());
        return Result::UnknownItem;
    }
    ue_wrap::FVector at{};
    float fx = 1.f, fy = 0.f;
    if (!BodyOf(slot, at, fx, fy)) return Result::NoPlace;
    const float ahead = inFront ? kFrontCm : kBesideCm;
    int made = 0;
    for (int i = 0; i < count; ++i) {
        const ue_wrap::FVector where{at.X + fx * ahead, at.Y + fy * ahead, at.Z + kLiftCm + kStackCm * static_cast<float>(i)};
        if (E::SpawnActor(cls, where)) ++made;
    }
    if (spawned) *spawned = made;
    UE_LOGI("cheats: spawned %d of %d x '%ls' %s slot %d at (%.0f, %.0f, %.0f)", made, count, canonical.c_str(),
            inFront ? "in front of" : "at", slot, at.X, at.Y, at.Z);
    return made > 0 ? Result::Ok : Result::Failed;
}

}  // namespace coop::cheats::item_spawn
