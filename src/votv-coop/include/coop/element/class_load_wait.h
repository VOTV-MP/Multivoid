// coop/element/class_load_wait.h -- the one wait for a spawn whose class is still to load.
//
// ue_wrap::class_resolve loads a class this peer has not loaded inside a per-tick time budget, and answers Pending past
// it. A spawn that got Pending waits here and runs again from the frame tail, as an MTA entity waits in the client's
// model request queue and is created when the model arrives (Client CClientModelRequestManager.cpp, Request and
// DoPulse), never dropped.
//
// Game thread only.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace coop::element::class_load_wait {

// The waits are keyed by CLASS: the first waiter of a class asks the resolver again (the load), the others run once it
// answers, so the number of pending loads is the number of distinct classes (the registry's bound) and the spawns waiting
// on one are as many as the element registry allows. There is no queue-full drop.
//
// An eid parked here is MARKED in bind_wait (coop/element/bind_wait.h), the one hold for a message whose prop has no
// actor yet: messages for it wait there with the receivers that hold, a spawn that binds releases them, and a spawn that
// ends without a bind (its class refused or absent, its spawn failing) gives the eid up, so the park ends in exactly one
// of: bound, given up, or cancelled by the destroy of the eid.
//
// Parks the spawn of `eid`, whose class `className` answered Pending. `retry` runs the spawn again; `bound` says after it
// whether the spawn left a mirror for the eid. `site` names the caller (a string literal). After a retry that did not
// park again, a bound spawn releases the mark (OnBound) and any other gives it up.
void ParkSpawn(const char* site, const std::wstring& className, uint32_t eid, std::function<void()> retry,
               std::function<bool()> bound);

// The same wait for a lane with no element id (the owner-entity spawn): `key` is what its destroy carries, and there is
// no bind_wait mark.
void Park(const char* site, const std::wstring& className, uint32_t key, std::function<void()> retry);

// Removes the waiting retry of `site` under `key` (CancelIf: every one `match` accepts) without running it. True when one
// was waiting; the count for CancelIf. The caller says it once and gives the eid up.
bool Cancel(const char* site, uint32_t key);
size_t CancelIf(const char* site, const std::function<bool(uint32_t)>& match);

// Whether a retry of `site` waits under `key`: a caller that ran its spawn again asks whether it parked anew.
bool IsParked(const char* site, uint32_t key);

// Once per frame, after ue_wrap::class_resolve::Tick has renewed the load budget: while the budget has room each class's
// first waiter runs once (a class past the budget waits for the next tick untouched), and
// the rest of its class once it no longer waits. A world change drops every wait. Idle: nothing. Game thread.
void Tick();

}  // namespace coop::element::class_load_wait
