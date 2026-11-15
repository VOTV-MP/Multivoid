// coop/world/agrav_sync.h -- the anti-gravity event's props reach the clients through the driven-prop channel.
//
// trigger_agrav_C (the `agrav` row) lifts every prop within 100 m of the host's camera, one at a time: it turns a
// prop's gravity off, gives it a slow upward velocity and a spin, and after a hold turns gravity back on one prop per
// frame. None of that is a verb a lane watches, so the host's props moved and every client's copy stood still.
// While the event is in flight (its trigger actor in the game's active-event registry) the host reads the trigger's
// `comps` list and claims, for the drive, the lifted props nearest to any player (the host's and each client's),
// up to what the channel's rows can carry, and lets each go when its gravity returns; the drive then ends by its own
// rest rule. When the trigger is gone, each lifted prop that was never streamed and moved is handed to the drive
// once, nearest first and paced, so its copy ends where the host's does. The event itself is never replayed on a
// client: its shuffle is random and its first act breaks a generator. Host only; a client reads nothing.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::agrav_sync {

// Host, game thread, every gameplay tick, before prop_drive_host::Pump. Polls five times a second, hands the end
// corrections on as they fall due, and returns at once on a client or when nothing is in flight.
void Tick(coop::net::Session* s);

// Forgets the props held and the corrections waiting; the drive's own OnDisconnect closes its rows.
void OnDisconnect();

}  // namespace coop::agrav_sync
