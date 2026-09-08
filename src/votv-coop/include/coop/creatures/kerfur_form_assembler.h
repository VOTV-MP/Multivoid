// coop/creatures/kerfur_form_assembler.h -- the kerfur FORM-FLIP assembler, first
// consumer of the EX_Local* VM-dispatch substrate (ue_wrap/vm_dispatch).
//
// The turn-on and turn-off verbs (spawnKerfuro / dropKerfurProp) are BP-internal
// EX_LocalVirtualFunction self-calls, invisible to both ProcessEvent and Func
// patches (docs/coop-dispatch-visibility.md), so which successor a conversion
// produced could only be guessed at, by proximity and timing. The substrate gives
// a DETERMINISTIC bracket at the verb itself.
//
// On each in-bracket (0x45) or in-request-scope (CallFunction) kerfur-form
// successor spawn, this module STORES the finished actor B -- pointer, live index
// and form -- in a one-shot thread-local slot, and does nothing else with it:
// kerfur_convert CONSUMES it at the paired A-destroy edge, which is what lets the
// converge skip the (0,0,0) post-destroy proximity anchor. The routing stays
// there; this file only publishes B.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::kerfur_form_assembler {

// A DETERMINISTIC conversion successor B, captured in-bracket at its FinishSpawningActor
// (2a-capture). {nullptr, -1} = nothing captured / stale / class-mismatch / index-dead.
struct CapturedForm { void* actor; int32_t idx; };

// One-shot: return the last in-bracket / in-req-scope kerfur-form successor B whose class
// matches wantNpc (true = kerfurOmega_C NPC successor of a turn-ON; false = prop_kerfurOmega_C
// successor of a turn-OFF), if it is fresh (captured within the verb window) and still live,
// then CLEAR the slot. Returns {nullptr, -1} otherwise (the caller falls back to its legacy
// proximity search). GT-only: the store is thread-local to the game thread the verb runs on,
// so it is race-free and needs no lock. See kerfur_convert's two consume sites.
CapturedForm ConsumeCapturedForm(bool wantNpc);

// Clear the capture slot for the current thread. Called at the OnConvertRequest CallFunction
// bracket entry (the 0x45 route clears itself at OnVerbEntry) so every conversion bracket
// starts with an empty slot and never consumes a prior bracket's stale B.
void ClearCapturedForm();

// NON-CONSUMING peek: is `actor` the DETERMINISTIC conversion successor currently in the capture
// slot (fresh + live)? The Init POST keyed-express suppressor (prop_lifecycle) reads this to make
// the kerfur layer the SOLE express (redesign 10.3) -- suppress the generic PropSpawn for a prop
// the conversion just produced, letting KerfurConvert own it. Distinct from ConsumeCapturedForm:
// this does NOT clear the slot (the deferred converge still consumes it). GT-only, thread-local.
bool IsCapturedForm(void* actor);

// Register the two conversion verbs with ue_wrap::vm_dispatch and open the session
// gate (SetEnabled(true)) -- both roles, since a client needs the bracket to observe
// its own conversion. Called from subsystems::Install (world-up, session active).
// Idempotent: the substrate de-dups the registrations. The capture store and the
// observe counters accrue for as long as the session is active, behind neither the
// [dev] vm_dispatch_log gate nor its line cap, so a run cannot end having captured
// nothing merely because logging was off.
void Install(coop::net::Session* session);

// Drive the substrate's deferred GT FName resolution + (dev) the 1/s stats line.
// Called from the game-thread TickGameplay fanout. Cheap no-op once resolved and
// when vm_dispatch_log is off.
void Tick();

// Close the session gate (SetEnabled(false)). Called from the DisconnectAll
// teardown fanout. The swap itself stays (process-lifetime by design).
void OnDisconnect();

}  // namespace coop::kerfur_form_assembler
