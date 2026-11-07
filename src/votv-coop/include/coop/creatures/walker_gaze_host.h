// coop/creatures/walker_gaze_host.h -- the host's walking mannequin freezes on its TARGET's gaze, and on no one else's.
//
// The walker's Blueprint freezes it (`looking`) while it is on screen: its tick reads the HOST's camera with the game's frustum
// test (bpCodeLib.isBoxInFrustum, once) and the mesh's WasRecentlyRendered(3) (twice), and keeps `saw` and its own timer_saw for
// the rest. While a CLIENT is the walker's target this module answers those reads, at the walker's own pinned calls, with the
// level that client's player sent (WalkerSeen), so the Blueprint's own `looking`, `saw` and timer_saw run on the target's gaze
// with no write of ours. A host target, or none, leaves the reads to the game.
// THE LEVEL: a plain latest value per (walker, slot), no budget and no queue; 0 (unseen) until the target's first word. It is
// dropped at the walker's end, at a change of target (every level of the walker), and at the slot's leave. Refused, counted and
// said once each: a sender that is not the current target, an unknown key, a puppet beyond kWalkerGazeReachCm, a flag not 0/1.
// THE HOOKS: both natives are called by every Blueprint, so each hook's first act is one compare of the calling frame's function
// with the walker's graph; they are armed only while a held walker has a client target and its graph is pinned (the target
// module's subscriptions), and name the walker's calls by the offsets they end at (coop/creatures/walker_graph). Host only,
// game thread inside the engine's call. Coop layer.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct WalkerSeenPayload;
}  // namespace coop::net

namespace coop::walker_gaze_host {

// The farthest a target's player may be from its walker for its word to count: the Blueprint's own 20000 uu hunt range and a
// margin.
inline constexpr float kWalkerGazeReachCm = 30000.f;

// The host's subscriptions to the target module, once for the process, from the session's first tick. A client pays nothing.
// Game thread.
void Install(coop::net::Session& session);

// A client's word (WalkerSeen) from `senderSlot`, judged here: refused unless it names a walker whose current target that slot
// is, from within reach, with a flag of 0 or 1; a word that repeats the level held is ignored at one compare. Game thread.
void OnSeen(const coop::net::WalkerSeenPayload& p, uint8_t senderSlot);

// A peer left: its level is dropped from every walker. Game thread.
void OnPeerLeft(uint8_t slot);

// The session ended: every level is dropped, the hooks are disarmed and the refusals are said. Game thread.
void OnDisconnect();

}  // namespace coop::walker_gaze_host
