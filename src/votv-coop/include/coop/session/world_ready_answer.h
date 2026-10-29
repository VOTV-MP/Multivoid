// coop/session/world_ready_answer.h -- the host's answer to a client's ClientWorldReady announce.
//
// The first announce of an occupancy completes the join: it marks the slot world-ready and runs the
// connect replay. A later announce is capped by a per-slot IntentBucket with NO refill -- at most two
// re-announce replays per occupancy, then the slot is ended (MV-H34). An honest client re-announces only
// on a world change (net_pump::MaybeRequestReAnnounce), unobserved in 1825 archived sessions and at most
// once when it fires, so a cap of two is pure margin; a flood of re-announces at ANY pace -- the
// refill-matching pace a rate bound would miss included -- is ended. MTA disconnects a repeated
// initial-data-stream request (CRPCFunctions.cpp:110-124, InitialDataStream, IsJoined-guarded, banning
// nothing); our ClientWorldReady is that request's analogue (net_pump.cpp:453-457). The burst of two is
// the one divergence from MTA's kick-the-first-repeat, a margin for the re-announce MTA has no counterpart
// of. There is no deferred or owed replay: a capped miss is ended, not queued.
#pragma once

namespace coop::net { class Session; }

namespace coop::world_ready_answer {

// Host only, slots 1..kMaxPeers-1 (a no-op otherwise). The whole body of event_feed's ClientWorldReady
// case: the first announce completes the join, a later one runs under the replay bucket or ends the slot.
// Game thread, from the event_feed drain.
void OnAnnounce(coop::net::Session& session, int slot);

namespace dev {
// Default off; the throwjoin drill's. While a slot is held, its FIRST announce is deferred: the slot stays not
// world-ready (so every fan-out skips it) and its join gate stays held, the way container_act_host's HoldReplies keeps a
// grant back. Release runs the deferred answer (the world-ready mark, the connect replay) at once; a slot that was not
// caught is only unheld. Game thread.
void HoldSlot(int slot, bool on);
// True once the hold has caught the slot's announce.
bool Caught(int slot);
void Release(int slot);
// The drill's reset: every hold and catch dropped without answering.
void Forget();
}  // namespace dev

}  // namespace coop::world_ready_answer
