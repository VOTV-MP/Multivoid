// coop/session/local_grants.h -- which local dev features this machine may use: the answer the
// feature gates read, from any thread. In a running session it is the host's grant for this
// machine (the host computes its own, a client is sent its own: coop/session/grants_sync); solo
// and the menu keep today's answer, true. One packed word holds the session serial and the bits, so
// a reader never pairs one session's bits with another's; a word written for another serial reads
// as the role's default (coop/permissions/grants_core.h).
// The state is stamped by the session serial, not ended at a session-end seam, a divergence from
// both precedents: readers on several threads cannot take a clear mid-read, and a word of another
// session reads as the role default. Nearest shape: MTA's sync time context, a stamp the receiver
// compares instead of a reset (reference/mtasa-blue/Server/mods/deathmatch/logic/CElement.cpp:1281-1306).
#pragma once

#include "coop/permissions/grants_core.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::session::local_grants {

// Once, at session bring-up, beside the other dev installs: the process's one long-lived Session.
void Install(coop::net::Session* session);

// May this machine use the local feature `n`? Any thread, lock-free. No session or not running:
// true. Running: the packed word read FIRST, then the current serial.
bool Has(coop::permissions::grants::Projected n);

// True when `Has` is true for any of the projected dev features (every one but the cheats right, which the player
// tools read themselves): the F1 dev panes have something to show. Any thread.
bool AnyDevLocal();

// The serial the stored bits were written for (the packed word's high half; 0 before any write).
// Any thread.
uint32_t AppliedSerial();

// Game thread, the writer: one release store of the packed `serial` and `bits`.
void SetBits(uint32_t serial, uint32_t bits);

}  // namespace coop::session::local_grants
