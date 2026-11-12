// coop/player/nightmare_roll.h -- a client's nightmare roll, at the host's pace. Game thread throughout.

// The game rolls for a nightmare from its sleep: a latent Delay of 500 s, then RandomBoolWithWeight(bedSleepProb()),
// looping while it sleeps; a win runs wakeup() and createDream(). The Delay counts world time, which the host's sleep
// runs at 20, so a host rolls every 25 real seconds. A client's rest keeps its world at 1 (coop/player/sleep_rest),
// so the game's own roll would come once in 500 real seconds, past the end of any rest.

// A client's own roll is therefore held off (sleep_sync writes its dreamProbability 0, which bedSleepProb answers
// before the bed) and this module makes it in the game's place: every 25 real seconds of a rest, with the chance the
// game would use (the bed's dreamProb, 0.15 with no bed, none with nightmares off in the settings), and a win runs the
// game's own wakeup() and createDream(). The nightmare is this player's alone: the dream is spawned on this machine,
// only this player is woken and teleported into it, and every other player keeps sleeping. The rest state it leaves
// (dreaming) reaches the others through sleep_sync. A host's roll is the game's own and is not touched.

#pragma once

namespace coop::net { class Session; }

namespace coop::nightmare_roll {

// Cache the session. Called from the subsystems Install fanout.
void Install(coop::net::Session* session);

// A dream is spawned far outside the map and the dreamer is teleported into it, so its copy on every other machine
// would cross the world to it and back. On every peer the copy of a player whose rest state is dreaming is hidden
// for the length of the dream (AActor::SetActorHiddenInGame) and shown again when it is not.
//
// Per pump tick: on a connected client, count the rest's real seconds and roll at each 25 (cheap when awake); on
// every peer, once a second, hide or show each player's copy by its rest state.
void Tick();

// The session ended: the count starts again and every copy this hid is shown.
void OnDisconnect();

}  // namespace coop::nightmare_roll
