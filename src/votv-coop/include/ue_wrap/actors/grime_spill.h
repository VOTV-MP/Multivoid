// ue_wrap/actors/grime_spill.h -- a spilled liquid's decal (grimeProjectile_C and the grime it leaves).
//
// A liquid prop spills by spawning a grimeProjectile_C with its `grunge` class and its `velocity`: the coffee cup when
// it tips or flies fast (grime_coffee_C), beer and wine (grime_beer_C, grime_wine_C), the gas bottle on an impact
// (grime_gasoline_C). The projectile flies on its sphere, and where its trace meets a body that does not simulate it
// spawns its `grunge` decal, oriented to the surface and at its own scale, plays a splash and destroys itself. The decal
// is a grime_C (ue_wrap/devices/grime): its begin play picks its material and turns the decal component at random.
// Game thread; the two Is tests are field reads, safe inside a FinishSpawningActor callback.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace ue_wrap::grime_spill {

// One spilled decal: its class, its transform, the decal component's world rotation and its dirt.
struct Spill {
    std::wstring className;
    FVector location{};
    FRotator rotation{};
    FVector scale{1.f, 1.f, 1.f};
    FRotator decalRotation{};
    float process = 0.f;
    int32_t type = 0;
};

// Resolve grimeProjectile_C and grime_C; true once both are loaded. Call from a tick, not a callback.
bool EnsureResolved();

bool IsProjectile(void* actor);

// A beer or a wine (prop_snack_beer_C and its children), once something loaded it. A field read and a bounded walk.
bool IsBottle(void* actor);

// The projectile's `size`: a bottle's pour sets it to 1, its break leaves the class default. A field read.
bool ReadProjectileSize(void* projectile, float& out);

// grimeProjectile_C's default `size`, or -1 unread. Call from a tick.
float DefaultProjectileSize();

// A grime decal of any grime_* class, once grime_C has resolved.
bool IsDecal(void* actor);

// True when `cls` is grime_C or a grime_* class.
bool IsDecalClass(void* cls);

bool ReadSpill(void* decal, Spill& out);

// The live decal of `className` within `toleranceCm` of `location`, or null.
void* FindSpill(const std::wstring& className, const FVector& location, float toleranceCm);

// A decal as `s` describes it: deferred so its scale, dirt and type stand before its begin play paints it, then its
// decal turned. Null when the class is not a loaded grime class or the spawn failed.
void* SpawnSpill(const Spill& s);

}  // namespace ue_wrap::grime_spill
