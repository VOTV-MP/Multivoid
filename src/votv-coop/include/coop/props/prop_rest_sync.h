// coop/props/prop_rest_sync.h -- a client's resting copy of a keyed prop put where the host's rests.
//
// A copy moved by a stream ends where the host's did, the End carrying the pose. Some copies move with none: a joiner's
// world settles as it loads (110 props of 1100 near the base over 5 cm off), a thin prop an End teleports onto a table top
// is pushed through it, a client's own body moves its copy alone. The host is the truth of where a prop rests:
//   * the HOST answers PropRestQuery with PropRestPose rows, its pose of each asked prop it holds at rest, in no hand and
//     under no row (a moving one's End is its answer); 30 s after a joiner's world is ready it sends that joiner every
//     keyed prop at rest the same way, a rest pass paced at kRestPassRowsPerSec, once per world.
//   * a CLIENT asks for a prop once its copy, moved with no stream on it, has rested a second (NoteLocalMove: body_push's
//     watch round its own body, and WatchAfterEnd), and puts a resting copy in no hand or stream that is over 2 cm or 3
//     degrees from a row where the row says, with the host's static, frozen and sleep, asleep. A prop corrected three
//     times in a minute is left alone, said once.
// Rows are by host element id. Reliable, to one client. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PropRestQueryPayload;
struct PropRestPosePayload;
}  // namespace coop::net

namespace coop::prop_rest_sync {

// The joiner's rest pass rows, per second, per slot.
inline constexpr int kRestPassRowsPerSec = 150;

// CLIENT: this client's copy of `actor` moved with no stream on it. Asked for once it rests. Game thread.
void NoteLocalMove(void* actor);

// CLIENT: a driven row's End just put `actor` at the host's pose. For kWatchAfterEndMs the copy is watched; a move of
// its own over 2 cm or 3 degrees in that time (a thin prop the End's teleport pushed through a table top, a stack that
// gave way) is a NoteLocalMove. Game thread.
inline constexpr int kWatchAfterEndMs = 6000;
void WatchAfterEnd(void* actor);

// HOST: slot `slot`'s world is up (the connect replay): its rest pass is armed for 30 s on. Game thread.
void OnPeerWorldReady(int slot);

// HOST: a client asks for the host's rest pose of props by eid. Game thread.
void OnQuery(int slot, const coop::net::PropRestQueryPayload& p);

// CLIENT: the host's rest poses. Game thread.
void OnPose(const coop::net::PropRestPosePayload& p);

// Per game tick, both roles: the host drains its answers and rest passes, a client asks for its rested props. Game thread.
void Tick(coop::net::Session& s, bool isHost);

// Session end. Game thread.
void OnDisconnect();

}  // namespace coop::prop_rest_sync
