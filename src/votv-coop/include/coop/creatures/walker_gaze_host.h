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

// WHERE THE LEVEL IS JUDGED. Source and MTA judge a player's view on the server: Source asks the player entity whether a point is
// in its view cone (reference/source-sdk-2013/src/game/server/ai_playerally.cpp:426-433), and MTA carries the camera rotation to
// the server in the player's puresync (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CPlayerPuresyncPacket.cpp:202-204).
// Here the walker's test is a frustum test plus WasRecentlyRendered, and rendering is per viewer: the host cannot render a
// client's view, so the target's own level is taken and trusted as a pose is. A client that sends a false level freezes or
// releases only the walker that hunts it, and the checks named above (target, key, reach, flag) bound the rest.
// MTA also refuses a word from an earlier tenure of the same syncer, by a sync time context regenerated at each start of the sync
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CPedSync.cpp:243, CElement.cpp:1281-1304). WalkerSeen carries no such
// field: a word from a client's earlier turn as target that lands after the host chose it again is taken as the new turn's
// level, until the first judgement a new row always sends (walker_gaze, told -1) arrives behind it on the ordered stream.
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

// [dev], for the walker drill: how many of the walker's gaze reads this session were answered with a client target's level (the
// host's own camera and render state were overwritten) rather than left to the game. Any thread.
uint32_t AnsweredReads();

// A peer left: its level is dropped from every walker. Game thread.
void OnPeerLeft(uint8_t slot);

// The session ended: every level is dropped, the hooks are disarmed and the refusals are said. Game thread.
void OnDisconnect();

}  // namespace coop::walker_gaze_host
