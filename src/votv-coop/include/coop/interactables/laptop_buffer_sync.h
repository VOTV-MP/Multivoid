// coop/interactables/laptop_buffer_sync.h -- the laptop file-buffer QUAD sync lane {floppyData, floppyBuffer,
// floppyBufferUIDs, floppyReadwrites}, between two changes of which disc the laptop holds.
//
// Shape: a client sends change-edge EDIT-SCRIPT batches in the grammar the native verbs actually use (removeAt, and
// append at the END -- nothing moves in place); the host applies them content-anchored and answers with an
// UNCONDITIONAL canonical, which IS the acknowledgement; its own edits go straight to a canonical. Receivers adopt
// canonicals from slot 0 only, draining pending edits first, skipping the rebuild when the content already matches,
// and rebuilding the widget EAGERLY -- updFloppy regenerates floppyBuffer FROM bufferSlots.
//
// Wire: ReliableKind::LaptopQuad over BlobChunkPayload, head byte 0 a client's batch, 1 the host's canonical. Both
// carry the laptop's generation from coop/interactables/floppy_slot_sync, which owns which disc the slot holds, and
// ride the Default lane behind its slot states: the host refuses a batch of another generation, a client drops a
// canonical of another, and neither
// end sends while that lane has not settled the disc. Never refanned. Game thread throughout.

#pragma once

#include "coop/net/protocol.h"
#include "ue_wrap/devices/laptop.h"  // BufferQuad, for the drill's forged canonical

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::laptop_buffer_sync {

void Install(coop::net::Session* session);

// 4 Hz: int pre-filter (fdN/fbN/uidN/rw -- every native verb is int-visible,
// rw monotone proof) + derive/send (client) or canonical (host organic). A change of
// which disc the laptop holds is the slot lane's, which primes the shadow as it takes it.
void Tick();

// LaptopQuad chunks: host consumes batches (+ answers canonical); clients
// adopt host-authored canonicals.
void OnQuadChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// The shadow's prime at each change of which disc the laptop holds that the slot lane takes -- a
// client's claim, the host's publish, a canonical written -- so the rows that change carried never
// read as a file edit, and an edit after it does.
void PrimeQuadBaseline();

// What the lane did this session, for a drill.
struct Counts {
    uint64_t canonicalsTaken   = 0;  // CLIENT: the host's canonicals applied (or already matching)
    uint64_t canonicalsDropped = 0;  // CLIENT: of another generation, or while its own change of the disc was unanswered
    uint64_t batchesRefused    = 0;  // HOST: of another generation, or while its disc changed unpublished
};
Counts ReadCounts();

// [dev] The laptop drill's race legs. HOST: eject the laptop's disc at the head of the next client
// batch, before it is judged -- an eject racing that edit. CLIENT: a batch appending `row` to the
// file rows on `generation`, what a client whose edit was made on a disc the host has since
// published a change of sends. HOST: the canonical `q` on `generation` to every ready client, one
// minted before a change of the disc arriving after it. Game thread.
void DevEjectAtNextBatch();
bool DevSendAppendOn(uint32_t generation, const std::wstring& row);
bool DevSendCanonicalOn(uint32_t generation, const ue_wrap::laptop::BufferQuad& q);

// HOST: ship the joiner the canonical quad, after the slot lane's connect set on the same lane.
void QueueConnectBroadcastForSlot(int peerSlot);

void OnDisconnect();

}  // namespace coop::laptop_buffer_sync
