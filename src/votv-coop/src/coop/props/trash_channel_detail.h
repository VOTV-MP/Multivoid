// coop/props/trash_channel_detail.h -- INTERNAL seam between trash_channel.cpp
// (the ctx/carry-latch/land-settle/birth-certificate core) and
// trash_grab_intent.cpp (the client-initiated grab/throw intent lane).
//
// Sibling-internal header (the prop_element_tracker_detail.h precedent) -- NOT
// part of the public coop/ include surface; only the two TUs above include it.
//
// The HELD_BY registry (eid -> holder peer slot) is
// OWNED by trash_grab_intent.cpp; the core never touches the map directly --
// TickCarry / the birth prune / the takeover / OnDisconnect go through these ops.
// All game-thread-only, like every trash_channel entry point.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::trash_channel {

// Is `eid` currently held by ANY peer's puppet (a client-initiated grab)?
bool HeldByAny(uint32_t eid);

// Drop `eid`'s HELD_BY record if present (land commit / dead-close / rest-close
// end the hold; the eid becomes re-grabbable). Idempotent.
void ClearHeldBy(uint32_t eid);

// The host's hand or a broom took `eid` out of a client's carry: drop the hold and tell that
// holder, alone, that its carry is over. A no-op when no client holds `eid`.
void EndHoldTakenOver(coop::net::Session& s, uint32_t eid);

// Gross reset of the intent lane (net disconnect): HELD_BY + the client-side
// pending-grab / carry toggles.
void ResetIntentState();

}  // namespace coop::trash_channel
