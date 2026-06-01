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

// Arm a transparent bypass for `ms` milliseconds (<=0 clears it immediately).
// While armed, the ProcessEvent detour forwards STRAIGHT to the real engine,
// skipping all our logic (interceptors, observers, the posted-task pump,
// diagnostics, and the outer crash-firewall SEH). It auto-expires after `ms` so
// no thread is needed to clear it. Use to flee a dying world: VOTV's
// transition("/Game/menu") teardown of the 50k-object untitled_1 world hangs when
// it dispatches ReceiveEndPlay/EndPlay through our detour per dying actor; arming
// the bypass for the teardown window lets the travel complete natively, then the
// fresh menu world runs with our layer fully normal again. Lock-free.
void SetTransparentBypass(int ms);

// Queue `task` to run on the game thread, inside the next ProcessEvent call.
// Thread-safe; returns immediately. Tasks run in FIFO order. A task may Post
// further tasks (they run on a subsequent pump, not the current one).
void Post(Task task);

// True if the caller is on the game thread (known once the detour has run at
// least once). Useful for asserting before touching engine state directly.
bool IsGameThread();

// True only if we can PROVE the caller is NOT the game thread -- i.e. the game
// thread id is KNOWN (the detour has run at least once) AND the current thread
// differs from it. Distinct from `!IsGameThread()`, which is ALSO true before
// the id is known (id==0) and would therefore false-positive at boot. This is
// the predicate the HotPathGuard (ue_wrap/hot_path_guard.h) fires on: it must
// never trip for "don't-know-yet", only for an actual proven off-thread access
// to a game-thread-only-by-convention side-table.
bool IsDefinitelyOffGameThread();

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
// the table is full or arguments are null. Multiple distinct interceptors may
// target one UFunction (keyed on the (target, cb) pair); FireInterceptors
// consults each and stops at the first returning true. Re-registering the exact
// same (target, cb) is an idempotent no-op.
bool RegisterInterceptor(void* targetUFunction, UFunctionInterceptor cb);

// Remove the specific (targetUFunction, cb) interceptor. No-op if absent.
void UnregisterInterceptor(void* targetUFunction, UFunctionInterceptor cb);

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
// MULTIPLE observers per UFunction are supported: the table is keyed on the
// (target, cb) PAIR, so several subsystems can observe the SAME UFunction and
// the detour fires ALL of their cbs. (2026-05-30: keying on target alone let
// the second registrant silently overwrite the first -- weather_lightning's
// BeginDeferred POST clobbered npc_sync's NPC actor-bind, killing host
// destroy-sync. Unregister is correspondingly (target, cb)-specific.)
//
// Performance: on the hot path, the detour walks the table comparing the
// dispatched function pointer against each registered target, bounded by the
// live count (found==active early-out), not kMaxObservers. Constant-time, no
// allocation, no hashing. Sizing history:
//   16 (initial) -> 64 (2026-05-25 after subclass-aware Aprop_C::Init
//   override scan ran 54 unique BP subclasses through the table)
//   -> 128 (2026-05-27 after Phase 5W weather added 5 scheduler + 3
//   mutator observers + 1 lightning POST + the existing flashlight/grab
//   subsystems all stacked; 64 silently dropped the last ~10 registrations)
//   -> 256 (2026-05-30: ~73 POST observers steady-state already, and the
//   multi-observer-per-target fix adds a slot per co-registration; headroom).
// Memory cost: 256 slots * 16 B/slot * 2 tables (post + pre) = 8 KB. Trivial.
// Per-dispatch cost is count-bounded, so the larger table does not slow the
// common case.
//
// Thread-safety: registration uses an atomic store so the detour reads a
// consistent state. The table is fixed-size; no rehash, no realloc. Game-
// thread-only is NOT required for the observer callback itself, but the
// ProcessEvent detour always fires on the dispatching thread (usually
// game thread; sometimes a task-graph worker for parallel anim).
using ProcessEventObserverFn = void(*)(void* self, void* function, void* params);
inline constexpr int kMaxObservers = 256;

// Register a POST-dispatch observer for `targetUFunction`. Returns false
// if the table is full or arguments are null. Safe to call from any
// thread before or after Install().
bool RegisterPostObserver(void* targetUFunction, ProcessEventObserverFn cb);

// Register a PRE-dispatch observer for `targetUFunction`. Same shape.
bool RegisterPreObserver(void* targetUFunction, ProcessEventObserverFn cb);

// Remove the specific (targetUFunction, cb) observer from whichever table
// (post or pre) holds it. cb-specific so a co-registered observer on the same
// UFunction survives. No-op if not registered. Used at session-end / DLL-unload
// and on rollback of a half-installed observer pair.
void UnregisterObservers(void* targetUFunction, ProcessEventObserverFn cb);

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
