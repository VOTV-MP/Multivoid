// ue_wrap/engine/actor_end_play.h -- every actor's end of play, the moment the engine announces it.
//
// AActor::EndPlay is where the engine ends an actor's play: RouteEndPlay calls it for a destroy of
// any kind (UWorld::DestroyActor's Destroyed(), which a Blueprint's K2_DestroyActor on any route, a
// lifespan's expiry, a fall out of the world and a native Destroy all reach), for a level streamed
// out and for the world's teardown at a travel or the quit. In the shipped binary 17 of the 18 C++
// overrides reach it through Super; the 18th, AChaosSolverActor's, is a physics solver and never one
// of the game's entities. A native detour on its entry sees every actor that began play end it, with
// the reason, while the actor is still allocated and not yet marked for death (an override's own
// teardown before its Super call has run).
//
// Not the Blueprint event: AActor::ProcessEvent returns before UObject::ProcessEvent for a function
// that is neither native nor carries script, so ReceiveEndPlay on a class that does not implement it
// never reaches the ProcessEvent detour. An actor that never began play (a deferred spawn destroyed
// before it finished) has no end of play. Engine-wrapper layer (principle 7): no gameplay logic.

#pragma once

#include <cstdint>

namespace ue_wrap::actor_end_play {

// EEndPlayReason, as the engine passes it.
enum class Reason : uint8_t {
    Destroyed = 0,         // destroyed in play, by any route
    LevelTransition = 1,   // the world is unloading for a travel
    EndPlayInEditor = 2,   // never in a packaged game
    RemovedFromWorld = 3,  // its level was streamed out
    Quit = 4,              // the application is exiting
};

// A sink runs on the game thread inside the engine's EndPlay, before the actor's components end play
// and before a destroy marks it for death, so its reads of the actor are valid; it must be cheap,
// must not destroy or spawn, and must return at once for a reason it does not handle: a level
// transition calls it for every actor of the world. A fault in a sink is absorbed and said.
using Sink = void (*)(void* actor, Reason reason);

// Resolve AActor::EndPlay from Actor's vtable, check its body, and detour it. Idempotent; false
// (logged) when Default__Actor is not found, the slot does not lead to EndPlay's body on this build,
// or the detour does not install. Boot, after the health checks.
bool Install();
bool IsInstalled();

// Add a sink. Idempotent per sink; false when the table is full. Game thread: the detour reads the
// table lock-free, and two adders at once could take one slot.
inline constexpr int kMaxSinks = 16;
bool AddSink(Sink sink);

// The ends of play seen (actors that had begun play), and those that arrived off the game thread,
// passed to the engine without the sinks (the engine never ends an actor off it; a count above zero
// names a reading this module does not expect).
struct Stats {
    unsigned long long seen;
    unsigned long long offGameThread;
};
Stats GetStats();

}  // namespace ue_wrap::actor_end_play
