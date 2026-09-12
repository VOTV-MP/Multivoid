// ue_wrap/core/script_gate.h -- observe and cancel a Blueprint function body, per call, on every
// route. Every script function body the VM runs, however it was reached (ProcessEvent, a
// Blueprint's own local or virtual call, a call through a context switch, an ubergraph entry),
// executes inside one engine function, the VM's local script loop, which receives the frame
// already built: the instance, the function, the evaluated parameters, the caller's frame. A
// MinHook detour on that loop is therefore the one seam that sees every call with its arguments
// and can refuse to run the body. Consumers watch a UFunction (exact object) or a function name
// (every class's function of that name), get a pre callback with the frame and a verdict, and a
// post callback after the body. The MTA shape is the multiplayer_sa hook layer: a detour at the
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
    void*    stack;           // the raw FFrame, for OutParamPtr
    int      tag;             // the watch's tag, echoed back
    int      depth;           // nesting of watched bodies on this thread; 1 = outermost
    bool     fromOurCode;     // inside a reflection::CallFunction of ours
};

// Cancel skips the body: the return value and every out parameter keep whatever the caller
// initialised them to, and the post callbacks do not fire. Run executes it.
enum class Verdict : uint8_t { Run, Cancel };

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
// function of a subclass as it does for the base one, which an exact watch on the base cannot.
// `name` must have static lifetime. The name resolves on the game thread (the string-to-name
// conversion dispatches ProcessEvent), so the watch is inert until ResolvePendingNames has run;
// registration posts one attempt and a game-thread tick drives the rest. Any thread.
bool WatchName(const wchar_t* name, int tag, PreFn pre, PostFn post);
void ResolvePendingNames();

// The session gate: disabled (the default, the solo single-player state) the detour pays one
// load and a branch per call and never consults the tables. Any thread.
void SetEnabled(bool on);
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
