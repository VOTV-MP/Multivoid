// coop/creatures/kerfur_convert_client.h -- the client half of the kerfur conversion:
// conversion-ghost custody (claim and park, cleanup reap, take-by-eid adopt) and the
// KerfurConvert wire apply (mirror destroy, adopt, materialise, restore). The detection (the
// death-watch poll and the first-refusal seams) and the host executor live in kerfur_convert
// and kerfur_convert_host; kerfur_convert.h carries the feature narrative. Gameplay layer:
// engine access through ue_wrap only.

#pragma once

#include "coop/element/element.h"

#include <cstdint>

namespace coop::net {
class Session;
struct KerfurConvertBroadcastPayload;
}  // namespace coop::net

namespace coop::kerfur_convert_client {

// The session pointer for the wire-apply role gate; the kerfur_convert install calls it on
// every attempt, so a reconnect that changes the session stays fresh here.
void SetSession(coop::net::Session* session);

// The resolved kerfur class pointers (the NPC base, the prop base, the floppy), pushed by the
// install on every attempt as soon as they resolve.
void SetClasses(void* npcClass, void* propClass, void* floppyClass);

// Find and park the client's own just-spawned conversion ghosts near the site; the poll's
// client branch drives it on a detected local toggle. The custody narrative is in the .cpp.
void ClaimConversionGhosts(uint32_t srcEid, bool wantNpc, float x, float y, float z);

// Reap parked ghosts: adopted and dead ones dropped, unconfirmed orphans destroyed after the
// timeout. Driven at the poll cadence; a cheap no-op when empty.
void CleanupParkedGhosts();

// Take (find and remove) the parked ghost tagged with `srcEid` of the requested form (the
// parked turn-on NPC, or the turn-off prop), returning its actor or null. The client's own
// toggle spawns a local kerfur through the EX_CallMath path ProcessEvent cannot see; the poll
// parks it tagged with the converting eid, and the EntitySpawn receiver (turn-on) and
// OnKerfurConvert adopt that exact actor by eid. Null for a peer that did not initiate, which
// fresh-spawns. Game thread.
void* TakeParkedGhostByEid(uint32_t srcEid, bool wantNpc);

// The client-only receiver for KerfurConvert, host to all. The sole conversion-transition
// signal: destroy the old-form mirror at oldEid, then adopt this
// peer's own claimed ghost to the authoritative newEid (the initiator) or materialise a fresh
// mirror (other peers). A rejected payload means the host refused: keep the mirror, or restore
// it if we converted optimistically. `localPlayer` may be null; it feeds the prop teardown and
// materialise (the held-prop guard and the handle release). Game thread.
void OnKerfurConvert(const coop::net::KerfurConvertBroadcastPayload& payload, void* localPlayer);

// Clear the parked-ghost list; fanned from the kerfur_convert disconnect.
void OnDisconnect();

}  // namespace coop::kerfur_convert_client
