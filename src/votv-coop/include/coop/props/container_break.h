// coop/props/container_break.h -- what a client breaks, or saws, is judged by the host.
//
// A container's `broken` spills what it holds, then its physicsImpact component destroys it. A client's copy breaks only
// when the client authors it, and its spill would be the client's own births, which never reach the host, while its destroy
// does. So the damage gate (coop/props/prop_damage_gate) cancels a lethal impact or fire on a container the client authors
// and hands it here: the host breaks its own copy by the same lethal call, when the sender authors the container (a row of
// its own stands on it, or its held-pose stream moves it) and it is in reach; a sender has a budget and a queue of its own.
// A hacksaw's cut (coop/props/hacksaw_cut) is asked before it completes, and the host GRANTS it: a saw is in the sender's
// applied hand, the prop is there, not granted to another slot, and within the saw's reach. A grant (op 3) is recorded and
// answered to the sender alone (an answer that cannot be sent is owed and sent at the next Tick, and a grant is recorded only
// once it went); the host's copy breaks (a container's spill is the host's) when the sender's destroy of
// the prop arrives, so a cut never finished changes nothing, and a cut that ends without it hands the grant back (op 4, the
// client's withdrawal), which ends the record. The client's saw runs the rest. A refusal
// (op 2) carries its reason; a cut's request is never dropped without one, under a budget of replies. Game thread.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::props::container_break {

// Store the session. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// CLIENT: whether `owner` is a container this lane breaks on the host; when it is, the request is queued and the
// caller cancels the local call. `fire` names a fireDamage. Game thread.
bool RequestFromClient(void* owner, bool fire);

// CLIENT, from the saw's tick: the cut of the prop keyed `propKey` is asked of the host (the request waits in the
// client's bounded queue until its world is announced). False when the queue is full: the caller answers the cut
// itself. Game thread.
bool RequestSaw(const std::wstring& propKey);

// CLIENT, from the saw: the granted cut of the prop keyed `propKey` ended without its destroy, so the host is told (op 4) and
// ends its record. False when the queue is full. Game thread.
bool WithdrawSaw(const std::wstring& propKey);

// HOST, from the receive path of a client's PropDestroy: when the sender was granted a cut of the prop keyed `key`, the
// host's own copy breaks first (its spill is the host's), and the destroy then applies. Game thread.
void OnClientDestroy(int senderSlot, const std::wstring& key);

// The requests this client has asked of the host since the session began (the drills read it).
uint32_t AskedCount();

// The host's refusals this client has been answered since the session began: the event a drill's arm that expects a
// refusal reads (the drills read it).
uint32_t RefusedAnswerCount();

// The client's queue, and the host's served requests. Game thread.
void Tick();

// A ContainerBreakRequest arrived (game thread, in the drain). HOST: a client's op 1, queued and served in Tick, or its op 4
// (a withdrawal of a grant), applied at once. CLIENT: the host's op 2 (a refusal) or op 3 (a grant of a cut), handed on to the
// saw. The size is exactly a payload's and every field is checked here; a message that does not add up is dropped and
// counted.
void OnMessage(coop::net::Session& session, int senderSlot, const void* payload, size_t payloadLen);

// The session ended: everything is forgotten.
void OnDisconnect();

}  // namespace coop::props::container_break
