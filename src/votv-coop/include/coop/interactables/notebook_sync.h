#pragma once

#include "coop/net/protocol.h"
#include <cstdint>

namespace coop::net { class Session; }
namespace coop::notebook_sync {
void Install(coop::net::Session* session);
void Tick();
void OnReliable(const coop::net::BlobChunkPayload& chunk, uint8_t senderSlot);
void OnDisconnectForSlot(int slot);
void OnDisconnect();
}
