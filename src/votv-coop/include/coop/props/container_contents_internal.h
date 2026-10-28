// coop/props/container_contents_internal.h -- what container_contents_sync gives container_truth: the role, the clock,
// the world-container resolve and the targeted send. Not an API: only those two files include it. Game thread.

#pragma once

#include <cstdint>

namespace coop::props::container_contents_sync::internal {

bool IsHostRole();
uint64_t NowMs();

// The inventory of the live world container `eid` names, or null.
void* LiveWorldInventory(uint32_t eid);

// The container's live contents to one slot, forced past the unchanged-hash guard. False when refused.
bool SendContainerTo(uint32_t eid, void* inv, int slot);

}  // namespace coop::props::container_contents_sync::internal
