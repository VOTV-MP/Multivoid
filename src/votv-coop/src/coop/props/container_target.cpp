// coop/props/container_target.cpp -- see coop/props/container_target.h.

#include "coop/props/container_target.h"

#include "coop/interactables/atv_trunk_sync.h"
#include "coop/interactables/drone_cargo_sync.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_truth.h"

namespace coop::props::container_target {

void MarkWritten(uint32_t eid) {
    if (IsDroneCargo(eid)) coop::drone_cargo_sync::MarkHostWrite();
    else if (IsAtvTrunk(eid)) coop::atv_trunk_sync::MarkHostWrite(eid);
    else coop::props::container_contents_sync::MarkHostWrite(eid);
}

void ForgetApplied(uint32_t eid) {
    if (IsDroneCargo(eid)) coop::drone_cargo_sync::ForgetApplied();
    else if (IsAtvTrunk(eid)) coop::atv_trunk_sync::ForgetApplied(eid);
    else coop::props::container_contents_sync::ForgetApplied(eid);
}

bool SendTruth(uint32_t eid, int slot, bool bounded) {
    if (IsDroneCargo(eid)) return coop::drone_cargo_sync::SendTruthToSlot(slot, bounded);
    if (IsAtvTrunk(eid)) return coop::atv_trunk_sync::SendTruthToSlot(eid, slot, bounded);
    return bounded ? coop::props::container_truth::SendTruthBounded(eid, slot)
                   : coop::props::container_contents_sync::SendTruthToSlot(eid, slot);
}

}  // namespace coop::props::container_target
