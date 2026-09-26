// coop/interactables/meadow_db_internal.h -- what the meadow lane's join seed shares with the rest of
// the lane, between its two translation units: meadow_db_sync.cpp owns every line the lane authors or
// applies and the state below, meadow_db_join.cpp takes a joiner's snapshot and sends its seed. Nothing
// outside coop/interactables/ includes this -- the lane's public surface is
// coop/interactables/meadow_db_sync.h. Game thread throughout.

#pragma once

#include <cstdint>
#include <vector>

namespace coop::net { class Session; }

namespace coop::meadow_db_sync::internal {

// An authored line not yet delivered: its send failed, or a client authored it before its world-ready.
struct Pending {
    uint64_t hash = 0;
    bool     isDelete = false;
    uint64_t bornOp = 0;                 // vs a join snapshot's opAt (mask criterion)
    uint32_t excludeMask = 0;            // slots the seed already covered
    uint32_t sentMask = 0;               // slots already delivered (masked retry)
    std::vector<uint8_t> blob;           // append: serialized row (sans image)
};

// The waiting lines, in the order they were authored.
std::vector<Pending>& Waiting();

// The line-author counter: one step per line this peer authored this session.
uint64_t OpCounter();

coop::net::Session* SessionPtr();
bool IsHost();

// The sequence number for the lane's next chunked line.
uint32_t NextSeq();

// One order line: a negative slot broadcasts (the host canonical); otherwise point-to-point (a
// client's op to the host, or the join seed's canonical to one joiner).
bool SendOrder(coop::net::Session* s, const std::vector<uint64_t>& seq, int toSlot);

// An order held for the retry, which sends it once no line waits.
void HoldOrder();

// The seed's lines, in the session totals.
void CountSeedLines(uint64_t n);

// A recycled slot must not finish the departed peer's half-sent row or order.
void ClearSlotAssemblies(uint8_t slot);

// meadow_db_join.cpp: every slot's snapshot and first-seed mark dropped, at the session's end.
void ResetJoinSeeds();

}  // namespace coop::meadow_db_sync::internal
