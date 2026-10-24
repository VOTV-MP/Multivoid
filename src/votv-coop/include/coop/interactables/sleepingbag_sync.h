// coop/interactables/sleepingbag_sync.h -- client intent for the sleepingbag's two actor transitions.
// The host executes the packed prop's actionOptionIndex or the deployed prop's fold(); ordinary
// prop birth/death and the host save carry the resulting forms.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::sleepingbag_sync {

void Install(coop::net::Session* session);
void Tick(coop::net::Session& session);
void OnIntent(coop::net::Session& session, const coop::net::SleepingbagIntentPayload& payload,
              uint8_t senderSlot);
void OnDisconnect();

}  // namespace coop::sleepingbag_sync
