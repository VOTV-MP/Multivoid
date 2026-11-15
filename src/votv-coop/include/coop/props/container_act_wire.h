// coop/props/container_act_wire.h -- the BYTES of a container act's request and commit, and nothing else. Pure
// byte work, no engine access, through the coop/items/inventory_item_wire grammar; what an act MEANS belongs to
// container_act_host and container_act_request.
//
// Request: `[u16 id][u8 family][u8 pad=0][u32 eid]`, then by family
//   1 (take)        `[u16 index][u64 hash]`   the entry's address: the index it had, the hash of its wire form
//   2 (put record)  `[u32 n][n bytes]`        one entry in the inventory_item_wire grammar
//   3 (take stack)  `[u16 index][u64 hash][u8 amount]`   part of a stack: the entry's address, the units taken
//   4 (merge stack) `[u16 index][u64 hash][u16 indexB][u64 hashB]`   the entry at `index` absorbs the one at `indexB`
// `eid` names a world container's element, or the delivery drone's cargo (container_target::kDroneCargoEid).
// Commit: `[u16 id][u8 committed][u8 pad=0][u32 eid]`, then iff the commit carries an entry
//   `[u16 index][u32 n][n bytes]`             a put's done: where it landed and the entry, as the container stored it
// HOSTILE-INPUT CONTRACT: the peer is not trusted. Every field is bounds-checked, an entry's declared length is held to
// inventory_item_wire::kMaxItemBytes BEFORE any parse, and any byte after a family's tail is Malformed. The whole blob is
// already bounded by blob_chunks::MaxBlobBytes at the assembler.

#pragma once

#include "ue_wrap/actors/inventory_item.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coop::props::container_act_wire {

// A put's entry is stored by the container with its own canvas placement, and the commit carries it as stored. A client
// asks only an entry at most this much under inventory_item_wire::kMaxItemBytes, so the stored one always packs.
inline constexpr size_t kPutStoredSlack = 64;
struct Request {
    uint16_t id = 0;
    uint8_t family = 0;
    uint32_t eid = 0;
    uint16_t index = 0;                       // Take, TakeStack, MergeStack (the entry that stays)
    uint64_t hash = 0;                        // the same
    uint8_t amount = 0;                       // TakeStack
    uint16_t indexB = 0;                      // MergeStack: the entry that is folded in
    uint64_t hashB = 0;                       // MergeStack
    ue_wrap::inventory_item::Item record{};  // PutRecord
};

struct Commit {
    uint16_t id = 0;
    uint8_t committed = 0;
    uint32_t eid = 0;
    bool hasRecord = false;                   // a PutRecord's done
    uint16_t index = 0;
    ue_wrap::inventory_item::Item record{};
};

enum class Parse : uint8_t { Ok, Malformed, UnknownFamily };

// PackRequest and PackCommit return an empty vector when the entry is past inventory_item_wire::kMaxItemBytes: a
// blob the parser would refuse is never built.
std::vector<uint8_t> PackRequest(const Request& r);

// Ok, Malformed (`why` names it), or UnknownFamily: the head parsed and the family is none of ours; `out.id`,
// `out.family` and `out.eid` are set so the host can answer it.
Parse ParseRequest(const std::vector<uint8_t>& b, Request& out, const char** why);

// PackCommit appends the record tail iff `c.hasRecord && c.committed == 1`.
std::vector<uint8_t> PackCommit(const Commit& c);
Parse ParseCommit(const std::vector<uint8_t>& b, Commit& out, const char** why);

}  // namespace coop::props::container_act_wire
