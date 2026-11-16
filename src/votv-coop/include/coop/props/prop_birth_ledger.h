// coop/props/prop_birth_ledger.h -- the host birth path's traffic ledger (a diagnostic).
//
// The birth path of coop/props/prop_save_data publishes a record for EVERY prop the host gives birth to at runtime, not
// only discs, so what it costs is measured rather than assumed: one line a minute with the window and the session so
// far (a rig peer is killed, not disconnected, so the running total is what survives), and the session again at
// teardown. Game thread.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coop::prop_birth_ledger {

// A birth whose class carries no save state of its own.
void NoteUncovered();

// A covered birth the publisher sent nothing for.
void NoteDeclined();

// A covered birth whose record of `bytes` was sent.
void NoteSent(const std::wstring& className, size_t bytes);

// Host only, once a second: the minute's line and the running session line under it.
void Tick(uint64_t nowMs);

// The session ended: the session line is said and the ledger cleared.
void OnDisconnect(uint64_t nowMs);

}  // namespace coop::prop_birth_ledger
