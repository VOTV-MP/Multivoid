// coop/kerfur_reconcile.h -- the join-window kerfur off->active dup RETIRE (scope A, 2026-06-24).
//
// THE forward off->active dup root (census-pinned 2026-06-24, docs/kerfur/03 + 07): a kerfur that was
// OFF in the transferred save but the host turns ON in the join-load window. The host no longer expresses
// it as an off-prop (it is now an active NPC on the npc channel), so the joining client's OWN save-loaded
// local off-prop is never adopted/claimed as a host mirror -- it SURVIVES beside the active NPC (the dup,
// census 15:43 + 16:37: an UNCLAIMED prop_kerfurOmega_C). The divergence sweep does NOT retire it (a
// kerfur off-prop that got fuzzy-adopted is a host MIRROR, sweep-exempt; one that stayed local races the
// bracket). So we retire it explicitly, keyed by the SAVE-TIME position.
//
// TRIGGER (scope A v1, 2026-06-24, after the 16:37 hands-on): the retire is armed from the kerfur's npc
// EntitySpawn (npc_mirror::OnEntitySpawn reads payload.hasMatchPos), NOT the KerfurConvert. A join-window
// turn-on's KerfurConvert SendReliable FAILS (the joiner isn't ready for reliable gameplay mid
// save-transfer -- host log 16:37:10), so the convert never reaches the client; the active form rides the
// npc EntitySpawn instead (which DOES reach the joiner -- npc-adopt eid=3149 at 16:37:16). The host stamps
// the off->active kerfur's blob-instant save-time pos onto that EntitySpawn (kerfur_entity::
// GetSaveTimePosForEid, sourced from BindFormActor's KerfurRecord.saveTimePos). The retire itself then
// runs at QUIESCENCE (the sweep below, driven by kerfur_convert::PollKerfurConversions) once the client's
// local off-prop has async-loaded -- a GUObjectArray walk because the local save off-prop is NOT
// resolvable by key in remote_prop's maps (16:37 "no local match (key or eid)").
//
// This is L1 pile_reconcile's shape applied to the kerfur object channel (the THIRD mirror-identity
// window-race instance -- save-time exact key + post-quiescence reconcile + position uniqueness). It is
// kept in its own file (RULE 2026-05-25 one-feature-per-file); the shared mirror-identity layer is
// extracted AFTER this works (fix-then-generalize, [[feedback-fix-then-generalize-mirror-identity]]).
//
// CLIENT-only. Game-thread only (the EntitySpawn apply + the quiescence sweep both run on the GT drain).

#pragma once

#include "coop/element/element.h"  // ElementId
#include "ue_wrap/types.h"         // ue_wrap::FVector

namespace coop::kerfur_reconcile {

// Drop all pending retires (session end). Mirrors pile_reconcile::Reset.
void Reset();

// Arm a retire for a join-window-turned-ON kerfur whose save-time pos arrived on its npc EntitySpawn
// (npc_mirror::OnEntitySpawn, payload.hasMatchPos). Keyed by the host eid (idempotent per re-announce).
// The actual retire runs at the next quiescence sweep -- the client's stale local off-prop may not have
// async-loaded yet when the EntitySpawn arrives. Game thread.
void ArmPendingRetire(uint32_t hostEid, const ue_wrap::FVector& saveTimePos);

// Post-quiescence retire of every armed pending. Driven from kerfur_convert::PollKerfurConversions (the
// CLIENT poll, already load-tail-quiescence-gated, bracket-INDEPENDENT) so it fires even when no pile
// bracket armed (the SnapshotBegin-lost flake). For each pending (hostEid -> save-time key) a FRESH
// GUObjectArray walk finds the LOCAL (non-mirror) off-prop within 1 cm and retires it. SAFETY = the EXACT
// 1cm key + position uniqueness + ambiguous(>1)->skip + the non-mirror IsKerfurPropClass gate (NO ratio
// valve -- it false-aborted the lone-stale-off-prop case, hands-on 17:06; the cpp explains why). Returns
// the count retired; clears the pending set (self-limits to one pass per arming). Game thread.
int SweepReconcileSaveTimeKerfurs();

}  // namespace coop::kerfur_reconcile
