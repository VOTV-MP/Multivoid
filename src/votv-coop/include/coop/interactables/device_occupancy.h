// coop/device_occupancy.h -- v63 enterable-device OCCUPANCY (base computers /
// terminals phase 1).
//
// USER DESIGN (2026-06-11, binding): devices you "enter" (E -> camera zooms
// to the RT screen) are limited to ONE peer at a time via a "busy" state; a
// second peer pressing E gets DENIED + the game's existing save-denied fail
// sound. (Screen-STATE mirroring -- desk blob, dish aim, sky-signal
// host-roller, email deltas -- is phase 2; see the RE doc §4-5.)
//
// RE (votv-base-computers-RE-2026-06-11.md): the census is CLOSED at 8
// enterable devices; every enter/exit dispatch is EX_LocalVirtualFunction =
// ProcessEvent-INVISIBLE, so:
//   DETECT  -- poll mainPlayer.activeInterface (the discriminator field) for
//              rising/falling edges each pump tick (1 ptr read; classify only
//              on an edge).
//   ARBITRATE -- host-authoritative first-wins claim table (MTA shape:
//              server-arbitrated entity locks -- vehicle-entry occupancy,
//              first-claim-wins + relay; reference/mtasa-blue CVehicle
//              occupant slots). A client claims optimistically (stays in;
//              collisions only happen in a sub-100 ms race) and force-exits
//              if the host replies that another slot holds the device.
//   DENY    -- PRE-observe InpActEvt_use (the ONE PE-visible seam on the
//              enter path); if the aim resolves to a wire-busy device, null
//              lookAtActor + HitResult.Actor for that single dispatch (the
//              door HostAuth Active-gate precedent -- the native chain no-ops
//              on its own icast guards), restore in POST, play
//              button_keypad_deny (coop::prop_sound::PlayDenyClick).
//   FORCE-EXIT -- reflected setActiveInterface(null): the game's own forced-
//              exit path (ragdollMode rides it), restoring input/FOV/movement.
//   RELEASE -- the activeInterface falling edge covers EVERY exit cause (ESC,
//              ragdoll, death -- ragdollMode also exits via the same field);
//              the host clears a leaver's claims on its disconnect edge.
//
// Claim keys are SHARED-WIDGET identities ("desk"/"sat"/"radar"/"reactor"/
// "laptop" + per-instance "tfm_<posKey>"/"arc_<posKey>") -- five device
// families literally render ONE shared widget instance each, so per-widget
// claims are correctness, not simplification (two peers in "different"
// laptops would type into the same screen). See ue_wrap/device_screen.
//
// Game thread throughout (poll from the net-pump tick, OnReliable from the
// event_feed drain, observers from the ProcessEvent detour).

#pragma once

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::device_occupancy {

// Store the session + install the InpActEvt_use PRE/POST deny observers
// (idempotent; retried until mainPlayer_C loads). Both roles -- the deny
// gate runs on host AND clients.
void Install(coop::net::Session* session);

// Per-tick: resolve offsets/classes (throttled), poll the local player's
// activeInterface for claim edges, retry a pending claim send. Cheap when
// idle (one Local() + one pointer read).
void Tick();

// Wire ingest (both roles). HOST: arbitrate a claim/release request and
// broadcast the verdict. CLIENT: mirror the busy table; force-exit + deny
// sound if the broadcast says another slot holds the device we are inside.
void OnReliable(const coop::net::DeviceClaimPayload& p, uint8_t senderSlot);

// True iff the LOCAL peer currently holds the claim `key`. The v64 state
// channels gate their owner-streams on holding "desk". Game thread.
bool LocalHolds(const wchar_t* key);

// The slot currently holding `key`, or 0xFF if unclaimed. On the host this
// is authoritative; on a client it is the broadcast mirror. Game thread.
uint8_t HolderOf(const wchar_t* key);

// HOST: send the current claim table to a joining peer (world-ready replay).
void QueueConnectBroadcastForSlot(int peerSlot);

// Per-slot disconnect: drop every claim held by `slot` (HOST also broadcasts
// the releases so nobody stays locked out by a leaver).
void OnDisconnectForSlot(int slot);

// Aggregate teardown: clear the table, the local claim, and any pending send.
void OnDisconnect();

}  // namespace coop::device_occupancy
