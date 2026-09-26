// ue_wrap/devices/portable_pc.h -- the portable PC prop (Aprop_portablePc_C) wrapper.
//
// The portable PC is a REMOTE TERMINAL to the base laptop (bindPC(gamemode.laptop.laptop));
// its only own world state is the LID (`opened`, runtime-only; the .cpp resolves it by name
// over a measured fallback). The class can be a new object in the next world, so it is held by slot
// and serial and looked up by name in the object index again when the slot no longer holds it, one
// lookup and never an object-array walk; on the rig's worlds the index listed it at world-ready,
// before any PC was there. Until the index lists it, an actor's own class answers by name. No class
// derives from it in this cook, so a PC is an object of exactly this class.
//
// No network logic, no coop state (principle 7). Game thread only.

#pragma once

#include <cstddef>
#include <cstdint>

namespace ue_wrap::portable_pc {

// Whether `actor` is a portable PC: its class against the class this world holds.
bool IsPortablePc(void* actor);

// Every portable PC the object index lists, handed to `fn` with `ctx`, with no walk: the
// class's default object is among them, and a caller that needs a placed PC checks for it
// (an element id does). Returns the count. `fn` must not create or destroy objects.
using PcFn = void (*)(void* ctx, void* pc);
size_t ForEachPc(PcFn fn, void* ctx);

// The lid state (`opened`) of an actor IsPortablePc accepted: it reads the offset and checks nothing
// else. False when actor is null or the class has not loaded yet.
bool ReadOpened(void* actor, bool& outOpened);

// Reflected custom event open(opened) -- the lid's one writer (sets `opened`, toggles the top
// collision, plays or reverses the lid timeline; uber@1801/@526), resolved per call from the
// instance's class.
bool CallOpen(void* actor, bool opened);

}  // namespace ue_wrap::portable_pc
