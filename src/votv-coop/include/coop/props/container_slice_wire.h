// coop/props/container_slice_wire.h -- the BYTES of one world container's contents slice, and
// nothing else: `[u8 op=0][u32 eid][u16 n][u8 flags]` then n records in the
// coop/items/save_record_wire grammar. Pure byte work, no engine access -- what a record MEANS
// (is it a nested container, may this author write it) and what a flag MEANS belong to the lanes
// that own them, coop/props/container_contents_sync and coop/props/container_arming.
//
// The header and the records are parsed by SEPARATE calls on purpose: the host decides whether to
// take a client's slice from the eid alone (it takes a thrown container's birth and nothing else), and
// a refused slice must not have cost a full record parse first.

#pragma once

#include "ue_wrap/actors/save_record.h"

#include <cstdint>
#include <vector>

namespace coop::props::container_slice_wire {

// A container with more records than this is neither shipped nor accepted: the blob would approach
// the transport ceiling, and a truncated blob is a silent lie. Real containers hold single digits.
inline constexpr size_t kMaxRecords = 512;

// The flags byte. Bit 0: the container's overlap box is armed on the host (coop/props/container_arming). The other bits
// are zero on send and ignored on receive: a receiver masks with kKnownFlags, so no unknown bit reaches a hash.
inline constexpr uint8_t kFlagArmed = 1;
inline constexpr uint8_t kKnownFlags = kFlagArmed;

std::vector<uint8_t> Pack(uint32_t eid, const std::vector<ue_wrap::save_record::SaveRecord>& recs, uint8_t flags);

// The hash of a pack: it names the contents and the flags, so the same records and flags hash the same whichever peer
// authored them. The parity report compares by it.
uint64_t ContentHash(uint32_t eid, const std::vector<ue_wrap::save_record::SaveRecord>& recs, uint8_t flags);

// The hash of the records alone (ContentHash with no flag set): what a sender's and a receiver's "unchanged" guards
// compare, beside the flags they keep apart, so a change of flags with the same records is told from a change of records.
uint64_t RecordsHash(uint32_t eid, const std::vector<ue_wrap::save_record::SaveRecord>& recs);

// The hash of ONE record: its save_record_wire bytes alone, no eid and no header. The caller hashes the record in
// the form the wire carries (read with neuterNested=true), so a host record and its client mirror hash alike; two
// equal records hash alike and are the same item to every peer.
uint64_t RecordHash(const ue_wrap::save_record::SaveRecord& r);

// RecordHash of a record already serialised, so a caller that has the bytes does not serialise it again.
uint64_t HashBytes(const std::vector<uint8_t>& recordWire);

// The size of Pack(eid, recs, flags) without building it: the header and each record's wire size, one record
// serialised at a time into a buffer that is reused.
size_t SliceBytes(const std::vector<ue_wrap::save_record::SaveRecord>& recs);

// The flags byte of a built pack, rewritten: the host relays a client's slice with its own reading of the flags, never the
// sender's. False, `blob` untouched, for a blob shorter than the header.
bool SetFlags(std::vector<uint8_t>& blob, uint8_t flags);

// False on a blob that is not one of ours, or too short. Leaves `o` at the first record. `outFlags` is the byte as sent,
// unmasked.
bool ParseHeader(const std::vector<uint8_t>& b, size_t& o, uint32_t& outEid, uint16_t& outCount, uint8_t& outFlags);

// The `count` records after the header. False on an over-cap count, a count the remaining bytes cannot feed, or a
// malformed record; `outWhy` then names which for the caller's log line.
bool ParseRecords(const std::vector<uint8_t>& b, size_t& o, uint16_t count,
                  std::vector<ue_wrap::save_record::SaveRecord>& out, const char** outWhy);

}  // namespace coop::props::container_slice_wire
