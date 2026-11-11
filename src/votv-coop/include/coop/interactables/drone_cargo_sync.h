// coop/interactables/drone_cargo_sync.h -- the delivery drone's CARGO, the host's, mirrored on every client.
// Gameplay/network layer (principle 7): the wire, the host's publication and the client's apply; the engine is
// reached through ue_wrap::drone.
//
// The cargo is the drone's own inventoryComponent_C (drone_C.inventoryComponent). The order fills it on the host
// when the drone lands (compileOrder), and the drone's tick is suppressed on a client, so a client's copy of the
// component stays what the join gave it unless the host says otherwise. The inventory screen a player opens on
// the drone (the open option's ui_FLI_inventory.begin) reads that component, so the client's copy has to hold the
// host's entries for the cargo to be seen and opened there.
//
// The host reads the entries twice a second and sends them whole, in the container slice grammar
// (coop/props/container_slice_wire, eid 0), when their hash changed; a joiner gets the current entries at its
// world-ready edge, empty included. A client writes them over its component and re-derives the volumes through the
// game's own verb. Every verb that writes the drone's component other than the host's tick is the host's to run: a
// client's drop-sack press goes to the host as an intent (coop/interactables/drone_call_intent), and the client's
// putSackOn (a sack touching the drone) and playerUsedOn (a hand item put into the drone) are refused here, since
// each would write a copy the host never sees.
//
// A player's take from the cargo, put into it and merge of its stacks on a client are acts the host judges
// (coop/props/container_act_request names them, coop/props/container_act_host judges them with the cargo as the target
// coop/props/container_target::kDroneCargoEid); the host's write of a granted act is published at once through
// MarkHostWrite, a refused client's mirror is put back by SendTruthToSlot, and a client that changed its mirror with a
// verb of its own forgets the hash it last applied (ForgetApplied), so the host's next word applies even when it equals
// the last.
//
// Host to client only and never relayed (ReliableKind::DroneCargo). The pacing and the readiness of the pen are
// coop/net/blob_chunks'.

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

// Dev: the hash a peer holds of the drone's cargo as it reads now, 0 when it does not read or the drone is absent.
// Both peers print it; the drills compare. Game thread.
uint64_t CargoHash();

// Dev: the number of non-blank entries the drone's cargo holds, -1 when it does not read. Game thread.
int CargoCount();

}  // namespace coop::drone_cargo_sync
