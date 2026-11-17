// coop/interactables/atv_rider_watch.h -- the host's watch of a kerfur's seat calls. A kerfur sits on an ATV through its own
// `sitOnCar` and leaves through `getOffCar`, both Blueprint-internal local calls a ProcessEvent hook never sees, so the
// script gate watches them by name. On the host each one writes the riding kerfur's element id into that ATV's table entry
// (atv_seat_table::SetRider), which every block the host sends then carries (AtvStatePayload::riderEid). A client does not
// publish: its own game's `getOffCar` and `tryToOccupyCar` on a kerfur that is the host's mirror are cancelled (the mirror's
// seat is the host's, coop/creatures/npc_ride), and every other call there runs. Game thread.

#pragma once

#include <string>

namespace coop::net { class Session; }
namespace coop::atv_sync { struct AtvEntry; }

namespace coop::atv_rider_watch {

// Host only: the entry's ATV is read for a kerfur already riding it (its own `sittingKerfuro`, attached to it, with an element
// id) and that kerfur is named the entry's rider. Run where an entry is created or gets its actor, for a ride begun before the
// watch was live or before this session reached its joiners. A riding kerfur with no element id yet is said once per (key,
// kerfur) and the entry is left waiting: SeedWaiting says so, and the next index pass that sees the actor runs it again. A
// client does nothing.
void SeedRider(coop::atv_sync::AtvEntry& e);
bool SeedWaiting(const std::wstring& key);
// The entry under `key` exited (its ATV is gone or the session ended): its wait is dropped. Game thread.
void ForgetSeedWait(const std::wstring& key);

// Store the session and register the watches; asked again each call until each watch settles live or dead. Idempotent.
void Install(coop::net::Session* session);

// The session ended: the once-per-session notice is armed again.
void OnDisconnect();

}  // namespace coop::atv_rider_watch
