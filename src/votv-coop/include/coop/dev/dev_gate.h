// coop/dev/dev_gate.h -- the CLIENT lockout for every dev feature.
//
// User rule (2026-06-12): "The dev features shouldn't work when you're
// client. Strictly." Dev tools exist for development and for a HOST
// administering their own world; a JOINED CLIENT firing them (freecam,
// +points into the SHARED balance, vitals refill, local NPC spawns) is
// cheating in someone else's game.
//
// Allowed() is false IFF a coop session is RUNNING and the local role is not
// Host. Solo / pre-session = allowed (dev testing); host = allowed (their
// server, their rules). The F1 menu hides its dev tree off this, but UI
// hiding is NOT the gate -- every mutating dev entry point checks it itself
// (hotkeys, the ini boot toggles, and a patched menu all bypass UI).

#pragma once

namespace coop::net {
class Session;
}

namespace coop::dev_gate {

// Wire the live session. Harness StartCoopSession, alongside the other
// dev-module SetSession calls. Thread-safe.
void SetSession(coop::net::Session* session);

// False iff a session is running and we are not its host. Thread-safe
// (atomic loads only) -- callable from the render thread, hotkey threads,
// and the game thread alike.
bool Allowed();

}  // namespace coop::dev_gate
