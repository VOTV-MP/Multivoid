// coop/props/disc_record_ceiling.h -- the one byte ceiling of a disc's record on the wire.
//
// A disc crosses two ways: as a slot body in the floppy lane's blobs (a claim or a canonical) and as a
// prop's save record in the record lane's. Both are bounded by what one blob carries, and the slot lane
// puts a head before its entries, so the largest body either lane can SEND is the transport's maximum
// less that head. Both lanes refuse a body over it on the way out and on the way in; two constants would
// disagree in the gap between them, where a body is taken in and cannot be passed on.

#pragma once

#include "coop/net/blob_chunks.h"

#include <cstddef>

namespace coop::disc_record {

// The head of the slot lane's canonical blob, before its first entry: [u8 op][u8 deviceKind][u32 generation][u16 count].
inline constexpr size_t kSlotSetHeadBytes = 8;

// The transport's largest blob, less that head: the largest slot entry ([u8 index][slot]) or record body a peer can send.
inline constexpr size_t kTransportBodyBytes = coop::blob_chunks::MaxBlobBytes() - kSlotSetHeadBytes;

// The ceiling in force. A body over it is refused, never cut: half a record through loadData is worse than none.
size_t MaxBodyBytes();

}  // namespace coop::disc_record
