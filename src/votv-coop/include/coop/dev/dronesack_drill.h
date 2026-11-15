// coop/dev/dronesack_drill.h -- [dev] the drone sack measure drill: what happens to what is dropped onto the drone
// sack (a world container that takes what touches it into its own inventory), on each peer; it changes no product
// behaviour. Lines are tagged [SACK-DRILL]; a stuck leg says ABANDONED (--dead-marker).
// Both peers run it with the same dronesack_drill value; the host puts a prop_dronesack_C in front of itself, names it
// in the lobby chat, and waits for the client to say its mirror is bound.
//   1 -- the host drops a prop_container_C crate 150 cm above the sack; for 15 s each peer logs the crate's local
//        destroy with the caller's class, the client whether its seam sent a PropDestroy, the host whether one reached it.
//   2 -- the host spawns six loose prop_C props (named "cup") on a ring round the sack; the client walks to each with
//        the director, grabs it, carries it over the sack and lets go, and says whether its mirror died, by which caller
//        and whether the seam sent a PropDestroy. The host then lets the drone's own sack go, puts the drill's sack aboard
//        (putSackOn moves its entries into the drone's cargo), calls the drone through the console's keyboard verb and
//        logs when the sale clears the cargo. Both peers read a census of every world container ([SACK-CENSUS]) at the
//        call, at the sale and 5 s later, and name what changed.
//   3 -- the host itself drops six cups and one fresh crate onto the sack, one at a time, then the same call, sale and
//        census.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dev::dronesack_drill {

bool IsEnabled();

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* s);

// The session's end: a rejoin runs the drill again.
void OnDisconnect();

// The hooks of the destroy seam and the PropDestroy receiver; each is a bool read until the drill arms them.
// NoteDestroy: an actor's K2_DestroyActor has returned; `caller` is the object whose script called it (null: native).
void NoteDestroy(void* actor, void* caller);
// NoteBroadcast: the destroy seam sent a PropDestroy for `actor`.
void NoteBroadcast(void* actor);
// NoteWirePropDestroy: an accepted PropDestroy from peer slot `slot`, before the receiver destroys anything.
void NoteWirePropDestroy(uint32_t eid, const char* key, uint8_t keyLen, int slot);

}  // namespace coop::dev::dronesack_drill
