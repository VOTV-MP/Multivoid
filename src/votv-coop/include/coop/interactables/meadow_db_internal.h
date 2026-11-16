// coop/interactables/meadow_db_internal.h -- what the meadow lane's join seed shares with the rest of
// the lane, between its two translation units: meadow_db_sync.cpp owns every line the lane authors or
// applies and the state below, meadow_db_join.cpp takes a joiner's snapshot and sends its seed. Nothing
// outside coop/interactables/ includes this -- the lane's public surface is
// coop/interactables/meadow_db_sync.h. Game thread throughout.

#pragma once

#include "coop/player/slot_mask.h"

#include <cstdint>
#include <vector>

namespace coop::net { class Session; }

namespace coop::meadow_db_sync::internal {

// An authored line not yet delivered: its send failed, or a client authored it before its world-ready.
struct Pending {
    uint64_t hash = 0;
    bool     isDelete = false;
    uint64_t bornOp = 0;                 // vs a join snapshot's opAt (mask criterion)
    coop::players::SlotMask excludeMask;   // slots the seed already covered
    coop::players::SlotMask sentMask;      // slots already delivered (masked retry)
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

// The seed's canonical order owed to `slot`: the retry sends it once no line waits.
void OweOrderTo(int slot);

// The seed's lines, in the session totals.
void CountSeedLines(uint64_t n);

// A departed slot: a recycled one must not finish the departed peer's half-sent row or order, nor be owed
// its seed's order.
void ForgetSlot(uint8_t slot);

// meadow_db_join.cpp: every slot's snapshot and first-seed mark dropped, at the session's end.
void ResetJoinSeeds();
// The seeds owed to joiners whose world-ready came while this host's database was away: run once it is back,
// before the pen drains. Dropped with a database a load replaces.
bool HasOwedSeeds();
void RunOwedSeeds();
void DropOwedSeeds();
// Whether this peer's database or its laptop widget is away (the lane's own test).
bool DatabaseAway();

}  // namespace coop::meadow_db_sync::internal
