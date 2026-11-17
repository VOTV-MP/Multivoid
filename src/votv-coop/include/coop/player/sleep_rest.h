// coop/player/sleep_rest.h -- a client's rest at the game's native sleep rate, with the world's clock
// left at 1. Game thread throughout.

// The game's sleep is one engine call, SetGlobalTimeDilation(20), and every need it restores rides the
// dilated tick delta. A client cannot run the world at 20 (the host's world is what the others live in),
// so a script_gate POST watch on mainGamemode_C::sleep takes the dilation back to 1 in the frame of the
// call and, for the rest's length, gives the actors that carry its needs the same factor as their own
// CustomTimeDilation: the player's body (sleep, health, irradiation) and each effect actor of the
// gamemode's list (the vaccine's drain, the sleepy effect). Food, whose native fall divides the delta by
// the GLOBAL dilation, is restored by (1 - 1/F) of each fall; coffee, which drains in the gamemode's
// tick, loses (F - 1) times each fall more. The rest ends when the game's `wakeup` has run (a second POST watch), with the
// session, or when the gamemode or the body changes. Timer-driven owners (the ariral pillow's rolls, the
// nightmare) are scaled by no dilation at 1 and are not touched here; the nightmare's roll is made by
// coop/player/nightmare_roll, which counts this rest. A host runs it only as the host's setting world.sleep_speedup
// says: `host` leaves the host's sleep the game's own, at single player's speed;
// `everyone` rests the host at 1 as a client's while a connected player is awake; `never` always.

#pragma once

// Divergence from MTA, which syncs one game speed to every client (CMapManager.cpp:463): only the host
// runs the game's native x20. An awake client at x20 is unplayable, and this rest restores the client's
// needs without speeding its game.

namespace coop::net { class Session; }

namespace coop::sleep_rest {

// The world factor the game's own sleep sets (SetGlobalTimeDilation(20)), which a rest here gives the body and the
// effects instead of the world. Also the ratio between a sleep's world time and its real time.
inline constexpr float kNativeSleepDilation = 20.0f;

// Cache the session and subscribe to the host rule's changes: a peer's rest state (coop/player/sleep_sync), a player's
// arrival or leaving (coop/player/roster_ledger) and the row world.sleep_speedup. Called from the subsystems Install
// fanout.
void Install(coop::net::Session* session);

// Per pump tick: register the sleep and wakeup watches until they are live; while a rest runs, hold the factor on the
// effects and settle food and coffee. Cheap when idle: no engine read at all. A host's rest is switched on the edges
// Install names and the watches announce, never here.
void Tick();

// A rest is in progress (a client's, or the host's at 1): begun in the frame of the game's own `sleep` call (the
// gate's watch announces it) and over when the game's `wakeup` has run, which the wakeup watch announces. No engine
// read. Game thread.
bool Resting();

// The sleep and wakeup watches a rest stands on: Pending until both are live, Dead when the gate settled one without
// it (no rest is carried out then, and the host's sleep stays the game's own). For a drill's precondition. Game thread.
enum class WatchState { Pending, Live, Dead };
WatchState Watches();

// The session ended: a client's rest in progress is handed back at factor 1, and a host's sleep takes the game's factor.
// A host's last client left with the session living on: the host's sleep is declared again with nobody connected.
void OnDisconnect();

}  // namespace coop::sleep_rest
