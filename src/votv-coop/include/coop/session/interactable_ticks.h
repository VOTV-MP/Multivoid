// coop/session/interactable_ticks.h -- one run of the session tick's per-module passes, kept in the order
// coop/session/subsystems runs them: the desk and its devices, the drives, the mail and signal shadows, the
// refiner, voice, the order queue, the coin gun's barrier and the base windows. Each pass says what it does
// where it is called. Game thread, from the session tick.

#pragma once

namespace coop::interactable_ticks {

void Tick();

}  // namespace coop::interactable_ticks
