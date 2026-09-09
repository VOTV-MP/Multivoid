// coop/dev/leak_probe.h -- dev-only RAM-leak ATTRIBUTION probe (RULE 3: never ships).
//
// The answer to "what part leaks memory" without bisecting subsystems by hand: attribute the
// growth directly. Gated on ini `leak_probe=1`. Every kInterval it walks GUObjectArray,
// histograms LIVE UObjects by UClass, and logs the total live-object count plus the classes
// whose count GREW the most since the previous snapshot. A class climbing monotonically IS a
// UObject leak -- orphaned particle components, un-destroyed actors, leaked widgets or anim
// instances -- and the line names the exact class, and so whether it is one we spawn or a
// base-game one.
//
// Discriminator: a TOTAL live-object count climbing in lockstep with RSS is a UObject leak,
// which this probe pinpoints. A FLAT total while RSS climbs means raw heap instead -- FString,
// FText, TArray, malloc -- which needs an allocator hook with call-stack attribution, the next
// tool up. Game thread only (it walks GUObjectArray and reads ClassOf), and cheap enough at
// the ~4 s cadence: class names are resolved only for the handful of top growers at log time.

#pragma once

namespace coop::dev::leak_probe {

// Self-gated (reads ini `leak_probe` once, cached) + self-throttled (~4 s). Safe to call
// every frame from the pump composite; a no-op until armed and between intervals.
void Tick();

}  // namespace coop::dev::leak_probe
