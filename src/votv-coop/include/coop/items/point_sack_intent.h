// coop/items/point_sack_intent.h -- a client's point sack, opened on the host.
//
// prop_pointSack_C (and its sized variants) pays on its own action: actionOptionIndex runs
// lib_C::addPoints(points), a hint naming the sum, and K2_DestroyActor on itself. On a client that
// credit lands only in the client's mirror of the shared balance, and the host's next balance row
// takes it away again, while the sack's destroy reaches the host and the points are lost for both.
// A running client's action on a sack is refused at the script-body gate and sent to the host as
// a PointSackRedeem naming the sack; the host checks the sender's reach, destroys its copy,
// and pays its own `points` only when that incarnation is no longer live. The sack is the
// consumption record: a repeated or late request finds no sack and pays nothing. A failed send
// during joining leaves the sack intact for retry.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PointSackRedeemPayload;
}

namespace coop::point_sack_intent {

void Install(coop::net::Session* session);

// Settles the gate watch; game thread.
void Tick();

// HOST: a client's request. Game thread.
void OnRedeem(coop::net::Session& session, const coop::net::PointSackRedeemPayload& payload, uint8_t senderSlot);

void OnDisconnect();

// HOST: the redemptions paid this session, for the sack drill. Game thread.
uint64_t PaidCount();

}  // namespace coop::point_sack_intent
