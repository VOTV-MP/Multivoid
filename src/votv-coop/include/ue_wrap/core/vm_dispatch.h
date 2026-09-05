// ue_wrap/core/vm_dispatch.h -- blueprint-VM dispatch interception, the hook engine's third
// primitive. The ProcessEvent detour and the UFunction native-pointer patch both miss one
// dispatch class: a script function invoked blueprint-internally through the local
// virtual-call and local final-call opcodes, which the interpreter runs inline, so neither
// fires (the kerfur conversion verbs live there). This module swaps the exec-handler table
// entry for the local virtual call with a wrapper that brackets the dispatch and fires
// registered per-verb callbacks. The substrate is coop-ignorant: consumers register a verb by
// name and receive a bracket carrying only the context object. The facts it rests on: the
// handler table is found by a pattern on a dispatch site and validated; the operand is a
// 12-byte script name whose comparison and display indices are equal in the shipping build,
// so the key is the comparison index at the cursor and the number 8 bytes in, never the raw
// first 8 bytes (which never match). The filter, name first: the enabled load, the
// game-thread check, an operand peek, the name compare; the name is the correctness gate,
// and class or authority discrimination is the consumer's. The disabled fast path is one
// relaxed load, a branch and a tail call; the swap is process-lifetime, gated per session.

#pragma once

#include <cstdint>

namespace ue_wrap::vm_dispatch {

// What a consumer callback receives at the entry of a matched dispatch. Fires on the game
// thread only (an off-thread match is counted as a tripwire and the callback skipped).
// Carries no argument values, which exist only inside the interpreter's execution window; a
// consumer needing values gets a per-site solution (an existing finish-spawn or destroy
// native seam).
struct Bracket {
    void* ctx;      // Stack.Object -- the Context/self actor the verb runs on (LIVE at entry).
    int   verbId;   // the consumer's registered id for the matched verb.
    int   depth;    // re-entrancy depth through MATCHED verbs (1 = outermost).
};

using EntryFn = void (*)(const Bracket&);

// The calling thread's currently active matched verb, if any, for a consumer's own
// downstream native-seam hooks (the finish spawn, the destroy) that fire inside the verb
// body and need to know whether they are inside a bracketed verb, which one, and on what
// context. `active` is false when no matched verb is on this thread's stack; `ctx` is the
// innermost active verb's context, the self-destroy identity key (a conversion verb
// destroys itself, so a destroy whose dying actor equals the context is the verb's own
// victim, distinguished from unrelated churn by identity, not class). Thread-local, valid
// only synchronously within the verb body; published under an unwind-safe RAII bracket, so
// it cannot leak past the verb. Gate on `verbName`, not on `active`, and never on `verbId`:
// the id is a caller-chosen tag unique only within the registering consumer (several
// modules use 1 and 2 for different verbs), so for a cross-module read the id is
// meaningless, and `active` alone is worse, true for any registered verb on this thread, so
// a consumer testing it misattributes every other module's verb to its own. The name is
// the registered blueprint function name (static lifetime, required by the registration),
// unique by construction; compare it by pointer for your own literal or by wcscmp.
struct ActiveVerb {
    bool           active;
    int            verbId;
    int            depth;
    void*          ctx;
    const wchar_t* verbName;   // the matched verb's registered name; null iff !active.
};
ActiveVerb CurrentThreadVerb();

// Register a name-keyed local-virtual-call verb. `verbName` is the blueprint function name
// and must have static lifetime (a literal). `verbId` is the caller's tag echoed back in the
// bracket (one callback can serve several verbs). `cb` fires on the game thread at the entry
// of any dispatch whose operand name matches. Installs the swap on the first successful
// registration (no consumer, no table mutation). Idempotent per name and callback. False if
// the verb table is full or the handler table cannot be resolved or validated. Any thread.
// Name resolution is deferred to the game thread (the string-to-name conversion dispatches
// ProcessEvent): the verb is inert until TickResolvePending resolves it; registration posts
// one resolve attempt itself, and a game-thread tick drives the rest.
bool RegisterVirtualVerb(const wchar_t* verbName, int verbId, EntryFn cb);

// Resolve any registered but unresolved verb names. A no-op once all resolve, and a no-op
// off the game thread. Call from a game-thread tick (the consumer's tick).
void TickResolvePending();

// The session gate. Disabled (the default, and the state in solo single player) means the
// hot path pays only the fast-path tax and never runs the filter. Flip true at coop session
// start, false in the teardown fan-out. Any thread.
void SetEnabled(bool on);
bool IsEnabled();

// True once the swap is installed (a consumer registered at least one verb).
bool IsInstalled();

// Diagnostic counters (monotonic since install) for a once-a-second stats line and the
// tripwires.
struct Stats {
    unsigned long long gtDispatch;      // 0x45 dispatches on the game thread while enabled
    unsigned long long workerDispatch;  // 0x45 dispatches off the game thread while enabled
    unsigned long long nameMatch;       // dispatches whose operand matched a registered verb
    unsigned long long offGtMatch;      // TRIPWIRE: a watched verb matched OFF the game thread
    unsigned long long callbackFired;   // consumer callbacks fired (GT matches)
    int  registeredVerbs;
    int  resolvedVerbs;
    bool enabled;
    bool installed;
};
Stats GetStats();

}  // namespace ue_wrap::vm_dispatch
