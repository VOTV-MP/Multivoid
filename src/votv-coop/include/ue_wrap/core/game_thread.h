// ue_wrap/game_thread.h -- work on the engine's game thread. UObject::ProcessEvent, and so
// reflection::CallFunction, must run there; a UFunction called from our own thread races the
// engine. The engine calls ProcessEvent constantly during play, always on the game thread, so a
// detour on it is a free per-call game-thread callback: it drains a posted-task queue
// (reentrancy-guarded, so a task that calls CallFunction does not recurse the pump), fires the
// interceptors and observers, then forwards to the real ProcessEvent through the trampoline.
// Requires reflection::Resolve to have found ProcessEvent.

#pragma once

#include <functional>

namespace ue_wrap::game_thread {

using Task = std::function<void()>;

// Install the ProcessEvent hook. Idempotent; false if ProcessEvent is unresolved or the hook
// fails.
bool Install();

// Remove the hook. Posted tasks no longer run after this.
void Uninstall();

bool IsInstalled();

// Arm a transparent bypass for `ms` milliseconds (0 or less clears it). While armed the detour
// forwards straight to the engine, skipping the interceptors, the observers, the task pump, the
// diagnostics and the crash-firewall SEH; it expires on its own, with no thread to clear it. The
// flee from a dying world uses it: the menu transition's teardown of the gameplay world hangs
// when it dispatches EndPlay through our detour per dying actor. Lock-free.
void SetTransparentBypass(int ms);

// The condition-based variant: the bypass holds over the world teardown and lifts the instant
// ProcessEvent dispatches `resumeOnFunction`. For the death flee that is ui_menu_C::Tick, the
// first menu-widget tick, so the menu injection's observer runs on the first menu frame with no
// fixed-timer gap. `maxMs` is only a ceiling for a function that never dispatches; a null
// function behaves as SetTransparentBypass(maxMs). Lock-free.
void SetTransparentBypassUntil(void* resumeOnFunction, int maxMs);

// Queue `task` to run on the game thread inside the next ProcessEvent call. Thread-safe,
// returns at once, FIFO; a task may post further tasks, which run on a later pump.
void Post(Task task);

// True if the caller is on the game thread (known once the detour has run at least once).
bool IsGameThread();

// True only if the caller is provably not the game thread: the thread id is known and differs.
// Distinct from `!IsGameThread()`, which is also true before the id is known and would
// false-positive at boot. The predicate ue_wrap/core/hot_path_guard.h fires on.
bool IsDefinitelyOffGameThread();

// The number of tasks the dispatcher has run (diagnostics and the self-test).
unsigned long long TasksRun();

// The pre-dispatch interceptor: when ProcessEvent fires for `targetUFunction` the detour calls
// `cb(self, params)`, and a true return skips the original for this dispatch, replacing the
// UFunction's body; false runs it normally. A fixed-size table keyed on the (target, cb) pair,
// so several interceptors may target one UFunction, and any one returning true cancels. Used to
// substitute a body for specific objects (filtered on `self` inside cb): suppressing client-side
// NPC spawns, cancelling the client's weather schedulers. Per dispatch the cost is a Bloom
// rejection for a non-intercepted function and a count-bounded walk on a hit. Registration is
// an atomic store. An interceptor must not post tasks or call CallFunction without re-entrancy
// care: the pump's reentrancy guard does not cover it.
using UFunctionInterceptor = bool(*)(void* self, void* params);
// Capacity is registration-time only; the per-dispatch walk is count-bounded behind the Bloom
// reject, so headroom costs memory (16 bytes a slot), not hot-path time. A static census cannot
// size this: several call sites register in loops over tables, and a guard that cannot register
// fails in the direction of a mirror the authority still owns.
inline constexpr int kMaxInterceptors = 56;

// Register a pre-dispatch interceptor. False if the table is full or an argument is null.
// Several interceptors may target one UFunction, consulted in order until one returns true;
// re-registering the same pair is a no-op.
bool RegisterInterceptor(void* targetUFunction, UFunctionInterceptor cb);

// Remove the (target, cb) interceptor. A no-op if absent.
void UnregisterInterceptor(void* targetUFunction, UFunctionInterceptor cb);

// The observers are side-effect only: the original body always runs. A post-observer runs after
// the original, to read state the BP just wrote (what a physics handle just grabbed); a
// pre-observer runs before, to snapshot state the BP is about to clear (the grabbed component
// before a release). The table is keyed on the (target, cb) pair, so several subsystems can
// observe one UFunction and every cb fires; keyed on the target alone, a second registrant once
// silently replaced the first. The hot path walks the table bounded by the live count, with no
// allocation and no hashing, so the size costs memory only (two tables of 256 slots, 8 KB).
// Registration is an atomic store. The callback fires on the dispatching thread, which is
// usually the game thread and sometimes a task-graph worker for parallel animation.
using ProcessEventObserverFn = void(*)(void* self, void* function, void* params);
inline constexpr int kMaxObservers = 256;

// Register a post-dispatch observer. False if the table is full or an argument is null; any
// thread, before or after Install.
bool RegisterPostObserver(void* targetUFunction, ProcessEventObserverFn cb);

// Register a pre-dispatch observer. The same shape.
bool RegisterPreObserver(void* targetUFunction, ProcessEventObserverFn cb);

// Remove the (target, cb) observer from whichever table holds it; a co-registered observer on
// the same UFunction survives. A no-op if absent.
void UnregisterObservers(void* targetUFunction, ProcessEventObserverFn cb);

// Drop both observer tables. Called from Uninstall.
void ClearAllObservers();

// The perf instrumentation, driven by coop/dev/perf_probe. The detour is the hottest path in the
// program, so its counters are gated: the dispatch count costs one relaxed load when off, and the
// self-time samples one dispatch in 256, bracketing the detour body without the engine's own
// ProcessEvent. The getters return monotonic totals the probe diffs per second. Lock-free.
void SetPerfCounting(bool countDispatches, bool sampleSelfTime);
unsigned long long PeDispatchCountTotal();   // dispatches observed (all threads) since counting was armed
unsigned long long PeDispatchCountGTTotal(); // game-thread subset of the above
// The subset that originated in our code (reflection::CallFunction): real blueprint run on the
// game thread on our behalf, the share of the engine's script load we author.
unsigned long long PeDispatchCountCoopTotal();
unsigned long long PeSelfNsTotal();          // summed detour self-time (ns) over sampled dispatches
// The whole-detour totals. PeSelfNsTotal brackets the body from inside the impl, so the outer
// frame and the SEH frame are excluded by construction; these bracket the outer detour and
// record the engine's own ProcessEvent on the same sampled dispatches, so the true per-dispatch
// cost is (whole minus engine) over the sample count. Sampled 1 in 256 at top level only.
unsigned long long PeWholeNsTotal();         // outer-detour wall time (ns), sampled dispatches
unsigned long long PeEngineNsTotal();        // engine ProcessEvent (ns) within those samples
unsigned long long PeWholeSampleTotal();     // number of whole-detour samples taken
// The top-level dispatch count, the population the whole-detour samples come from and their
// only correct denominator: the all-dispatch count includes nested dispatches, whose time a
// top-level bracket already contains, so dividing by it inflates the result by the nesting
// factor. Counts only while self-timing is armed.
unsigned long long PeTopLevelCountTotal();
unsigned long long PeSelfSampleTotal();      // number of self-time samples taken
unsigned long long PeObserverBodyNsTotal();  // summed observer+interceptor cb-body time (ns)
unsigned long long PeObserverWorstNs();      // worst single cb-body call (ns)
void*              PeObserverWorstFn();       // the UFunction* of that worst call (resolve name via reflection)
int PostObserverCount();                     // live POST-observer slots (the per-dispatch walk length)
int PreObserverCount();                      // live PRE-observer slots
int InterceptorCount();                      // live interceptor slots

}  // namespace ue_wrap::game_thread
