// coop/interactables/meadow_db_sync.h -- the MEADOW signal-DATABASE mirror
// (saveSlot.savedSignals_0 and the boot-persistent ui_laptop widget arrays).
// Overview: docs/signals.md. Game thread throughout.
//
// The store has a MOVE verb (sortSignal) and Blueprint moves deep-copy FStrings,
// so neither the deck list's positional prefix walk nor pointer RowKeys work here.
// The shadow is a content-hash MULTISET instead, re-hashed at 1 Hz only when the
// live count differs from the shadow sum, a scoped dirty-mark fired
// (addSignal/removeSignal/sortSignal on a ui_laptop_C), or an order change is
// pending retry.
//
// A count increment authors a MeadowAppend, a decrement a MeadowDelete, and a
// reorder of the common elements a MeadowOrder. Applies run through reflected
// addSignal and removeSignal and update the shadow game-thread-atomically, which
// is what suppresses the echo. [dev] meadow_selftest=1 injects a row, then removes it.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::meadow_db_sync {

void Install(coop::net::Session* session);

// Per-tick: throttled resolve, the 1 Hz pre-gated poll, tombstone and pending
// retry. The CLIENT lane sends nothing until its own world-ready announce --
// pre-ready organics accumulate as pending and flush at ready -- so a client line
// cannot reach the host before the flip and ride the seed back as a duplicate.
void Tick();

// Wire ingest: one chunk of an appended row (MeadowAppend).
void OnAppendChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Wire ingest: one content-keyed delete (MeadowDelete). A delete that matches no
// row leaves a tombstone (one per outstanding count, 20 s each) that the next
// matching append consumes, which is what covers the delete-beats-append race.
// Named residual: a same-content re-add inside that window can be consumed by it.
void OnDelete(const coop::net::ContentHashPayload& p, uint8_t senderSlot);

// Wire ingest: one chunk of an order-as-state line (MeadowOrder) -- the sort order
// IS synced, as state: the baseline is the hash SEQUENCE. Client lines are
// host-terminal (the host applies last-writer-wins and broadcasts ITS canonical);
// clients apply host-authored lines only. Apply = byte-permute + reflected
// genSignalList. An order line is deferred while any append or delete is still
// pending, so it can never overtake a line it references.
void OnOrderChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST, save_transfer OnRequest (same GT callback as the scratch-save capture):
// snapshot the current multiset for this joining slot (the seed baseline).
void CaptureJoinSnapshot(int peerSlot);

// HOST, the ready edge (subsystems ConnectReplayForSlot): send the seed delta to
// this slot and drop its snapshot. Per hash over the union, the delta is current
// minus snapshot minus the unmasked pending, masked by a game-thread op counter so
// that a pending op born before the snapshot -- whose effect the save already
// carries -- is not sent twice. That closes the window between the snapshot and
// world-ready, where a reliable send skips a not-yet-ready slot. It also stamps
// the pending exclude-masks that keep the seed and the retry from ever delivering
// the same line.
void QueueConnectBroadcastForSlot(int peerSlot);

// HOST: a join stream was cancelled / the slot disconnected -- drop the slot's
// snapshot and scrub its bit out of every pending exclude-mask (slot reuse).
void CancelJoinSnapshot(int peerSlot);

// Aggregate teardown.
void OnDisconnect();

}  // namespace coop::meadow_db_sync
