// coop/props/container_break.h -- a container a client breaks breaks on the host.
//
// A container (prop_container_C) breaking spills what it holds: its own `broken` spawns every record at random points
// inside it, then its physicsImpact component destroys it. A client's copy breaks only when the client authors the
// container (coop/props/prop_damage_gate: it holds it, threw it or syncs it), and its spill there is the client's own
// births, which never reach the host, while its destroy does: the host's container went, with its contents. So the
// damage gate cancels a lethal impactDamage or fireDamage on a container the client authors and hands it here: the
// client asks the host (ContainerBreakRequest), and the host breaks its own copy by the same lethal call, checked for
// the sender's world and reach. The host's spill and destroy reach every peer by the prop lanes. Game thread.

#pragma once

namespace coop::net {
class Session;
struct ContainerBreakPayload;
}  // namespace coop::net

namespace coop::props::container_break {

// Store the session. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// CLIENT: whether `owner` is a container this lane breaks on the host; when it is, the request is queued and the
// caller cancels the local call. `fire` names a fireDamage. Game thread.
bool RequestFromClient(void* owner, bool fire);

// The client's queue, and the host's served requests. Game thread.
void Tick();

// The host's receiver: a client's break.
void OnRequest(int senderSlot, const coop::net::ContainerBreakPayload& p);

// The session ended: everything is forgotten.
void OnDisconnect();

}  // namespace coop::props::container_break
