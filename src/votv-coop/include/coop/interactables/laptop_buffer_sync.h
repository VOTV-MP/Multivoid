// coop/interactables/laptop_buffer_sync.h -- the laptop file-buffer QUAD sync lane
// {floppyData, floppyBuffer, floppyBufferUIDs, floppyReadwrites}.
//
// Shape: a client sends change-edge EDIT-SCRIPT batches in the grammar the native verbs actually
// use (removeAt, and append at the END -- nothing moves in place); the host applies them
// content-anchored and answers with an UNCONDITIONAL canonical, which IS the acknowledgement. A
// host's own edits go straight to a canonical on change, since no host-side derivation exists.
//
// Receivers adopt canonicals from slot 0 only, draining pending edits first, skipping the
// rebuild when the content already matches, and rebuilding the widget EAGERLY -- updFloppy
// regenerates floppyBuffer FROM bufferSlots, so a stale widget stomps wire values at every site.
//
// Wire: ReliableKind::LaptopQuad over BlobChunkPayload, with the blob's head byte naming the
// direction: 0 is a client-to-host batch, 1 a host-to-clients canonical. Never refanned. Game
// thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::laptop_buffer_sync {

void Install(coop::net::Session* session);

// 4 Hz: int pre-filter (fdN/fbN/uidN/rw -- every native verb is int-visible,
// rw monotone proof) + floppyType predicate (slot machinery owns slot
// transitions) + derive/send (client) or canonical (host organic) + selftest.
void Tick();

// LaptopQuad chunks: host consumes batches (+ answers canonical); clients
// adopt host-authored canonicals.
void OnQuadChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// The PrimeBaselines piggyback: every wire-driven laptop write path ends in
// laptop_sync::PrimeBaselines, which calls this.
void PrimeQuadBaseline();

// HOST: ship the joiner the canonical quad (after the op=3 + slot content).
void QueueConnectBroadcastForSlot(int peerSlot);

void OnDisconnect();

}  // namespace coop::laptop_buffer_sync
