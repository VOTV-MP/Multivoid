// coop/dev/traffic_census.h -- [dev] what this peer receives, by kind, in packets and wire bytes (the dev row
// traffic_census, env twin VOTVCOOP_TRAFFIC_CENSUS; DEV diagnostic).
//
// Purpose: the measurement a traffic optimisation is judged against. Each inbound packet is counted at Session::HandleMessage,
// after the slot check: a stream packet under its MsgType, a reliable under its ReliableKind, each with the datagram's full
// length. A host's census is therefore what its clients send it and a client's is what the host sends it (its own messages and
// the ones it relays), so the two logs of a two-peer run together hold both directions. Every ten seconds the rows that moved
// are logged as running totals in `[TRAFFIC]` lines (`S<type>=<packets>/<bytes>` a stream, `R<kind>=<packets>/<bytes>` a
// reliable), stamped with the seconds since this peer's first counted packet; a script names the numbers from protocol.h.
//
// coop/dev/wire_census counts without bytes and logs each reliable on its own line, which a join's burst would drown.
// All entry points are NET-THREAD only (Session::HandleMessage and the NetThread loop); state is plain statics on that thread.

#pragma once

#include <cstdint>

namespace coop::dev::traffic_census {

// Latched read of the environment switch (the first call latches).
bool Enabled();

// One inbound packet of a stream type (any MsgType but Reliable), or one inbound reliable of a kind.
void NoteStream(unsigned msgType, uint32_t bytes);
void NoteReliable(unsigned kind, uint32_t bytes);

// The ten-second flush, from the NetThread loop, so the last window before a disconnect is logged too.
void Tick();

}  // namespace coop::dev::traffic_census
