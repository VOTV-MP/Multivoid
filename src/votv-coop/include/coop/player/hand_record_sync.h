// coop/player/hand_record_sync.h -- what another player's held item shows: its own save record on its hand mirror.
//
// A player's hand item reaches the others as a class and a name (HandItem), and each peer spawns a display-only mirror
// of it on the puppet from the class alone, so the mirror showed the class's defaults: a held can opened and half
// eaten was full and shut in the other players' view. Here the hand's owner reads its hand item's record through the
// item's own getData when its hold changes (the game's updateHold, watched at the script gate) or a held can's init
// runs, and sends it (HandRecord, chunked): a client to the host, which parks it, then decodes it once its pacing
// token is taken, judges it (the sender's applied hand holds that class, the item has a key, every number is finite
// and every field is within the lane's bounds, every class it names resolves to a class), stores its own copy and
// sends that on to every other world-ready peer, never the client's bytes; the host its own to everyone. Each peer
// writes the stored record into its mirror of that hand through the item's own loadData (ue_wrap/actors/save_record's
// ApplyRecord, the leaf state only), again whenever the mirror is made anew, and a joiner gets every hand's record at
// its ready edge. Only the hold and a held can's own change are carried: another class's state changed in the hand
// (an extinguisher's charge) is not announced by the game, and its mirror shows the record of the hold. The mirror
// stays display only; the hand's owner is the authority of its hand, as for HandItem. Game thread.
#pragma once

#include <cstdint>
#include <string>

#include "ue_wrap/actors/save_record.h"

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::hand_record_sync {

// Keep the session, and watch the game's own hold change (mainPlayer.updateHold) for the owner's side. Idempotent.
void Install(coop::net::Session* session);

// Which count or length of `r` is past this lane's bounds, said for a line; empty when the record is within them. Pure.
std::string BoundsFault(const ue_wrap::save_record::SaveRecord& r);

// The local hand item `actor` changed under the same hold (a can's init: opened, eaten from): its record is read and
// sent at the next Tick. Nothing for an actor that is not this player's hand. Game thread.
void NoteHeldItemChanged(void* actor);

// Why the host refused a client's record, counted per sender. Forged: the class is not the one the sender's applied
// hand holds (and was not held a moment ago). The others are the record's own shape.
enum class Refusal : uint8_t { Oversize, Malformed, WrongSlot, NonFinite, Forged, ClassUnknown, Keyless, Bounds, kCount };

// What the drill reads, per slot: the records written into this peer's mirror of the slot, the times its record was
// cleared (the host) or taken as cleared by the host's empty record (a peer), whether this peer holds a record for the
// slot now, and the records the host's judge stored for that slot. Game thread.
struct SlotCounts {
    uint32_t written = 0;
    uint32_t cleared = 0;
    bool stored = false;
    uint32_t accepted = 0;
};
SlotCounts CountsFor(uint8_t slot);

// The records the host refused from `slot` for `why`; and those it dropped as stale (a class the sender held a moment
// ago, behind its hand: an honest race, not counted as a forgery). Host side. Game thread.
uint32_t RefusedCount(uint8_t slot, Refusal why);
uint32_t StaleDropped(uint8_t slot);

// Whether this peer's stored record of `slot` is the one its mirror shows: nothing parked, nothing waiting to be
// written, the mirror it was written into is the one standing. The point after which a compare of the mirror to the
// owner's own state cannot move. Game thread.
bool Settled(uint8_t slot);

// Whether this peer's stored record of `slot` is of the class named `cls`.
bool StoredClassIs(uint8_t slot, const wchar_t* cls);

// The lane's own state: whether this peer's own hand record is still owed (the hand changed and its record is not yet
// sent: a client's to the host, the host's stored and emitted), and joiners' snapshots sent.
struct LaneCounts {
    bool ownPending = false;
    uint32_t snapshots = 0;
};
LaneCounts LaneCountsNow();

// The watch's names resolved; this peer's hand read and sent when it changed, once its hand lane has announced the item;
// the host's parked records judged; every stored record written into its mirror. Per frame.
void Tick();

// HOST, at a joiner's world-ready edge: every hand's record, to that slot; a record the link refused is owed to it.
void SendJoinSnapshotForSlot(int slot);

// A chunk of a hand record, from `senderSlot`.
void OnChunk(const coop::net::BlobChunkPayload& c, uint8_t senderSlot);

// A peer left: its record and its half-assemblies go.
void OnPeerLeft(uint8_t slot);

// Session end.
void OnDisconnect();

}  // namespace coop::hand_record_sync
