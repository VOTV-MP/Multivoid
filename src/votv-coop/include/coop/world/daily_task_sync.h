// coop/world/daily_task_sync.h -- the L7 daily-task mirror (saveSlot.taskNew).
// HOST-authored: every live taskNew writer is host-only (createNewTask -- the client
// daynightCycle is frozen at TimeScale=0; lib.processTask -- reachable only via
// setTaskNew + sell; droneSellLocation.sell -- the client drone tick is suppressed;
// GUID census across all dumped assets). Wire: ReliableKind::TaskNewState=103
// (host ~1 Hz change-hash poll; fires a few times per game-day). Rewards themselves
// ride the existing balance_sync (points) + email_sync (mail) lanes.
//
// Design of record: research/findings/computers-devices/votv-tape-caddy-L7-impl-DESIGN-2026-07-17.md (D3).

#pragma once

#include <cstdint>

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::daily_task_sync {

// Store the session + log the install latch. Idempotent. Game thread.
void Install(coop::net::Session* session);

// Per-net-pump tick: HOST-only ~1 Hz change-hash poll -> broadcast on change.
// A client tick is a no-op. Game thread.
void Tick();

// Wire handler (CLIENT): one GT task applying scalars + the three int32 arrays
// (in-place or engine-realloc rebuild). Trust-gated to the host sender.
void OnTaskNewState(const coop::net::TaskNewStatePayload& p, uint8_t senderSlot);

// Session teardown: reset the hash baseline + timers. Game thread.
void OnDisconnect();

}  // namespace coop::daily_task_sync
