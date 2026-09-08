// coop/flashlight_click_sound.h -- a 3D positional click at a puppet's location when a remote
// peer toggles their flashlight.
//
// Plays the game's own `flashlight` USoundWave through UGameplayStatics::PlaySoundAtLocation
// with a USoundAttenuation we build at runtime, so no cooked `att_*` content asset is borrowed.
// Sphere, inverse distance, a third of the baseline in ue_wrap::engine::SoundAttenuationConfig:
// full volume out to 6.67 m and silence by ~73 m, where the baseline reaches 20 m and 220 m.
// That baseline carries a click across the whole outdoor map, which is right for the local
// player and far too generous for someone else's toggle. SpawnSoundAttenuation pins the object
// for the process, since a C++ static is invisible to UE's reachability scan.
//
// Only a state CHANGE clicks: a hold-F mode-change packet keeps state=on and moves the cones
// alone. The last applied state is kept per peer SLOT rather than per puppet pointer, which
// would dangle after a disconnect and a respawn into the same slot. Game thread only -- the
// reflection calls touch UObject memory directly.

#pragma once

#include <cstdint>

namespace coop::flashlight_click_sound {

// Play the click for a wire packet's just-applied state on `puppetActor`. A `newState` equal to
// the last one applied for `peerSlot` is a no-op: that packet was a mode change. Otherwise the
// sound asset and the attenuation are resolved on first use and PlaySoundAtLocation is
// dispatched. Game thread only.
void PlayIfStateChanged(void* puppetActor, uint8_t peerSlot, bool newState);

}  // namespace coop::flashlight_click_sound
