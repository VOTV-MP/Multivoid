// coop/dev/native_pile_inert_probe.h -- GO/NO-GO gate for nativizing the trash pile mirror.
//
// THE QUESTION: can we replace the bare AStaticMeshActor PROXY (coop/trash_proxy) with a ROOTED real
// actorChipPile_C native for resting/runtime/re-pile piles -- which would restore the native hover GUI +
// collision + rotation + occlusion + movement-block FOR FREE (a real chipPile IS int_player_C by
// construction)? The static RE (2026-06-30) found NO autonomous self-init path (no LifeSpan, no despawn
// timer, no self-register, a resting pile morphs ONLY on a host-routed grab) and said GC is the death
// mode that AddToRoot stops. BUT the decisive confound: the original 2026-06-21 dup was an UNROOTED
// client-spawned chipPile, so "rooting fixes it" is a HYPOTHESIS -- the proxy's 870-instance non-death
// proves rooting stops GC for a NO-BP actor, NOT that a rooted REAL chipPile with a live ubergraph stays
// inert. The untraced BeginPlay (idx 2498) + init() are a [V]-as-proof gap. PROBE-DON'T-GUESS: excite
// that exact path. [[feedback-probe-dont-guess-rule]] [[feedback-rule2-exempts-probes-diagnostics-tools]]
//
// THE PROBE: gated by multivoid.ini [dev] native_pile_inert_probe=1. ~8s after the world settles it
// SpawnActor's ONE real actorChipPile_C ~2.5m in front of the player and forces the ACTUAL nativization
// recipe -- COLLISION-ON (native trace + movement-block), AddToRoot, tick-off, Movable -- then logs
// `[INERT-PROBE]` IsLive + class once a second for 60s. (NoCollision was already proven inert 39s on
// 2026-06-30; this run excites the collision-ON CONTACT path -- the one remaining "should hold" that is
// unverified on the same axis as the original confound -- and doubles as a live preview: aim at it for
// the native hover GUI, walk into it for movement-block.) Verdicts:
//   - stays live + 'actorChipPile_C' for 60s -> GO: a rooted runtime native is INERT (nativize; retire
//     the bare proxy + the rotation-fix + the hover-GUI subsystem under RULE 2).
//   - goes NOT-LIVE despite AddToRoot -> NO-GO: the BP self-destructs (autonomy, not GC).
//   - class changes (self-morph) -> NO-GO.
// The bare proxy is then proven load-bearing -> push the rotation-fix + finish the hover-GUI.
//
// Read-only diagnostic; cleans up its actor at the end. Best run on a HOST or a standalone New Game (no
// client-reconcile confound). Game thread.

#pragma once

namespace coop::dev::native_pile_inert_probe {

void Install();
void Tick(bool connected, bool isHost);

}  // namespace coop::dev::native_pile_inert_probe
