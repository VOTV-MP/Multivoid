// coop/interactables/floppybox_sync.h -- the disc crate (Aprop_floppyBox_C) LIFO stack sync
// over {floppyTypes[], floppyData[]}.
//
// The same shape as the drive rack: client tail value-ops (push{type,dataString} and
// pop{contentHash}) -> host tail-anchored apply -> canonical arrays after EVERY op and on an
// organic host change. A pop-miss is DENIED, and the author then reaps its just-spawned
// in-hand disc if it is still alive, or skips if it was consumed -- content duplication is
// native-legal, and no prop dupe survives an insert. The disc ACTORS cross on existing lanes
// (the destroy seam, the birth channels and hand-item); only the box arrays are new wire
// state.
//
// Wire: ReliableKind::FloppyBoxState (BlobChunkPayload; blob head [u8 op][u32 eid]; op 0=push
// 1=pop 2=deny 3=canonical). Never refanned; clients accept canonicals and denies from the
// host only; a canonical for a box not bound yet is held in bind_wait. Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::floppybox_sync {

void Install(coop::net::Session* session);

// 1 Hz element sweep (pointer class gate; first-sight silent prime): client
// derives tail ops vs the shadow -> host; host organic change -> canonical.
void Tick();

// FloppyBoxState chunks.
void OnBoxChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: ship the joiner one canonical per live box.
void QueueConnectBroadcastForSlot(int peerSlot);

void OnDisconnect();

// [dev] how many host canonicals this peer has applied to a bound box since its session began; a read-only
// question for the late-join drill. Game thread.
uint32_t AppliedCanonicals();

}  // namespace coop::floppybox_sync
