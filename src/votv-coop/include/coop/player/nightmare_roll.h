// coop/player/nightmare_roll.h -- a client's nightmare roll, at the host's pace, and the hiding of a dreamer's copy.
// Game thread throughout.

// The game rolls once per sleep: a latent Delay of 500 s after fellAsleep, then RandomBoolWithWeight(bedSleepProb()); a
// win runs wakeup() and createDream(nullptr). The Delay counts world time, which a host's sleep runs at
// sleep_rest::kNativeSleepDilation, so a host rolls 500 / 20 real seconds into the rest. A client's rest keeps its world
// at 1 (coop/player/sleep_rest), so the same Delay would fire after 500 real seconds.

// WHEN is the one piece of the roll that is ours: this module rolls once, 500 / kNativeSleepDilation real seconds into
// a client's rest. The chance is the game's (ue_wrap/actors/sleep ReadNightmareChance: the bed's dreamProb, 0.15 with no
// bed, none with nightmares off in the settings) and a win runs the game's own wakeup() and createDream(). The game's
// native roll stays held off on a client (sleep_sync writes its dreamProbability 0), since it would roll a second time
// 500 real seconds into a long rest. The nightmare is this player's alone: the dream is spawned
// on this machine, only this player is woken and teleported into it, and every other player keeps sleeping. The dream's
// own spawns (a mannequin, a boulder) are neither props nor lane actors, so no lane carries them to another peer. The
// rest state it leaves (dreaming) reaches the others through sleep_sync. A host's roll is the game's own and is not
// touched.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::nightmare_roll {

// Cache the session and take sleep_sync's rest-state announcements. Called from the subsystems Install fanout.
void Install(coop::net::Session* session);

// A dream is spawned far outside the map and the dreamer is teleported into it, so its copy on every other machine
// would cross the world to it and back. On every peer the copy of a player whose rest state is dreaming carries the dream
// reason at coop/player/puppet_hide (which owns the hidden flag) for the length of the dream, and loses it when the
// state is not. The rest state is taken from sleep_sync's announcement of a change, and a copy that is spawned after its
// player went to dream is hidden by the puppet drive's call below.
//
// The puppet drive calls it once for each puppet it has just spawned, so a copy that did not exist when its player's state
// arrived (a late join, a world change) is hidden too.
void OnPuppetSpawned(int slot);

// Per pump tick, on a connected client only: count the real seconds of the client's rest (sleep_rest's announced rest:
// a bool, no engine read) and roll once, 500 game seconds / sleep_rest::kNativeSleepDilation of them in. The tick stays because nothing announces the passing of those seconds;
// the rest and its end do not need a poll here.
void Tick();

// How many times this process's rest reached its roll point (the rest's one roll, made or held back by the red arm). An
// instrument: the nightmare drill decides "no dream came" at that point, since no dream can come in the rest after it.
uint32_t RollPoints();

// The session ended: the count starts again and no slot's player is in a dream (puppet_hide's own end shows the copies).
void OnDisconnect();

}  // namespace coop::nightmare_roll
