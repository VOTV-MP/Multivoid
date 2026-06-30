// coop/kerfur_reconcile.h -- the join-window kerfur off->active dup RETIRE (scope A, 2026-06-24).
//
// THE forward off->active dup root (census-pinned 2026-06-24, docs/kerfur/03 + 07): a kerfur that was
// OFF in the transferred save but the host turns ON in the join-load window. The host no longer expresses
// it as an off-prop (it is now an active NPC on the npc channel), so the joining client's OWN save-loaded
// local off-prop is never adopted/claimed as a host mirror -- it SURVIVES beside the active NPC (the dup,
// census 15:43 + 16:37: an UNCLAIMED prop_kerfurOmega_C). The divergence sweep does NOT retire it. So we
// retire it explicitly.
//
// TRIGGER: the retire is armed from the kerfur's npc EntitySpawn (npc_mirror::OnEntitySpawn reads
// payload.retireOffEid), NOT the KerfurConvert. A join-window turn-on's KerfurConvert SendReliable is
// pre-world-gated mid save-transfer (v56 B2 invariant -- host log 16:37:10 + 13:21:20:54), so the convert
// never reaches the joiner; the active form rides the npc EntitySpawn instead (which DOES reach it). The
// host stamps the off-prop's HOST EID onto that EntitySpawn (kerfur_entity::GetOriginOffEidForEid, sourced
// from BindFormActor's KerfurRecord.originOffEid = the off-prop's save eid).
//
// v91 DETERMINISTIC (replaces the position-fuzzy 1cm match): the joiner already binds its save-loaded
// off-prop to that SAME host eid via save_identity_bind (Build 3 ordinal bind), so the off-prop is a Prop
// MIRROR at retireOffEid. The retire is a direct MirrorManager<Prop>::Get(retireOffEid) lookup + teardown --
// no GUObjectArray walk, no fuzzy 1cm position match, no collision class. The off-prop may not have
// async-loaded/bound yet when the EntitySpawn arrives, so the retire runs at QUIESCENCE (the sweep below) and
// a pending eid not yet bound is KEPT for the next sweep (never dropped early -> no duplicate-survives risk).
//
// THE RETIRE MECHANISM lives here; its SEQUENCING is owned by the ONE join-window order owner
// (coop::element::quiescence_drain::RunReconcile drains it as a step, just as it calls save_identity_bind).
// It is NOT driven from kerfur_convert::PollKerfurConversions any more -- that was a THIRD parallel order
// owner for the join-window axis (anti-smear refactor 2026-06-30, [[feedback-one-owner-order-axis]]). The
// order owner's steady-state tick fires bracket-INDEPENDENTLY (every tick, gated only on quiescence +
// HasPendingWork, which ORs in HasPendingRetire), so the "no pile bracket armed" case is still covered.
//
// CLIENT-only. Game-thread only (the EntitySpawn apply + the quiescence sweep both run on the GT drain).

#pragma once

#include "coop/element/element.h"  // ElementId

namespace coop::kerfur_reconcile {

// Drop all pending retires (session end). Mirrors quiescence_drain::Reset.
void Reset();

// Arm a retire of the off-prop MIRROR bound at host eid `offEid` -- a join-window-turned-ON kerfur whose
// off-prop eid arrived on its npc EntitySpawn (npc_mirror::OnEntitySpawn, payload.retireOffEid). Idempotent
// per eid (a re-announce is a no-op). The actual retire runs at the next quiescence sweep -- the joiner's
// save-loaded off-prop may not have bound to `offEid` yet when the EntitySpawn arrives. Game thread.
void ArmPendingRetireByEid(coop::element::ElementId offEid);

// Post-quiescence retire of every armed pending. Driven as a step of the ONE order owner's sequence
// (coop::element::quiescence_drain::RunReconcile), itself fired by the join-window sweep AND the steady-state
// tick (the latter bracket-INDEPENDENT, so it fires even when no pile bracket armed). For each pending offEid,
// resolve the Prop MIRROR at that eid (MirrorManager<Prop>::Get) and, if it is a live kerfur off-prop, tear it
// down (mirror-aware: clear maps, destroy actor, RetireMirror). A pending eid whose off-prop is not yet bound
// is KEPT (retried next sweep -- never an early drop, so a late async bind can't leave a surviving duplicate).
// Returns the count retired. Game thread.
int SweepReconcileSaveTimeKerfurs();

// True iff there is at least one armed-but-unretired pending kerfur. The order owner's HasPendingWork ORs this
// in so its steady-state tick drives the kerfur retire even when no pile work is pending. Game thread.
bool HasPendingRetire();

}  // namespace coop::kerfur_reconcile
