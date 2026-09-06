// coop/element/intent_authority.h -- the SOLE owner of "may this sender name this artifact?"
//
// Before it, a client-intent handler received a bare `uint8_t senderSlot`, resolved whatever
// artifact the client named and acted on it. Only about ten of the hundred-odd sender-taking
// handlers ask a genuine authority question, each bespoke, sharing no notion of authority with
// any other, and nothing makes a new lane inherit any of it.
//
// What makes the check hard to skip is not a classification word a handler can declare and
// ignore but the ARGUMENT TYPE: a client-intent handler takes an `IntentTarget` instead of a
// bare slot, and an actor comes back only from `Resolve`. THE CEILING, stated so it is not
// overclaimed later: this gates the SLOT, not the actor. The element registry is a global
// singleton reachable from anywhere, so a handler holding a token can still resolve by hand;
// skipping authority becomes a deliberate act rather than an omission.
//
// GT-only: every path here touches engine object state.

#pragma once

#include <cstdint>

#include "coop/element/element.h"
#include "ue_wrap/engine/engine.h"

namespace coop::net { class Session; }

namespace coop::element {

// WHY the resolve did not produce a usable actor. This is an OUTCOME, not a bool, and that is
// the whole point of the type: trash_grab_intent branches three ways with OPPOSITE remedies on
// what a pointer-returning resolver collapses into one `nullptr`. An absent row and a
// stale-dead actor both broadcast the ghost-heal destroy, while a LIVE actor of the wrong class
// must never be destroyed and is healed by re-asserting one authoritative PropSpawn, a branch
// that consumes the actor. A pointer cannot carry that distinction, which is why this type
// exists rather than an authority argument bolted onto the plain lookup.
enum class IntentOutcome : uint8_t {
    Ok = 0,      // live actor of the requested type, within the sender's reach
    NoRow,       // no Element row under this eid
    StaleDead,   // row present, actor pointer no longer live
    WrongType,   // a LIVE actor of another ElementType -- `actor` IS SET
    NoBody,      // the sender has no live puppet on the host: no body to measure a reach from
    OutOfReach,  // live and of the right type, but outside the sender's reach -- `actor` IS SET
};

// One authorized subject. `actor` is populated for `Ok`, `WrongType` and `OutOfReach`, because all
// three name a real entity and callers legitimately act on two of them.
//
// `StaleDead` deliberately does NOT carry the actor, even though a pointer exists: it is a pointer
// to an object the engine has already reclaimed, and handing it out invites exactly the deref this
// module's `IsLiveByIndex` check exists to prevent. Callers that used to read that pointer were
// reading it to tell 'no row' from 'row with a dead actor', and `outcome` now answers that directly.
struct IntentSubject {
    IntentOutcome outcome = IntentOutcome::NoRow;
    void*         actor   = nullptr;
    uint8_t       slot    = 0;
    float         reachUU = 0.f;   // what was allowed, prop bounds + staleness included
    float         distUU  = -1.f;  // what was measured, -1 when no measurement was possible

    explicit operator bool() const { return outcome == IntentOutcome::Ok; }
};

// A short, stable, log-safe reason string. ASCII on purpose -- these end up in `%s` in lines that
// are grepped by the autonomous harness.
const char* OutcomeName(IntentOutcome o);

// The ARGUMENT TYPE. A client-intent handler takes this instead of `uint8_t senderSlot`.
//
// `reachUU` is the LANE'S OWN reach and belongs to the caller, not to this module: the
// coin-gun's comes from its own trace at 1000 uu, while grab, collect and drop derive from
// `mainPlayer.armLength`. A single shared constant here would be this module inventing a
// number the game already answers per verb.
class IntentTarget {
  public:
    // The sender is a CLIENT slot. Minting for slot 0 is a programming error (the host does not
    // send itself intents) and yields a token whose every Resolve answers `NoBody`.
    static IntentTarget ForClientIntent(coop::net::Session& session, uint8_t senderSlot,
                                        float reachUU);

    uint8_t Slot()    const { return slot_; }
    float   ReachUU() const { return reachUU_; }

    // Resolve + authorize in ONE call. Order is deliberate: identity first, so a caller that
    // branches on WrongType gets its actor even when the sender could not have reached it, then
    // reach. Fail-CLOSED on an unreadable body, as both lanes that already ask this do: inventing
    // a reach for a body we cannot see is the hole the whole type exists to close.
    IntentSubject Resolve(ElementId eid, ElementType type) const;

    // The REACH primitive, for a caller that already holds the artifact because it named it by KEY
    // rather than by eid. Not a second API for one concept: four wire payloads carry BOTH `key` and
    // `elementId` for one artifact, so naming by key and naming by eid are two spellings of the
    // same subject and `Resolve` is identity followed by this. Never returns NoRow, StaleDead or
    // WrongType -- the caller already answered identity.
    IntentSubject Authorize(void* actor) const;

  private:
    coop::net::Session* session_ = nullptr;
    uint8_t             slot_    = 0;
    float               reachUU_ = 0.f;
};

// UN-GATED, once per session start. Drives the real reach arithmetic through branches a two-peer
// session cannot reach on demand. As with `movement_ledger`, a wrong verdict here does not
// crash, it merely reads wrong.
void RunSelftest();

}  // namespace coop::element
