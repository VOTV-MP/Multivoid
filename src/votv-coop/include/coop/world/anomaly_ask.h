// coop/world/anomaly_ask.h -- a client asks the host to run an Anomalous entity's act (AnomalyAsk).
//
// A client's own copy of the act is cancelled where it runs (the lanes' watches), and the act is asked here instead:
// op 1, its dirt hole dug up the basalt pillar's stone (coop/world/hexpillar_sync); op 2, its player woke with the
// Christmas tree up (coop/creatures/nisse_sync). The host keeps one ask a second per sender and hands each op to its
// lane, which judges the sender and runs the act on the host's copy. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct AnomalyAskPayload;
}  // namespace coop::net

namespace coop::anomaly_ask {

// CLIENT: ask the host. False when not sent.
bool Send(uint8_t op, float x, float y, float z);

// HOST: one client's ask, from the dispatch.
void OnAsk(coop::net::Session& session, uint8_t senderSlot, const coop::net::AnomalyAskPayload& p);

void Install(coop::net::Session* session);
void OnDisconnect();

}  // namespace coop::anomaly_ask
