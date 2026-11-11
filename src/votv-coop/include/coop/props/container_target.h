// coop/props/container_target.h -- what a client's act on a shared inventory names. Most are world containers, named by
// their element id; the delivery drone's cargo has no element (it is the one drone's own inventory component), so it is
// named by a reserved id that no element ever takes. The helpers below are the few places the act lane's host and
// client halves need to tell the two apart: the rest of both halves works on the id alone. Gameplay layer (principle 7).
// Game thread.

#pragma once

#include <cstdint>

namespace coop::props::container_target {

// The drone's cargo as a target. Element ids count up from 1 and 0xFFFFFFFF is the invalid id, so this one is never a
// container's.
inline constexpr uint32_t kDroneCargoEid = 0xFFFFFFFEu;
inline constexpr bool IsDroneCargo(uint32_t eid) { return eid == kDroneCargoEid; }

// An ATV's trunk: the base plus 20 bits of a hash of the ATV's key (coop/interactables/atv_trunk_sync). The block
// 0xFFE00000..0xFFEFFFFF holds no element id (they count up from 1) and neither the drone's id nor the invalid one.
inline constexpr uint32_t kAtvTrunkBase = 0xFFE00000u;
inline constexpr uint32_t kAtvTrunkMask = 0xFFF00000u;
inline constexpr bool IsAtvTrunk(uint32_t eid) { return (eid & kAtvTrunkMask) == kAtvTrunkBase; }

// A target that is a vehicle's inventory component, not a world container's element.
inline constexpr bool IsVehicleInventory(uint32_t eid) { return IsDroneCargo(eid) || IsAtvTrunk(eid); }

// HOST: this peer wrote the target's entries (a granted act's step, or a restore): the lane that publishes it is told.
void MarkWritten(uint32_t eid);

// CLIENT: this peer changed the target's mirror with a verb of its own, or an act of it ended: the host's next word for
// the target applies even when it equals the last one applied.
void ForgetApplied(uint32_t eid);

// HOST: the target's live entries to one slot. `bounded`: at most the lane's one truth per interval per slot, for a
// refusal the sender can repeat; otherwise unthrottled, for a send owed once. False when nothing was sent.
bool SendTruth(uint32_t eid, int slot, bool bounded);

}  // namespace coop::props::container_target
