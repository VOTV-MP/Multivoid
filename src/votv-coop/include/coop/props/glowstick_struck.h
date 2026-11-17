// coop/props/glowstick_struck.h -- which glowsticks wait for, or hang from, which struck prop.
//
// A glowstick stuck to a keyed, simulating prop is restored by the game's own processKeys through the prop's key
// (coop/props/glowstick_sync), and that call shows the game's "INVALID" hint and makes no constraint when the key names
// nothing here. A stick whose struck prop is not bound on this peer yet (a joiner's world, a prop born later) WAITS for
// it: the key index's table version is read once a tick and the wait is retried only when it has moved (the retry
// shape of coop/interactables/garbage_bin_sync, whose container state waits for its props). A wait ends when its
// glowstick dies, when a newer record for it lands, when the glowstick is released, or with the session.
// The same unit keeps the other half of the relation, the glowsticks attached to each struck key, so that the host,
// told the struck prop was destroyed, can publish each one's record. Game thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop::glowstick_struck {

// Whether a live prop is bound under `key` on this peer now.
bool IsBound(const std::wstring& key);

// `glowstick` waits for the prop under `key` (replaces any wait it has).
void Wait(void* glowstick, const std::wstring& key);

// `glowstick` hangs from the prop under `key` (replaces any it has).
void Attach(void* glowstick, const std::wstring& key);

// The glowstick is not stuck to a keyed prop any more: its wait and its attachment end.
void Forget(void* glowstick);

using ReadyFn = void (*)(void* glowstick);

// Per tick: when the key index's table has moved since the last walk, each wait whose prop is bound now is handed to
// `ready` (and is no longer waiting); a wait whose glowstick is gone is dropped. A world change drops every wait.
void TickWaits(ReadyFn ready);

// Whether any glowstick hangs from a keyed prop (the destroy seam asks first: it fires for every destroy).
bool AnyAttached();

// The glowsticks that hung from the prop under `key`, taken out of the index (live ones only).
void TakeAttached(const std::wstring& key, std::vector<void*>& out);

// Session teardown.
void Clear();

// Counts for the session's summary line.
uint32_t WaitsStarted();

}  // namespace coop::glowstick_struck
