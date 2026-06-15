// coop/comp_sync.h -- v65: the desk REFINER (decode pane) mirror.
//
// SINGLE-SIMULATOR DOCTRINE (RE: the 2026-06-12 comp agent pass). The decode
// ticker (desk ReceiveTick -> calculate_comp) is gated ONLY on active_comp +
// comp_isDecodeActive -- no occupancy/player condition -- and completion
// fires world triggers (level-3: the theEvil_C spawn / deer / rozship) plus
// profile writes. So exactly ONE machine may hold the latch:
//   - The peer whose comp_isDecodeActive latched NATIVELY (the claim-owner
//     who pressed start, or the host whose save-load setData auto-resumed)
//     is THE SIMULATOR. It streams CompState ~1 Hz while decoding + on
//     edges; its completion/level-up propagates via the CompData edge.
//   - Every other peer is a PASSIVE MIRROR: raw-writes progress/downloading,
//     direct-paints the two texts nothing native repaints (progress %,
//     process phase), drives the working-loop/wind-down cues + the
//     completion beep on WIRE edges -- and never writes the latch.
//   - CLIENT WORLD-UP UNLATCH: the v56 save transfer ships analogPanelsData;
//     the joiner's loadObjects -> setData -> comp_start(comp_progress)
//     auto-resumes the decode NATIVELY on the joiner -- a pre-existing
//     double-simulation bug (both peers would decode with per-tick RNG
//     drift and both complete). A client clears the latch once per
//     world-up; the host keeps its natural latch (it owns the world).
//   - Simulator leaves mid-decode: the decode PAUSES (mirrors freeze at the
//     last wire state, cue winds down); any claim-owner pressing start
//     later resumes natively from the mirrored comp_progress -- manual
//     resume is the native path, no auto-adopt machinery.
//
// comp_data_0 (the loaded signal, Fstruct_signalDataDynamic) mirrors on
// CHANGE EDGES (drive upload / eject / completion level-up) as a
// signal_wire blob (CompData chunks) + the host adopt at connect-replay.
// Known accepted divergences (documented): the image PNG (bulk-lane
// deferral), drive CONTENT (own future feature), and a seated peer swapping
// drives while another peer's decode runs (native SP can't express that
// state; the swap wins and re-mirrors).
//
// Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::comp_sync {

void Install(coop::net::Session* session);

// Per-tick: throttled resolve + 1 Hz poll (simulator stream + data edges +
// the client world-up unlatch).
void Tick();

// Wire ingest: the simulator's scalar state (mirror apply: writes + paints +
// cue edges).
void OnState(const coop::net::CompStatePayload& p, uint8_t senderSlot);

// Wire ingest: one chunk of the comp_data_0 blob.
void OnDataChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Host: queue the adopt snapshot (CompState + CompData) for a joiner.
void QueueConnectBroadcastForSlot(int peerSlot);

// A peer left: if it was the streaming simulator, wind the mirror down
// (cue stop + idle paint) -- the decode pauses until someone restarts it.
void OnPeerDisconnect(uint8_t slot);

// Aggregate teardown.
void OnDisconnect();

}  // namespace coop::comp_sync
