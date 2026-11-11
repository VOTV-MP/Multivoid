// coop/props/container_slice_wire.h -- the BYTES of one world container's contents slice, and
// nothing else: `[u8 op=0][u32 eid][u16 n]` then n inventory entries in the
// coop/items/inventory_item_wire grammar. Pure byte work, no engine access -- what a record MEANS
// (is it a nested container, may this author write it) belongs to the lane that owns it,
// coop/props/container_contents_sync.
//
// The header and the records are parsed by SEPARATE calls on purpose: the host decides whether to
// take a client's slice from the eid alone (it takes a thrown container's birth and nothing else), and
// a refused slice must not have cost a full record parse first.

#pragma once

#include "ue_wrap/actors/inventory_item.h"

#include <cstdint>
#include <vector>

namespace coop::props::container_slice_wire {

// A container with more records than this is neither shipped nor accepted: the blob would approach
// the transport ceiling, and a truncated blob is a silent lie. Real containers hold single digits.
inline constexpr size_t kMaxRecords = 512;

std::vector<uint8_t> Pack(uint32_t eid, const std::vector<ue_wrap::inventory_item::Item>& recs);

// The hash of the records as a pack carries them: what a sender's and a receiver's "unchanged" guards compare, so the
// same records hash the same whichever peer authored them.
uint64_t RecordsHash(uint32_t eid, const std::vector<ue_wrap::inventory_item::Item>& recs);

// The hash of ONE record: its inventory_item_wire bytes alone, no eid and no header. The caller hashes the record in
// the form the wire carries , so a host record and its client mirror hash alike; two
// equal records hash alike and are the same item to every peer.
uint64_t RecordHash(const ue_wrap::inventory_item::Item& r);

// RecordHash of a record already serialised, so a caller that has the bytes does not serialise it again.
uint64_t HashBytes(const std::vector<uint8_t>& recordWire);

// The size of Pack(eid, recs, flags) without building it: the header and each record's wire size, one record
// serialised at a time into a buffer that is reused.
size_t SliceBytes(const std::vector<ue_wrap::inventory_item::Item>& recs);

// False on a blob that is not one of ours, or too short. Leaves `o` at the first record.
bool ParseHeader(const std::vector<uint8_t>& b, size_t& o, uint32_t& outEid, uint16_t& outCount);

// The `count` records after the header. False on an over-cap count, a count the remaining bytes cannot feed, or a
// malformed record; `outWhy` then names which for the caller's log line.
bool ParseRecords(const std::vector<uint8_t>& b, size_t& o, uint16_t count,
                  std::vector<ue_wrap::inventory_item::Item>& out, const char** outWhy);

}  // namespace coop::props::container_slice_wire
