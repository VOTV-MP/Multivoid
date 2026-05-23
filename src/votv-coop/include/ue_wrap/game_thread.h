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
// false, the original runs normally. Multiple interceptors are NOT supported;
// a second SetInterceptor overwrites.
//
// Use case: substitute a UFunction body for SPECIFIC objects (filter on `self`
// inside cb). Born from Bug 2 -- the kerfur AnimBP's BlueprintUpdateAnimation
// writes spd=0 on a null-Pawn puppet, clobbering our streamed locomotion data.
// Intercepting BUA for the puppet's AnimInstance lets us write spd OURSELVES
// in BUA's slot, with no race against EvaluateGraphExposedInputs.
//
// Performance: the detour does ONE pointer compare per ProcessEvent dispatch
// (ProcessEvent is hot -- thousands of calls/sec -- so no map lookup, no
// allocation, no virtual). Game-thread-only (interceptors must NOT post tasks
// or call CallFunction recursively without re-entrancy care; the detour's
// t_inPump guard does NOT shield interceptor execution from re-entry).
using UFunctionInterceptor = bool(*)(void* self, void* params);
void SetInterceptor(void* targetUFunction, UFunctionInterceptor cb);
void ClearInterceptor();

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
// table (currently 16) comparing the dispatched function pointer against each
// registered target. 16 pointer compares per dispatch -- same cost class as
// the existing single-target SetInterceptor pointer compare, and unlike a
// map lookup involves no allocation or hashing.
//
// Thread-safety: registration uses an atomic store so the detour reads a
// consistent state. The table is fixed-size; no rehash, no realloc. Game-
// thread-only is NOT required for the observer callback itself, but the
// ProcessEvent detour always fires on the dispatching thread (usually
// game thread; sometimes a task-graph worker for parallel anim).
using ProcessEventObserverFn = void(*)(void* self, void* function, void* params);
inline constexpr int kMaxObservers = 16;

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

}  // namespace ue_wrap::game_thread
