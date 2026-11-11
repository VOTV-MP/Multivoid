// coop/props/container_contents_io.cpp -- see coop/props/container_contents_io.h.

#include "coop/props/container_contents_io.h"
#include "coop/props/container_slice_wire.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/log.h"

namespace coop::props::container_contents_io {

namespace II = ue_wrap::inventory_item;
namespace cw = coop::props::container_slice_wire;
namespace ci = ue_wrap::container_inventory;

bool ReadRecords(void* inv, std::vector<II::Item>& out) {
    int32_t count = -1;
    if (!ci::ReadContents(inv, out, cw::kMaxRecords, &count)) {
        if (count > static_cast<int32_t>(cw::kMaxRecords))
            UE_LOGW("container_contents: %d entries exceeds the %zu cap -- refusing to ship a "
                    "truncated slice", count, cw::kMaxRecords);
        return false;
    }
    return true;
}

// Fail closed: only a world container's own inventory is this lane's to author (ci::IsWorldInventory); a player's
// pockets, a drone's or an ATV's component is never written from here. The previous buffer is orphaned by the write, so
// the caller's content-hash gate bounds the apply, since an apply happens only when the contents changed; the record
// editor's callers orphan one array per grant and per restore, bounded by the act's queue budget at 4 writes a second
// per client at most.
WriteResult WriteRecords(uint32_t eid, void* inv, const std::vector<II::Item>& recs) {
    if (!ci::IsWorldInventory(inv)) {
        UE_LOGW("container_contents: eid=%u resolves to no world container's inventory -- refusing to apply", eid);
        return WriteResult::Refused;
    }
    switch (ci::WriteContents(inv, recs)) {
        case ci::WriteResult::Written:  return WriteResult::Written;
        case ci::WriteResult::NotReady: return WriteResult::NotReady;
        case ci::WriteResult::Refused:  break;
    }
    UE_LOGW("container_contents: eid=%u -- the %zu entries were not written; leaving contents", eid, recs.size());
    return WriteResult::Refused;
}

}  // namespace coop::props::container_contents_io
