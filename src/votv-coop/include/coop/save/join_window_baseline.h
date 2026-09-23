// coop/save/join_window_baseline.h -- what the host's world was at the instant a joiner's blob was
// cut, and the corrections that reconcile the host's live world against it. Host, game thread.
//
// The blob is a PHOTOGRAPH, and the host keeps playing while it travels, is written and is loaded
// -- a minute on a slow link. Two kinds of difference are answered per joiner, each photograph
// having been cut at its own instant. GONE: keyed props the blob holds that the host no longer
// has, sent as an explicit PropDestroy per key ahead of the bracket (MTA's Packet_EntityRemove),
// so the divergence sweep need not INFER the delete. MOVED: the save-time position of every
// chipPile, clump, off-form kerfur and keyed prop, re-asserted once the joiner has quiesced -- a
// pile carries no position on the wire at all, and a keyed prop's is clobbered by the joiner's own
// loadObjects afterwards. The flush is LATE-ARMED past the connect edge, because a pile moved late
// in a long load tail would get no correction from a one-shot; its expiry is the join window's
// true close and retires the maps. A stale-fallback join captures nothing and every entry point is
// then a no-op, the divergence sweep keeping full responsibility. See docs/join.md.

#pragma once

#include "coop/element/element.h"  // ElementId
#include "ue_wrap/core/types.h"    // ue_wrap::FVector

namespace coop::net { class Session; }

namespace coop::join_window_baseline {

// Remember the session for sends. Once at harness boot, beside the transfer's own install.
void Install(coop::net::Session* session);

// THE CAPTURE INSTANT. Record the host's keyed-prop key set and the save-time position of every
// chipPile, garbage clump, off-form kerfur and keyed prop. Called from the transfer's request
// handler on the live-capture success path only, in the same breath as the blob it describes.
void CaptureForSlot(int peerSlot);

// The connect edge: what this joiner's blob has and the host no longer does, as one explicit
// PropDestroy per key. Consumes the key set. A no-op without a live-capture baseline.
void SendDivergenceDeletes(int peerSlot);

// The connect edge, after the snapshot trigger: one position correction per save-authoritative
// entity the host moved since the capture. Arms the late window and resets its dedupe.
void FlushDivergedPositions(int peerSlot);

// The late-arm cadence: re-flush each armed joiner's diverged positions until its window expires,
// then retire that slot's maps. From the transfer's host tick.
void TickLateArm();

// The save-time position of keyless chipPile `eid` for `peerSlot`. False (out untouched) for a
// stale-fallback join, an unseeded or post-save pile, or an out-of-range slot. The connect-replay
// snapshot builder stamps it onto the pile's spawn so the client's twin destroy reconciles a pile
// the host moved in the join window.
bool TryGetPileXform(int peerSlot, coop::element::ElementId eid, ue_wrap::FVector& out);

// The same for an entity that was a garbage CLUMP in the save this joiner loaded: its own copy is
// a clump at this position.
bool TryGetClumpXform(int peerSlot, coop::element::ElementId eid, ue_wrap::FVector& out);

// Like TryGetPileXform but across all active join slots: a convert broadcast is a single fan-out
// with no slot, and a pile eid is unique, so at most one slot holds it.
bool TryGetPileXformAnySlot(coop::element::ElementId eid, ue_wrap::FVector& out);

// The save-time position of off-form kerfur `eid`, across every active slot for the same reason.
// False if no slot captured it: a stale-fallback join, a kerfur bought after the save, or one
// already active at every blob instant. The kerfur table reads it at the first conversion to
// record the off-prop's origin eid, which the connect snapshot carries to the joiner as the eid to
// retire.
bool TryGetKerfurXformAnySlot(coop::element::ElementId eid, ue_wrap::FVector& out);

// Record the pre-grab position of pile `eid` into every active slot's map, at the seam where a
// grabbed pile's clump is born and before the pile dies in place, so what is recorded is still its
// save position -- the key the joiner's native sits at. The landing convert then carries it and the
// client arms a pending save-time twin. A no-op outside a join; a re-grab overwrites.
void RecordGrabTimePileXform(coop::element::ElementId eid, const ue_wrap::FVector& preGrabLoc);

// A peer left, or its stream was cancelled: drop its baseline and disarm its late flush.
void ClearForSlot(int peerSlot);

// True while `peerSlot`'s late flush is armed: a keyed prop the host moves now reaches that joiner
// as a position correction, the join's own reconcile rather than the lane that owns the move. A
// drill measuring a lane's own delivery starts once this is false.
bool IsLateWindowOpen(int peerSlot);

}  // namespace coop::join_window_baseline
