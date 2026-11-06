// coop/creatures/kerfur_command_wire.h -- the BYTES of a client's kerfur command, and the chunk assembly that brings
// them to the host. Pure byte work, no engine access; what a command MEANS belongs to kerfur_command.
//
// A command is one chunked blob (blob_chunks, its sequence the command's requestId): the 12-byte
// KerfurCommandPayload head, then iff head.flags bit 0 is set
//   `[u16 n][n bytes]`   one prop record in coop/props/prop_record_wire's form: the floppy disc the client holds, for a
//                        get_reports and for no other command
// A bare command is a blob of the head alone: every command takes one path.
//
// HOSTILE-INPUT CONTRACT: the peer is not trusted. A blob past kMaxBlobBytes is refused when it completes, before any
// parse; the head's pad bytes and the flag bits no build sets must be zero; a record on any command but get_reports,
// a record that does not fill the blob exactly, and any byte after the head of a command that carries none are
// Malformed. The record's own bounds are prop_record_wire's.

#pragma once

#include "coop/net/protocol.h"
#include "coop/props/prop_record_wire.h"
#include "ue_wrap/actors/save_record.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::net { class Session; }

namespace coop::kerfur_command_wire {

inline constexpr uint8_t kFlagRecord = 0x01;  // head.flags: a record follows the head

inline constexpr size_t kHeadBytes = sizeof(coop::net::KerfurCommandPayload);
// The most a blob completes to: the head, a record's length and the record at its cap.
inline constexpr size_t kMaxBlobBytes = kHeadBytes + sizeof(uint16_t) + coop::props::prop_record_wire::kMaxRecordBytes;
// The chunks of that ceiling, at the transport's chunk size (BlobChunkPayload::data, 220 bytes): 19.
inline constexpr size_t kChunkBytes = sizeof(coop::net::BlobChunkPayload::data);
inline constexpr size_t kMaxChunks = (kMaxBlobBytes + kChunkBytes - 1) / kChunkBytes;

struct Request {
    coop::net::KerfurCommandPayload head{};
    bool hasRecord = false;                      // head.flags bit 0, parsed
    ue_wrap::save_record::SaveRecord record{};   // the held disc, when hasRecord
};

// The blob of `r`: the head with its flag set from `hasRecord`, then the record. Empty when the record is past its cap
// or a record rides a command that is not get_reports: a blob the parser would refuse is never built.
std::vector<uint8_t> Pack(const Request& r);

// Ok, or Malformed with `why` naming the first check that failed. `out` is overwritten.
enum class Parse : uint8_t { Ok, Malformed };
Parse ParseBlob(const std::vector<uint8_t>& blob, Request& out, const char** why);

// CLIENT: the command's blob, shipped to the host under its requestId. False when it was not accepted whole.
bool Send(coop::net::Session* session, const Request& r);

// HOST: one chunk of a command from `senderSlot`. Incomplete until the blob completes; then Ok with `out` the parsed
// command, or Malformed with `why` (the blob was past its ceiling, or failed the parse). Game thread.
enum class Feed : uint8_t { Incomplete, Ok, Malformed };
Feed OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot, Request& out, const char** why);

// HOST, every pump tick: half-assembled commands older than the lane's TTL are dropped. Cheap while none is held.
void Sweep();

// A peer left: its half-assembled commands go, so a recycled slot never merges into them.
void OnPeerGone(uint8_t slot);

// The selftest of the blob's hostile contract (pure bytes), run once per process; logs ALL PASS or each failure.
bool RunSelftest();

// Session end: every half-assembled command goes.
void OnDisconnect();

}  // namespace coop::kerfur_command_wire
