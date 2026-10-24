// coop/interactables/coffee_intent.h -- coffee appliance and final cup actions run by the host.
#pragma once
#include <cstdint>

namespace coop::net {
class Session;
struct CoffeeActionIntentPayload;
struct CoffeeDeviceStatePayload;
}

namespace coop::coffee_intent {
void Install(coop::net::Session* session);
void Tick();
void OnAction(coop::net::Session& session, const coop::net::CoffeeActionIntentPayload& payload,
              uint8_t senderSlot);
void OnState(const coop::net::CoffeeDeviceStatePayload& payload);
void SnapshotToSlot(int slot);
void OnDisconnect();
}
