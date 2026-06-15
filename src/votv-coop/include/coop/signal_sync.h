// coop/signal_sync.h -- v65: the desk SIGNAL-LIBRARY mirror
// (gamemode.savedSignals_0): appends + deletes, the email_sync shadow shape.
//
// USER ASK (binding phase-2 design): the desk screens mirror -- the saved
// signals list (play pane) is the desk's core content.
//
// Mechanism (the audited email_sync pattern -- see coop/email_sync.h for the
// invariant discussion; deliberate second instance, extract on a third):
//   - Every peer shadows the array: per-row POD instance key (raw bytes,
//     zero reflected calls at cadence; ue_wrap::saved_signals::RowKey) + the
//     row's cross-peer identity = signal_wire::ContentHash of its serialized
//     blob (image-free).
//   - 1 Hz positional diff under the append-at-tail invariant (saveSignal /
//     copySignal append; deleteSignal removes):
//       APPEND -> serialize + BlobChunk broadcast (SavedSignalAppend,
//                 host-relayed); receivers re-play the native
//                 gamemode.saveSignal (append + pane rebuild + specials/
//                 forceObjects bookkeeping; validity-gated).
//       SHRINK -> removed hashes broadcast (SavedSignalDelete); receivers
//                 resolve their index by hash + reflected deleteSignal.
//                 Covers the plain delete AND the export-to-drive MOVE's
//                 list side (drive CONTENT sync is a documented gap).
//   - Wire applies register (key -> wire hash) marks; the next poll adopts
//     them as sent (echo-proof). Tombstones close delete-beats-append.
//   - World-down drops the shadow; re-prime silently at world-up.
//
// A user RENAME (slot widget edit box) reallocs the row's name FString ->
// instance-key change -> the diff re-broadcasts the row as delete+append --
// renames converge (the row moves to the tail on mirrors; cosmetic).
//
// Join: savedSignals_comp_0 rides the v56 save transfer; this channel covers
// live deltas only. The image PNG (laptop photo) is a deferred bulk-lane
// increment -- live-mirrored rows carry an empty image until then.
//
// Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::signal_sync {

void Install(coop::net::Session* session);

// Per-tick: throttled resolve + the 1 Hz shadow poll + tombstone retry.
void Tick();

// Wire ingest: one chunk of an appended row.
void OnAppendChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Wire ingest: one content-keyed delete.
void OnDelete(const coop::net::ContentHashPayload& p, uint8_t senderSlot);

// Aggregate teardown.
void OnDisconnect();

}  // namespace coop::signal_sync
