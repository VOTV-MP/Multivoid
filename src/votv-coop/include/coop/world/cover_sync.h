// coop/world/cover_sync.h -- the covers players open on the base's machines are the host's: the signal desk's back
// plates and a transformer station's maintenance door.
//
// The signal desk (analogDScreenTest_C) has four back plates over its physical-module slots, kept in two arrays of its
// own: `panelsUnscrew` (a plate unbolted, tilted loose by `updPanels`) and `panelsState` (a plate on). A tool held on
// a plate runs a timed unbolt that ends in `unscrewPanel`, which flips the plate's bolt; the E press on an unbolted
// plate that is on takes it off, spawning its prop (prop_computerBackPanel_tl/bl/tr/br_C) into the hand; that prop
// touching its place puts it back (ReceiveActorBeginOverlap). A transformer station (generator_C) has one door over its
// repair panel, `opened`, which its E press swings through `openDoor`. Each ran on the one machine.

// Here a client refuses each of them and asks the host (CoverIntent): an unbolt or a bolt, a take-off, a plate set
// back, a door press. The host judges the sender's reach and its own desk's or station's state and runs the change on
// its copy; a take-off's plate is the host's prop, spawned where the plate was and answered to the asker, which takes
// it into its hand. The host sends the desk's plates and each station's door when they change and to a joiner at its
// ready edge (CoverState); a client writes the plates and repaints through the desk's own `updPanels`, and swings a
// door through the station's own `openDoor`. Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct CoverPayload;
}  // namespace coop::net

namespace coop::cover_sync {

// Keep the session and register the watches on the desk's plates and the stations' door press. Idempotent.
void Install(coop::net::Session* session);

// HOST: judge the queued asks, send each change. CLIENT: apply the host's states. Settles the watches. Per frame.
void Tick();

// HOST, at a joiner's world-ready edge: the desk's plates and every station's door, to that slot.
void SendJoinSnapshotForSlot(int slot);

// HOST: a client's ask, its length checked by the dispatcher.
void OnIntent(int senderSlot, const coop::net::CoverPayload& p);

// CLIENT: the host's state or answer, its length checked by the dispatcher.
void OnState(const coop::net::CoverPayload& p);

// Session end: the records are cleared.
void OnDisconnect();

}  // namespace coop::cover_sync
