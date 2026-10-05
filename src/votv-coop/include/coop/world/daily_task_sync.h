// coop/world/daily_task_sync.h -- the L7 daily-task mirror (saveSlot.taskNew).
// HOST-authored: every live taskNew writer is host-only (createNewTask -- part of the midnight
// the client's parked clock never reaches; lib.processTask -- reachable only via
// setTaskNew + sell; droneSellLocation.sell -- the client drone tick is suppressed;
// GUID census across all dumped assets). Wire: ReliableKind::TaskNewState=103
// (host ~1 Hz change-hash poll; fires a few times per game-day), plus the whole task to a
// joiner at its world-ready: a change made while it loaded was hashed but never reached it.
// Rewards themselves ride the existing balance_sync (points) + email_sync (mail) lanes.

#pragma once

#include <cstdint>

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::daily_task_sync {

// Store the session + log the install latch. Idempotent. Game thread.
void Install(coop::net::Session* session);

// Per-net-pump tick: HOST ~1 Hz change-hash poll -> broadcast on change; CLIENT 1 Hz
// retry of a received task that did not apply. Game thread.
void Tick();

// HOST: the current task to one joiner, whatever the change hash holds (ConnectReplayForSlot,
// at its world-ready). Game thread.
void SendCurrentToSlot(int slot);

// Wire handler (CLIENT): one GT task applying scalars + the three int32 arrays
// (in-place or engine-realloc rebuild). A task that does not apply whole is kept and retried
// by Tick until it does; a newer one replaces it. Trust-gated to the host sender.
void OnTaskNewState(const coop::net::TaskNewStatePayload& p, uint8_t senderSlot);

// Session teardown: reset the hash baseline + timers. Game thread.
void OnDisconnect();

}  // namespace coop::daily_task_sync
