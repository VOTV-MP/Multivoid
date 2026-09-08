// coop/dev/wire_census.h -- env-gated inbound-wire census (DEV diagnostic; probe family,
// exempt from the no-migration-baggage rule).
//
// Purpose: measure whether a peer whose world is DYING -- an exit to menu with the layer still
// live, before the flee poll notices -- leaks wire traffic about dying-world actors that the
// receiving peer then applies. Armed on the HOST with VOTVCOOP_WIRE_CENSUS=1, it logs every
// inbound reliable individually plus per-second aggregated stream counts, each line stamped with
// GetTickCount64(). That stamp is machine-global, so on one test machine the census lines up
// exactly against the quitting client's own transition marker in the other log.
//
// All entry points are NET-THREAD only (Session::HandleMessage and the NetThread loop); state is
// plain statics on that single thread.

#pragma once

namespace coop::dev::wire_census {

// Latched read of VOTVCOOP_WIRE_CENSUS=1 (first call latches).
bool Enabled();

// Per-second flush of the aggregated stream counters, called from the NetThread
// loop so the FINAL second still flushes after the sender disconnects (an
// arrival-driven flush would silently drop the census tail -- the very seconds
// the wire-window question is about).
void Tick();

// One inbound unreliable/stream packet (any non-Reliable MsgType), attributed
// to the LOGICAL origin slot. Aggregated, flushed by Tick().
void NoteStream(int routeSlot, unsigned msgType);

// One inbound reliable, logged individually (host inbound reliable rate is
// client-action-paced -- no aggregation needed; the connect-time snapshot burst
// is host->client, never inbound on the host).
void NoteReliable(int routeSlot, unsigned kind);

}  // namespace coop::dev::wire_census
