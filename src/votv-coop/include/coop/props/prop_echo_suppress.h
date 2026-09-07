// coop/prop_echo_suppress.h -- one-shot echo-suppression sets, so a spawn or destroy that arrived
// from the OTHER end of the wire is not broadcast back. The receiver side (coop::remote_prop's
// OnSpawn and OnDestroy) calls Mark*; the symmetric observer in coop::prop_lifecycle calls
// Consume*. Without them our own receiver-applied spawn or destroy re-broadcasts to the original
// sender, and the packets ping-pong.
//
// These are internals shared by exactly two translation units, prop_lifecycle and remote_prop, and
// they live in their own header so remote_prop's public surface stays free of them.
//
// Game thread only. Each set's capacity is bounded internally, and on overflow the set is cleared:
// a one-shot stale lookup on a never-consumed entry is harmless, since it only lets a wire-induced
// spawn re-broadcast once and the other side de-dupes it by key.

#pragma once

#include <string>

namespace coop::prop_echo_suppress {

void MarkIncomingSpawn(void* actor);
bool ConsumeIncomingSpawn(void* actor);
// Non-destructive membership check. The FinishSpawningActor callback must EXCLUDE wire and display
// mirror spawns, which are marked before Finish, WITHOUT eating the mark that the Init POST
// observer consumes on its own path.
bool PeekIncomingSpawn(void* actor);
void MarkIncomingDestroy(void* actor);
bool ConsumeIncomingDestroy(void* actor);

// ---- the ARBITER-CONSUMED key ---------------------------------------------------------------
//
// "I, the host, already destroyed the prop with this save key myself, as the authority's half of a
// transaction a client asked for. The client's own destroy for it is following behind me on the
// same lane, and it is an ECHO."
//
// KEYED BY SAVE KEY, not by actor pointer, and that is the whole reason it exists: by the time the
// echo arrives the actor is gone, so there is no pointer left to mark. Without it the echo reaches
// the destroy receiver's key fallback, misses the index -- we evicted the key when we destroyed it
// -- and pays a full ue_wrap::prop::FindByKeyString walk of GUObjectArray to discover what the host
// already knew, turning an O(1) index hit into a guaranteed cold scan on EVERY arbiter-performed
// transaction.
//
// One-shot and capped like the pointer sets above; a stale entry that is never consumed costs one
// missed short-circuit, never a wrong destroy. Game thread only.
void MarkArbiterConsumedKey(const std::wstring& key);
bool ConsumeArbiterConsumedKey(const std::wstring& key);

// Mirror-spawn re-entrancy scope. The receiver's BeginDeferred UFunction call dispatches through
// ProcessEvent, so the BeginDeferred POST observers -- host_spawn_watcher's ambient broadcaster
// among them -- fire INSIDE it, before MarkIncomingSpawn can run, because the actor only exists
// once Begin returns. The ambient broadcaster is peer-symmetric, so a mirror spawn of an ambient
// class would re-broadcast and ping-pong. The receiver wraps its spawn call in this scope and the
// broadcaster checks it. Game thread only: the POST fires synchronously inside the wrapped call.
class ScopedMirrorSpawn {
 public:
    ScopedMirrorSpawn();
    ~ScopedMirrorSpawn();
    ScopedMirrorSpawn(const ScopedMirrorSpawn&) = delete;
    ScopedMirrorSpawn& operator=(const ScopedMirrorSpawn&) = delete;
};
bool InMirrorSpawnScope();

}  // namespace coop::prop_echo_suppress
