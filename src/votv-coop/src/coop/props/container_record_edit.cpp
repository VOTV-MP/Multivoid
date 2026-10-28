// coop/props/container_record_edit.cpp -- see coop/props/container_record_edit.h.

#include "coop/props/container_record_edit.h"

#include "coop/element/registry.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_slice_wire.h"
#include "coop/props/prop_element_tracker.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"

#include <algorithm>
#include <utility>

namespace coop::props::container_record_edit {
namespace {

namespace SR = ue_wrap::save_record;
namespace ci = ue_wrap::container_inventory;
namespace cio = coop::props::container_contents_io;
namespace cw = coop::props::container_slice_wire;

// The host's own role and the container's inventory, or the reason there is none. Role only: the ends that restore
// run while a session is stopping, after it stops running.
EditResult Resolve(uint32_t eid, void*& outInv) {
    UE_ASSERT_GAME_THREAD("container_record_edit");
    outInv = nullptr;
    if (!coop::prop_element_tracker::SessionIsHost()) return EditResult::NotHost;
    void* actor = coop::element::LivePropActor(eid);
    if (!actor || !ci::IsContainer(actor)) return EditResult::NoElement;
    void* inv = ci::InventoryOf(actor);
    if (!inv || !ci::IsWorldInventory(inv)) return EditResult::NoInventory;
    outInv = inv;
    return EditResult::Done;
}

// Write `recs` over the container and publish it.
EditResult Commit(uint32_t eid, void* inv, const std::vector<SR::SaveRecord>& recs, const char* what,
                  size_t index) {
    if (recs.size() > cw::kMaxRecords) return EditResult::Refused;
    switch (cio::WriteRecords(eid, inv, recs, &ue_wrap::object_index::ClassByName)) {
        case cio::WriteResult::NotReady: return EditResult::NotReady;
        case cio::WriteResult::Refused:  return EditResult::Refused;
        case cio::WriteResult::Written:  break;
    }
    coop::props::container_contents_sync::MarkHostWrite(eid);
    // An open screen onto this container does not rebuild on a raw write; with none open this does nothing,
    // which is also the case when a session's teardown restores a record.
    ue_wrap::container_view::Redraw(coop::element::LivePropActor(eid));
    UE_LOGI("container_record_edit: eid=%u %s index=%zu -> %zu records", eid, what, index, recs.size());
    return EditResult::Done;
}

}  // namespace

EditResult Probe(uint32_t eid) {
    void* inv = nullptr;
    if (const EditResult r = Resolve(eid, inv); r != EditResult::Done) return r;
    return ci::ContentsSlot(inv) ? EditResult::Done : EditResult::NotReady;
}

EditResult ReadRecords(uint32_t eid, std::vector<SR::SaveRecord>& out, bool neuterNested) {
    void* inv = nullptr;
    if (const EditResult r = Resolve(eid, inv); r != EditResult::Done) return r;
    return cio::ReadRecords(inv, out, neuterNested) ? EditResult::Done : EditResult::Unreadable;
}

EditResult RemoveAt(uint32_t eid, uint16_t index, uint64_t expectedHash, SR::SaveRecord& outRecord) {
    void* inv = nullptr;
    if (const EditResult r = Resolve(eid, inv); r != EditResult::Done) return r;
    std::vector<SR::SaveRecord> recs;
    if (!cio::ReadRecords(inv, recs, /*neuterNested=*/false)) return EditResult::Unreadable;
    if (index >= recs.size()) return EditResult::OutOfRange;
    {   // the judge's hash is over the wire form, so the check is too
        SR::SaveRecord wireForm = recs[index];
        internal::NeuterIfNested(wireForm);
        if (cw::RecordHash(wireForm) != expectedHash) return EditResult::Mismatch;
    }
    SR::SaveRecord taken = std::move(recs[index]);
    recs.erase(recs.begin() + index);
    const EditResult r = Commit(eid, inv, recs, "remove", index);
    if (r == EditResult::Done) outRecord = std::move(taken);
    return r;
}

EditResult InsertAt(uint32_t eid, uint16_t index, const SR::SaveRecord& record, Source src) {
    void* inv = nullptr;
    if (const EditResult r = Resolve(eid, inv); r != EditResult::Done) return r;
    std::vector<SR::SaveRecord> recs;
    if (!cio::ReadRecords(inv, recs, /*neuterNested=*/false)) return EditResult::Unreadable;
    SR::SaveRecord copy = record;
    if (src == Source::Wire) internal::NeuterIfNested(copy);
    const size_t at = internal::ClampIndex(index, recs.size());
    recs.insert(recs.begin() + static_cast<std::ptrdiff_t>(at), std::move(copy));
    return Commit(eid, inv, recs, "insert", at);
}

const char* EditResultName(EditResult r) {
    switch (r) {
        case EditResult::Done:        return "done";
        case EditResult::NotHost:     return "not-host";
        case EditResult::NoElement:   return "no-element";
        case EditResult::NoInventory: return "no-inventory";
        case EditResult::Unreadable:  return "unreadable";
        case EditResult::NotReady:    return "not-ready";
        case EditResult::Refused:     return "refused";
        case EditResult::OutOfRange:  return "out-of-range";
        case EditResult::Mismatch:    return "mismatch";
    }
    return "?";
}

namespace dev {

EditResult Clear(uint32_t eid, size_t& cleared) {
    cleared = 0;
    void* inv = nullptr;
    if (const EditResult r = Resolve(eid, inv); r != EditResult::Done) return r;
    std::vector<SR::SaveRecord> recs;
    if (!cio::ReadRecords(inv, recs, /*neuterNested=*/false)) return EditResult::Unreadable;
    cleared = recs.size();
    return Commit(eid, inv, std::vector<SR::SaveRecord>{}, "clear", 0);
}

}  // namespace dev

namespace internal {

size_t ClampIndex(uint16_t index, size_t length) { return std::min<size_t>(index, length); }

void NeuterIfNested(SR::SaveRecord& r) {
    if (ci::IsContainerRecord(r)) ci::ClearInventoryIndex(r);
}

}  // namespace internal

}  // namespace coop::props::container_record_edit
