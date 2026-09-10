// coop/dev/eid_lifetime_trace.h -- the eid-lifetime TRACE (read-only, dev-only, HOST-side).
//
// THE QUESTION: the index-to-eid map and the bind on top of it rest on ONE unproven assumption --
// that the eid minted for a keyless native at SAVE-CAPTURE is the SAME eid the host later puts on
// the WIRE for it. If a re-mint, or a reap and re-seed, comes between them, the map binds the
// client native to one eid while the host poses another, the bind never matches, and the lane fails
// silently. Prove it before building the bind.
//
// THE TRACE: record actor-to-eid at capture, through the same GetPropElementIdForActor the map will
// use, then at the wire-expression (BuildPropSpawnPayload_) compare the eid it sends against the
// recorded one. The verdict at drain-complete is STABLE (every wire-eid equals its capture-eid) or
// DIVERGES. It observes and counts only.
//
// RULE-2-exempt diagnostic. Ini-gated [dev] eid_lifetime_trace=1 on the HOST; absent or 0 makes
// every call a cheap no-op. Game-thread only.
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
