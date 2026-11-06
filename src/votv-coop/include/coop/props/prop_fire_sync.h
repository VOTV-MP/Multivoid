// coop/props/prop_fire_sync.h -- a burning prop burns on the host, and every peer sees its flame.
//
// A prop's fire is its flameBase, a comp_flammable_C with its fuel and the speed it spends it. The flame burns the prop down
// through its own fireDamage, which only the host decides (coop/props/prop_damage_gate), and it lights what it touches. So
// the host owns every keyed prop's fire:
//   - HOST: a poll lists the burning props and sends each one's flame, at its edge, on a refuel and every 2 s while it
//     burns, and once when it goes out (PropFireState). A client's light (PropFireRequest) is checked -- the sender's world,
//     the prop, its reach -- and the prop's own ignite runs on the host's copy, as single player would.
//   - CLIENT: a prop's ignite is refused on its copy whatever called it. Inside the local player's own lighter or torch verb
//     it is the player's light, sent to the host with its fuel; anything else (a flame spreading, a burning body) is the
//     host's to roll. The host's flame is shown on the copy: lit by prop_C's own ignite, its fuel and speed written, or put
//     out. A copy's flame still burns this client's own player, as the game's fire does on every peer.
// Salt rockets keep their own lane (coop/world/salt_rocket_ignite), a prop in a hand is left to its holder, and a keyless
// prop is not carried. Adapted from Watchers of the Void's prop fire lane (ElvardFIC/WatchersOfTheVoid, MIT); see
// docs/credits.md. Game thread.

#pragma once

namespace coop::net {
class Session;
struct PropFirePayload;
}  // namespace coop::net

namespace coop::prop_fire_sync {

// Store the session and register the ignite watch, once for the process. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// The poll and the inboxes, on either role. Game thread.
void Tick();

// A client's receiver: the host's flame on a prop.
void OnState(const coop::net::PropFirePayload& p);

// The host's receiver: a client's player lit a prop.
void OnRequest(int senderSlot, const coop::net::PropFirePayload& p);

// Every burning prop again for a joiner.
void QueueConnectBroadcastForSlot(int slot);

// The session ended: everything is forgotten.
void OnDisconnect();

}  // namespace coop::prop_fire_sync
