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

#include "ue_wrap/facts/function_row.h"

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
    uintptr_t* scopeMark;     // the scope word of this call's dispatch (MarkScope); null for a post
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

// Patch the loop, found from the image alone (script_loop::ByCode), behind a guard that forwards every
// body until Install arms it. At the loader's call, on its thread, before any other hooker in the
// process runs (bootstrap/boot.cpp says why). Idempotent; false when the loop is not found (logged).
bool Patch();

// Arm the patched loop, once the exec-handler table is filled and its two local-call handlers name the
// same function. Idempotent; false, and every watch refused, when nothing was patched or the handlers
// name another function (logged). Boot, after reflection has resolved.
bool Install();
bool IsInstalled();

// The VM's exec-handler table (GNatives), 256 entries, one per opcode, each evaluating one expression
// of the bytecode stream into its result as FFrame::Step calls it: (context, FFrame&, result). Set by
// Install, the arm step, once the table is filled (script_loop::TableFilled), and kept when a later check
// refuses the loop; null before.
// ue_wrap/core/ufunction_hook steps a native call's parameters through it. Any thread.
using ExecHandler = void(__fastcall*)(void* context, void* stack, void* result);
const ExecHandler* ExecHandlers();

// Every watch is registered through its row (ue_wrap/facts/function_row.h), so the dependency is declared
// once and the row records whether the gate took it.
//
// Watch one UFunction, a row resolved to it. Either callback may be null. Idempotent per (function, tag,
// pre, post). False for a row not resolved yet (nothing is asked of the gate, nothing recorded), a native
// function (it never runs through the loop), a full table, or a gate that did not install (logged once).
// Any thread.
bool Watch(const facts::Resolved& fn, int tag, PreFn pre, PostFn post);
bool Unwatch(const facts::Resolved& fn, int tag, PreFn pre, PostFn post);

// A name watch. An AnyClass row watches every function of its name on any class: it fires for a
// subclass's overriding function as for the base one, which an exact watch on the base cannot -- the
// limit Relay's README Blueprint states for a watch registered on a parent class (docs/credits.md). A
// Declared row watches the body its class owns: the name watch's entry also carries the class's name,
// compared after the function's name matched, so other classes' bodies of that name pass with no
// callback; a subclass that overrides the name owns its own body and is not matched, one that inherits
// the class's body is. For "an instance of the class or below", filter on the call's object's class
// chain in the callback, comparing names up the super chain, since a class can be a new object in every
// world. Keyed on names, a watch is registered once and matches every incarnation of its class. The class
// is matched by its short name, where UE4SS matches a hooked function's whole outer path: two loaded
// classes of one name would both match. Another mode is refused, logged. Any thread.
bool WatchName(const facts::Function& row, int tag, PreFn pre, PostFn post);

// The names resolve on the game thread (the string-to-name conversion dispatches ProcessEvent), so a name
// watch is inert until ResolvePendingNames has run; registration posts one attempt and a game-thread tick
// drives the rest. The name resolves whatever it spells, since the conversion adds it, so a Declared
// row's class is asked too, at the resolve when it is loaded, else when the class set next moves: one that
// declares no such function, or declares it native, kills the watch, logged.
void ResolvePendingNames();

// Retire a name watch given its row, tag and callbacks; false when none matches. Its slots stay keyed, so
// the probe chain stays walkable and they still count against the table's capacity; one retired while its
// name was pending is re-keyed disabled at the resolve, so watching it again re-enables its slot. Any
// thread.
bool UnwatchName(const facts::Function& row, int tag, PreFn pre, PostFn post);

// Is that name watch LIVE -- registered AND its names resolved, so the gate really will intercept?
// WatchName returns true the moment it registers, so a consumer that publishes its own readiness asks
// this instead. Any thread.
bool NameWatchLive(const facts::Function& row, int tag);

// Has it SETTLED: live, or dead for good because its name resolved into a full table or its class
// declares no such function (logged by the gate)? A consumer waiting on a watch asks this, so a dead watch
// cannot hold it back for ever; NameWatchLive tells the two apart. Any thread.
bool NameWatchSettled(const facts::Function& row, int tag);

// How many registered name watches still wait for their FName. At 0 every name watch is settled:
// live, or dead for good because its name resolved into a full table (the gate logs which). A
// consumer re-counting its own watches can stop there. Any thread.
int PendingNameCount();

// Every enabled watch: an exact one by its function (name and class null), a name watch by its literals
// (function null). For coop/dev/dependency_dump's equivalence record; game thread.
using WatchVisitor = void (*)(void* ctx, void* ufunction, const wchar_t* className, const wchar_t* name, int tag);
void DebugForEachWatch(WatchVisitor visit, void* ctx);

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

// A scope mark: bits a PRE puts on the scope of the body it is about to let run, readable by any seam on
// this thread while that body runs. MarkScope ORs `bits` into the dispatch's own word, which lives on the
// stack of the dispatch and is zero at its start, so every PRE of one call adds to the same word and
// nothing outlives the call: a nested dispatch inside a PRE has a word of its own, a Cancel never builds a
// scope, and a fault the firewall absorbs unwinds the word with its frame. The body's scope takes the word
// once no PRE cancelled. A mark from a post, from a body, or with no Call is a no-op. The Call a PRE
// receives, and its scopeMark with it, is valid only inside that PRE: a copy kept past its return points
// into a dispatch frame that is gone. Game thread.
void MarkScope(const Call& call, uintptr_t bits);

// The bits of the innermost scope on this thread's chain whose body is `function`, else 0. Readable from any
// seam on the thread (a PRE, a post, a native hook) while the body runs; 0 before the body's scope is
// built, after it ends, and for a body nobody marked.
uintptr_t ScopeBits(void* function);

// The bit `owner` (any stable address the consumer owns) holds: the same bit on every call for one owner, a
// new one for a new owner, at most 64 owners in a process. 0 past that, logged once: a consumer that gets 0
// treats its install as failed. Scans the owners taken so far: resolve once at install, not per dispatch.
// Game thread.
uintptr_t ScopeBitFor(const void* owner);

// The caller's storage for the out parameter at `paramOffset` (an Offset_Internal), or null when
// the frame carries no such record.
uint8_t* OutParamPtr(const Call& call, int32_t paramOffset);

// The function of the outermost Blueprint frame on the chain `call` was entered through (PreviousFrame, up to
// its end): the script function that began the stack, or the call's own function when it was entered with no
// caller. Valid in a PRE, while the frame is live. Null for a call with no frame. Game thread.
void* OutermostFunction(const Call& call);

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
