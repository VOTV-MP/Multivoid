// coop/dev/server_breaker_census.h -- dev-only, read-only census of what the server breaker rolls
// from (ini server_breaker_census=1, off by default; run it on BOTH peers).
//
// The game breaks a signal server from ticker_serverBreaker_C.ReceiveTick: it returns unless the
// game instance's mode byte is 0, 1 or 6, sets its own tick interval from the clock, returns unless
// mainGamemode.brokenServers is below 5, rolls the difficulty's weight and then the day's, and calls
// breakServer on a random entry of mainGamemode.servers, a list filled once at the gamemode's begin
// play. This census prints, every 30 s, each input of that chain in one line, so a session where
// the servers stop breaking names the input that stopped it:
//   server_breaker_census: role=<host|client> tickers=<n> interval=<s> tickOn=<0|1> gmByte=<n>
//     difficulty=<n> day=<n> totalTime=<f> servers=<len> dead=<n> null=<n> liveBoxes=<n>
//     brokenServers=<n> brokenActual=<n> ticks=<n since the last line> unlisted=<n>
// and, per call of breakServer, fix and break_type on a box, one line with the gamemode's
// brokenServers before and after. It never refuses a call and writes nothing.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::server_breaker_census {

// The memoized flag.
bool IsEnabled();

// Cache the session, for the role. Called from the dev lanes' install fanout every pump tick;
// idempotent. No-op when the flag is off.
void Install(coop::net::Session* session);

// Register the watches (once per process) and print the periodic line. Every pump tick; one
// bool read when off. Game thread.
void Tick();

}  // namespace coop::dev::server_breaker_census
