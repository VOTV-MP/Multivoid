// coop/props/container_contents_io.h -- the raw I/O of one world container's inventory slice: the
// records are read out of its GObjStack array, and written back into it through the engine's own
// verbs. Holds no lane state, no session and no maps; the lane beside it (container_contents_sync)
// owns the hashes, the pen and the arbitration, and calls these on the game thread.

#pragma once

#include "ue_wrap/actors/save_record.h"

#include <cstdint>
#include <vector>

namespace coop::props::container_contents_io {

// Read the inventory's records into `out`. False when the slice does not read, or exceeds the lane's
// record cap (logged). `neuterNested` writes -1 into a nested container's own index so a slot number
// of this peer's array never travels; false keeps the records raw, for a merge that stays in this array.
bool ReadRecords(void* inv, std::vector<ue_wrap::save_record::SaveRecord>& out, bool neuterNested = true);

enum class WriteResult : uint8_t {
    Written,   // the slice is in the array and the shown state is re-derived
    NotReady,  // the inventory's slice slot does not resolve yet -- the caller waits
    Refused,   // deliberately not written: a personal inventory, no allocator, or an alloc failure
};

// Write `recs` over the inventory's slice. `eid` is for the log lines only. `resolve` resolves each
// record's class names (null = reflection::FindClass).
WriteResult WriteRecords(uint32_t eid, void* inv,
                         const std::vector<ue_wrap::save_record::SaveRecord>& recs,
                         ue_wrap::save_record::ClassResolver resolve);

}  // namespace coop::props::container_contents_io
