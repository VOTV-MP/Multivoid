// ue_wrap/vm_dispatch.h -- EX_Local* VM-dispatch interception (GNatives swap).
//
// The hook engine's THIRD primitive (docs/COOP_VM_DISPATCH_PLAN.md, option A).
// Our two existing primitives -- the ProcessEvent MinHook detour and the
// UFunction::Func patch -- both miss ONE dispatch class: a SCRIPT (BP bytecode)
// UFunction invoked BP-internally via EX_LocalVirtualFunction / EX_LocalFinal-
// Function. The interpreter runs ProcessLocalScriptFunction inline; neither PE
// nor Func ever fires (docs/COOP_DISPATCH_VISIBILITY.md; the kerfur conversion
// verbs live here). This module closes that seam by swapping the GNatives[256]
// exec-handler table entry for EX_LocalVirtualFunction (opcode 0x45) with a
// wrapper that brackets the dispatch and fires registered per-verb callbacks.
//
// Engine-wrapper layer (principle 7): this substrate is COOP-IGNORANT. It knows
// nothing about kerfurs, mirrors, or eids -- consumers register a verb by NAME
// and receive a Bracket carrying only the Context object. All gameplay/identity
// logic lives in the consumer (coop/creatures/kerfur_form_assembler is the first).
//
// Measured facts it is built on (STEP 1.0 LIVE-CATCH, hands-on host+client
// 2026-07-13; research/findings/world-systems/votv-vm-dispatch-RE-2026-07-13.md):
//   - GNatives base = AOB on a dispatch site (rel32); table validated (>=200 of
//     256 slots point into the main image).
//   - FFrame +0x20 = Code (bytecode cursor); handler ABI rcx=Context, rdx=&Stack,
//     r8=Result; the handler returns a value the dispatcher ignores but we forward.
//   - The 0x45 operand is a 12-byte FScriptName {ComparisonIndex@0, DisplayIndex@4,
//     Number@8}. In VOTV's shipping non-case-preserving build ComparisonIndex ==
//     DisplayIndex, so the real FName KEY is {ComparisonIndex = op[0], Number =
//     op[2]} (Number sits at byte 8). Match op[0]==FName.ComparisonIndex &&
//     op[2]==FName.Number -- NOT the raw 8 bytes 0-7 (that compares {CmpIdx,DispIdx}
//     against {CmpIdx,0} and NEVER matches; it was the v1 probe's silent-miss bug,
//     caught by the STEP 1.0 probe-first gate BEFORE this permanent swap landed).
//
// Filter shape (name-first, plan R12): enabled load -> IsGameThread ->
// non-destructive operand peek -> FName compare against the registered verbs.
// The name IS the correctness gate (a real kerfur variant matches by name
// regardless of its class descent); any further class/authority discrimination
// is the CONSUMER's job inside its callback (it has the Context object).
//
// Cost: the disabled fast path (no coop session) is one relaxed atomic load +
// a predicted-not-taken branch + tail-call to the original handler -- the eternal
// solo-SP tax of the never-un-swapped table pointer. Enabled, a non-matching
// dispatch costs IsGameThread + one counter + the O(N) name compare (N = the few
// registered verbs). STEP 1.0 measured ~0.006-0.015 ms/frame@120 with a strictly
// heavier per-dispatch class walk, so this leaner filter clears the <=0.1 gate
// with margin.
//
// SHIPS (RULE 3: standalone, no UE4SS). The swap is process-lifetime by
// simplicity (our DLL never unloads mid-session); SetEnabled gates ACTIVITY per
// coop session. This is NOT the throwaway coop/dev/gnatives_probe -- that probe
// (which proved the premise + operand layout) is retired with the substrate work.

#pragma once

#include <cstdint>

namespace ue_wrap::vm_dispatch {

// What a consumer callback receives at the ENTRY of a matched EX_Local* dispatch.
// Fires on the GAME THREAD only (an off-GT match is counted as a tripwire and the
// callback is skipped). Carries NO argument values -- they exist only inside the
// interpreter's execution window; a consumer needing values gets a per-site
// solution (e.g. an existing FinishSpawningActor / K2_DestroyActor Func seam).
struct Bracket {
    void* ctx;      // Stack.Object -- the Context/self actor the verb runs on (LIVE at entry).
    int   verbId;   // the consumer's registered id for the matched verb.
    int   depth;    // re-entrancy depth through MATCHED verbs (1 = outermost).
};

using EntryFn = void (*)(const Bracket&);

// The calling thread's currently-active MATCHED verb, if any. For a consumer's own
// downstream Func-seam hooks (e.g. FinishSpawningActor / K2_DestroyActor) that fire
// INSIDE the verb body and need to know: am I inside a bracketed verb, which one,
// and on what Context. `active` is false when no matched verb is on this thread's
// stack. `ctx` is the innermost active verb's Context -- the self-destroy identity
// key (a conversion verb destroys ITSELF, so a K2_DestroyActor whose dying actor ==
// ctx is the verb's own victim, distinguished from unrelated churn by IDENTITY, not
// class). Thread-local; valid only synchronously within the verb body. The window is
// published under an unwind-safe RAII bracket, so it cannot leak past the verb.
struct ActiveVerb {
    bool  active;
    int   verbId;
    int   depth;
    void* ctx;
};
ActiveVerb CurrentThreadVerb();

// Register a name-keyed EX_LocalVirtualFunction (0x45) verb. `verbName` is the BP
// function name (e.g. L"dropKerfurProp") and MUST have static lifetime (a string
// literal). `verbId` is the caller's tag echoed back in the Bracket (one cb can
// serve several verbs). `cb` fires on the game thread at the ENTRY of any 0x45
// dispatch whose operand name matches. Installs the 0x45 swap on the FIRST
// successful registration (the swap is gated on >=1 consumer -- no consumer, no
// table mutation). Idempotent per (verbName, cb). Returns false if the verb table
// is full or GNatives cannot be resolved/validated. Any thread.
//
// FName resolution is DEFERRED to the game thread (StringToFName dispatches
// ProcessEvent -> GT-only): the verb is inert until TickResolvePending() resolves
// it. Registration posts one resolve attempt itself; drive TickResolvePending from
// a game-thread tick as the belt.
bool RegisterVirtualVerb(const wchar_t* verbName, int verbId, EntryFn cb);

// Resolve any registered-but-unresolved verb FNames. No-op once all resolve, and
// a no-op off the game thread. Call from a game-thread tick (the consumer's Tick).
void TickResolvePending();

// The session gate. Disabled (the default, and the state during solo single-player)
// = the hot path pays only the fast-path tax and never runs the filter. Flip true
// at coop session start, false in the teardown fanout (OnDisconnect). Any thread.
void SetEnabled(bool on);
bool IsEnabled();

// True once the 0x45 swap is installed (a consumer registered at least one verb).
bool IsInstalled();

// Diagnostic counters (monotonic since install) for a 1/s stats line + tripwires.
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
