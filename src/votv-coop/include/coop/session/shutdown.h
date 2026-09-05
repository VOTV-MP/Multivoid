// coop/session/shutdown.h -- centralised close and process-exit handling. Our ProcessEvent
// detour stays installed through the engine's teardown, which keeps firing ProcessEvent
// calls during GC, RHI and world teardown after the window close; those land in our
// trampoline, an observer body reads half-destroyed object memory, a fault inside the
// trampoline interacts badly with the loader lock, and the OS spins the process with the
// engine's whole working set still mapped. Detached worker threads also loop on a sleep with
// no exit condition and keep posting game-thread tasks into a dead pump. The fix: subclass
// the game window's procedure and, on the close message, run the shutdown before the engine
// starts its teardown calls, in this order: set the flag, stop the net session (joining the
// net thread), sleep briefly so polling worker loops observe the flag and exit, disable the
// ProcessEvent patch so later teardown calls hit the original engine code, then lift every
// remaining patch. Nothing is freed on this path: MinHook is never uninitialised and no hook
// removed, since both free the trampoline a thread may still be returning through (see
// ue_wrap/core/hook.h). Every infinite worker loop polls the flag. Nothing waits from
// DllMain under the loader lock, and nothing swallows the detour's fault: it is removed first.

#pragma once

namespace coop::net { class Session; }

namespace coop::shutdown {

// The global shutdown flag: tripped once (by the close subclass, the process-detach last
// resort, or a manual DoShutdown) and never cleared, since the process is going down. The
// flag is private to shutdown.cpp; this is its only public accessor.
bool IsShuttingDown();

// Install the close subclass on the game window. Idempotent; called once at boot after the
// game window exists (the harness locates the window). Holds the session pointer so the
// close handler can stop it.
void Install(coop::net::Session* session);

// Set the window title to the host or client form depending on the session role.
// Idempotent, and it fires only once the session has started (the role is the default before
// the start, so it defers until running). Called per tick from the boot loop; a no-op until
// the conditions hold. Separate from Install, which runs before the session start and would
// always see the host role.
void UpdateWindowTitle();

// Run the cleanup sequence: the flag, the session stop, the sleep, disable the ProcessEvent
// patch, lift every remaining patch. Frees nothing (see hook.h). Idempotent; safe from any
// thread, the internal mutex serialises. Not callable from process detach; see
// PersistAtProcessExit.
void DoShutdown();

// The only teardown work the process-detach path may do: write the host's per-peer inventory
// to disk, and nothing else. The detach branch once ran the full shutdown under a claim that
// it only set a flag and uninstalled the detour; it does far more, and seven of the
// operations it reaches are hostile under the loader lock: it takes the slow mutex (at
// process exit the owner may be a thread Windows already terminated, and such a mutex never
// unlocks); the session stop joins the net thread, closes connections with a network-I/O
// linger loop, and resets the signaling socket; a sleep; the game-thread uninstall reaches
// MinHook's thread freeze (a toolhelp snapshot plus thread suspends) with another sleep; and
// the blanket disable reaches the same freeze, a documented deadlock risk on this path. None
// of it is useful here: the loader pins the module before anything else, so this branch runs
// only at process exit, where Windows has already terminated every other thread; quiescing
// waits on the dead, persisting does not. So persist and get out of the way, not gated on
// the shutdown latch (a flush terminated mid-teardown may never have happened, and
// re-flushing is free, since it self-guards on its dirty bit); the flush takes no locks. A
// hard crash delivers no detach, so the inventory write-rate window is a separate defect.
void PersistAtProcessExit();

}  // namespace coop::shutdown
