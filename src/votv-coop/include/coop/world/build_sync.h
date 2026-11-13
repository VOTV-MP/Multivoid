// coop/world/build_sync.h -- nails, custom walls and posters on a wall are the host's, and every peer has them.
//
// Each is a save actor its own record rebuilds (ue_wrap/actors/build_piece), so the lane carries records by save key:
//   - HOST: a poll sends each piece's record when it is new or changed (BuildState), and its key once when it goes. A
//     client's new piece (BuildIntent, its record) is checked -- a class this lane carries, in the world, near its sender,
//     a key no piece here has -- and made here under that key; a client's removal is checked against its reach and the
//     host's piece destroyed, dropping what the game drops.
//   - CLIENT: the host's piece is made on this copy, or the record applied to the one of that key, a nail tied to this
//     world's actors of its keys. A copy runs nothing of its own: a nail drops no item and its tie is held but while
//     this player holds an end, whose pull snaps go to the host to judge; an unfinished wall counts no bricks, wet
//     concrete does not dry, and a wall's health is scaled so no hit here breaks it; this player's hit or repair goes
//     to the host as it lands and shows here until the host answers. A piece this player made goes to the host, whose
//     state answers; one unanswered in 10 s was refused and goes. A nail, unfinished wall or poster this player took
//     away goes to the host as a removal; one lost here otherwise is made again from its record.
// Game thread.

#pragma once

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::build_sync {

void Install(coop::net::Session* session);

// The poll and the inboxes, on either role.
void Tick();

// A client's receiver of the host's pieces.
void OnStateChunk(const coop::net::BlobChunkPayload& p, int senderSlot);

// The host's receiver of a client's piece or removal.
void OnIntentChunk(const coop::net::BlobChunkPayload& p, int senderSlot);

// Every piece again, for a joiner.
void QueueConnectBroadcastForSlot(int slot);

void OnPeerLeftSlot(int slot);

// The wall hits and repairs this client has sent, for the drill.
unsigned WallChangesSent();

void OnDisconnect();

}  // namespace coop::build_sync
