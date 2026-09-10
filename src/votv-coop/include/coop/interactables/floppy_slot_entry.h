// coop/interactables/floppy_slot_entry.h -- which overlap into a device's slot becomes an insert.
//
// A device takes a disc by two entries: a player pressing E, and the hitbox reporting whatever
// touches the slot. The second fires for a disc our own code has just materialised inside the
// device -- every disc another peer ejected -- and swallowing that raises a destroy we relay, so
// the disc dies on every peer. The rule therefore sits on the DISC: a disc is in transit for a
// moment after it materialises, and no device swallows one in transit. Anchored at the disc's own
// appearance on each machine, so every peer runs one rule against one local event, nothing is owed
// and nothing can starve. MTA latches an entity's transition the same way and clears it by the
// ANSWER (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3153); we clear on the
// window instead, because there the client cannot see the transition and here both peers see the
// birth. The window, and the game behaviour that sets it: docs/devices.md, "The floppy slot".
//
// Game thread. No wire kind and no message: the whole lane is local.

#pragma once

namespace coop::net { class Session; }

namespace coop::floppy_slot_entry {

// Register the pre-dispatch interceptor on every device kind's overlap entries, and the spawn seam
// that marks a materialising disc. Idempotent and retry-throttled while a Blueprint class is not
// loaded yet; safe to call every subsystem-install tick.
void Install(coop::net::Session* session);

// The session summary, and the interceptors handed back: the device wrapper drops its overlap
// UFunctions with the rest of its cache, so a registration left against one would judge whatever
// takes that address next. A refusal count that stays zero across a session where discs were
// ejected says the interceptor is not on the path players use, not that the path is clean.
void OnDisconnect();

}  // namespace coop::floppy_slot_entry
