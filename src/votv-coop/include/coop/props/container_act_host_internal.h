// coop/props/container_act_host_internal.h -- what the host's three files share: container_act_host.cpp (the queue, the
// holds' ends, the take), container_act_host_put.cpp (the judge of a put from a record, its reservation and its late
// commit) and container_act_host_overlap.cpp (the judge of a world prop that enters a container's box). Not an API: only
// those files include it. Game thread.

#pragma once

#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_act_state.h"
#include "coop/props/container_act_wire.h"
#include "coop/props/container_record_edit.h"

#include <cstdint>
#include <optional>

namespace coop::props::container_act_host::detail {

// The host's tally and its refusal log (one line per window, then summed).
void Bump(Tally t);
void NoteRefusal(uint8_t slot, const char* why);

// Write a hold's record into its container; a write that cannot happen now is owed and retried.
void Place(container_act_state::Hold h, const char* why, container_record_edit::Source src, const char* what);

// The answer to a request, and its payload for a verdict.
bool Answer(uint8_t slot, const coop::net::ContainerActReplyPayload& reply);
coop::net::ContainerActReplyPayload ReplyOf(internal::Verdict v, uint16_t id);

// A put's own terms for `record` into the container `eid`, read after the common ones (see DecidePut for the order). The
// record is the wire's or the host's own capture of a world prop; `slowable` says whether a sender's lapsed reservation
// refuses the put (a hand item's, not a world prop's).
void ReadPutInputs(uint8_t slot, uint32_t eid, const ue_wrap::save_record::SaveRecord& record, bool slowable,
                   internal::PutInputs& p);

// A put's grant: room is reserved for the asked record and nothing is written. Empty when the grant stands.
std::optional<coop::net::Served> GrantPut(uint8_t slot, const container_act_wire::Request& q);

// A commit whose hold is gone. True when it was a tombstone's (consumed whatever came of it); false when it has none.
bool LateCommit(uint8_t slot, const container_act_wire::Commit& c);

// A timely done of a closed reservation `h`: re-checked on the live container, then inserted, or lost and named.
void PlaceTimelyDone(container_act_state::Hold h, const container_act_wire::Commit& c);

// The put drill's reds (container_act_host_put.cpp), put back at the session's end.
void ResetPutDev();

// A put head was judged: counted while the drill asks (CountPutJudgements).
void NoteJudged(uint8_t slot, uint16_t id);

// A grant sent (or queued) or a put commit inserted, in the order the host did them: kept for the drill while it asks
// (CountPutJudgements), read by dev::OrderSeq.
void NoteOrder(uint8_t slot, uint32_t eid, bool grant);

// The host's truth of a container to the sender of a put commit it refused or lost (MTA syncs a refused change back to
// its source, CGame.cpp:2779-2795). Unlike a request's refusal this is bounded by the commit, one per hold.
void SendTruthNow(uint8_t slot, uint32_t eid);

}  // namespace coop::props::container_act_host::detail
