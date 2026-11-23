// coop/props/butter_churn_sync.h -- the butter churn's milk, speed and butter are the host's.
//
// The churn (prop_butterchurn_C) takes a held milk carton poured on it (its playerUsedOn: the carton destroyed, its
// `milkPercent` 100), is churned by its action 4 (its `speed` raised, its tick on), turns the milk to butter in its own
// tick, and gives the butter to the player whose action 7 takes it. Each peer ran that on its own copy, so a client's
// churn made butter on its machine alone and two players could each take one. Here every act is the host's: a client's
// churn and take are refused at the script-body gate and asked of the host (ButterChurnAsk); its pour runs on its copy
// (the carton is in its own hand) and is asked too. The host judges the sender's reach, runs the churn on its own copy,
// sets the milk, or clears the butter and grants the take to that one asker, whose copy then runs its own action 7 into
// its own inventory. Each change goes to every client (ButterChurnState), which writes the host's milk and butter and
// replays a churn through its copy's own action 4, so its crank turns and its sound plays; a joiner gets every churn at
// its ready edge. Game thread.
#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct ButterChurnStatePayload;
struct ButterChurnAskPayload;
}  // namespace coop::net

namespace coop::butter_churn_sync {

// Keep the session and register the watches on the churn's actions and pour. Idempotent.
void Install(coop::net::Session* session);

// Settles the watches. Per frame.
void Tick();

// HOST, at a joiner's world-ready edge: every churn's state, to that slot.
void SendJoinSnapshotForSlot(int slot);

// HOST: a client's ask, its length checked by the dispatcher.
void OnAsk(int senderSlot, const coop::net::ButterChurnAskPayload& p);

// CLIENT: the host's state, its length checked by the dispatcher.
void OnState(const coop::net::ButterChurnStatePayload& p);

// CLIENT (coop/props/remote_prop): a prop copy was bound to element id `eid`; a state held for it is applied.
void OnBound(uint32_t eid, void* actor);

// CLIENT: the world changed under the session: the states held go.
void OnClientWorldReady();

// Session end.
void OnDisconnect();

}  // namespace coop::butter_churn_sync
