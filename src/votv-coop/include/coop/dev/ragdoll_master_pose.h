// coop/dev/ragdoll_master_pose.h -- PROBE: full-skeleton ragdoll coupling for the puppet.
//
// The v22 mirror couples ONE bone: the puppet actor is pelvis-attached to the invisible
// playerRagdoll_C body and the streamed pelvis rotation is driven onto the actor -- the
// kel tumbles as a RIGID piece. The user ask (2026-07-03): keep the pelvis attach, but
// couple the REMAINING bones too. There is no per-bone runtime write (bone writes are
// AnimGraph-only -- [[project-ragdoll-sync]] RE) and a component has exactly one attach
// parent, so per-bone attaches are out. The engine's one-call mechanism IS
// USkinnedMeshComponent::SetMasterPoseComponent: both kel body meshes become pose SLAVES
// of the mirror body's simulating mesh (engine-side full bone copy by NAME, per frame).
//
// Rig facts (static, 2026-07-03): the kel player-body skeleton has SIX bones -- lowlegs,
// thighs, pelvis, chest, head, head_end (tools/client_model/SPEC.md ReferenceSkeleton,
// parsed from the cooked mesh) -- and the playerRagdoll_C body mesh is the same 6-bone
// family (the bone overlay counts 6 live; 'pelvis' name-matches already). Expected
// coverage: 6/6, logged per apply as a [MASTER-POSE] line; a miss names the bone.
//
// Master-pose copies bones in COMPONENT space, so while coupled the probe also pins both
// kel mesh components' WORLD transform onto the body mesh component each attached tick
// (K2_SetWorldLocationAndRotation) -- otherwise the pose renders rigidly offset by the
// slave-vs-master component delta (the pelvis attach parks the actor NEAR, not exactly
// ON, the body's component origin). On recover both meshes get master=null plus their
// saved attach-relative transforms back; the AnimBP resumes on its own the next tick.
//
// PROBE (RULE-2-exempt diagnostic, [[feedback-rule2-exempts-probes-diagnostics-tools]]):
// judged BY EYE against the plain pelvis attach -- promotion to always-on shipping
// behavior is a separate decision after the hands-on verdict. OFF by default; F1 ->
// Player -> HUD checkbox (host-only via dev_gate, next to the bone visualizer -- which
// is exactly the tool to watch it with) or [dev] ragdoll_master_pose=1.
//
// Called from RagdollDisplay (game thread): Drive() per attached tick (lazily applies
// when enabled, pins, restores when toggled off mid-flop or the master dies), Stop() on
// the recover edge. All coupling state is game-thread-only.
#pragma once

#include <cstdint>

namespace coop::dev::ragdoll_master_pose {

// Read [dev] ragdoll_master_pose once at boot (force-enable). Render/menu thread safe.
void InitFromIni();

// Menu checkbox state (role-aware: reports OFF while a connected client, dev_gate).
bool IsEnabled();
void SetEnabled(bool on);

// Per-attached-tick hook (RagdollDisplay::DriveAttached, live-body branch): applies the
// coupling on the first enabled tick of a flop (coverage-logged), then pins the slave
// mesh components onto the master; restores if disabled mid-flop or the master died.
// Inert (one atomic load + one map lookup) while disabled with nothing applied.
void Drive(void* puppetActor, int32_t puppetIdx, void* body, int32_t bodyIdx);

// Recover hook (RagdollDisplay::Stop, BEFORE the body is detached/destroyed): un-slave
// both meshes + restore their saved attach-relative transforms. Safe no-op when nothing
// was applied for this puppet.
void Stop(void* puppetActor);

}  // namespace coop::dev::ragdoll_master_pose
