// coop/items/hook_anchor.h -- the handoff at the anchor, the one moment a hook changes hands.
//
// The rest of the lane is `coop/items/hook_sync`; this file is the arbiter half, and the reason it
// is its own file is that it is a different job. `hook_sync` streams a hook its owner is holding;
// this decides, once, that a hook has stopped being anyone's and become the host's.
//
// THE TRANSACTION, which is MTA's syncer handover -- its start packet carries a full state seed so
// the new owner, in MTA's own words, starts off synced with the server:
//
//   owner  -- HookAnchorCommit -->  host      the hook's own getData record, chunked
//   host   -- arbitrates -------->            splice, resolve, authorize, clamp
//   host   -- HookAnchored ----->  everyone   one atomic statement: retire the mirror, build this
//
// Game thread.

#pragma once

#include <cstdint>

#include "ue_wrap/actors/hook.h"

namespace coop::net {
class Session;
struct BlobChunkPayload;
}  // namespace coop::net

namespace coop::hook_anchor {

// WHAT THE ARBITER TAKES FROM THE SENDER, AND WHAT IT DOES NOT. Aactor_save_C's loadData writes the
// actor's save key, and hook_C's writes both attach keys, both component names and the cable
// length, which processKeys then turns into two real actors and a physics constraint between them.
// Handed a record whole, a client could name the host's new save-participating actor and pick two
// arbitrary keyed objects to tie together. So the host BUILDS the record it applies: it supplies
// the class, the save key and the base transform; it takes the anchor geometry from the sender,
// bounds-checked; it clamps the cable length to the hook's own maxDist, the clamp the game itself
// applies; and it reach-checks every attach key that resolves to something it can see.
// `ue_wrap/actors/save_record` states the same rule for props and splices for the same reason --
// this is that rule applied to a lineage its codec cannot reach.

void Install(coop::net::Session* session);

// OWNER: this hook has both ends anchored and the game has let go of it. Capture its record and
// send the commit. False if the capture or the send failed, in which case the caller keeps
// streaming and tries again on the next poll.
bool SendCommit(uint16_t seq, ue_wrap::hook::Kind kind, void* hookActor,
                const ue_wrap::hook::State& st);

// HOST: a commit arrived.
//
// A key that resolves to nothing is passed through unchanged rather than refused: our key index
// covers the props WE track, not everything the game's registry holds, so refusing an unresolvable
// key would break a legitimate anchor onto something we simply do not index. An unresolvable key
// leaves the hook tied to nothing, the game's own no-anchor case, and is harmless. An out-of-reach
// anchor is LOGGED and applied -- bounds are client-scoped and a discontinuity costs trust, never
// display.
void OnCommitChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// EVERYONE: the host has taken one. The original owner destroys its real hook here and takes a
// mirror; every other peer swaps its owner-phase mirror for the anchored one; a joiner whose save
// already carries this hook does nothing.
void OnAnchoredChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: death-watch the hooks this host has adopted. When one dies -- the player retrieved it, a
// lifespan, the anchor prop was destroyed -- announce it on the SAME (ownerSlot, seq) the handoff
// preserved. Without this the anchored half has no destroy channel and every peer keeps a copy of a
// hook the host retired.
void TickHost(uint64_t nowMs);

// HOST: replay every adopted hook to a joiner at its world-ready edge.
//
// THE KEY THE HOST MINTS is what the joiner dedups against. It boots anchored hooks out of the
// host's save, and it is the LOAD path that registers a key in the game's own key-to-actor map;
// setKey alone does not. So the joiner skips the ones whose key the game already resolves --
// exactly the set the save gave it. Without this replay, a hook anchored in the tens of seconds
// between the joiner's save capture and its world-ready is in nobody's save and is dropped by the
// pre-world gate: permanently invisible to that peer.
void QueueConnectBroadcastForSlot(int peerSlot);

void OnDisconnect();

}  // namespace coop::hook_anchor
