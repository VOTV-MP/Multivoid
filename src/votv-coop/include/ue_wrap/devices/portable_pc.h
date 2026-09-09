// ue_wrap/devices/portable_pc.h -- the portable PC prop (Aprop_portablePc_C) wrapper.
//
// The portable PC is a REMOTE TERMINAL to the base laptop (bindPC(gamemode.laptop.laptop));
// its only own world state is the LID (`opened`, runtime-only; the .cpp resolves it by name
// over a measured fallback). The class is BUYABLE and
// loads on purchase, so a FindClass poll would walk GUObjectArray forever; identity resolves
// per-instance from a ClassOf verdict cache (NameOf runs once per distinct UClass ever seen).
//
// No network logic, no coop state (principle 7). Game thread only.

#pragma once

#include <cstdint>

namespace ue_wrap::portable_pc {

// ClassOf-verdict: is this UClass Aprop_portablePc_C? Cheap pointer-compare
// after the first sighting; NameOf only on cache miss.
bool IsPortablePcClass(void* cls);

// The lid state (`opened`). False when the offset is unresolved or actor null.
bool ReadOpened(void* actor, bool& outOpened);

// Reflected custom event Open(opened) -- the native re-applier (sets `opened`,
// toggles the top collision, plays/reverses the lid timeline; uber@1801/@526).
bool CallOpen(void* actor, bool opened);

void ResetCache();

}  // namespace ue_wrap::portable_pc
