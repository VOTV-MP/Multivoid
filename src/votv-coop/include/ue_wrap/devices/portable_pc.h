// ue_wrap/devices/portable_pc.h -- the portable PC prop (Aprop_portablePc_C) wrapper.
//
// RE ground truth: votv-laptop-pc-RE-2026-07-17.md + the OPEN-10 pass
// (votv-laptop-v2-OPEN10-impl-DESIGN-2026-07-18.md SS3): the portable PC is a
// REMOTE TERMINAL to the base laptop (bindPC(gamemode.laptop.laptop)); its only
// own world state is the LID (`opened` @0x398, runtime-only). The class is
// BUYABLE (loads on purchase) -- NO FindClass polling (the device_screen
// forever-walk lesson); identity resolves per-instance via a ClassOf verdict
// cache (NameOf runs once per distinct UClass ever seen).
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
