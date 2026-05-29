// coop/flashlight_click_sound.h -- 3D positional click sound at puppet
// position when a remote peer toggles their flashlight (Phase 5F v6).
//
// Plays VOTV's own `/Game/audio/effects/flashlight` USoundWave at the
// puppet's world location via UGameplayStatics::PlaySoundAtLocation, with
// a RUNTIME-CONSTRUCTED USoundAttenuation override (RULE 1 native path --
// no borrowing of VOTV's cooked `att_*` content assets). Sphere shape:
// 20m inner radius (full volume) -> 200m falloff to silence, inverse
// distance algorithm. The attenuation object is `R::AddToRoot`ed so UE4
// GC never collects it (a C++ static void* is invisible to the GC reach
// scan; without rooting the object dangles on the next GC pass and
// PlaySoundAtLocation crashes on rapid toggle).
//
// Gated on state CHANGE only: hold-F mode-change packets keep state=on
// and mutate cones, but MUST NOT produce a click. Per-peer last-applied
// state tracked in a fixed-size array keyed on peer slot (kMaxPeers=4) --
// NOT raw puppet pointers (audit fix: would dangle after disconnect+
// respawn for the same peer slot). v13 (A4 2026-05-29): the caller now
// resolves the peer slot from the wire packet's senderElementId via
// element::Registry::Get -> Player::PeerSlot; this entry point keeps the
// uint8_t peer-slot interface so the per-peer state map indexing stays
// O(1) and the dangle-immune semantics unchanged.
//
// Game thread only -- the reflection calls (FindObject, ParamFrame,
// PlaySoundAtLocation) touch UObject memory directly.

#pragma once

#include <cstdint>

namespace coop::flashlight_click_sound {

// Apply the click sound effect for a wire packet's just-applied state on
// the given puppet. If `newState` matches the LAST applied state for
// `peerSlot`, this is a no-op (the packet was a mode-change, not a
// state-change). Otherwise resolves cached UFunction pointers + sound
// asset + attenuation override on first use, then dispatches
// PlaySoundAtLocation. Game thread only.
void PlayIfStateChanged(void* puppetActor, uint8_t peerSlot, bool newState);

}  // namespace coop::flashlight_click_sound
