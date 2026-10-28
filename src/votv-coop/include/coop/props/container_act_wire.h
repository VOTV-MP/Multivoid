// coop/props/container_act_wire.h -- the BYTES of a container act's request and commit, and nothing else. Pure
// byte work, no engine access, through the coop/items/save_record_wire grammar; what an act MEANS belongs to
// container_act_host and container_act_request.
//
// Request: `[u16 id][u8 family][u8 pad=0][u32 eid]`, then by family
//   1 (take)        `[u16 index][u64 hash]`   the record's address: the index it had, the hash of its wire form
//   2 (put record)  `[u16 n][n bytes]`        one record in the save_record_wire grammar
//   3 (put prop)    `[u32 propEid]`
// Commit: `[u16 id][u8 committed][u8 pad=0][u32 eid]`, then iff the commit carries a record
//   `[u16 index][u16 n][n bytes]`             a put's done: where it landed and the record, as the host stored it
//
// HOSTILE-INPUT CONTRACT: the peer is not trusted. Every field is bounds-checked, a record's declared length is held
// to kMaxRecordBytes BEFORE any DeSave (so a refused or unserved put costs a bounds check, not a parse), and any byte
// after a family's tail is Malformed. The whole blob is already bounded by blob_chunks::MaxBlobBytes at the assembler.

#pragma once

#include "ue_wrap/actors/save_record.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::props::container_act_wire {

// One record's bytes, at most: 16 times the largest record measured on the wire.
inline constexpr size_t kMaxRecordBytes = 4096;

// A put's record is stored by the container with the owner's fridge flag appended to bools[0], and the commit carries it
// as stored. A client asks only a record at most this much under kMaxRecordBytes, so the stored one always packs.
inline constexpr size_t kPutStoredSlack = 64;
// The most the fridge flag adds to a record's wire form: a new bools group (its u32 element count) and the flag's byte.
inline constexpr size_t kFridgeElementBytes = sizeof(uint32_t) + sizeof(uint8_t);
static_assert(kMaxRecordBytes - kPutStoredSlack + kFridgeElementBytes <= kMaxRecordBytes,
              "the stored form of the largest record a client may ask still packs under the byte cap");

// The record's wire size in the grammar a request and a commit carry it in.
size_t RecordBytes(const ue_wrap::save_record::SaveRecord& r);

struct Request {
    uint16_t id = 0;
    uint8_t family = 0;
    uint32_t eid = 0;
    uint16_t index = 0;                       // Take
    uint64_t hash = 0;                        // Take
    ue_wrap::save_record::SaveRecord record{};  // PutRecord
    uint32_t propEid = 0;                     // PutProp
};

struct Commit {
    uint16_t id = 0;
    uint8_t committed = 0;
    uint32_t eid = 0;
    bool hasRecord = false;                   // a PutRecord's done
    uint16_t index = 0;
    ue_wrap::save_record::SaveRecord record{};
};

enum class Parse : uint8_t { Ok, Malformed, UnknownFamily };

// PackRequest and PackCommit return an empty vector when the record is past kMaxRecordBytes: a blob the parser
// would refuse is never built, and the length is never cut to 16 bits.
std::vector<uint8_t> PackRequest(const Request& r);

// Ok, Malformed (`why` names it), or UnknownFamily: the head parsed and the family is none of ours; `out.id`,
// `out.family` and `out.eid` are set so the host can answer it.
Parse ParseRequest(const std::vector<uint8_t>& b, Request& out, const char** why);

// PackCommit appends the record tail iff `c.hasRecord && c.committed == 1`.
std::vector<uint8_t> PackCommit(const Commit& c);
Parse ParseCommit(const std::vector<uint8_t>& b, Commit& out, const char** why);

}  // namespace coop::props::container_act_wire
