// coop/player/level_visit_sky.h -- the world's sky held off while a visited map is loaded into the running world.

// A map loaded in place, far below the world, would have all of the world's sky: the fog that covers it, the sun and the
// sky light that light a maze the game leaves dark, the unbound post process that sets the view. The visit hides it on
// this machine, keeps it hidden against the day-night cycle, and puts it back (coop/player/level_visit).

#pragma once

namespace coop::level_visit_sky {

// Hide the world's sky components and switch off its post process volumes; `mapLevel` is the visited ULevel, whose own
// lights and volumes are left alone and only counted. Game thread.
void Hide(void* mapLevel);

// Per tick while the visit runs: what the day-night cycle shows again is hidden again, once a second. Game thread.
void Hold();

// Show what Hide hid and switch its volumes on again; nothing is done when nothing is hidden. Game thread.
void Show();

}  // namespace coop::level_visit_sky
