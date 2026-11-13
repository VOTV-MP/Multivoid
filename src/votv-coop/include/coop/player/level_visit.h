// coop/player/level_visit.h -- a trip to the backrooms (or the two maps a clip can pick instead) is the clipping
// player's alone, and comes back to the world as a death does.

// A player's tick traces from last frame's camera to this one, and a hit on static geometry saves the game and
// travels to sl_backroomsObsolete, or by a roll to untitled_untitled or untitled_219 (mainPlayer_C's ReceiveTick,
// through lib_C::loadLevel). A session holds one world, and the travel took the clipping peer out of it, or every
// peer with the host. While a session runs that travel is refused at the script-body gate and the map is loaded into
// the running world instead, far below it (ue_wrap/world/level_instance), on the clipping player's machine alone; the
// player is moved into it and every other player stays in the world. A peer's copy authors no trip on this machine:
// the game's clip tick trips only for the player the local view follows, and a copy's actor tick is off.

#pragma once

#include <cstdint>
#include <string>

// Each map ends the game itself: the backrooms after 600 s (lib_C::End, which quits), the others by lib_C::End and
// direct calls of QuitGame on their own timers and triggers. A call of either from the visited level is refused and
// answered as a death: coop/player/death_revive revives the player in the world, carrying what it carried. A visitor
// who comes back any other way (a death the usual revive answers) ends the visit too. At the end the level is
// unloaded and what its script changed for the whole process is put back: the game instance's slot name and game
// mode, the event it raised, the reverb, and the two strings the clip writes (a map checks them). A map's own props
// stay out of the prop lanes, its copy of the world's ATV is removed and its day-night cycle stops ticking. On every
// peer a player's copy is hidden by its pose: every pose a peer sends carries its position, so a pose below
// kVisitFloorZ is a player in a visited map. Divergence from MTA, which keeps a player's interior and dimension as
// a reliable state with a snapshot for a joiner (CStaticFunctionDefinitions.cpp:1526,1592; CPlayerListPacket.cpp:201,208):
// here the visited map is a place in the same world, so the position the pose stream already carries is the state, and
// it is read on every pose (level-triggered), so a joiner, a respawned copy and a lost packet need nothing of their own.

namespace coop::net { class Session; }

// A map loaded into the running world brings what a world of its own has. Its placed player takes this machine's
// controller as its level begins play: the controller goes back to the visitor, and the placed player and the black
// screen it put up are removed. The world's sky, which reaches that far down without bound (its height fog, sun, sky
// light, atmosphere, clouds and post process), is hidden on this machine for the visit, so the map is as dark as the
// game leaves it. A trip asked while seated or riding is refused, as the game's clip check refuses one. What a map's
// script spawns lands in the world's own level, far below it, and counts as the map's.
// untitled_untitled's hook check ends the map for a grappling hook in the map only, not for one in the world.
// The game's own clip check (mainGamemode_C::canBackrooms, which the player's tick asks before every clip trip) is
// answered no for this machine's player while a visit runs and for 5 s after it, so the view's jump into the map and
// back out is not a clip, and while the host has closed the backrooms (world.backrooms, F1's Administration > Server
// settings): no clip is taken and no trip is sent. Open, the game's own logic answers. A visit already running ends as
// usual.

namespace coop::level_visit {

// A pose below this height is a player in a visited map: its copy is hidden while its pose stays below it.
inline constexpr float kVisitFloorZ = -30000.f;

// Cache the session, register the watches (lib_C::loadLevel, lib_C::End and canBackrooms), the QuitGame hook, the
// world.backrooms follower and the pose listener. Idempotent; safe every tick. Game thread.
void Install(coop::net::Session* session);

// Per pump tick: the visit's load, arrival and return, and the visit reason of each copy's hidden flag, from what its
// last pose recorded.
void Tick();

// A slot's occupant left: what its poses recorded is forgotten.
void OnPeerLeft(uint8_t slot);

// The session ended: a visit in progress is undone and the player put back where it clipped.
void OnDisconnect();

// True while a visit runs and `actor` belongs to a level other than the world's own, or stands far below the world
// (what the map's script spawned): an actor of the visited map, which the prop lanes leave alone (it is this
// machine's, and goes with the map). Game thread.
bool InVisitedMap(void* actor);

// [dev] Instruments for the drill: whether a visit runs (loading or arrived), its map, and how it last ended.
bool Visiting();
std::wstring CurrentMap();
std::wstring LastEnd();
void* CurrentLevel();  // the visited ULevel once the player is in it, else null
bool ReturnPending();  // the visitor's dropped item is still to be given back
struct DrillStats {
    uint32_t tripsStarted;       // trips this machine's player took (a clip's loadLevel the gate turned into a visit)
    uint32_t clipsHeldClosed;    // the game's clip check answered no because the host closed the backrooms
    uint32_t clipChecksAsked;    // the game's clip check asked for this machine's own player
    int lastClipWhy;             // last ask answered by: 0 the game's logic, 1 closed, 2 visit, 3 quiet window, -1 none
};
DrillStats Stats();

}  // namespace coop::level_visit
