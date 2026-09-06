// coop/trash_collect_sync.h -- mirror a freshly-spawned-and-grabbed item (the VOTV trash-pile
// collect).
//
// Pressing E on a trashBitsPile_C ("trash stack") spawns one Aprop_C trash item and auto-grabs
// it into the player's hands the same frame. The item is born with Key == None, because its BP
// UCS has not minted a NewGuid yet, so the held-prop pose stream would emit unmatchable
// PropPose key='None' and the Init POST broadcast would skip it -- the held item never mirrors.
//
// The collect CANNOT be caught with a UFunction observer: trashBitsPile_C::playerTryToCollect is
// dispatched BP->BP through ProcessInternal, which bypasses our ProcessEvent detour entirely
// (measured: a POST observer installed and NEVER fired while the item was clearly held). So
// detection happens where the engine state IS visible -- net_pump's held-prop send, which
// already resolves grabbing_actor every tick. From the mint onward the existing pose stream
// carries the item into the collector's hands, and PropRelease/PropDestroy unwind it like any
// held prop.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::trash_collect_sync {

// Install the pile-grab observer (a PRE observer on AmainPlayer_C::InpActEvt_use). Caches
// `session` (re-cached every call for reconnect) and registers the observer once mainPlayer_C is
// loaded (idempotent; retries on later calls until the class resolves). Call from the
// world-gated subsystem install. This PRE observer is the HOST-GRAB seam: it records the aimed
// pile's eid as diagnostics, the binding itself riding the spawn thunk's birth certificate, and
// the held-object edge adopts the spawned clump onto that eid. (The clump's BeginDeferred spawn
// is EX_CallMath, so it is invisible to a spawn-POST hook.) Game thread.
void Install(coop::net::Session* session);

// Game thread. If `heldActor` is a live, UNKEYED (Key=None) Aprop_C, force-mint a stable Key on
// it and broadcast a PropSpawn under that Key, so peers spawn a mirror the held-pose stream can
// then drive into the collector's hands. Returns true iff it minted and broadcast. A no-op
// (false) for null/dead actors, for already-keyed actors (an ordinary world-prop grab the peer
// already has), for the host-authoritative garbageClump (trash_channel owns it through the
// grabbed pile's eid and it is never authored here), and for any other non-Aprop_C. That last
// one is CRASH SAFETY: a transient chip/clump is broadcast but never physics-driven on the
// receiver -- GetStaticMesh returns null for a non-Aprop_C, so the mirror spawns physics-free
// and is driven KINEMATICALLY by per-tick SetActorLocation. Resolving their real mesh and
// physics-driving a self-morphing actor is a use-after-free. Idempotent: once minted the Key is
// non-None, so a repeat call returns false.
bool EnsureHeldItemBroadcast(void* heldActor, coop::net::Session* session);

// Drop the cached session (full session teardown / aggregate disconnect). The trash entity ctx
// map is cleared separately by trash_channel::OnDisconnect.
void OnDisconnect();

// TEST-ONLY (VOTVCOOP_RUN_GRAB_INTENT_TEST): send a GrabIntent{eid} from THIS client to the
// host, using the cached client session. Exercises the full client->host wire, the host's
// OnGrabIntent and the puppet hand-drive without the client suppress-native / collision path.
// No-op if the session is not a running client. Returns true iff sent. Game thread.
bool DebugSendGrabIntent(uint32_t eid);

// TEST-ONLY: send a ThrowIntent{eid} from THIS client -- the throw fallback for when the real
// InpActEvt_use toggle cannot be driven. No-op if not a running client. Returns true iff sent.
// Game thread.
bool DebugSendThrowIntent(uint32_t eid);

}  // namespace coop::trash_collect_sync
