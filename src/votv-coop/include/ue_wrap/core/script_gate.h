// ue_wrap/core/script_gate.h -- observe and cancel a Blueprint function body, per call, on every
// route. Every script function body the VM runs, however it was reached (ProcessEvent, a
// Blueprint's own local or virtual call, a call through a context switch, an ubergraph entry),
// executes inside one engine function, the VM's local script loop, which receives the frame
// already built: the instance, the function, the evaluated parameters, the caller's frame. A
// MinHook detour on that loop is therefore the one seam that sees every call with its arguments
// and can refuse to run the body. Consumers watch a UFunction (exact object) or a function name
// (every class's function of that name, or one class's), get a pre callback with the frame and a
// verdict, and a post callback after the body. The MTA shape is the multiplayer_sa hook layer: a detour at the
// game function's entry, a handler that returns whether the original runs
// (reference/mtasa-blue/Client/multiplayer_sa/CMultiplayerSA.cpp:6355, ProcessCollisionHandler).
// Engine-wrapper layer (principle 7): no gameplay or network logic.

#pragma once

#include <cstdint>

namespace ue_wrap::script_gate {

// A call at the body's entry. `locals` is the parameter frame: a value parameter sits at its
// Offset_Internal (reflection::FindParamOffset), an out parameter's storage is the caller's
// (OutParamPtr). `callerObject` and `callerFunction` name the Blueprint frame that made the
// call; both are null when the call came through ProcessEvent (an engine event, a delegate, a
// timer, or a reflected call of ours). `result` is where the return expression writes, and may
// be null. Fires on the game thread only; an off-thread match is a counted tripwire.
struct Call {
    void*    object;          // Stack.Object, the instance the body runs on (live at entry)
    void*    function;        // Stack.Node, the UFunction
    uint8_t* locals;          // Stack.Locals
    void*    result;          // the return value's destination, or null
    void*    callerObject;    // PreviousFrame->Object, or null
    void*    callerFunction;  // PreviousFrame->Node, or null
    uint8_t* callerLocals;    // PreviousFrame->Locals, which an out argument naming a caller local points into
    void*    stack;           // the raw FFrame, for OutParamPtr
    int      tag;             // the watch's tag, echoed back
    int      depth;           // nesting of watched bodies on this thread; 1 = outermost
    bool     fromOurCode;     // inside a reflection::CallFunction of ours
};

// The watch surface follows the Watch that Relay, Moddy's VOTV mod, publishes in its README: a
// pre phase, a cancelable form that can stop the call, a post phase that only observes, and the
// calling Blueprint frame handed to the handler. Relay's README says cancellation rewrites the
// function's bytecode; this gate leaves the bytecode alone and refuses the body at the loop.
// Links: docs/credits.md.
// Cancel skips the body: the return value and every out parameter keep whatever the caller
// initialised them to, and the post callbacks do not fire. Run executes it.
enum class Verdict : uint8_t { Run, Cancel };

// A callback may call into the engine, a pre callback before its verdict included: it is a plain
// call on the game thread, nothing of the gate is locked around it, and a watched body the call
// reaches fires that body's own callbacks.
using PreFn  = Verdict (*)(const Call&);
using PostFn = void (*)(const Call&);

// Detour the loop. Idempotent; false when the loop cannot be derived from the exec-handler table
// (logged with what each derivation found). Boot, after reflection has resolved.
bool Install();
bool IsInstalled();

// Watch one UFunction. Either callback may be null. Idempotent per (function, tag, pre, post).
// False for a null function, a native function (it never runs through the loop), a full table,
// or a gate that did not install (logged once). Any thread.
bool Watch(void* ufunction, int tag, PreFn pre, PostFn post);
bool Unwatch(void* ufunction, int tag, PreFn pre, PostFn post);

// Watch every function called `name`, on any class: a name watch fires for the overriding
// function of a subclass as it does for the base one, which an exact watch on the base cannot --
// the limit Relay's README Blueprint states for a watch registered on a parent class
// (docs/credits.md). `name` must have static lifetime. The name resolves on the game thread (the
// string-to-name conversion dispatches ProcessEvent), so the watch is inert until
// ResolvePendingNames has run; registration posts one attempt and a game-thread tick drives the
// rest. Any thread.
bool WatchName(const wchar_t* name, int tag, PreFn pre, PostFn post);
void ResolvePendingNames();

// Is that watch LIVE -- registered AND its FName resolved, so the gate really will intercept?
// `WatchName` returns true the moment it registers, and the watch is inert until the resolve has
// run, so a consumer that publishes its own readiness has to ask this instead. `name` is the same
// literal that was registered. Any thread.
bool NameWatchLive(const wchar_t* name, int tag);

// Has that name watch SETTLED: live, or dead for good because its name resolved into a full table
// (logged by the gate)? A consumer that waits on a watch before going on asks this, so a dead watch
// cannot hold it back for ever; NameWatchLive tells the two apart. The same literal. Any thread.
bool NameWatchSettled(const wchar_t* name, int tag);

// Watch the body of function `name` that class `className` owns: a name watch whose entry also
// carries the owning class's name, compared only after the function's name has matched, so other
// classes' bodies of that name are passed over with no callback. The owner is the function's outer,
// the class whose body runs: a subclass that overrides `name` owns its own body and is not matched,
// one that inherits `className`'s body is. For "an instance of the class or below", filter on the
// call's object's class chain in the callback instead, comparing names up the super chain, since a
// class can be a new object in every world. Keyed on names, the watch is registered once and matches
// every incarnation of a class that comes back as a new object. Both names must have static lifetime
// and resolve on the game thread as a name watch's does. The class is matched by its short name, where
// UE4SS matches a hooked function's whole outer path, package included: two loaded classes of one name
// would both match. Any thread.
bool WatchClassName(const wchar_t* className, const wchar_t* name, int tag, PreFn pre, PostFn post);

// Retire a name watch, or a class-scoped one, given the literals, tag and callbacks that registered it;
// false when none matches. Its slots stay keyed, so the probe chain stays walkable and they still
// count against the table's capacity; one retired while its name was pending is dropped at the
// resolve. Any thread.
bool UnwatchName(const wchar_t* name, int tag, PreFn pre, PostFn post);
bool UnwatchClassName(const wchar_t* className, const wchar_t* name, int tag, PreFn pre, PostFn post);

// Is that class-scoped watch LIVE, both of its names resolved? The literals that were registered.
// Any thread.
bool ClassNameWatchLive(const wchar_t* className, const wchar_t* name, int tag);

// How many registered name watches still wait for their FName. At 0 every name watch is settled:
// live, or dead for good because its name resolved into a full table (the gate logs which). A
// consumer re-counting its own watches can stop there. Any thread.
int PendingNameCount();

// The gate runs while anything holds it; with no holder (the default, the solo single-player
// state) the detour pays one load and a branch per call and never consults the tables. The coop
// session holds it for its life (coop/session/subsystems), a save capture for its one call, a dev
// probe that must see outside a session for as long as it runs. No holder turns it off under
// another: the last release does. A switch every consumer set once turned it off in mid-session
// for the lanes that had not re-asserted it. `who` names the holder on the edge's log line. Any
// thread.
void Acquire(const char* who);
void Release(const char* who);
class Hold {
public:
    explicit Hold(const char* who) : who_(who) { Acquire(who_); }
    ~Hold() { Release(who_); }
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
private:
    const char* who_;
};
bool IsEnabled();

// The calling thread's innermost watched body, for a consumer's own downstream seams (a finish
// spawn, a destroy) firing inside the body. `name` is the registered name of a name watch, null
// for an exact watch; gate on `function` or `name`, never on `active` alone, which is true for
// any consumer's watch on this thread.
struct Active {
    bool           active;
    int            tag;
    int            depth;
    void*          object;
    void*          function;
    const wchar_t* name;
};
Active CurrentThreadCall();

// Is `function`'s body running anywhere on this thread's chain of watched bodies -- not only as
// the innermost one -- and, when `callerFunction` is given, was it entered FROM that caller? Null
// `callerFunction` means any caller.
//
// This is what a consumer asking "is my own verb running right now?" must use. Two things it does
// that the innermost-only `CurrentThreadCall` and a consumer-side pre/post counter do not. It sees
// past an inner watched body another consumer owns, which would otherwise hide the outer one. And
// it cannot get stuck open: the answer lives in the RAII scope the gate pushes around the body, so
// it unwinds on every path out, including a fault the ProcessEvent firewall absorbs and a Cancel
// from another consumer -- both of which skip the post callbacks, so a counter incremented in a
// pre would stay raised for the life of the process and answer "my verb is running" forever.
// Game thread in practice (the gate never fires callbacks off it); any thread reads its own chain.
bool IsBodyActive(void* function, void* callerFunction = nullptr);

// The caller's storage for the out parameter at `paramOffset` (an Offset_Internal), or null when
// the frame carries no such record.
uint8_t* OutParamPtr(const Call& call, int32_t paramOffset);

// Counters for the perf probe and the drills. `calls` and `callsGameThread` count only while
// SetPerfCounting is on (an increment on every script call otherwise); the rest always.
struct Stats {
    unsigned long long calls;
    unsigned long long callsGameThread;
    unsigned long long matched;        // watched bodies reached on the game thread
    unsigned long long cancelled;
    unsigned long long offGameThread;  // TRIPWIRE: a watched body off the game thread, skipped
    unsigned long long faults;         // callback faults absorbed
    int  watches;
    int  nameWatches;
    bool enabled;
    bool installed;
};
Stats GetStats();
void SetPerfCounting(bool on);

}  // namespace ue_wrap::script_gate
