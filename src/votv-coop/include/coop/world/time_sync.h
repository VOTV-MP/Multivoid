// coop/world/time_sync.h -- host-authoritative WORLD CLOCK sync (time of day and the day number).
// Owns the wire, the host's send decision and the client's parked cycle; reaches the engine only
// through ue_wrap::daynightcycle. Weather is weather_sync's.
//
// The host streams its cycle's two accumulators and its day number; the client's cycle is a parked
// mirror: before each of its ticks (a pre-observer on the cycle's own ReceiveTick) its time scale is
// held at 0 and the newest sample is written in, so its `day` moves only by the host's samples,
// which are below maxTime because the host wraps inside its own tick. The client never runs the
// midnight's shared outputs; the host's midnight reaches it as one sample, a small `day` with the
// next day number. Its settime pulses do run -- the sun, the decal, the weekday, the stings are each
// machine's own -- and the pulse outputs that write shared state are parked by their own lanes. MTA's
// CClock sends an anchor and lets the client's clock run; here a running client clock reaches its own
// midnight first, so the value streams, a sample each time the host's clock has moved half a game
// minute and at least twice a second: the client's settime sees every minute, the shared sleep too.

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

// CLIENT: the host's day number as the last applied sample carried it, -1 before the first and
// after a disconnect. Read-only, for instruments. Any thread.
int32_t LastHostDayZ();

// Session teardown: hand the client's clock back (time scale 1) and reset the host's send state.
// Game thread.
void OnDisconnect();

}  // namespace coop::time_sync
