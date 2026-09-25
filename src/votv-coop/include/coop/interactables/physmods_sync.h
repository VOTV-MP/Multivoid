// coop/interactables/physmods_sync.h -- the desk PHYSICAL-MODULES array sync (PhysModsState).
//
// Value ops over a host-canonical array. The array is a SET -- the native dup-check makes every
// module byte unique -- so an op needs no slot. DETECT is at the verbs: the desk's two bodies that
// edit the array from play, plugInModule and playerHitWith (its unplug branch), are watched at the
// script gate, and each sends what its body changed; the save load's setData is not an edit, so a
// joiner's own load sends nothing. The OPS are plug{byte} and unplug{byte}, peer to host and
// host-terminal: the HOST applies to ITS array, runs updPhysMods, then broadcasts the FULL
// canonical array, and every peer -- the presser included -- adopts wholesale and re-runs
// updPhysMods, which is idempotent because the verb is a pure function of the array.
// A DENY goes back to the no-op author: a duplicate PLUG is refunded by a host respawn at the
// desk, and a no-op UNPLUG destroys the author's hand ghost, or sweeps its untracked actors of
// that byte when the module was already dropped. The module props need almost no lane code,
// riding the destroy seam and the birth watchers. JOIN: the save transfer seeds the array and the
// host ships the canonical array in connect-replay, which parks a still-pre-desk joiner.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::physmods_sync {

void Install(coop::net::Session* session);

// Per-net-pump: applies a canonical that arrived before the desk resolved, once it has.
void Tick();

// PhysModsState from the wire (router: event_dispatch_signal.cpp).
void OnPhysMods(const coop::net::PhysModsStatePayload& p, uint8_t senderSlot);

// HOST: ship the canonical array to a joiner (connect replay).
void QueueConnectBroadcastForSlot(int slot);

// HOST consult from the ReelEjectIntent birth author: reap a module birth that matches a
// fresh unplug-deny for this sender, which makes it the ghost of a drop that raced the
// deny. True = refuse.
bool HostShouldReapModuleBirth(uint8_t senderSlot, void* moduleClass);

// Full state reset. Wired into the subsystems teardown fanout.
void OnDisconnect();

}  // namespace coop::physmods_sync
