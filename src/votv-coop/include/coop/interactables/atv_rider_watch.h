// coop/interactables/atv_rider_watch.h -- the host's watch of a kerfur's seat calls. A kerfur sits on an ATV through its own
// `sitOnCar` and leaves through `getOffCar`, both Blueprint-internal local calls a ProcessEvent hook never sees, so the
// script gate watches them by name. On the host each one writes the riding kerfur's element id into that ATV's table entry
// (atv_seat_table::SetRider), which every block the host sends then carries (AtvStatePayload::riderEid). A client's calls
// are not its own to publish: the callbacks return at once there. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::atv_rider_watch {

// Store the session and register the watches; asked again each call until each watch settles live or dead. Idempotent.
void Install(coop::net::Session* session);

// The session ended: the once-per-session notice is armed again.
void OnDisconnect();

}  // namespace coop::atv_rider_watch
