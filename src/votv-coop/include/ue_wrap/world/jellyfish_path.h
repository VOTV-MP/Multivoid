// ue_wrap/world/jellyfish_path.h -- the space jellyfish's run: the level's path (jellyfishPath_C) and its fish
// (jellyfish_C). The path's spawn() -- a no-op while a run is active -- spawns seven fish inside its own graph
// and adds each to its list, which only grows; each fish steers at the path's target, which the path's tick
// walks along its spline from the fish's average place, and at the spline's end the path destroys the seven
// and ends the run. The day cycle's 18:00 roll calls spawn with p 0.007. Engine access only. Game thread.
#pragma once

#include "ue_wrap/core/types.h"  // FVector

namespace ue_wrap::jellyfish_path {

// The level's path in the running world, or null when none is loaded.
void* Path();

// The path's own spawn(), as the day cycle's roll calls it. False when there is no path or the verb did not
// resolve; true when it was called, whatever its body then did (a run already active, or a refusal).
bool CallSpawn();

// Whether the path's run is active. False when there is no path or the field did not resolve.
bool ReadActive(bool& active);

// How many fish the path's list holds, the destroyed ones of earlier runs included. False when there is no
// path or the field did not resolve.
bool ReadListed(int& listed);

// Every live fish of the running world with its place, in no particular order; returns the count. The
// callback may be null, and must not create or destroy objects.
using FishFn = void (*)(void* ctx, void* fish, const FVector& at);
int ForEachFish(FishFn fn, void* ctx);

}  // namespace ue_wrap::jellyfish_path
