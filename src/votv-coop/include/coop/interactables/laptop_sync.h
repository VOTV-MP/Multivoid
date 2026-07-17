// coop/interactables/laptop_sync.h -- v116: the stationary PC (Alaptop_C)
// power + floppy sync lane.
//
// RE base: research/findings/computers-devices/votv-laptop-pc-RE-2026-07-17.md.
// Design: qf rounds 7-9 (scratchpad thread) -- presser-authored edges, ONE
// destroy owner (the pre-existing v106 K2_DestroyActor seam), HOST content
// authority, joiner ground-truth rows.
//
// Axes (v1):
//   POWER  -- isOpened edge (4 Hz poll; every entry verb is EX-invisible).
//             Receivers replay the native actionOptionIndex(b8) under the
//             wire-apply echo guard when local != wire; a receiver whose
//             powered/anim gate declines retries at the next poll until
//             convergence (power_sync converges the wall-power input).
//   FLOPPY -- floppyType change edges (insert/eject) + slot scalars + the
//             content strings (chunked op=4). The disc PROP lifecycle is NOT
//             this lane's: the insert destroy already crosses via the generic
//             destroy seam; the eject spawn crosses via the birth channels.
//   DISC CONTENT -- op=4 kind=1 {eid}: the HOST loadDatas its authoritative
//             disc actor and re-fans; receivers write their mirror when it
//             materializes (deferred retry). A client-ejected disc's content
//             arrives client->host after the adoption eid-binding.
//
// OUT of v1 (TRACKER row): the PC buffer (floppyBuffer/UIDs) + the portable
// PC device (prop_portablePc_C; shares the 'laptop' claim key -- the
// per-device discrimination is that row's first design question).
//
// Game thread throughout (net-pump tick + reliable dispatch).

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::laptop_sync {

void Install(coop::net::Session* session);

// 4 Hz: resolve + instance-generation check, the power/slot edge polls, the
// client pending-eject-content drain, the deferred disc-content retries.
void Tick();

// Wire ingest (both roles). HOST: applies + re-fans (except origin); a
// client-authored op=4 kind=1 lands on the host's authoritative disc first.
void OnLaptopState(const coop::net::LaptopStatePayload& p, uint8_t senderSlot);

// HOST: ship the joiner the full laptop state (op=3 + op=4 kind=0 chunks) +
// one op=4 kind=1 row per live content-bearing disc (ground-truth read).
void QueueConnectBroadcastForSlot(int peerSlot);

void OnDisconnect();

}  // namespace coop::laptop_sync
