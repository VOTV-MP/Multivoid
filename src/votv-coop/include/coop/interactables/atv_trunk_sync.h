// coop/interactables/atv_trunk_sync.h -- an ATV's TRUNK, the host's, mirrored on every client. Gameplay/network layer
// (principle 7): the wire, the host's publication and the client's apply; the engine is reached through ue_wrap::atv.
//
// In 0.10 the trunk is the ATV's own inventoryComponent_C (ATV_C.inventoryComponent): the open option runs
// ui_FLI_inventory.begin on it and a hand item put into it runs addItemAsObject on it, so no world container, element
// or container lane covers it. It is a shared inventory of the drone cargo's kind (coop/interactables/drone_cargo_sync):
// the host's component is the truth and a client's is a copy.
// Identity is the ATV's own game Key (coop/interactables/atv_sync). The container act lane names a target by one u32,
// so a trunk's target id is container_target::kAtvTrunkBase plus 20 bits of a hash of the ATV's key; an ATV whose id
// another live ATV shares is not shared (said once). Reach is judged at the ATV itself (coop/props/container_reach).
// The host reads each trunk twice a second and sends it whole, in the container slice grammar (coop/props/
// container_slice_wire, eid = the target id), when its hash changed; a joiner gets every trunk at its connect replay. A
// client holds the newest blob per target until its ATV is live, then writes it over its component. A client's take,
// put and stack are acts the host judges (coop/props/container_act_request, container_act_host) and its hand put is
// refused here. Host to client only, never relayed (ReliableKind::AtvTrunk).

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::atv_trunk_sync {

// Cache the session and register the client's refusal. Idempotent; the per-tick ensure path. Game thread.
void Install(coop::net::Session* session);

// Per pump tick: the host publishes each changed trunk, a client applies the newest blob it holds for each. Game thread.
void Tick();

// Receiver entry (a client): one chunk of a trunk blob. Called from the reliable drain.
void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot);

// HOST: every trunk as it stands to one freshly connected client `peerSlot`, forced past the unchanged-hash guard.
void QueueConnectBroadcastForSlot(int peerSlot);

// Session teardown: the held blobs, the hashes and the assembler are dropped.
void OnDisconnect();

// The target id an ATV key maps to (container_target::kAtvTrunkBase | 20 bits of a hash of the key).
uint32_t EidOfKey(const std::wstring& key);

// The live ATV in this world whose key maps to `eid`; null when there is none or when two share the id. Game thread.
void* AtvOfEid(uint32_t eid);

// The target id of the trunk `inventory` is, or 0 when it is no shared ATV's component. Game thread.
uint32_t EidOfInventory(void* inventory);

// HOST: the trunk `eid` was written by a granted act; the next tick publishes it. Game thread.
void MarkHostWrite(uint32_t eid);

// CLIENT: this peer's copy of trunk `eid` was changed by a verb of its own; the host's next word for it applies whatever
// the last one was.
void ForgetApplied(uint32_t eid);

// HOST: trunk `eid` as it stands to one client whose view the host refused, forced past the unchanged-hash guard. With
// `bounded` at most one per kTruthEveryMs per slot and trunk goes (a refusal the sender can repeat); otherwise it is
// unthrottled (a send owed once). False when nothing was sent. Game thread.
inline constexpr uint64_t kTruthEveryMs = 500;
bool SendTruthToSlot(uint32_t eid, int peerSlot, bool bounded);

// Dev: the number of non-blank entries trunk `eid` holds, -1 when it does not read. Game thread.
int TrunkCount(uint32_t eid);

// Dev: call `fn` for each live ATV that has a shared trunk in this world (a readable component, a key that maps to a
// target id no other ATV shares), with its target id. Game thread.
void ForEachTrunk(void (*fn)(void* ctx, uint32_t eid, void* atv), void* ctx);

}  // namespace coop::atv_trunk_sync
