// coop/dev/end_play_probe.h -- [dev] measures the actor end-of-play seam (ue_wrap/engine/actor_end_play)
// against the K2_DestroyActor seam it is meant to replace. Every end of play is counted by reason, and
// every destroyed one is matched to the K2_DestroyActor call it ran inside. The two kinds of miss are
// named by class. An end of play with no K2_DestroyActor around it is a destroy only the new seam sees:
// a lifespan, a fall out of the world, a native Destroy, or a destroy inside another hook's callback. A
// K2_DestroyActor with no end of play is an actor that never began play, one already dying, or one
// whose destroy was asked for inside its own BeginPlay: the engine carries that one out natively once
// BeginPlay returns, so the same actor also counts as an end of play with no K2_DestroyActor. Armed
// per run by end_play_probe=1; read-only.
#pragma once

namespace coop::dev::end_play_probe {

// The pump tick's entry: one latched flag read when off. Installs its sink and its K2_DestroyActor
// post-hook once, classifies the destroys earlier ticks left unmatched, and logs a summary every 30 s.
// Game thread.
void Tick();

}  // namespace coop::dev::end_play_probe
