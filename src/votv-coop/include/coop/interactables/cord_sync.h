// coop/interactables/cord_sync.h -- the shared cord-to-socket relationship.
#pragma once

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::cord_sync {
void Install(coop::net::Session* session);
void Tick();
void OnReliable(const coop::net::CordLinkPayload& payload, uint8_t senderSlot);
void QueueConnectBroadcastForSlot(int peerSlot);
void OnDisconnect();
}
