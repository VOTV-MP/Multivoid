// ue_wrap/ufunction_hook.h -- patch a native UFunction's Func pointer (the
// standalone "UE4SS RegisterHook" technique) to catch a call our ProcessEvent
// detour can NEVER see.
//
// Engine-wrapper layer (principle 7): NO gameplay/network logic. Our only MinHook
// seam is UObject::ProcessEvent -- the OUTER entry to the BP-VM. A BP-internal call
// (EX_CallMath / EX_FinalFunction / EX_VirtualFunction / EX_Local*Function) routes
// UObject::CallFunction -> UFunction::Invoke -> (Context->*UFunction::Func)(Stack,
// Result), ONE LAYER BELOW the detour, and never re-enters ProcessEvent. So a
// ProcessEvent observer on such a callee registers but never fires (the chipPile
// grab + the clump re-pile spawn are exactly this -- EX_CallMath). To catch one we
// patch the callee UFunction's Func pointer (UFunction + off::UFunction_Func) with
// our own transparent forwarder.
//
// *** SCOPE, AND IT IS LOAD-BEARING (corrected 2026-08-24, /qf rounds 34-35) ***
// The routes listed above funnel through `Func` ONLY WHEN THE CALLEE IS **NATIVE**.
// Both dispatch handlers branch on `FunctionFlags & 0x400 (FUNC_Native)` @UFunction+0xB0:
// a NATIVE callee goes UFunction::Invoke -> Func@+0xD8 (what we patch), while a
// **SCRIPT (BP-bytecode) callee goes ProcessScriptFunction -> ProcessInternal and NEVER
// READS Func AT ALL**. Every Func patch this facility ships today is native
// (BeginDeferredActorSpawnFromClass, K2_DestroyActor, AudioComponent::Play), which is
// why the sentence above was written unqualified -- but a Func patch on a BP function
// called via EX_Local* INSTALLS SUCCESSFULLY (Func = ProcessInternal, non-null, so it
// passes the null guard below), LOGS "patched", AND NEVER FIRES. A script UFunction
// called via EX_Local* is THE ONLY REMAINING INVISIBLE CLASS and is reachable only via
// the 0x45 GNatives swap (ue_wrap/core/vm_dispatch.h).
// Authority: docs/COOP_DISPATCH_VISIBILITY.md:88 (bold) + docs/COOP_VM_DISPATCH_PLAN.md:300-304
// ("Option E ELIMINATED BY MEASUREMENT"). A design cascade was built on the unqualified
// sentence and had to be reversed; see [[lesson-a-module-header-is-not-the-capability-map]].
// (docs/COOP_DISPATCH_VISIBILITY.md;
// research/findings/piles-trash/votv-chippile-dispatch-and-thunk-hook-RE-2026-06-21.md, IDA-pinned.)
//
// The forwarder reads FFrame::Object (the actor whose bytecode is executing = the
// SOURCE of a spawn issued from its ubergraph) BEFORE forwarding, forwards to the
// original Func (which steps the params from the bytecode stream + runs the impl +
// writes *Result), then reads *Result (the native fn's RESULT_PARAM out-value) and
// hands BOTH raw UObject*s to the gameplay callback. No engine layout leaks upward.
//
// THREADING: native UFunction dispatch (e.g. UWorld::SpawnActor) is GAME-THREAD only,
// so the callback runs on the game thread -- but DEEP inside an engine call, so it
// MUST be cheap and MUST NOT throw (the facility SEH-wraps it as a crash backstop, the
// same firewall contract as the ProcessEvent observers).

#pragma once

namespace ue_wrap::ufunction_hook {

// Post-native observer. `context` = the dispatch Context (RCX of the exec-thunk ABI --
// for a member call the object the function runs ON, e.g. the DYING actor for
// K2_DestroyActor; for a static GameplayStatics call the world-context-ish caller);
// `sourceObject` = FFrame::Object (the actor whose BYTECODE is executing = the caller,
// e.g. the re-piling clump that issued a spawn); `spawnedResult` = *Result (the native
// fn's RESULT_PARAM -- for BeginDeferred the new actor; MAY BE NULL on a failed spawn).
// Fires AFTER the original Func returns (so `spawnedResult` is populated). Raw UObject*s.
using PostNativeCallback = void(*)(void* context, void* sourceObject, void* spawnedResult);

// Patch `ufunction`'s native Func (UFunction + off::UFunction_Func) with a transparent
// forwarder that invokes `cb(FFrame::Object, *Result)` after forwarding to the original.
// Idempotent per (ufunction, cb). Returns false if args are null, the small fixed table
// is full, or the Func slot reads null (offset wrong for this build -> refuse rather
// than corrupt the UFunction). PROCESS-LIFETIME: no unpatch (RULE 2 -- a Func-patch
// replaces an observation scheme wholesale once proven). Call on the game thread.
bool InstallPostHook(void* ufunction, PostNativeCallback cb);

}  // namespace ue_wrap::ufunction_hook
