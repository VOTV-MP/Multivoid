// coop/interactables/battery_charger_sync.h -- the battery charger (prop_batteryCharger_C), the host's.
//
// The charger holds four rechargeable batteries as charges, b1..b4 (-1 empty), and its own ReceiveTick raises each by
// a third a second, shared among the batteries in it (`plugged`), while it is `powered`. A press (actionOptionIndex,
// action 4, into putIn) on the slot looked at puts the rechargeable battery in the hand there, which it destroys, or
// takes the slot's battery out into the presser's inventory. Every peer ran its own: a client's put or take changed its
// copy alone, the host's charging reached no one, and a joiner kept the slots of the save it was sent.
//
// The host owns the slots. On ChargerState it sends each charger's row when a slot fills or empties, when plugged or
// powered changes, every 2 s while a charge moves, and every one to a joiner; a client writes the row and runs upd, and
// its own ReceiveTick is refused. A client's put runs on its own copy as the game's does (the battery leaves its hand)
// and goes to the host as ChargerIntent with the battery's charge; the host judges the reach and the slot and writes it,
// and a put it refuses has its battery spawned beside the sender. A client's take is cancelled and asked; the host runs
// it on its own copy and answers ChargerGive with the charge, and the client puts that battery into its inventory as the
// game's take does.

#pragma once

namespace coop::net {
class Session;
struct ChargerStatePayload;
struct ChargerIntentPayload;
struct ChargerGivePayload;
}  // namespace coop::net

namespace coop::battery_charger_sync {

void Install(coop::net::Session* session);

// The mod's tick: the watches until they settle; the host's rows; a client's parked rows. Game thread.
void Tick();

// HOST: every charger's row to a joiner at its world-ready.
void OnPeerWorldReady(int slot);

// HOST: a client's put or take.
void OnIntent(int slot, const coop::net::ChargerIntentPayload& p);

// CLIENT: the host's rows; the host's answer to this client's take.
void OnState(const coop::net::ChargerStatePayload& p);
void OnGive(const coop::net::ChargerGivePayload& p);

void OnDisconnect();

}  // namespace coop::battery_charger_sync
