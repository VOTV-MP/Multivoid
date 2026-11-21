// coop/interactables/car_charger_sync.h -- the car battery charger's clips and a loose ATV battery's charge, the host's.
//
// The car battery charger (prop_carBatteryCharger_C) charges an ATV battery (prop_atvcarbattery_C) in its own tick, a
// sixtieth of a point a second while its cord is in and both clips sit on the battery, red on red and blue on blue (the
// wrong way round it sparks). A clip goes on in its used event and off at the drag's start, on the machine of the player
// who moved it, and the charge was only ever that machine's: a clip moved on a client reached no one, and a charge set on
// one peer after the battery's birth stayed there.
//
// The host owns both. CarChargerState carries a car charger's clips (the battery each is on, its polarity, done) and its
// cord, and a loose battery's charge and pins: a charger's row when they change, a battery's when its pins change or its
// charge moves (every 2 s, or at once past 5 points), and every one to a joiner. A client writes them, and its charger's
// tick is refused, so the charge and the spark are the host's alone; its own cord event is put back to the host's. A
// client's clip move is seen against the host's last row every 250 ms and goes to the host as CarChargerIntent; the host
// judges the reach and the batteries and sets it.

#pragma once

namespace coop::net {
class Session;
struct CarChargerStatePayload;
struct CarChargerIntentPayload;
}  // namespace coop::net

namespace coop::car_charger_sync {

void Install(coop::net::Session* session);

// The mod's tick: the watch until it settles; the host's rows; a client's clip edits and parked rows. Game thread.
void Tick();

// HOST: every car charger and loose battery to a joiner at its world-ready.
void OnPeerWorldReady(int slot);

// HOST: a client's clip move.
void OnIntent(int slot, const coop::net::CarChargerIntentPayload& p);

// CLIENT: the host's rows.
void OnState(const coop::net::CarChargerStatePayload& p);

void OnDisconnect();

}  // namespace coop::car_charger_sync
