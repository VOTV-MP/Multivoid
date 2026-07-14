// coop/creatures/kerfur_form_assembler.h -- the kerfur FORM-FLIP assembler:
// first consumer of the EX_Local* VM-dispatch substrate (ue_wrap/vm_dispatch).
//
// docs/COOP_VM_DISPATCH_PLAN.md S3. The kerfur turn-on / turn-off conversion
// verbs (spawnKerfuro / dropKerfurProp) are BP-internal EX_LocalVirtualFunction
// self-calls -- invisible to both ProcessEvent and Func patches -- which forced
// three probabilistic fragment-stitching crutches (fresh-spawn stamps, destroy-
// edge capture, death-watch poll; takes 8/9/10, take-10 regressed). The substrate
// gives us a DETERMINISTIC bracket at the verb itself; this module will grow into
// the capture + destroy-suppress + deferred-converge assembler that retires those
// crutches (increments 2a-2c).
//
// INCREMENT 1 (observe-only, retained): register the two verbs with the substrate
// and, on each caught dispatch, LOG the Context class + depth + the 2a-observe gate
// counters. This proved the PERMANENT substrate delivers the catch end-to-end and
// turned the inferred kerfur variant family into a MEASURED set (G1 13:40 take:
// formIn/selfIn in-window, DESTROY_NO_SPAWN=0, Bindex live).
//
// INCREMENT 2a-CAPTURE (2026-07-14, this file now feeds behavior): on each in-bracket
// (0x45) OR in-request-scope (CallFunction) kerfur-form successor spawn, STORE the
// finished actor B (pointer + live index + form) in a one-shot thread-local slot. The
// convert layer (coop::kerfur_convert) CONSUMES it at the paired A-destroy edge --
// TryCaptureKerfurPropDestroy (client relay-suppress / host inline converge) and
// ConvergeAfterConversion (request route) -- to bypass the (0,0,0) post-destroy
// proximity anchor that starved the guard on every real toggle (take-8/take-10
// misfilter class; measured live in the G1 take). The capture is the DETERMINISTIC
// which-B; the converge/suppress logic itself is unchanged and still lives in
// kerfur_convert. Still NO suppress/converge IN THIS FILE (2b/2c own routing) -- the
// verb bodies run unchanged; this module only publishes B.
//
// Logging is gated behind [dev] vm_dispatch_log (default off) and capped, so a
// normal session is silent and a hands-on observation run is not spammed. The capture
// STORE + the observe counters accrue whenever the session is active (behind neither
// gate) so a run can never end having captured/measured nothing.

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
// gate (SetEnabled(true)). Called from subsystems::Install (world-up, session
// active). Idempotent -- the substrate de-dups the registrations.
void Install(coop::net::Session* session);

// Drive the substrate's deferred GT FName resolution + (dev) the 1/s stats line.
// Called from the game-thread TickGameplay fanout. Cheap no-op once resolved and
// when vm_dispatch_log is off.
void Tick();

// Close the session gate (SetEnabled(false)). Called from the DisconnectAll
// teardown fanout. The swap itself stays (process-lifetime by design).
void OnDisconnect();

}  // namespace coop::kerfur_form_assembler
