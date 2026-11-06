// coop/props/prop_record_wire.h -- one prop record on the wire, and the bounds a peer's word about it is held to.
// Pure byte work, no engine access, through the coop/items/save_record_wire grammar. A request that carries a prop's
// save record (a container act's put, a kerfur command's held disc) takes its codec and its bounds from here, so the
// record is read one way and the host's limits on a client's record are one set.
//
// A record on the wire is `[u16 n][n bytes]`: n bytes of the save_record_wire form. HOSTILE-INPUT CONTRACT: the peer
// is not trusted. The declared length is held to kMaxRecordBytes BEFORE any DeSave (so a refused or unserved record
// costs a bounds check, not a parse), and any byte after the record is the caller's Malformed.

#pragma once

#include "ue_wrap/actors/save_record.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::props::prop_record_wire {

// One record's bytes, at most: 16 times the largest record measured on the wire.
inline constexpr size_t kMaxRecordBytes = 4096;
// The longest prop name a request may carry (the game's own are short): a longer one is refused before any engine call.
inline constexpr size_t kMaxPutNameChars = 128;

// The record's wire form in the grammar a request carries it in, and its size.
std::vector<uint8_t> RecordWire(const ue_wrap::save_record::SaveRecord& r);
size_t RecordBytes(const ue_wrap::save_record::SaveRecord& r);

// `[u16 n][n bytes]` of one record appended to `b`; false, with nothing appended, when the record is past kMaxRecordBytes:
// a blob the parser would refuse is never built, and the length is never cut to 16 bits.
bool AppRecord(std::vector<uint8_t>& b, const ue_wrap::save_record::SaveRecord& r);

// `[u16 n][n bytes]` read at `o` and running to the end of `b`: one record that fills the rest exactly. False, with
// `why` naming the first check that failed, for a missing or zero length, a length past kMaxRecordBytes, a length that
// does not fill the blob, and a record that does not parse or ends short of its length. `o` is advanced on success.
bool RdRecordToEnd(const std::vector<uint8_t>& b, size_t& o, ue_wrap::save_record::SaveRecord& out, const char** why);

// The record names a prop (names[0][0]) and the name is at most kMaxPutNameChars: the name a request's record carries is
// held to the bound where the record is judged.
bool PutNameOk(const ue_wrap::save_record::SaveRecord& r);

}  // namespace coop::props::prop_record_wire
