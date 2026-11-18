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
// its copy; a take-off's plate is the host's prop, spawned where the plate was, for the asker to grab as any prop (the
// game's own take-off hands it to the taker's pickupObjectDirect, which a client's copy of a host prop does not take). The host sends the desk's plates and each station's door when they change and to a joiner at its
// ready edge (CoverState); a client writes the plates and repaints through the desk's own `updPanels`, and swings a
// door through the station's own `openDoor`. Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct CoverPayload;
}  // namespace coop::net

namespace coop::cover_sync {

// Keep the session and register the watches: the desk's plates and the stations' door press, and the game's announcements
// of a plate change (updPanels) and a door's end (doorAnim_TL__FinishedFunc). Idempotent.
void Install(coop::net::Session* session);

// HOST: judge the queued asks and tend a fresh plate prop. CLIENT: look for a put-back's lid touch while a plate is off.
// Settles the watches. Per frame. The plates and the doors are not read here: the game's updPanels and its door timeline's
// end send them (HOST) and keep them (CLIENT).
void Tick();

// HOST, at a joiner's world-ready edge: the desk's plates and every station's door, to that slot.
void SendJoinSnapshotForSlot(int slot);

// HOST: a client's ask, its length checked by the dispatcher.
void OnIntent(int senderSlot, const coop::net::CoverPayload& p);

// CLIENT: the host's state or answer, its length checked by the dispatcher.
void OnState(const coop::net::CoverPayload& p);

// [dev] The cover drill's reads and its one raw ask. AnswersSeen: the op 5 answers this client has had with that
// CoverReason (the byte of N::CoverReason); LastTakeOffEid: the plate prop's element id of the last granted take-off,
// 0 before one; DrillAsk: an ask sent as the lane's own verbs send it, for a stale or out-of-reach ask the verbs cannot
// make; false when it was not sent.
uint32_t AnswersSeen(uint8_t reason);
uint32_t LastTakeOffEid();
uint32_t DeskStatesApplied();  // the host's plate states written into this client's desk
bool PlateArmed(uint8_t plate);  // the lane has seen plate `plate`'s prop in this player's hand, off its lid: a touch asks the put-back
bool DrillAsk(uint8_t op, uint8_t what, uint8_t index, uint8_t bits, uint32_t eid);

// [dev] A client's broadcast of an actor's destroy (prop_destroy_seam), counted when the actor is a plate prop: a client
// authors none, so any is a plate prop the desk destroyed on its own copy. ClientPlateDestroys is the count.
void NoteBroadcastDestroy(void* actor);
uint32_t ClientPlateDestroys();

// Session end: the records are cleared.
void OnDisconnect();

}  // namespace coop::cover_sync
