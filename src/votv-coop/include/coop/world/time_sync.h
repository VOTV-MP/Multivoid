// coop/world/time_sync.h -- host-authoritative WORLD CLOCK sync (time of day and the day number).
// Owns the wire, the host's send decision and the client's parked cycle; reaches the engine only
// through ue_wrap::daynightcycle. Weather is weather_sync's.
// The host streams its cycle's two accumulators and its day number. The client's cycle is a parked
// mirror: before each tick (a pre-observer on its own ReceiveTick) its rate is held at 0 and the host's
// last applied sample written in, a newly arrived one or else the one before it again, so neither its
// own advance nor a local write (the cheat menu's day buttons) reaches a midnight -- a sample's `day`
// is below maxTime, as the host wraps inside its tick. Its settime pulses run per machine; the weather
// births, the event walk and the 6 am order are held, the sky eye, jellyfish, flesh rain, gifts and red
// sky's noon end still run per peer. MTA's server keeps the anchor
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CClock.cpp:35-42) and sends the time and minute
// length at a join (reference/mtasa-blue/Server/mods/deathmatch/logic/CMapManager.cpp:444-455); its
// client can freeze its clock (reference/mtasa-blue/Client/game_sa/CClockSA.cpp:34). A running client
// clock here reaches its own midnight first, so the value streams: a sample per half game minute of
// the host's clock, at least twice a second.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::time_sync {

// Store the session pointer and, once the daynightCycle_C class has loaded, register the pre-observer
// on the cycle's tick (once per process). Called every pump tick by the install fanout. Game thread.
void Install(coop::net::Session* session);

// Per-tick pump, HOST: read the clock and hand the net thread a sample when one is due. The stream
// reaches every connected peer from the connect on, so a joiner's first sample once its world exists
// is its late-join answer. The client's work runs at the cycle's own tick. Game thread.
void Tick();

// CLIENT: whether this client's clock mirror owns a cycle about to tick -- a connected client
// session, and a cycle the gamemode has not flagged as the menu scene's. It fails toward holding: a
// cycle whose gamemode is null, dead or unresolvable counts as a world's. The event walk's hold asks
// the same question, so the two switch on together. Game thread.
bool HoldsCycle(void* cycle);

// CLIENT: the host's day number as the last applied sample carried it, -1 before the first and
// after a disconnect. Read-only, for instruments. Any thread.
int32_t LastHostDayZ();

// Session teardown: hand the client's clock back (time scale 1) and reset the host's send state.
// Game thread.
void OnDisconnect();

}  // namespace coop::time_sync
