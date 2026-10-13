// harness/join_leave.h -- a client join's leave of a gameplay world for the menu: before the join, and after it ended.
//
// The menu join assumes the menu: it connects there, downloads the host's save and loads it. Started inside a
// world, its world-ready is announced in that world at the connect, and its load of the host's slot answers
// "already in gameplay" without opening it, so the player plays the session in a world of their own. The leave
// travels to the menu through the game mode's own `transition`, the verb the game's quit to menu runs, and hands
// the join back once the menu stands.

// The same travel takes the player back after a client session that a timeline stop site ended (a Cancel, the
// join's timeout, a world that would not load): the world they stand in is the host's, which the join loaded, or
// one the host's state was replayed into, and MTA forces its main menu back after every unload
// (reference/mtasa-blue/Client/core/CModManager.cpp:286-289). A session a flee ended (a death, a quit, the host
// lost) is the flee's, which travels on its own.
//
// A phase of the play loop, not a wait inside it: nothing of the session runs until the join starts, the loop
// keeps its idle tick and its cancel drain, and a join cancelled meanwhile is dropped here. TimelineThread.

#pragma once

#include "ue_wrap/engine/world_identity.h"

namespace coop::net { struct Config; }

namespace harness::join_leave {

// Takes the join's config when a gameplay world stands, and answers true: the join waits at the leave, which
// Step drives. False when no gameplay world stands, so the join starts at once; also false when the world reader
// is degraded and cannot tell, said once by name (a gate that acts on the world does not act on Unknown).
bool Begin(const coop::net::Config& cfg);

// The return: a client session that ended mid-join, with `now` the world kind read on the game thread after its
// teardown. True when the return began, `now` being anything but Other: a world stands, or one is still arriving,
// and Step travels only once a gameplay world stands. False at Other (the menu: the caller opens the browser) and
// when the world reader is degraded, said once by name: it cannot tell a world from the menu, so the player is not
// moved. Only from a running client session's end, so never while a join's leave runs: that leave ends before its
// session starts.
bool BeginReturn(ue_wrap::world_identity::WorldKind now);

enum class Outcome {
    Idle,          // no leave under way
    Waiting,       // the travel is asked for, or under way
    AtMenu,        // a join's leave: the menu stands, the join's config is handed back
    Cancelled,     // a join's leave: the join ended meanwhile (a cancel, a failure, a shutdown) with its world still
                   // standing; a return: the process is shutting down
    CancelledLeft, // a join's leave: the join ended meanwhile after its travel dispatched: the player ends at the menu
    Failed,        // a join's leave: the travel was refused or did not dispatch within 30 s, or the menu never stood
    Returned,      // a return: the menu stands, where the browser opens
    ReturnFailed,  // a return: as Failed; the player is still in the world, and the error line says so
};

// One step, from each pass of the play loop. The travel is asked of the game thread every 250 ms until a gameplay
// world stands with its game mode, which is missing from our object index for a few batches after its world appears
// and is never asked of a world mid-travel. Then it waits for the menu, the bound counting only while the game thread
// runs tasks (a level load holds it). A world left by other means before the travel goes on as at the menu. On AtMenu
// `out` holds the join's config; every other outcome leaves it alone.
Outcome Step(coop::net::Config& out);

// Whether a leave or a return is under way: the play loop's host boot and pending start wait for it, and its cancel
// drain names it.
bool Active();

}  // namespace harness::join_leave
