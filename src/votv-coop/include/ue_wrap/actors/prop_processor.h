// ue_wrap/actors/prop_processor.h -- the world's prop processor (propProcessor_C) and its pass count.
// The processor computes every prop's convex volume, then broadcasts intComs_propRenderer_finishProps
// to every int_coms actor; containers wait on that broadcast before their inventory initializes and
// rolls its loot. Its reflected int `fins` counts the processor's finished phases: the signals phase,
// the audio import, and the one that follows the broadcast. The game's own finish() waits for all
// three, so `fins >= 3` is the engine's statement that the world's props have been processed. This is
// what the host's world-settle edge reads. Engine-wrapper layer (principle 7): no wire, no arbitration.
// The reflected offset is resolved once and cached; -1 means looked and failed, never guessed. Game thread.
#pragma once

namespace ue_wrap::prop_processor {

// The running world's one propProcessor_C, live, or null when none is loaded.
void* WorldPropProcessor();

// The processor's `fins`, or -1 when `processor` is null or the field does not resolve.
int Passes(void* processor);

// Whether all three phases have finished: Passes >= 3, the game's own finish() gate.
bool Finished(void* processor);

}  // namespace ue_wrap::prop_processor
