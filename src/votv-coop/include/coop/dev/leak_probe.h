// coop/dev/leak_probe.h -- dev-only RAM-leak ATTRIBUTION probe (RULE 3: never ships).
//
// The RULE-1 answer to "what part leaks memory": instead of bisecting subsystems by
// hand, attribute the growth directly. Gated on ini `leak_probe=1`. Every kInterval it
// walks GUObjectArray (the same primitive we already resolve), histograms LIVE UObjects
// by UClass, and logs the total live-object count + the classes whose count GREW the
// most since the previous snapshot. A class climbing monotonically IS a UObject leak
// (orphaned ParticleSystemComponents, un-destroyed actors, leaked widgets/anim
// instances, etc.) and the line names the exact class -- and whether it is a class we
// spawn (our bug) or a base-game class (VOTV's own leak).
//
// Discriminator: if the TOTAL live-object count climbs in lockstep with RSS -> it is a
// UObject leak (this probe pinpoints the class). If the total is FLAT while RSS climbs
// -> the leak is raw heap (FString/FText/TArray/malloc), NOT UObjects -> escalate to a
// GMalloc allocator hook with call-stack attribution (the next tool up the ladder).
//
// Game thread only (it walks GUObjectArray + reads ClassOf). Cheap enough for a dev
// probe at the ~4 s cadence: a pointer-keyed histogram over the object array, with
// ClassNameOf resolved ONLY for the handful of top growers at log time.

#pragma once

namespace coop::dev::leak_probe {

// Self-gated (reads ini `leak_probe` once, cached) + self-throttled (~4 s). Safe to call
// every frame from the pump composite; a no-op until armed and between intervals.
void Tick();

}  // namespace coop::dev::leak_probe
