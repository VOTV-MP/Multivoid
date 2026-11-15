// coop/interactables/drone_cargo_sync.h -- the delivery drone's CARGO, the host's, mirrored on every client.
// Gameplay/network layer (principle 7): the wire, the host's publication and the client's apply; the engine is
// reached through ue_wrap::drone.
//
// The cargo is the drone's own inventoryComponent_C (drone_C.inventoryComponent). The order fills it on the host when the
// drone lands (compileOrder) and the drone's tick is suppressed on a client, so a client's copy stays what the join gave
// it unless the host says otherwise; the inventory screen a player opens on the drone reads that component.
// The host reads the entries twice a second and sends them whole, in the container slice grammar (coop/props/
// container_slice_wire, eid 0), when their hash changed; a joiner gets the current entries at its world-ready edge. A
// client writes them over its component and re-derives the volumes through the game's own verb. Every verb that writes
// the component is the host's to run: a client's drop-sack press is an intent (coop/interactables/drone_call_intent), its
// take, put and stack are acts the host judges (coop/props/container_act_request, container_act_host, target
// container_target::kDroneCargoEid), and its putSackOn and playerUsedOn are refused here. A granted act is published at
// once through MarkHostWrite, a refused client's mirror is put back by SendTruthToSlot, and a verb of the client's own
// makes it forget the hash it last applied (ForgetApplied). Host to client only, never relayed (ReliableKind::DroneCargo).

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::drone_cargo_sync {

// Cache the session and register the client's refusals. Idempotent; the per-tick ensure path. Game thread.
void Install(coop::net::Session* session);

// Per pump tick: the host publishes a changed cargo, a client applies the newest one it holds. Game thread.
void Tick();

// Receiver entry (a client): one chunk of the host's cargo blob. Called from the reliable drain.
void OnChunk(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot);

// HOST: the cargo as it stands to one freshly connected client `peerSlot`, forced past the unchanged-hash guard.
void QueueConnectBroadcastForSlot(int peerSlot);

// Session teardown: the held blob, the hashes and the assembler are dropped.
void OnDisconnect();

// HOST: the cargo was written by a granted act; the next tick publishes it. Game thread.
void MarkHostWrite();

// CLIENT: this peer's copy was changed by a verb of its own; the host's next cargo applies whatever the last one was.
void ForgetApplied();

// HOST: the cargo as it stands to one client whose view the host refused, forced past the unchanged-hash guard. With
// `bounded` at most one per kTruthEveryMs per slot goes (a refusal the sender can repeat); otherwise it is unthrottled
// (a send owed once). False when nothing was sent. Game thread.
inline constexpr uint64_t kTruthEveryMs = 500;
bool SendTruthToSlot(int peerSlot, bool bounded);

}  // namespace coop::drone_cargo_sync
