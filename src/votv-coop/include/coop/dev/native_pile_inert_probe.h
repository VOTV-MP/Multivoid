// coop/dev/native_pile_inert_probe.h -- the GO/NO-GO gate for nativizing the trash pile mirror.
//
// THE QUESTION: can the bare AStaticMeshActor proxy (coop/trash_proxy) be replaced by a ROOTED
// real actorChipPile_C, restoring the native hover GUI, collision, rotation and movement block
// for free? The blueprint has no autonomous self-init path -- no LifeSpan, no despawn timer, no
// self-register, and a resting pile morphs only on a host-routed grab -- which leaves GC as the
// death mode AddToRoot stops. The confound is that the dup this comes from was an UNROOTED
// client-spawned pile, so the proxy's survival proves rooting stops GC for an actor with no
// blueprint, not that a rooted REAL pile with a live ubergraph stays inert. Excite that path.
//
// THE PROBE, behind [dev] native_pile_inert_probe=1: about 8 s after the world settles it spawns
// ONE real actorChipPile_C in front of the player, forces the actual nativization recipe
// (collision on, AddToRoot, tick off, Movable) and logs IsLive and the class once a second for a
// minute. Staying live as actorChipPile_C is GO. Going not-live despite AddToRoot is NO-GO, the
// blueprint self-destructs; a class change is NO-GO too. Read-only, cleans up, game thread.

#pragma once

namespace coop::dev::native_pile_inert_probe {

void Install();
void Tick(bool connected, bool isHost);

}  // namespace coop::dev::native_pile_inert_probe
