// coop/prop_echo_suppress.h -- one-shot echo-suppression sets used by the
// host/client prop_lifecycle observers to skip broadcasting a spawn/destroy
// that originated from the OTHER end of the wire (the receiver-side OnSpawn
// / OnDestroy in coop::remote_prop calls Mark*; the symmetric observer in
// coop::prop_lifecycle calls Consume*).
//
// Without this, our own receiver-applied spawn/destroy would re-broadcast
// to the original sender = packet ping-pong.
//
// These are implementation details shared by exactly two TUs
// (prop_lifecycle and remote_prop). They live in a dedicated header
// rather than in coop/remote_prop.h to keep remote_prop's public API
// surface free of internals.
//
// Game-thread-only access. Set capacity is bounded internally; on overflow
// the set is cleared (a one-shot stale lookup on a never-consumed entry
// is harmless -- it lets a wire-induced spawn re-broadcast once, which
// the OTHER side de-dupes via FindByKeyString).

#pragma once

namespace coop::prop_echo_suppress {

void MarkIncomingSpawn(void* actor);
bool ConsumeIncomingSpawn(void* actor);
// Non-destructive membership check (v106 spawn-seam): the FinishSpawningActor
// Func callback must EXCLUDE wire/display mirror spawns (marked before Finish)
// WITHOUT eating the mark the Init-POST observer consumes on its normal path.
bool PeekIncomingSpawn(void* actor);
void MarkIncomingDestroy(void* actor);
bool ConsumeIncomingDestroy(void* actor);

}  // namespace coop::prop_echo_suppress
