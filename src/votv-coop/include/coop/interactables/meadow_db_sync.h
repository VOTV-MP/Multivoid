// coop/meadow_db_sync.h -- v120 (L9): the MEADOW signal-DATABASE mirror
// (saveSlot.savedSignals_0 + the boot-persistent ui_laptop widget arrays).
//
// Design of record: votv-meadow-db-L9-impl-DESIGN-2026-07-19.md (15-round /qf).
// Precedent lane: coop/signal_sync (v65 deck list) -- deliberately NOT a third
// email_sync-pattern instance: the meadow store has a MOVE verb (sortSignal)
// and BP moves deep-copy FStrings, so both the v65 positional prefix walk AND
// pointer RowKeys are invalid here.
//
// Mechanism:
//   - Shadow = content-hash MULTISET {signal_wire ContentHash -> count}.
//     Poll (1 Hz) pre-gate: full re-hash only when the live count differs from
//     the shadow sum, OR a scoped 0x45 dirty-mark fired (addSignal/
//     removeSignal/sortSignal FScriptNames, ctx class-checked ui_laptop_C),
//     OR an order change is pending retry (g_orderPending).
//   - ORDER IS SYNCED (v120 per-rule-1 user decision) as STATE: baseline =
//     the hash SEQUENCE; a reorder of the COMMON elements (= a sortSignal
//     move) authors a MeadowOrder line -- HOST-CANONICAL (see OnOrderChunk
//     below). FIFO guard: order lines are deferred while any append/delete
//     is still pending (an order must never overtake a line it references).
//   - Count increment -> MeadowAppend {row sans image, blob chunks}; decrement
//     -> MeadowDelete {contentHash}. Apply = reflected ui_laptop.addSignal
//     (id-preserving) / removeSignal (content-resolved index); the shadow is
//     updated GT-atomically at apply (echo suppression).
//   - Tombstones {hash -> outstanding count + per-entry deadline, 20 s} with
//     the append-consume rule. Named residual: a same-content re-add within
//     the TTL window can be consumed (v65-inherited, narrow).
//   - JOIN: the host captures a per-slot multiset snapshot at save_transfer
//     OnRequest (right after the scratch save serializes -- the g_blobKeys
//     idiom) and, at the ready edge, sends seedDelta(h) = curCount - snapCount
//     - unmaskedPendingNet (masks by GT op-counter: pending born before the
//     snapshot -> its effect is in the save -> masked for that slot). Closes
//     the [snapshot, world-ready] permanent-loss window (SendReliable skips
//     not-ready slots -- v56 B2).
//   - The CLIENT lane sends nothing until its own ClientWorldReady announce
//     (pre-ready organics accumulate as pending, flush at ready) -- kills the
//     pre-ready authoring dup (a client line reaching the host before the
//     flip would ride the seed back).
//   - [dev] meadow_selftest=1 (HOST): inject a synthetic row post-connect,
//     then remove it -- the smoke asserts the 0->1->0 digest transitions
//     ("meadow digest: n=X sum=Y", order-independent wrapping-u64 sum).
//
// Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::meadow_db_sync {

void Install(coop::net::Session* session);

// Per-tick: throttled resolve + the 1 Hz pre-gated poll + tombstone/pending retry.
void Tick();

// Wire ingest: one chunk of an appended row (MeadowAppend).
void OnAppendChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Wire ingest: one content-keyed delete (MeadowDelete).
void OnDelete(const coop::net::ContentHashPayload& p, uint8_t senderSlot);

// Wire ingest: one chunk of an order-as-state line (MeadowOrder; v120 per-rule-1
// user decision -- the sort order IS synced). Client lines are host-terminal
// (the host applies last-writer-wins + broadcasts ITS canonical); clients apply
// host-authored lines only. Apply = byte-permute + reflected genSignalList.
void OnOrderChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST, save_transfer OnRequest (same GT callback as the scratch-save capture):
// snapshot the current multiset for this joining slot (the seed baseline).
void CaptureJoinSnapshot(int peerSlot);

// HOST, the ready edge (subsystems ConnectReplayForSlot): send the seed delta
// to this slot and drop its snapshot. Also stamps pending exclude-masks per
// the "seed and retry never both deliver the same line" invariant.
void QueueConnectBroadcastForSlot(int peerSlot);

// HOST: a join stream was cancelled / the slot disconnected -- drop the slot's
// snapshot and scrub its bit out of every pending exclude-mask (slot reuse).
void CancelJoinSnapshot(int peerSlot);

// Aggregate teardown.
void OnDisconnect();

}  // namespace coop::meadow_db_sync
