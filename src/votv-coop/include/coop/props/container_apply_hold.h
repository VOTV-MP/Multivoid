// coop/props/container_apply_hold.h -- dev only: the completed ContainerContents blobs of one container kept in
// arrival order instead of applied, then replayed in order. Models the join drain's order, where a parked take's
// replay runs before the host's publication is applied. Called only from src/coop/dev/ (the container intent
// drill) and the lane (container_contents_sync); inert unless armed. Game thread.

#pragma once

#include <cstdint>
#include <vector>

namespace coop::props::container_apply_hold {

// The lane's apply entry: what an arriving blob does. Registered at the lane's Install; Release replays through it.
void SetApply(void (*apply)(const std::vector<uint8_t>& blob, uint8_t senderSlot));

// Client: from now, a completed blob for `eid` is kept by Keep instead of applied.
void Hold(uint32_t eid);

// Whether a hold is on. The lane reads a blob's header for Keep only while it is.
bool Armed();

// Ends the hold and replays the kept blobs in arrival order through the registered apply entry.
void Release();

// The lane's one call per completed blob, with the eid its header names. True: the blob is kept (moved out of
// `blob`); false: not held, `blob` is untouched and the lane applies it.
bool Keep(uint32_t eid, std::vector<uint8_t>&& blob, uint8_t senderSlot);

// Ends the hold and drops the kept blobs, unreplayed (the session's end).
void Reset();

}  // namespace coop::props::container_apply_hold
