// coop/props/container_contents_io.cpp -- see coop/props/container_contents_io.h.

#include "coop/props/container_contents_io.h"
#include "coop/props/container_slice_wire.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

namespace coop::props::container_contents_io {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace cw = coop::props::container_slice_wire;
namespace ci = ue_wrap::container_inventory;

// BOUNDARY 1, fail closed: only a world container's own inventory is this lane's to author
// (ci::IsWorldInventory); authoring a personal inventory from the host would wipe that peer's own.
// BOUNDARY 2: a nested container's ints[0][0] is its own GObjStack index, a slot number in the
// sender's array that must not survive the wire, so it is shipped and written as -1
// (ci::ClearInventoryIndex says why never by clearing ints).
bool ReadRecords(void* inv, std::vector<SR::SaveRecord>& out, bool neuterNested) {
    int32_t count = -1;
    if (!ci::ReadContents(inv, out, cw::kMaxRecords, &count)) {
        if (count > static_cast<int32_t>(cw::kMaxRecords))
            UE_LOGW("container_contents: %d records exceeds the %zu cap -- refusing to ship a "
                    "truncated slice", count, cw::kMaxRecords);
        return false;
    }
    if (neuterNested)
        for (auto& r : out)
            if (ci::IsContainerRecord(r)) ci::ClearInventoryIndex(r);
    return true;
}

WriteResult WriteRecords(uint32_t eid, void* inv, const std::vector<SR::SaveRecord>& recs,
                         SR::ClassResolver resolve) {
    if (!ci::IsWorldInventory(inv)) {           // BOUNDARY 1 (fail-closed)
        UE_LOGW("container_contents: eid=%u resolves to a PERSONAL inventory (or an unresolvable "
                "Player flag) -- refusing to apply", eid);
        return WriteResult::Refused;                  // resolved; deliberately not applied
    }
    uint8_t* slot = ci::ContentsSlot(inv);
    if (!slot) return WriteResult::NotReady;

    // The allocator pre-flight: without it an empty array would silently replace real contents.
    if (void* probe = R::EngineAlloc(16)) {
        R::EngineFree(probe);
    } else {
        UE_LOGW("container_contents: eid=%u -- EngineAlloc unavailable; refusing to write", eid);
        return WriteResult::Refused;
    }

    const int32_t n = static_cast<int32_t>(recs.size());
    void* buf = SR::AllocZeroed(static_cast<size_t>(n), static_cast<size_t>(SR::kSaveStride));
    if (!buf && n > 0) {
        UE_LOGW("container_contents: eid=%u -- alloc of %d records failed; leaving contents", eid, n);
        return WriteResult::Refused;
    }
    for (int32_t i = 0; i < n; ++i)
        SR::WriteSaveRecord(reinterpret_cast<uint8_t*>(buf) + static_cast<size_t>(i) * SR::kSaveStride,
                            recs[i], resolve);
    // The previous buffer is orphaned: freeing the nested sub-arrays, minted FStrings and signal
    // rows recursively is far more crash-prone than leaking them, and the engine never double-frees
    // a buffer it has lost. inventory::ApplyToSaveObject orphans the same way once per join; this
    // lane is steady-state, and the caller's content-hash gate bounds it, since an apply happens only
    // when the contents changed.
    SR::WriteArrHeader(slot, 0, buf, n);

    ci::RederiveShownState(ci::OwnerOf(inv), inv);
    return WriteResult::Written;
}

}  // namespace coop::props::container_contents_io
