// coop/props/container_contents_io.h -- the raw I/O of one world container's contents: the entries are read out of
// its inventory component's canvasInventory, and written back into it, the shown state re-derived through the
// engine's own verb. Holds no lane state, no session and no maps; the lane beside it (container_contents_sync) owns
// the hashes, the pen and the birth rule, and calls these on the game thread.

#pragma once

#include "ue_wrap/actors/inventory_item.h"

#include <cstdint>
#include <vector>

namespace coop::props::container_contents_io {

// Read the inventory's entries into `out`. False when the array does not read, or exceeds the lane's cap (logged).
bool ReadRecords(void* inv, std::vector<ue_wrap::inventory_item::Item>& out);

enum class WriteResult : uint8_t {
    Written,   // the entries are in the array; a screen showing the container is rebuilt from them, else the volumes are re-derived
    NotReady,  // the array or the entry layout does not resolve yet -- the caller waits
    Refused,   // deliberately not written: not a world container, no allocator, or an alloc failure
};

// Write `recs` over the inventory's entries. `eid` is for the log lines only.
WriteResult WriteRecords(uint32_t eid, void* inv, const std::vector<ue_wrap::inventory_item::Item>& recs);

}  // namespace coop::props::container_contents_io
