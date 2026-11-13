// coop/player/nightmare_roll.h -- a client's nightmare roll, at the host's pace, and the hiding of a dreamer's copy.
// Game thread throughout.

// The game rolls for a nightmare from its sleep: a latent Delay of 500 s, then RandomBoolWithWeight(bedSleepProb()),
// looping while it sleeps; a win runs wakeup() and createDream(). The Delay counts world time, which the host's sleep
// runs at sleep_rest::kNativeSleepDilation, so a host rolls every 500 / 20 real seconds. A client's rest keeps its world
// at 1 (coop/player/sleep_rest), so the game's own roll would come once in 500 real seconds, past the end of any rest.

// A client's own roll is therefore held off (sleep_sync writes its dreamProbability 0, which bedSleepProb answers
// before the bed) and this module makes it in the game's place: every 25 real seconds of a rest, with the chance the
// game's own bedSleepProb answers (ue_wrap/actors/sleep ReadNightmareChance, none with nightmares off in the settings),
// and a win runs the game's own wakeup() and createDream(). The nightmare is this player's alone: the dream is spawned
// on this machine, only this player is woken and teleported into it, and every other player keeps sleeping. The dream's
// own spawns (a mannequin, a boulder) are neither props nor lane actors, so no lane carries them to another peer. The
// rest state it leaves (dreaming) reaches the others through sleep_sync. A host's roll is the game's own and is not
// touched.

#pragma once

namespace coop::net { class Session; }

namespace coop::nightmare_roll {

// Cache the session and take sleep_sync's rest-state announcements. Called from the subsystems Install fanout.
void Install(coop::net::Session* session);

// A dream is spawned far outside the map and the dreamer is teleported into it, so its copy on every other machine
// would cross the world to it and back. On every peer the copy of a player whose rest state is dreaming is hidden for the
// length of the dream (AActor::SetActorHiddenInGame) and shown again when it is not. The rest state is taken from
// sleep_sync's announcement of a change, and a copy that is spawned after its player went to dream is hidden by the
// puppet drive's call below.
//
// The puppet drive calls it once for each puppet it has just spawned, so a copy that did not exist when its player's state
// arrived (a late join, a world change) is hidden too.
void OnPuppetSpawned(int slot);

// Per pump tick, on a connected client only: count the real seconds of the client's rest (sleep_rest's announced rest:
// a bool, no engine read) and roll at each 25. The tick stays because nothing announces the passing of those seconds;
// the rest and its end do not need a poll here.
void Tick();

// The session ended: the count starts again and every copy this hid is shown.
void OnDisconnect();

}  // namespace coop::nightmare_roll
