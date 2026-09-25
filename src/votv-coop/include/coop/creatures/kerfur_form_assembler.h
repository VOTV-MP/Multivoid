// coop/creatures/kerfur_form_assembler.h -- the kerfur FORM-FLIP assembler, a consumer of
// the script-body gate (ue_wrap/core/script_gate) on the two conversion verbs.
//
// The turn-on and turn-off verbs (spawnKerfuro / dropKerfurProp) are BP-internal
// EX_LocalVirtualFunction self-calls, invisible to both ProcessEvent and Func
// patches (docs/coop-dispatch-visibility.md), so which successor a conversion
// produced could only be guessed at, by proximity and timing. The substrate gives
// a DETERMINISTIC bracket at the verb itself.
//
// On each in-window kerfur-form
// successor spawn, this module STORES the finished actor B -- pointer, live index
// and form -- in a one-shot thread-local slot, and does nothing else with it:
// kerfur_convert CONSUMES it at the verb's return, where the host converges the
// conversion on exactly the successor the verb made. The routing stays there;
// this file only publishes B.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::kerfur_form_assembler {

// A DETERMINISTIC conversion successor B, captured in-bracket at its FinishSpawningActor
// {nullptr, -1} = nothing captured / stale / class-mismatch / index-dead.
struct CapturedForm { void* actor; int32_t idx; };

// One-shot: return the last in-bracket kerfur-form successor B whose class matches wantNpc
// (true = kerfurOmega_C NPC successor of a turn-ON; false = prop_kerfurOmega_C successor of a
// turn-OFF), if it is fresh (captured within the verb window) and still live, then CLEAR the
// slot. Returns {nullptr, -1} otherwise: the verb made no successor. GT-only: the store is
// thread-local to the game thread the verb runs on, so it is race-free and needs no lock. The
// consumer is the converge at the verb's return (kerfur_convert_host::ConvergeAtReturn).
CapturedForm ConsumeCapturedForm(bool wantNpc);


// NON-CONSUMING peek: is `actor` the DETERMINISTIC conversion successor currently in the capture
// slot (fresh + live)? The Init POST keyed-express suppressor (prop_lifecycle) reads this to make
// the kerfur layer the SOLE express -- suppress the generic PropSpawn for a prop
// the conversion just produced, letting KerfurConvert own it. Distinct from ConsumeCapturedForm:
// this does NOT clear the slot (the converge at the verb's return still consumes it). GT-only,
// thread-local.
bool IsCapturedForm(void* actor);

// NON-CONSUMING peek by direction: is a fresh, live successor of the wanted form in the slot? The
// prop destroy seam reads it inside a turn-on, where the prop dies after its NPC was captured: that
// death is the conversion's, and KerfurConvert carries it. GT-only, thread-local.
bool HasCapturedForm(bool wantNpc);

// Watch the two conversion verbs by name at ue_wrap::script_gate -- both roles: a client's
// verb is refused (kerfur_convert) and never opens a window, and its catches still count; the
// session's hold keeps the gate running. Called from subsystems::Install (world-up, session
// active); registers once per process. The capture store and the observe counters accrue for as
// long as the session is active, behind neither the [dev] script_gate_log gate nor its line cap,
// so a run cannot end having captured nothing merely because logging was off.
void Install(coop::net::Session* session);

// Drive the gate's deferred GT FName resolution + (dev) the 1/s stats line.
// Called from the game-thread TickGameplay fanout. Cheap no-op once resolved and
// when script_gate_log is off.
void Tick();

// The session-end summary, always. Called from the DisconnectAll teardown fanout, which
// also ends the session's hold on the gate.
void OnDisconnect();

// The summary lines the teardown prints -- the containment counters, the observe gates and the
// gate's own counts -- now, tagged `when`. For a drill that ends inside its session. Game thread.
void LogSummary(const char* when);

}  // namespace coop::kerfur_form_assembler
