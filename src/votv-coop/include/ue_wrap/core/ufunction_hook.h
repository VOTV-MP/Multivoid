// ue_wrap/core/ufunction_hook.h -- patch a native UFunction's Func pointer to catch a call our
// ProcessEvent detour can NEVER see.
//
// Engine-wrapper layer (principle 7): no gameplay or network logic. Our only MinHook seam is
// UObject::ProcessEvent, the OUTER entry to the Blueprint VM. A BP-internal call -- EX_CallMath,
// EX_FinalFunction, EX_VirtualFunction, EX_Local*Function -- routes through UFunction::Invoke to
// (Context->*Func)(Stack, Result), one layer BELOW the detour, and never re-enters ProcessEvent, so
// a ProcessEvent observer on such a callee registers and never fires. The chipPile grab and the
// clump re-pile spawn are exactly this.
//
// SCOPE, AND IT IS LOAD-BEARING: those routes funnel through `Func` only when the callee is NATIVE.
// Both dispatch handlers branch on FUNC_Native, and a SCRIPT (bytecode) callee goes through
// ProcessScriptFunction to ProcessInternal, never reading Func at all. Per-function detail:
// docs/coop-dispatch-visibility.md.

#pragma once

namespace ue_wrap::ufunction_hook {

// Post-native observer. `context` is the dispatch Context -- for a member call the object the
// function runs ON, such as the DYING actor for K2_DestroyActor; for a static GameplayStatics call
// the world-context-ish caller. `sourceObject` is FFrame::Object, the actor whose BYTECODE is
// executing, so the caller: the re-piling clump that issued a spawn. `spawnedResult` is *Result,
// the native function's RESULT_PARAM -- for BeginDeferred the new actor, and possibly null on a
// failed spawn. Fires AFTER the original Func returns, so `spawnedResult` is populated. Raw
// UObject*s.
//
// THREADING: native UFunction dispatch, UWorld::SpawnActor among it, is GAME-THREAD only, so this
// runs on the game thread -- but DEEP inside an engine call, so it MUST be cheap and MUST NOT
// throw. The facility SEH-wraps it as a crash backstop, the same firewall contract the ProcessEvent
// observers have.
using PostNativeCallback = void(*)(void* context, void* sourceObject, void* spawnedResult);

// Patch `ufunction`'s native Func with a transparent forwarder that reads FFrame::Object, forwards
// to the original Func (which steps the params off the bytecode stream, runs the implementation and
// writes *Result), then reads *Result and invokes `cb` with both. No engine layout leaks upward.
// Idempotent per (ufunction, cb). Returns false if the arguments are null, the table is full, or
// the Func slot reads null -- a wrong offset for this build, where refusing beats corrupting the
// UFunction. There is no unpatch: a Func patch replaces an observation scheme wholesale once
// proven. Game thread.
//
// The scope rule above is about the DISPATCH ROUTE, not the callee list. Script overrides ARE
// patched here and DO fire -- puppet_spawn's BlueprintUpdateAnimation, save_indicator_suppress's
// saveAnim and addHint -- because their dispatch is ProcessEvent, whose Invoke also reads Func.
// What does not work is a script function called via EX_Local*: the patch INSTALLS, since Func is
// ProcessInternal and non-null and passes the guard, it LOGS "patched", and it NEVER FIRES. That
// class belongs to the script-body gate (ue_wrap/core/script_gate.h), which sees every script body
// with its arguments and can refuse it.
bool InstallPostHook(void* ufunction, PostNativeCallback cb);

}  // namespace ue_wrap::ufunction_hook
