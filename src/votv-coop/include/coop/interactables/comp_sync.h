// coop/interactables/comp_sync.h -- the desk's refiner (the decode pane): the host decodes, every client mirrors.
//
// The decode's step -- the desk's tick reaching calculate_comp -- advances on the dream state, active_comp and
// comp_isDecodeActive alone, and its completion fires world triggers (theEvil_C, the deer, the rozship) and writes
// the running machine's profile, so one machine holds the latch: the host's. A client's start and stop are presses
// the host replays (desk_verb_intent), and every comp_start a client's machine reaches -- the join's restore resumes
// a saved decode -- is refused at the script gate with `succ` false.
//
// The decode's OWNER is the slot whose start latched the host's machine: a replayed start's presser, else the host;
// unchanged across the completion's continue, and the host's once its owner leaves. A client's completion gloss is
// forwarded to it (desk_verb_effects), and its signals_processed point put back on the host and sent. The host
// streams CompState while decoding, on its edges and on a completion, the level-up riding the CompData edge; a
// client applies the host's alone, writing progress and downloading, painting the texts the mirror's own calls
// leave wrong and driving the cues off the wire's edges, never the latch. Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::comp_sync {

// The session, and the refiner's two watches registered: from the session's first tick
// (subsystems::InstallLoadWatchers), since a joiner's restore of a saved decode runs inside its world load.
// Idempotent. Game thread.
void Install(coop::net::Session* session);

// Per tick in a world, polled at 1 Hz: the host's stream and data edges. The world's up edge primes the host's edge
// detectors and clears a client's latch from before its session; its down edge resets the mirror's trackers.
void Tick();

// CLIENT: the host's scalar state, applied as a mirror (writes, paints, cue edges). The host refuses a client's
// copy at its receive edge (client_authorship.h).
void OnState(const coop::net::CompStatePayload& p);

// CLIENT: one chunk of the host's comp_data_0 blob, the refiner's row, sent on its change edges (an upload, an
// eject, a completion's level) and to a joiner. The row crosses without its photo. The host refuses a client's
// copy at its receive edge.
void OnDataChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: queue a joiner's seed (CompState + CompData), once its world is up.
void QueueConnectBroadcastForSlot(int peerSlot);

// HOST: a peer left; a decode it owned is the host's from here.
void OnPeerLeft(uint8_t slot);

// Session end: the mirror winds down, the owner is the host again, the counts are said.
void OnDisconnect();

// CLIENT: the host's decode runs as this mirror last heard it (the wire's active state). For the drills.
bool MirrorActive();

}  // namespace coop::comp_sync
