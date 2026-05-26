// ue_wrap/game_thread.h -- run work safely on the engine's game thread.
//
// Engine-wrapper layer (principle 7). UObject::ProcessEvent (and therefore
// reflection::CallFunction) MUST run on the game thread -- calling a UFunction
// from our boot thread races the engine and crashes. This module gives us a
// guaranteed game-thread execution context by hooking ProcessEvent itself: the
// engine calls ProcessEvent constantly during play, always on the game thread,
// so our detour is a free per-call game-thread callback.
//
// The detour drains a posted-task queue (reentrancy-guarded so a task that
// calls CallFunction -- re-entering ProcessEvent -- does not recurse the pump)
// then forwards to the real ProcessEvent via the trampoline.
//
// Requires reflection::Resolve() to have found ProcessEvent first.

#pragma once

#include <functional>

namespace ue_wrap::game_thread {

using Task = std::function<void()>;

// Install the ProcessEvent hook (the game-thread anchor). Idempotent.
// Returns false if ProcessEvent is unresolved or the hook fails to install.
bool Install();

// Remove the hook. After this, posted tasks no longer run.
void Uninstall();

bool IsInstalled();

// Queue `task` to run on the game thread, inside the next ProcessEvent call.
// Thread-safe; returns immediately. Tasks run in FIFO order. A task may Post
// further tasks (they run on a subsequent pump, not the current one).
void Post(Task task);

// True if the caller is on the game thread (known once the detour has run at
// least once). Useful for asserting before touching engine state directly.
bool IsGameThread();

// Number of tasks the dispatcher has executed (diagnostics / self-test).
unsigned long long TasksRun();

// Pre-dispatch UFunction interceptor. When ProcessEvent fires with
// `function == targetUFunction`, the detour calls `cb(self, params)` and, if it
// returns true, returns WITHOUT forwarding to the original ProcessEvent --
// effectively replacing the UFunction's body for this dispatch. If cb returns
// false, the original runs normally.
//
// Multiple interceptors supported (fixed-size table, no allocation, no hashing
// -- same shape as the observer tables). Each (target, cb) pair is registered
// independently. If MULTIPLE interceptors fire for the same UFunction in the
// same dispatch (only possible if two registrations share a target), any one
// returning true cancels the original.
//
// Use cases:
//   - Substitute a UFunction body for SPECIFIC objects (filter on `self` inside
//     cb). Born from npc_sync -- suppressing client-side NPC spawns by
//     intercepting the spawn UFunction and returning early.
//   - Client-side weather scheduler suppression -- 5 distinct UFunctions
//     (timerRain/timerLightning/fogEvent/superFogEvent/permaRain_timer) on
//     AdaynightCycle_C all need PRE-cancel on the client; the host runs them
//     all unchanged. The multi-slot table lets one subsystem own multiple
//     slots without a global mutex or rendezvous between subsystems.
//
// Performance: the detour walks the table linearly; cost is O(kMaxInterceptors)
// pointer compares per dispatch. With kMaxInterceptors=16 and ProcessEvent at
// ~thousands/sec the loop is well under 1 us. Empty slots null-check out
// before the (rare) match path.
//
// Thread-safety: registration uses an atomic store so the detour reads a
// consistent state. Game-thread-only is NOT required for the interceptor cb
// itself, but interceptors must NOT post tasks or call CallFunction
// recursively without re-entrancy care; the detour's t_inPump guard does NOT
// shield interceptor execution from re-entry.
using UFunctionInterceptor = bool(*)(void* self, void* params);
inline constexpr int kMaxInterceptors = 16;

// Register a PRE-dispatch interceptor for `targetUFunction`. Returns false if
// the table is full or arguments are null. Idempotent: re-registering the
// same target replaces its cb.
bool RegisterInterceptor(void* targetUFunction, UFunctionInterceptor cb);

// Remove the interceptor (if any) for `targetUFunction`. No-op if absent.
void UnregisterInterceptor(void* targetUFunction);

// Post-dispatch UFunction observers. Unlike SetInterceptor, observers are
// SIDE-EFFECT ONLY -- the original UFunction body always runs. Multiple
// observers can be registered for distinct UFunctions; up to kMaxObservers
// active simultaneously. Each observer matches on a UFunction pointer
// (resolved by the caller via reflection::FindFunction at registration);
// the detour fires every matching observer for the dispatched UFunction.
//
// Two variants:
//   Post-observer: called AFTER the original ProcessEvent. Used to read
//     state the BP just wrote (e.g. read UPhysicsHandleComponent.GrabbedComponent
//     @+176 in a PHC.GrabComponentAtLocation observer to see what was just grabbed).
//   Pre-observer:  called BEFORE the original. Used to snapshot state
//     the BP is about to CLEAR (e.g. read GrabbedComponent in a
//     PHC.ReleaseComponent observer before PhysX clears it).
//
// Performance: on the hot path, the detour walks a fixed-size kMaxObservers
// table comparing the dispatched function pointer against each registered
// target. Still constant-time, no allocation, no hashing. Sizing history:
//   16 (initial) -> 64 (2026-05-25 after subclass-aware Aprop_C::Init
//   override scan ran 54 unique BP subclasses through the table)
//   -> 128 (2026-05-27 after Phase 5W weather added 5 scheduler + 3
//   mutator observers + 1 lightning POST + the existing flashlight/grab
//   subsystems all stacked; 64 silently dropped the last ~10
//   registrations).
// Memory cost: 128 slots * 16 B/slot * 2 tables (post + pre) = 4 KB. Trivial.
// Per-dispatch cost: 128 acquire-loads on the hot path; even at 50 k
// ProcessEvent/s that's well under 1 ms of CPU per second.
//
// Thread-safety: registration uses an atomic store so the detour reads a
// consistent state. The table is fixed-size; no rehash, no realloc. Game-
// thread-only is NOT required for the observer callback itself, but the
// ProcessEvent detour always fires on the dispatching thread (usually
// game thread; sometimes a task-graph worker for parallel anim).
using ProcessEventObserverFn = void(*)(void* self, void* function, void* params);
inline constexpr int kMaxObservers = 128;

// Register a POST-dispatch observer for `targetUFunction`. Returns false
// if the table is full or arguments are null. Safe to call from any
// thread before or after Install().
bool RegisterPostObserver(void* targetUFunction, ProcessEventObserverFn cb);

// Register a PRE-dispatch observer for `targetUFunction`. Same shape.
bool RegisterPreObserver(void* targetUFunction, ProcessEventObserverFn cb);

// Remove ALL observers (post + pre) targeting `targetUFunction`. No-op
// if not registered. Used at session-end / DLL-unload to detach cleanly.
void UnregisterObservers(void* targetUFunction);

// Drop the entire observer table (post + pre). Called from Uninstall().
void ClearAllObservers();

// ---- Diagnostic name-prefix dispatcher ----------------------------------
// Temporary tool for "I don't know which UFunction VOTV dispatches; log the
// names of all that match my filter". Single global filter (max 4 prefixes,
// each up to 31 wchars). On each ProcessEvent dispatch, the detour walks the
// 4-entry prefix table; for any function whose name starts with one of the
// configured prefixes, the configured callback fires with the function's
// FName-resolved wide string. Used when our named observers don't fire and
// we suspect the actual UFunction name differs from the SDK header.
//
// Performance: 4 string compares per ProcessEvent dispatch ONLY if at least
// one prefix is set. Set/cleared in the same atomic-publish pattern as the
// observers. Strip the diagnostic after the actual UFunction names are
// pinned down (RULE 2: this is a debug crutch with a documented retirement).
using ProcessEventNameDiagnosticFn = void(*)(void* self, const wchar_t* funcName, void* params);
inline constexpr int kMaxNameDiagnostics = 4;
inline constexpr int kMaxNameDiagnosticPrefixLen = 32;  // including null terminator

// Add a name-prefix filter. funcName starting with `prefix` triggers `cb`.
// Pass empty `prefix` or null `cb` to clear all entries.
bool SetNameDiagnostic(int slot, const wchar_t* prefix, ProcessEventNameDiagnosticFn cb);
void ClearAllNameDiagnostics();

// 2026-05-26 deep-RE trace: when SetCallTrace(true) is set, every
// ProcessEvent dispatch logs its UFunction name + self pointer at INFO
// level. Used to capture BP call chains when reflection-invocations
// look like they're not running the BP body. ONE-SHOT diagnostic --
// set false when done. Spam-volume can be enormous (UE4 dispatches
// thousands of UFunctions per second).
void SetCallTrace(bool enabled);
bool GetCallTrace();

}  // namespace ue_wrap::game_thread
