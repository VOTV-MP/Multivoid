// coop/dev/wire_census.h -- env-gated inbound-wire census (DEV diagnostic;
// probe family, RULE-2 exempt).
//
// Purpose (2026-08-22, the D2 wire-window probe -- design of record
// research/findings/tooling/votv-islive-zeroav-cachedobjref-DESIGN-2026-08-22.md
// section 6): measure whether a peer whose world is DYING (exit-to-menu with the
// layer live, before the flee's 4 s poll notices) leaks wire traffic about
// dying-world actors that the receiving peer then applies. Armed on the HOST via
// VOTVCOOP_WIRE_CENSUS=1, it logs EVERY inbound reliable individually and
// per-second aggregated stream counts, each line stamped with GetTickCount64()
// (machine-global ms, so the census aligns exactly against the quitting client's
// own "transition NOW" marker across the two logs on one test machine).
//
// All entry points are NET-THREAD only (called from Session::HandleMessage and
// the NetThread loop); state is plain statics on that single thread.

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
