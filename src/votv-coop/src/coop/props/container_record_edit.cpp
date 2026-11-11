// coop/props/container_record_edit.cpp -- see coop/props/container_record_edit.h.

#include "coop/props/container_record_edit.h"

#include "coop/element/registry.h"
#include "coop/interactables/atv_trunk_sync.h"
#include "coop/props/container_contents_io.h"
#include "coop/props/container_slice_wire.h"
#include "coop/props/container_target.h"
#include "coop/props/prop_element_tracker.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/devices/drone.h"

#include <algorithm>
#include <utility>

namespace coop::props::container_record_edit {
namespace {

namespace II = ue_wrap::inventory_item;
namespace ci = ue_wrap::container_inventory;
namespace cio = coop::props::container_contents_io;
namespace cw = coop::props::container_slice_wire;
namespace ct = coop::props::container_target;

ci::WriteResult ToWrite(cio::WriteResult r) {
    switch (r) {
        case cio::WriteResult::Written:  return ci::WriteResult::Written;
        case cio::WriteResult::NotReady: return ci::WriteResult::NotReady;
        case cio::WriteResult::Refused:  break;
    }
    return ci::WriteResult::Refused;
}

// The host's own role and the target's inventory, or the reason there is none. Role only: the ends that restore
// run while a session is stopping, after it stops running.
EditResult Resolve(uint32_t eid, void*& outInv) {
    UE_ASSERT_GAME_THREAD("container_record_edit");
    outInv = nullptr;
    if (!coop::prop_element_tracker::SessionIsHost()) return EditResult::NotHost;
    if (ct::IsDroneCargo(eid)) {
        void* const drone = ue_wrap::drone::Find();
        void* const inv = drone ? ue_wrap::drone::InventoryOf(drone) : nullptr;
        if (!inv) return EditResult::NoElement;
        outInv = inv;
        return EditResult::Done;
    }
    if (ct::IsAtvTrunk(eid)) {
        void* const atv = coop::atv_trunk_sync::AtvOfEid(eid);
        void* const inv = atv ? ue_wrap::atv::InventoryOf(atv) : nullptr;
        if (!inv) return EditResult::NoElement;
        outInv = inv;
        return EditResult::Done;
    }
    void* actor = coop::element::LivePropActor(eid);
    if (!actor || !ci::IsContainer(actor)) return EditResult::NoElement;
    void* inv = ci::InventoryOf(actor);
    if (!inv || !ci::IsWorldInventory(inv)) return EditResult::NoInventory;
    outInv = inv;
    return EditResult::Done;
}

// Write `recs` over the target and publish it.
EditResult Commit(uint32_t eid, void* inv, const std::vector<II::Item>& recs, const char* what,
                  size_t index) {
    if (recs.size() > cw::kMaxRecords) return EditResult::Refused;
    const ci::WriteResult written = ct::IsVehicleInventory(eid) ? ci::WriteContents(inv, recs)
                                                          : ToWrite(cio::WriteRecords(eid, inv, recs));
    switch (written) {
        case ci::WriteResult::NotReady: return EditResult::NotReady;
        case ci::WriteResult::Refused:  return EditResult::Refused;
        case ci::WriteResult::Written:  break;
    }
    ct::MarkWritten(eid);
    UE_LOGI("container_record_edit: eid=%u %s index=%zu -> %zu records", eid, what, index, recs.size());
    return EditResult::Done;
}

}  // namespace

EditResult Probe(uint32_t eid) {
    void* inv = nullptr;
    if (const EditResult r = Resolve(eid, inv); r != EditResult::Done) return r;
    return ci::ContentsSlot(inv) ? EditResult::Done : EditResult::NotReady;
}

EditResult ReadRecords(uint32_t eid, std::vector<II::Item>& out) {
    void* inv = nullptr;
    if (const EditResult r = Resolve(eid, inv); r != EditResult::Done) return r;
    return cio::ReadRecords(inv, out) ? EditResult::Done : EditResult::Unreadable;
}

EditResult RemoveAt(uint32_t eid, uint16_t index, uint64_t expectedHash, II::Item& outRecord) {
    void* inv = nullptr;
    if (const EditResult r = Resolve(eid, inv); r != EditResult::Done) return r;
    std::vector<II::Item> recs;
    if (!cio::ReadRecords(inv, recs)) return EditResult::Unreadable;
    if (index >= recs.size()) return EditResult::OutOfRange;
    if (cw::RecordHash(recs[index]) != expectedHash) return EditResult::Mismatch;
    II::Item taken = std::move(recs[index]);
    recs.erase(recs.begin() + index);
    const EditResult r = Commit(eid, inv, recs, "remove", index);
    if (r == EditResult::Done) outRecord = std::move(taken);
    return r;
}

EditResult TakeFromStackAt(uint32_t eid, uint16_t index, uint64_t expectedHash, uint8_t amount, II::Item& outRecord) {
    void* inv = nullptr;
    if (const EditResult r = Resolve(eid, inv); r != EditResult::Done) return r;
    std::vector<II::Item> recs;
    if (!cio::ReadRecords(inv, recs)) return EditResult::Unreadable;
    if (index >= recs.size()) return EditResult::OutOfRange;
    if (cw::RecordHash(recs[index]) != expectedHash) return EditResult::Mismatch;
    if (amount == 0 || amount >= recs[index].amount) return EditResult::Refused;
    II::Item taken = recs[index];
    taken.amount = amount;
    recs[index].amount = static_cast<uint8_t>(recs[index].amount - amount);
    const EditResult r = Commit(eid, inv, recs, "take-from-stack", index);
    if (r == EditResult::Done) outRecord = std::move(taken);
    return r;
}

EditResult MergeAt(uint32_t eid, uint16_t toIndex, uint64_t toHash, uint16_t fromIndex, uint64_t fromHash) {
    void* inv = nullptr;
    if (const EditResult r = Resolve(eid, inv); r != EditResult::Done) return r;
    std::vector<II::Item> recs;
    if (!cio::ReadRecords(inv, recs)) return EditResult::Unreadable;
    if (toIndex >= recs.size() || fromIndex >= recs.size()) return EditResult::OutOfRange;
    if (cw::RecordHash(recs[toIndex]) != toHash || cw::RecordHash(recs[fromIndex]) != fromHash)
        return EditResult::Mismatch;
    II::Item& to = recs[toIndex];
    const II::Item& from = recs[fromIndex];
    if (toIndex == fromIndex || to.classPath != from.classPath || to.name != from.name ||
        to.amount + from.amount > 255)
        return EditResult::Refused;
    to.amount = static_cast<uint8_t>(to.amount + from.amount);
    recs.erase(recs.begin() + fromIndex);
    return Commit(eid, inv, recs, "merge", toIndex);
}

bool PutTermsOf(uint32_t eid, ci::PutTerms& out) {
    out = ci::PutTerms{};
    if (ct::IsDroneCargo(eid)) {
        void* const drone = ue_wrap::drone::Find();
        return ci::ReadInventoryPutTerms(drone ? ue_wrap::drone::InventoryOf(drone) : nullptr, out);
    }
    if (ct::IsAtvTrunk(eid)) {
        void* const atv = coop::atv_trunk_sync::AtvOfEid(eid);
        return ci::ReadInventoryPutTerms(atv ? ue_wrap::atv::InventoryOf(atv) : nullptr, out);
    }
    void* const actor = coop::element::LivePropActor(eid);
    return actor && ci::ReadPutTerms(actor, out);
}

EditResult InsertAt(uint32_t eid, uint16_t index, const II::Item& record) {
    void* inv = nullptr;
    if (const EditResult r = Resolve(eid, inv); r != EditResult::Done) return r;
    std::vector<II::Item> recs;
    if (!cio::ReadRecords(inv, recs)) return EditResult::Unreadable;
    const size_t at = internal::ClampIndex(index, recs.size());
    recs.insert(recs.begin() + static_cast<std::ptrdiff_t>(at), record);
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
    std::vector<II::Item> recs;
    if (!cio::ReadRecords(inv, recs)) return EditResult::Unreadable;
    cleared = recs.size();
    return Commit(eid, inv, std::vector<II::Item>{}, "clear", 0);
}

}  // namespace dev

namespace internal {

size_t ClampIndex(uint16_t index, size_t length) { return std::min<size_t>(index, length); }

}  // namespace internal

}  // namespace coop::props::container_record_edit
