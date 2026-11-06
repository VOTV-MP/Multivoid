// coop/world/ground_hose_sync.h -- a laid garden hose is the host's, and every peer has it, tied to the same faucet and
// sprinkler.
//
// A hose is laid on one peer only: it follows that peer's own player until the coil finishes it (ue_wrap/actors/
// ground_hose). So the lane carries finished hoses, as the hose's own save record, and the host's copy is the one:
//   - HOST: a poll lists the finished hoses and sends each one's record when it is new or changed (GroundHoseState), and
//     its key once when it goes. A client's finished hose (GroundHoseIntent, a route) is checked -- a faucet of this
//     world, its points in the world and not too many, its sender standing by its end -- and laid here under the client's
//     key, or onto the host's hose of that key; a client's removal is checked against its reach and the hose destroyed.
//   - CLIENT: the host's hose is laid on this copy, made if missing, and tied to this world's faucet and sprinkler of the
//     record's keys; the faucet's water, already shared, then runs it here too. A hose this player laid, re-laid, cut from
//     its sprinkler or took up goes to the host, whose state answers. A hose being laid here is left to its player.
// The faucets themselves are coop/interactables/interactable_sync's. Adapted from Watchers of the Void's garden hose
// lane (ElvardFIC/WatchersOfTheVoid, MIT); see docs/credits.md. Game thread.

#pragma once

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::ground_hose_sync {

void Install(coop::net::Session* session);

// The poll and the inboxes, on either role.
void Tick();

// A client's receiver of the host's hoses.
void OnStateChunk(const coop::net::BlobChunkPayload& p, int senderSlot);

// The host's receiver of a client's hose.
void OnIntentChunk(const coop::net::BlobChunkPayload& p, int senderSlot);

// Every hose again, for a joiner.
void QueueConnectBroadcastForSlot(int slot);

void OnPeerLeftSlot(int slot);

void OnDisconnect();

}  // namespace coop::ground_hose_sync
