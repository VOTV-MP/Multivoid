// coop/kerfur_reconcile.h -- retire the join-window kerfur off-to-active duplicate. CLIENT-only,
// game thread only: both the EntitySpawn apply and the quiescence sweep run on the game-thread
// drain.
//
// THE duplicate's root is a kerfur that was OFF in the transferred save and that the host turns ON
// during the join-load window. The host no longer expresses it as an off-prop -- it is an active
// NPC on the npc channel now -- so the joining client's own save-loaded local off-prop is never
// adopted or claimed as a host mirror, and it SURVIVES beside the active NPC as an unclaimed
// prop_kerfurOmega_C. The divergence sweep does not retire it, so this module does, explicitly.
//
// TRIGGER: the retire is armed from the kerfur's npc EntitySpawn, where npc_mirror::OnEntitySpawn
// reads payload.retireOffEid -- NOT from the KerfurConvert, whose send is pre-world-gated mid
// save-transfer and so never reaches the joiner, while the active form rides the EntitySpawn, which
// does. The host stamps the off-prop's HOST eid onto that EntitySpawn through
// kerfur_entity::GetOriginOffEidForEid, sourced from BindFormActor's KerfurRecord.originOffEid.

#pragma once

#include "coop/element/element.h"  // ElementId

namespace coop::kerfur_reconcile {

// The match is DETERMINISTIC, not positional. The joiner already binds its save-loaded off-prop to
// that SAME host eid through save_identity_bind, so the off-prop is a Prop MIRROR at retireOffEid,
// and the retire is a direct MirrorManager<Prop>::Get(retireOffEid) plus teardown: no GUObjectArray
// walk, no fuzzy centimetre position match, no collision class. The off-prop may not have finished
// loading and binding when the EntitySpawn arrives, so the retire runs at QUIESCENCE, in the sweep
// below, and a pending eid that is not yet bound is KEPT for the next sweep rather than dropped
// early -- a dropped one would leave the duplicate standing.
//
// THE RETIRE MECHANISM lives here; its SEQUENCING belongs to the one join-window order owner,
// coop::element::quiescence_drain::RunReconcile, which drains it as a step just as it calls
// save_identity_bind. It is not driven from kerfur_convert::PollKerfurConversions, which would be a
// third parallel order owner on the same axis. The order owner's steady-state tick fires
// independently of any bracket -- every tick, gated only on quiescence and HasPendingWork, which
// ORs in HasPendingRetire -- so the case where no pile bracket is armed is still covered.

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
