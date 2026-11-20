// coop/player/level_visit_sky.h -- the world's sky held off while a visited map is loaded into the running world.

// A map loaded in place, far below the world, would have all of the world's sky: the fog that covers it, the sun and the
// sky light that light a maze the game leaves dark, the unbound post process that sets the view. The visit hides what is
// shown on this machine and holds every world sky component against the day-night cycle's own writes (a pre hook on
// SetVisibility, armed only during the visit, refuses a write on a held component and keeps its value), and at the visit's
// end each component takes the value the cycle last asked for, or the one it had when the visit began (coop/player/level_visit).

#pragma once

#include <vector>

namespace coop::level_visit_sky {

// Hold the world's sky components and switch off its post process volumes; `mapLevel` is the visited ULevel, whose own
// lights and volumes are left alone and only counted. Every readable component is held; the shown ones are hidden. Game thread.
void Hide(void* mapLevel);

// The visit's end: the hook is disarmed and each held component takes its wanted value (the cycle's last write, else its
// value before the visit); the volumes are switched on again. Nothing is done when nothing is held. Game thread.
void Show();

// [dev] Instruments for the level-visit drill: what the last Show applied, per held component. `before` is whether it
// showed when the visit began, `intended` what the world wanted at the end (differing from `before` only when the day-night
// cycle wrote it during the visit). Game thread.
struct SkyEnd {
    void* comp;
    bool before;
    bool intended;
};
const std::vector<SkyEnd>& LastVisitEnd();
// Components the last Hide could not read a bVisible of (not held).
int LastVisitUnreadable();
// One component's own bVisible; false when it is gone or does not read, `shown` untouched then. Game thread.
bool ReadShown(void* comp, bool* shown);

}  // namespace coop::level_visit_sky
