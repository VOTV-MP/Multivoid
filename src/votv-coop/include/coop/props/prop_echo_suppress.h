// coop/props/prop_echo_suppress.h -- the actors this peer spawned or destroyed on the wire's word, so
// the observers that broadcast a local spawn or destroy leave them alone and the packets do not
// ping-pong. A receiver marks an actor before the call that could trip an observer; the observers
// ask. Internals of the prop lane's receivers and observers, in their own header so remote_prop's
// public surface stays free of them.
//
// A mark names the OBJECT: its slot in the object array and the slot's serial, drawn as the engine's
// weak pointer draws one. The engine resets the serial when the object is freed, so a successor at a
// recycled address or in a reused slot never matches, and a mark holds for its actor's whole life
// with nothing to repay: a wire mirror stays one however late a drain asks. Keyed by pointer and
// repaid one-shot by an observer that never runs for a mirror, a stale mark at a recycled address
// passed a real birth off as an echo. Game thread only; each set prunes its dead slots as it doubles.

#pragma once

#include <string>

namespace coop::prop_echo_suppress {

// This peer is spawning `actor` as a mirror, of a wire spawn or a held item on display: marked before
// the finish, whose observers ask, and true while the actor lives.
void MarkMirrorSpawn(void* actor);
bool IsMirrorSpawn(void* actor);

// This peer is destroying `actor` on the wire's word, or as its own cleanup that no peer should hear:
// marked before the destroy, whose observer asks.
void MarkIncomingDestroy(void* actor);
bool IsIncomingDestroy(void* actor);

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
// One-shot, and cleared whole at its cap of 256: an entry lost that way, or never consumed, costs one
// missed short-circuit, never a wrong destroy. Game thread only.
void MarkArbiterConsumedKey(const std::wstring& key);
bool ConsumeArbiterConsumedKey(const std::wstring& key);

// Mirror-spawn re-entrancy scope. The receiver's BeginDeferred UFunction call dispatches through
// ProcessEvent, so the BeginDeferred POST observers -- host_spawn_watcher's ambient broadcaster
// among them -- fire INSIDE it, before MarkMirrorSpawn can run, because the actor only exists
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
