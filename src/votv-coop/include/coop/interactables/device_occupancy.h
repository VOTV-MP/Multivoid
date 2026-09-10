// coop/interactables/device_occupancy.h -- OCCUPANCY for the enterable devices, the base computers
// and terminals.
//
// A device you "enter" -- press E and the camera zooms to its RT screen -- is limited to ONE peer
// at a time by a busy state; a second peer pressing E is DENIED and hears the game's own
// save-denied fail sound. Mirroring what is ON those screens is a separate concern, owned by the
// state lanes.
//
// Claim keys are SHARED-WIDGET identities: "desk", "sat", "radar", "reactor", "laptop", plus a
// per-instance "tfm_<posKey>" and "arc_<posKey>". Five device families literally render ONE shared
// widget instance each, so a per-widget claim is correctness rather than simplification -- two
// peers in "different" laptops would be typing into the same screen. See ue_wrap/device_screen.
//
// Game thread throughout: the poll from the net-pump tick, OnReliable from the event_feed drain,
// the observers from the ProcessEvent detour.

#pragma once

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::device_occupancy {

// Store the session + install the InpActEvt_use PRE/POST deny observers
// (idempotent; retried until mainPlayer_C loads). Both roles -- the deny
// gate runs on host AND clients.
void Install(coop::net::Session* session);

// Per-tick: resolve the offsets and classes (throttled), poll the local player's activeInterface
// for claim edges, and retry a pending claim send. Cheap when idle: one Local() and one pointer
// read.
//
// DETECT is a poll because every enter and exit dispatch is an EX_LocalVirtualFunction, which
// ProcessEvent cannot see; the rising and falling edges of mainPlayer.activeInterface are read each
// pump tick and classified only on an edge. RELEASE rides the falling edge, which covers every exit
// cause -- ESC, ragdoll, death -- since ragdollMode leaves through the same field; the host also
// clears a leaver's claims on its disconnect edge.
void Tick();

// Wire ingest, both roles. HOST: arbitrate a claim or release request and broadcast the verdict,
// first-wins, in the MTA shape of a server-arbitrated entity lock (vehicle-entry occupancy,
// first-claim-wins plus relay; reference/mtasa-blue CVehicle occupant slots). CLIENT: mirror the
// busy table, and force-exit with the deny sound if the broadcast says another slot holds the
// device we are inside. A client claims optimistically and stays in, since a collision needs a
// sub-100 ms race.
//
// DENY is a PRE observer on InpActEvt_use, the one ProcessEvent-visible seam on the enter path:
// when the aim resolves to a wire-busy device it nulls lookAtActor and HitResult.Actor for that
// single dispatch, so the native chain no-ops on its own icast guards, restores them in POST, and
// plays button_keypad_deny. FORCE-EXIT is a reflected setActiveInterface(null, zoom=true, 3D = the
// player's live flag), the way ragdollMode -- the game's own forced exit -- calls it: held inputs
// cleared, GameOnly input and the cursor back, the default FOV restored, the outgoing widget left
// visible, exitInterface broadcast.
void OnReliable(const coop::net::DeviceClaimPayload& p, uint8_t senderSlot);

// True iff the LOCAL peer currently holds the claim `key`. The screen-state channels gate
// their owner-streams on holding "desk". Game thread.
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
