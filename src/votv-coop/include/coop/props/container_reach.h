// coop/props/container_reach.h -- a container's reach: the container itself, or an actor that opens
// it (ue_wrap/actors/container_openers), within the distance an author may act from. One judgement,
// shared by the host's arbitration of a client's slice (container_write_policy) and by the opener
// probe, so the number and the rule have one home. Game thread.

#pragma once

#include "coop/element/intent_authority.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::props::container_reach {

// The lane's own reach, before intent_authority adds the target's bounds and its pose-staleness
// budget. The game opens and mutates a container through the camera trace mainPlayer::arm, whose
// default length is armLength = 200 uu, so that is the number this lane owns.
inline constexpr float kReachUU = 200.0f;

enum class Reach : uint8_t {
    Reached,      // the author is at the container, or at an actor that opens it, or the element is not
                  // here to measure (NoRow, StaleDead: the hold's question, not this one)
    NoBody,       // the author has no body on this machine to measure a reach from yet
    Unreachable,  // neither the container nor any actor that opens it is within reach
};

struct ReachResult {
    Reach                        reach;
    coop::element::IntentOutcome outcome;  // the container's own outcome, for the caller's refusal log
    float                        distUU;
    float                        reachUU;
};

// Judge the author's reach of the prop `eid`. A container's contents are mutated through the player's
// own look-at trace, so an author that is neither at the container nor at an actor that opens it (the
// drone, its sack, the ATV) did not run the verb it is reporting. Logs, once per container and author,
// the first time an author reaches it through an opener.
ReachResult Judge(coop::net::Session& session, uint8_t authorSlot, uint32_t eid);

// Forget which (container, author) pairs have been logged as reached through an opener; session end.
void Reset();

}  // namespace coop::props::container_reach
