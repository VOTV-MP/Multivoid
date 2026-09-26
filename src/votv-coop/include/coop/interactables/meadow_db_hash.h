// coop/interactables/meadow_db_hash.h -- the laptop's signal database seen as content hashes, the one
// identity its rows have across peers: the database as a multiset and as a sequence, a row found by its
// content, and the order line (MeadowOrder), the sequence on the wire and the permutation it asks for.
// The database's rows carry no key, and a move deep-copies a row's strings, so a row is its content
// (coop/interactables/signal_wire). Stateless; coop::meadow_db_sync keeps the state. Game thread.

#pragma once

#include "ue_wrap/desk/signal_dynamic.h"

#include <cstdint>
#include <map>
#include <vector>

namespace coop::meadow_db_hash {

// One row's content identity: the hash of its wire blob. `scratch` holds the blob afterwards.
uint64_t HashRow(const ue_wrap::signal_dynamic::Row& r, std::vector<uint8_t>& scratch);

// The live database as content hashes: the multiset, and the sequence in the database's order when
// `seq` is given. False if any row is unreadable (a world in transition). A serialise per row: an edge
// path, never a per-tick one.
bool HashStore(std::map<uint64_t, int32_t>& counts, std::vector<uint64_t>* seq);

// The database index of the first row with this content, or -1. A serialise per row up to it.
int32_t IndexOf(uint64_t hash);

// The order line's bytes: a u16 count, then the hashes in order.
std::vector<uint8_t> OrderBlob(const std::vector<uint64_t>& seq);
bool ParseOrderBlob(const std::vector<uint8_t>& b, std::vector<uint64_t>& out);

// The permutation that puts `live` in `target`'s order: the target's hashes first, in its order, each
// taking the first unused row of its content (rows of one content are byte-identical); a hash the live
// database lacks is skipped, and rows the target does not list keep their relative order at the tail.
// perm[i] is the live index of the row that goes to position i.
std::vector<int32_t> PermutationTo(const std::vector<uint64_t>& live, const std::vector<uint64_t>& target);

}  // namespace coop::meadow_db_hash
