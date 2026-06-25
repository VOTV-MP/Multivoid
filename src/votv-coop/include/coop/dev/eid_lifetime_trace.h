// coop/dev/eid_lifetime_trace.h -- Phase 1 step 8.2: eid-lifetime TRACE (read-only, dev-only, HOST-side).
//
// THE QUESTION (eid-range bind mini-design S8.2): the whole index->eid map + the bind rest on ONE unproven
// assumption -- the host eid minted for a keyless native at SAVE-CAPTURE is the SAME eid the host later puts
// on the WIRE for that native. If a capture-eid != its wire-eid (a re-mint / reap+re-seed between capture and
// expression), the map would bind the client native to one eid while the host expresses/poses another -> the
// bind never matches -> Phase 1 silently fails. This is the last unproven correctness link; prove it before
// building the bind (probe-first discipline, as spawn-order was proven before the map -- step 1A).
//
// THE TRACE (read-only, HOST-side): record actor->eid at capture (the same GetPropElementIdForActor the
// IdMap will use, from CollectTrackedPileTransforms / CollectTrackedKerfurTransforms), then at the snapshot
// wire-expression (BuildPropSpawnPayload_) compare the eid the host is about to send against the recorded
// capture-eid for the same actor. Verdict at drain-complete: STABLE (every wire-eid == its capture-eid) or
// DIVERGES (a re-mint happened -> mini-design needs a fix before the bind). Observes + counts only.
// RULE-2-exempt diagnostic ([[feedback-rule2-exempts-probes-diagnostics-tools]]). Ini-gated
// [dev] eid_lifetime_trace=1 on the HOST; absent/0 = every call is a cheap no-op. Game-thread only.
#pragma once

#include <cstdint>

namespace coop::dev::eid_lifetime_trace {

bool IsEnabled();  // [dev] eid_lifetime_trace=1, latched

// HOST capture: record the eid resolved for `actor` at save-capture (CollectTracked{Pile,Kerfur}Transforms).
void RecordCaptureEid(void* actor, uint32_t eid);

// HOST wire-expression: the host is about to send `wireEid` for `actor` (BuildPropSpawnPayload_). If a
// capture-eid was recorded for `actor`, tally match/mismatch (deduped per actor).
void CheckWireEid(void* actor, uint32_t wireEid);

// HOST drain-complete: emit the verdict (captured / wire-checked / matched / mismatched) + STABLE|DIVERGES,
// then clear for the next join.
void EmitVerdict();

}  // namespace coop::dev::eid_lifetime_trace
