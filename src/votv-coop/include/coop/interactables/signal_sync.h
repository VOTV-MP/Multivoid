// coop/signal_sync.h -- the desk SIGNAL-LIBRARY mirror (gamemode.savedSignals_0): appends and
// deletes, in the email_sync shadow shape (coop/email_sync.h carries the invariant discussion; a
// deliberate second instance, to be extracted on a third). Game thread throughout.
//
// Every peer shadows the array with a per-row POD instance key (raw bytes, no reflected call at
// cadence; ue_wrap::saved_signals::RowKey), and a row's cross-peer identity is
// signal_wire::ContentHash over its serialized, image-free blob. A 1 Hz positional diff runs under
// the append-at-tail invariant -- saveSignal and copySignal append, deleteSignal removes. An APPEND
// broadcasts the serialized row (SavedSignalAppend, host-relayed) and receivers replay the native
// gamemode.saveSignal, which rebuilds the pane and the specials and forceObjects bookkeeping. A
// SHRINK broadcasts the removed hashes (SavedSignalDelete) and receivers resolve their own index by
// hash, then call deleteSignal; that covers the plain delete and the list side of the
// export-to-drive MOVE, while the drive's CONTENT is a known gap. A wire apply registers a
// key-to-hash mark the next poll adopts as sent, so an apply never echoes, and tombstones close
// delete-beats-append. World-down drops the shadow; it re-primes silently at world-up.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::signal_sync {

void Install(coop::net::Session* session);

// --- The ready-edge join seed: state authored during a joiner's 30-60 s load window would
// otherwise never be delivered, the joiner being past the save transfer and not yet replaying.
// savedSignals_comp_0, the save mirror of the live array, rides the save transfer; rows authored
// after that snapshot ride this seed instead, through the shared coop/session/join_seed helper.
// HOST, game thread. Capture at save_transfer's OnRequest scratch-serialize; seed (both signs, as a
// multiset) at subsystems::ConnectReplayForSlot; cancel at the transfer-teardown site. Per-slot;
// consume-once.
void CaptureJoinSnapshot(int peerSlot);
void CancelJoinSnapshot(int peerSlot);
void QueueConnectBroadcastForSlot(int peerSlot);

// Slot teardown (roster row transition): drop the leaver's half-assemblies +
// seed bracket so a recycled occupant can never inherit them.
void OnDisconnectSlot(int peerSlot);

// Per-tick: apply-park drain, throttled resolve, the 1 Hz shadow poll and the tombstone retry.
//
// A user RENAME (the slot widget's edit box) reallocates the row's name FString, so its instance
// key changes and the diff re-broadcasts the row as a delete plus an append. Renames converge; the
// row moves to the tail on the mirrors, which is cosmetic.
void Tick();

// Wire ingest: one chunk of an appended row. The image PNG (the laptop photo) is not carried yet,
// so a live-mirrored row arrives with an empty image.
void OnAppendChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Wire ingest: one content-keyed delete.
void OnDelete(const coop::net::ContentHashPayload& p, uint8_t senderSlot);

// Aggregate teardown.
void OnDisconnect();

}  // namespace coop::signal_sync
