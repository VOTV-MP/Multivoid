// coop/player/level_visit.h -- a trip to the backrooms (or the two maps a clip can pick instead) is the clipping
// player's alone, and comes back to the world as a death does.

// A player's tick traces from last frame's camera to this one, and a hit on static geometry saves the game and
// travels to sl_backroomsObsolete, or by a roll to untitled_untitled or untitled_219 (mainPlayer_C's ReceiveTick,
// through lib_C::loadLevel). A session holds one world, and the travel took the clipping peer out of it, or every
// peer with the host. While a session runs that travel is refused at the script-body gate and the map is loaded into
// the running world instead, far below it (ue_wrap/world/level_instance), on the clipping player's machine alone; the
// player is moved into it and every other player stays in the world. The same travel authored by a peer's copy on
// this machine is refused outright: the copy's clip is the other machine's to make.

#pragma once

#include <string>

// Each map ends the game itself: the backrooms after 600 s (lib_C::End, which quits), the others by lib_C::End and
// direct calls of QuitGame on their own timers and triggers. A call of either from the visited level is refused and
// answered as a death: coop/player/death_revive revives the player in the world, carrying what it carried. A visitor
// who comes back any other way (a death the usual revive answers) ends the visit too. At the end the level is
// unloaded and what its script changed for the whole process is put back: the game instance's slot name and game
// mode, the event it raised, the reverb, and the two strings the clip writes (a map checks them). A map's own props
// stay out of the prop lanes, its copy of the world's ATV is removed and its day-night cycle stops ticking. On every
// peer a player's copy is hidden while it stands far below the world.

namespace coop::net { class Session; }

namespace coop::level_visit {

// Cache the session and register the watches (lib_C::loadLevel and lib_C::End) and the QuitGame hook. Idempotent;
// safe every tick. Game thread.
void Install(coop::net::Session* session);

// Per pump tick: the visit's load, arrival and return, and the copies hidden below the world (once a second).
void Tick();

// The session ended: a visit in progress is undone and the player put back where it clipped.
void OnDisconnect();

// True while a visit runs and `actor` belongs to a level other than the world's own: an actor of the visited map,
// which the prop lanes leave alone (it is this machine's, and goes with the map). Game thread.
bool InVisitedMap(void* actor);

// [dev] Instruments for the drill: whether a visit runs (loading or arrived), its map, and how it last ended.
bool Visiting();
std::wstring CurrentMap();
std::wstring LastEnd();
void* CurrentLevel();  // the visited ULevel once the player is in it, else null

}  // namespace coop::level_visit
