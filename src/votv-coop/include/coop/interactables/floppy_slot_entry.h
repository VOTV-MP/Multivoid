// coop/interactables/floppy_slot_entry.h -- which overlap into a device's slot becomes an insert.
//
// A disc-holding device takes a disc by two entries. `playerUsedOn` is a player pressing E with
// the disc in hand. The other is the hitbox's BeginOverlap, which fires for whatever touches the
// slot -- including a disc our own code has just materialised inside it.
//
// The eject spawns the disc INSIDE the box it came out of, and the Blueprint guards that by
// turning off the hitbox of THAT box for a second. In single player the only birth that can land
// in a slot is that eject, so guarding the one box is enough. Coop adds a second birth -- our
// spawn of a disc another peer ejected -- and it lands in a box whose hitbox nobody turned off, on
// a machine where nobody ejected anything. That box swallows the disc and the destroy it raises is
// relayed, so the disc dies on every peer.
//
// So the rule moves from the box to the DISC: a disc is in transit for a moment after it
// materialises, and no device swallows a disc in transit, on any peer. The window is anchored at
// the disc's own appearance on THIS machine, so every peer runs one rule against one local event
// and nothing waits on a message. Nothing is owed and nothing can starve: the mark is a property
// of an actor and dies with it. Details in docs/devices.md.
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
