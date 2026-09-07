// coop/interactables/comp_sync.h -- the desk REFINER (decode pane) mirror.
//
// SINGLE-SIMULATOR DOCTRINE. The decode ticker -- the desk's ReceiveTick reaching calculate_comp --
// is gated on nothing but the dream state, active_comp and comp_isDecodeActive. There is no
// occupancy or player condition, and completion fires world triggers (the theEvil spawn, the deer,
// the rozship) plus profile writes, so exactly ONE machine may hold the latch.
//
// THE SIMULATOR is the peer whose comp_isDecodeActive latched NATIVELY: the claim-owner who pressed
// start, or the host whose save-load setData auto-resumed. It streams CompState while decoding and
// on edges, and completion and level-up ride the CompData edge. Every other peer is a PASSIVE
// MIRROR: it writes progress and downloading, paints the two texts nothing native repaints, drives
// the cues off WIRE edges, never the latch.
//
// Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::comp_sync {

void Install(coop::net::Session* session);

// Per-tick: throttled resolve, then a 1 Hz poll -- the simulator's stream, the data edges, and the
// client's world-up unlatch. That unlatch exists because the save transfer ships analogPanelsData,
// so the joiner's loadObjects -> setData -> comp_start resumes the decode NATIVELY; without it both
// peers decode, drift apart on per-tick RNG and both complete. A client clears the latch once per
// world-up, and the host keeps its natural latch because it owns the world.
void Tick();

// Wire ingest: the simulator's scalar state (mirror apply: writes + paints +
// cue edges).
void OnState(const coop::net::CompStatePayload& p, uint8_t senderSlot);

// Wire ingest: one chunk of the comp_data_0 blob -- the loaded signal, mirrored on CHANGE edges
// (drive upload, eject, completion level-up) as a signal_wire blob, plus the host adopt at
// connect-replay. Accepted divergences: the image PNG, drive CONTENT, and a seated peer swapping
// drives mid-decode, which single-player cannot express -- the swap wins and re-mirrors.
void OnDataChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Host: queue the adopt snapshot (CompState + CompData) for a joiner.
void QueueConnectBroadcastForSlot(int peerSlot);

// A peer left: if it was the streaming simulator, wind the mirror down (cue stop, idle paint). The
// decode PAUSES there -- mirrors hold the last wire state, and any claim-owner pressing start later
// resumes natively from the mirrored comp_progress. Manual resume is the native path; there is no
// auto-adopt.
void OnPeerDisconnect(uint8_t slot);

// Aggregate teardown.
void OnDisconnect();

}  // namespace coop::comp_sync
