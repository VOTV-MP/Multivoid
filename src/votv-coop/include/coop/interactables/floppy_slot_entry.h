// coop/interactables/floppy_slot_entry.h -- which overlap into a device's slot becomes an insert.
//
// A device takes a disc by two entries: a player pressing E, and the hitbox reporting whatever
// touches the slot. The second one fires for a disc our own code has just materialised inside the
// device -- which is every disc another peer ejected -- and swallowing that raises a destroy we
// relay, so the disc dies on every peer.
//
// The rule therefore sits on the DISC, not on the box: a disc is in transit for a moment after it
// materialises, and no device swallows one in transit. Anchored at the disc's own appearance on
// each machine, so every peer runs one rule against one local event and nothing waits on a
// message. Nothing is owed and nothing can starve. Why that window, and what the game does that
// makes it necessary: docs/devices.md, "The floppy slot".
//
// Game thread. No wire kind and no message: the whole lane is local.

#pragma once

namespace coop::net { class Session; }

namespace coop::floppy_slot_entry {

// Register the pre-dispatch interceptor on every device kind's overlap entries, and the spawn seam
// that marks a materialising disc. Idempotent and retry-throttled while a Blueprint class is not
// loaded yet; safe to call every subsystem-install tick.
void Install(coop::net::Session* session);

// The session summary: overlaps seen, inserts refused as in-transit, and marks evicted before
// their window closed. A refusal count that stays zero across a session where discs were ejected
// says the interceptor is not on the path players use, not that the path is clean.
void OnDisconnect();

}  // namespace coop::floppy_slot_entry
