// ue_wrap/engine/spawn_gate.cpp -- see spawn_gate.h for the contract and the
// join-window BeginDeferred-null failure it closes.

#include "ue_wrap/engine/spawn_gate.h"

#include <cstdint>

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/core/sdk_profile.h"

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

namespace ue_wrap::spawn_gate {
namespace {

}  // namespace

bool WorldRefusesSpawns() {
    void* gi = world_singleton::GameInstance();
    if (!gi) return false;  // pre-GameInstance boot: nothing to gate on
    // Virtual UObject::GetWorld -- the exact resolution UEngine::
    // GetWorldFromContextObject performs for every K2 spawn (GameInstance ->
    // WorldContext -> World). Returns null at the menu-less boot window;
    // the engine maintains the pointer across level travels, so the result is
    // as fresh as the world the next SpawnActor call would target.
    auto* vtbl = *reinterpret_cast<void***>(gi);
    using GetWorldFn = void* (*)(void*);
    void* world = reinterpret_cast<GetWorldFn>(
        vtbl[P::off::UObject_GetWorld_VtblOff / sizeof(void*)])(gi);
    if (!world) return false;
    const auto* bytes = reinterpret_cast<const uint8_t*>(world);
    return (bytes[P::off::UWorld_FlagsA] & P::off::UWorld_bIsRunningConstructionScript) != 0 ||
           (bytes[P::off::UWorld_FlagsB] & P::off::UWorld_bIsTearingDown) != 0;
}

}  // namespace ue_wrap::spawn_gate
