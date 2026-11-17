// coop/cheats/item_spawn.cpp -- see coop/cheats/item_spawn.h.

#include "coop/cheats/item_spawn.h"

#include "coop/cheats/item_catalog.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/votv_lib.h"
#include "ue_wrap/world/world_singleton.h"

#include <cmath>
#include <string>
#include <unordered_set>

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

// The class an item of its own is spawned as, or null when it has none. A name that has no class is remembered, so its
// failing load (a probe of the package, an engine warning) runs once. Game thread.
void* LoadItemClass(const std::wstring& canonical) {
    static std::unordered_set<std::wstring> sNoClass;
    if (sNoClass.find(canonical) != sNoClass.end()) return nullptr;
    const std::wstring path = L"/Game/objects/prop_" + canonical + L".prop_" + canonical + L"_C";
    void* const cls = ue_wrap::asset_load::LoadObjectByPath(path.c_str());
    if (!cls) sNoClass.insert(canonical);
    return cls;
}

}  // namespace

bool SpawnClassName(std::string_view name, std::wstring& className) {
    UE_ASSERT_GAME_THREAD("item_spawn::SpawnClassName");
    className.clear();
    std::wstring canonical;
    if (coop::cheats::item_catalog::Resolve(name, canonical) != coop::cheats::item_catalog::Lookup::Found) return false;
    void* cls = LoadItemClass(canonical);
    if (!cls) {   // the game's own verb reads the row, and the row names the class
        void* const gamemode = ue_wrap::world_singleton::Gamemode();
        ue_wrap::votv_lib::PropObject obj;
        if (!gamemode || !ue_wrap::votv_lib::PropToObject(ue_wrap::fname_utils::StringToFName(canonical), gamemode, obj))
            return false;
        cls = obj.objectClass;
    }
    if (!cls) return false;
    className = ue_wrap::reflection::ToString(ue_wrap::reflection::NameOf(cls));
    return !className.empty();
}

Result Spawn(std::string_view name, int count, int slot, bool inFront, int* spawned) {
    UE_ASSERT_GAME_THREAD("item_spawn::Spawn");
    if (spawned) *spawned = 0;
    std::wstring canonical;
    switch (coop::cheats::item_catalog::Resolve(name, canonical)) {
        case coop::cheats::item_catalog::Lookup::Found: break;
        case coop::cheats::item_catalog::Lookup::Unfilled: return Result::NotReady;
        case coop::cheats::item_catalog::Lookup::Unknown: return Result::UnknownItem;
    }
    const std::wstring path = L"/Game/objects/prop_" + canonical + L".prop_" + canonical + L"_C";
    // An item with a class of its own is spawned as that class. The rest of the game's list (food, a row of the
    // generic prop, a row that names another class) is spawned by the gamemode's own verb, which reads the row.
    void* const cls = LoadItemClass(canonical);
    void* const gamemode = cls ? nullptr : ue_wrap::world_singleton::Gamemode();
    void* const listSpawn =
        gamemode ? ue_wrap::reflection::FindFunction(ue_wrap::reflection::ClassOf(gamemode), L"spawnPropThroughGamemode") : nullptr;
    if (!cls && !listSpawn) {
        UE_LOGI("cheats: no class at %ls and no list spawn in this world", path.c_str());
        return Result::UnknownItem;
    }
    ue_wrap::FVector at{};
    float fx = 1.f, fy = 0.f;
    if (!BodyOf(slot, at, fx, fy)) return Result::NoPlace;
    const float ahead = inFront ? kFrontCm : kBesideCm;
    int made = 0;
    for (int i = 0; i < count; ++i) {
        const ue_wrap::FVector where{at.X + fx * ahead, at.Y + fy * ahead, at.Z + kLiftCm + kStackCm * static_cast<float>(i)};
        if (cls) {
            if (E::SpawnActor(cls, where)) ++made;
            continue;
        }
        ue_wrap::ParamFrame f(listSpawn);
        const ue_wrap::reflection::FName row = ue_wrap::fname_utils::StringToFName(canonical);
        const ue_wrap::FTransform xform = ue_wrap::MakeTransform(where);
        if (f.valid() && f.SetRaw(L"prop", &row, sizeof(row)) && f.SetRaw(L"InputPin", &xform, sizeof(xform)) &&
            f.Set<int32_t>(L"Amount", 1) && ue_wrap::Call(gamemode, f) && f.Get<void*>(L"actor "))
            ++made;
    }
    if (spawned) *spawned = made;
    UE_LOGI("cheats: spawned %d of %d x '%ls' %s slot %d at (%.0f, %.0f, %.0f)", made, count, canonical.c_str(),
            inFront ? "in front of" : "at", slot, at.X, at.Y, at.Z);
    return made > 0 ? Result::Ok : Result::Failed;
}

}  // namespace coop::cheats::item_spawn
