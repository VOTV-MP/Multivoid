// coop/dev/bind_wait_selftest.h -- [dev] the bind wait's rules on stated inputs, run on a client once its
// world is announced. It drives bind_wait (coop/element/bind_wait.h) with synthetic eids from outside every
// range the registry allocates (above kMaxElements, below kInvalidId; check 9 borrows a free host id) and a
// replay that records its calls; its checks are listed in the .cpp. After each check that leaves no hold
// stored, HeldOf == ReplayedOf + DroppedOf + GivenUpOf holds for every kind the checks use.
// "[BIND-WAIT-SELFTEST] PASS" is the --done-marker, "... FAIL" the --fail-marker. It waits up to 600 ticks for
// the session's real holds to clear, else prints "... SKIPPED: real holds present" and does not run. The last
// check Clears the wait, which ends the marks of the whole session. With bind_wait_red=1 only two checks run
// (a hold for an eid not deferred, and for a deferred one, is refused), the line is "RED ARM" (no PASS), the
// red: a drill expecting PASS must fail. Run with bind_wait_selftest=1; not with bind_wait_mark_unknown.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::bind_wait_selftest {

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's client runs the checks again.
void OnDisconnect();

}  // namespace coop::dev::bind_wait_selftest
