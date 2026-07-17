// ue_wrap/spawn_gate.cpp -- see spawn_gate.h for the contract + the 2026-07-04
// join-window BeginDeferred-null post-mortem this closes.

#include "ue_wrap/engine/spawn_gate.h"

#include <chrono>
#include <cstdint>

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

namespace ue_wrap::spawn_gate {
namespace {

// Cached GameInstance + its GUObjectArray index (the bug2 pattern from
// engine.cpp::EnsureWorldContext: validate the CACHED pointer by INDEX, never
// deref it first -- [[lesson-islive-recycled-slot-blind-use-by-index]]). The
// GameInstance persists for the process lifetime once created, so after the
// first resolve the steady path is a single IsLiveByIndex.
void* g_gameInstance = nullptr;
int32_t g_gameInstanceIdx = -1;

// Full-array FindObjectByClass is too hot to run per failed resolve (the gate
// is consulted on every non-empty pump drain attempt) -- throttle re-resolves
// to one per 2 s, same policy as the trash_collect gamemode-class resolve.
std::chrono::steady_clock::time_point g_nextResolve{};

void* EnsureGameInstance() {
    if (g_gameInstance && !R::IsLiveByIndex(g_gameInstance, g_gameInstanceIdx)) {
        g_gameInstance = nullptr;
        g_gameInstanceIdx = -1;
    }
    if (!g_gameInstance) {
        const auto now = std::chrono::steady_clock::now();
        if (now < g_nextResolve) return nullptr;
        g_nextResolve = now + std::chrono::seconds(2);
        g_gameInstance = R::FindObjectByClass(P::name::GameInstanceClass);
        g_gameInstanceIdx = g_gameInstance ? R::InternalIndexOf(g_gameInstance) : -1;
    }
    return g_gameInstance;
}

}  // namespace

bool WorldRefusesSpawns() {
    void* gi = EnsureGameInstance();
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
