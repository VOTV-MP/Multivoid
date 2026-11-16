// coop/interactables/meadow_db_sync.h -- the MEADOW signal-DATABASE mirror (saveSlot.savedSignals_0, and
// the ui_laptop widget's arrays in each world). Overview: docs/signals.md. Game thread throughout.
//
// The store has a MOVE verb (sortSignal) and Blueprint moves deep-copy FStrings, so neither the deck
// list's positional prefix walk nor pointer RowKeys work here. The shadow is a content-hash MULTISET
// instead (coop/interactables/meadow_db_hash), taken of the database -- its save object, which a travel
// keeps -- the first time the lane needs it, and compared with the database at the exit of each body that
// writes it: ui_laptop_C's addSignal, removeSignal and sortSignal, and the rename window's handler,
// watched at the script-body gate.
//
// A row that came authors a MeadowAppend, one that went a MeadowDelete (a rename is both), and an order
// that differs from what every peer will hold after those lines a MeadowOrder. Applies run through
// reflected addSignal and removeSignal inside the lane's own scope and update the shadow
// game-thread-atomically, which is what suppresses the echo. coop/dev/meadow_selftest drives the lane.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::meadow_db_sync {

void Install(coop::net::Session* session);

// Per-tick: the writers' watches until the gate settles them; the pen's parked lines, a few a tick,
// once the database is back; then the retries once a second while one waits (a pending line, a
// tombstone, a held or owed order, a half-assembled row).
// The CLIENT lane sends nothing until its own world-ready announce -- pre-ready
// organics accumulate as pending and flush at ready -- so a client line cannot reach
// the host before the flip and ride the seed back as a duplicate.
void Tick();

// Wire ingest: one chunk of an appended row (MeadowAppend). Each of the three ingests puts its line in the
// lane's pen (coop/interactables/meadow_db_park) while this peer's database or its widget is away, as in a
// travel, or while lines already wait there; they apply in the order they came once both are back.
void OnAppendChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Wire ingest: one content-keyed delete (MeadowDelete). A delete that matches no
// row leaves a tombstone (one per outstanding count, held 20 s of the lane's live
// time: its clock stands still while lines are parked or the database is away)
// that the next matching append consumes, which is what covers the delete-beats-
// append race. Named residual: a same-content re-add inside that window can be
// consumed by it.
void OnDelete(const coop::net::ContentHashPayload& p, uint8_t senderSlot);

// Wire ingest: one chunk of an order-as-state line (MeadowOrder) -- the sort order
// IS synced, as state: the baseline is the hash SEQUENCE. Client lines are
// host-terminal (the host applies last-writer-wins and broadcasts ITS canonical);
// clients apply host-authored lines only. After applying a client's append or
// delete the host sends its canonical too, since the author placed its row where
// its own lines left it. Apply = byte-permute + reflected genSignalList. An order
// line is deferred while any append or delete is still pending, so it can never
// overtake a line it references.
void OnOrderChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST, at save_transfer's capture (the same game-thread call as the scratch save):
// snapshot the current multiset for this joining slot (the seed baseline).
void CaptureJoinSnapshot(int peerSlot);

// HOST, the ready edge (subsystems ConnectReplayForSlot): send the seed delta to
// this slot and drop its snapshot. Per hash over the union, the delta is current
// minus snapshot minus the unmasked pending, masked by a game-thread op counter so
// that a pending op born before the snapshot -- whose effect the save already
// carries -- is not sent twice. That closes the window between the snapshot and
// world-ready, where a reliable send skips a not-yet-ready slot. It also stamps
// the pending exclude-masks that keep the seed and the retry from ever delivering
// the same line. The pen's parked lines from other clients follow the delta, since
// the session relayed them only to the peers ready when they came; while the
// host's own database is away the seed is owed, and runs once it is back, before
// the pen drains, forwarding only the lines parked before this joiner was ready.
void QueueConnectBroadcastForSlot(int peerSlot);

// HOST: a join stream was cancelled / the slot disconnected -- drop the slot's
// snapshot and clear its slot out of every pending exclude set (slot reuse).
void CancelJoinSnapshot(int peerSlot);

// Aggregate teardown.
void OnDisconnect();

// The appends, deletes and order lines this peer's lane has delivered this session, for the dev
// selftest's waits: `orders` the lines its own order changes sent, `canonicals` the host's canonical
// sent after a client's line, or owed to a joiner. Game thread.
struct SentCounts { uint64_t appends; uint64_t deletes; uint64_t orders; uint64_t canonicals; };
SentCounts SentLines();

// [dev] While held, this peer's appends wait as a refused send's do and go when released: the selftest's
// race, a row on this peer that no other peer has seen yet. Game thread.
void DebugHoldAppends(bool hold);

// [dev] Whether this host owes every peer its canonical order, for the selftest's race. Game thread.
bool OwesCanonical();

// [dev] While set, this peer's database reads as away, as in a travel: inbound lines wait in the pen. And
// whether an append of this row waits there. The selftest's away leg. Game thread.
void DebugAway(bool away);
bool ParkedRow(uint64_t hash);

// [dev] Called on the game thread after each line this peer's lane applies to its database -- an append,
// a delete, an order -- so the selftest sees every state the database passes through, not only the ones
// a tick lands on. Null to clear. Game thread.
using ApplyObserver = void (*)();
void SetApplyObserver(ApplyObserver fn);

}  // namespace coop::meadow_db_sync
