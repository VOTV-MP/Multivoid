// coop/save/puppet_save_skip.h -- a puppet is never a record of the world save.
// The game saves every actor carrying int_save whose ignoreSave answers false (mainGamemode_C.saveObjects), and
// mainPlayer_C answers false, since the player is a record of its own save (betterObjectsData). A puppet is a
// mainPlayer_C too, so the host's save would gain a player record per connected peer. The watch on
// mainPlayer_C::ignoreSave answers true for a puppet's body and lets the local player's call run. Local, no wire.

#pragma once

#include <cstdint>

namespace coop::puppet_save_skip {

// Registers, then settles, the ignoreSave watch. Called every frame by the harness pump; cheap once settled. Game
// thread.
void Install();

}  // namespace coop::puppet_save_skip
