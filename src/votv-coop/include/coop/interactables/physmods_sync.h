// coop/physmods_sync.h -- the desk PHYSICAL-MODULES array sync (PhysModsState).
//
// Value ops over a host-canonical array. The array is a SET -- the native dup-check makes every
// module byte unique -- so an op needs no slot. DETECT is a 1 Hz 12-byte poll on every peer,
// outcome-based, since the plug, unplug, deny and explosion branches all converge through the
// array; it is primed on the first connected poll, and draining before adopting closes the
// eaten-edge race. The OPS are plug{byte} and unplug{byte}, peer to host and host-terminal,
// derived from the local diff: the HOST applies to ITS array, runs updPhysMods, then broadcasts
// the FULL canonical array, and every peer -- the presser included -- adopts wholesale, primes
// and re-runs updPhysMods, which is idempotent because the verb is a pure function of the array.
// A DENY goes back to the no-op author: a duplicate PLUG is refunded by a host respawn at the
// desk, so the item is never lost, and a no-op UNPLUG destroys the author's hand ghost while the
// host reaps the denied byte's fresh birth on a TTL. The module props need almost no lane code,
// riding the destroy seam and the birth watchers. JOIN: the save transfer seeds the array and
// the host ships the canonical array in connect-replay, which parks a still-pre-desk joiner.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::physmods_sync {

void Install(coop::net::Session* session);

// Per-net-pump: resolve backoff, the 1 Hz diff poll (ops out), pending
// canonical apply.
void Tick();

// PhysModsState from the wire (router: event_dispatch_signal.cpp).
void OnPhysMods(const coop::net::PhysModsStatePayload& p, uint8_t senderSlot);

// HOST: ship the canonical array to a joiner (connect replay).
void QueueConnectBroadcastForSlot(int slot);

// HOST consult from the kind-104 birth author: reap a module birth that
// matches a fresh unplug-deny for this sender (the r8 ghost). True = refuse.
bool HostShouldReapModuleBirth(uint8_t senderSlot, void* moduleClass);

// Full state reset. Wired into the subsystems teardown fanout.
void OnDisconnect();

}  // namespace coop::physmods_sync
