// coop/element/intent_authority.h -- the SOLE owner of "may this sender name this artifact?"
//
// WHY THIS EXISTS (security A54). A client-intent handler used to receive a bare `uint8_t
// senderSlot`, resolve whatever artifact the client named, and act on it. `[V]` A handler-side
// census (2026-08-26) found 102 sender-taking handlers, of which roughly TEN ask a genuine
// authority question -- `device_occupancy::OnReliable`, `trash_grab_intent::OnGrabIntent`,
// `desk_input_sync::OnDeskInput`, `remote_prop::OnRelease`, `remote_prop_spawn::OnSpawn`,
// `chat_sync::OnReliable`, `console_state_sync::OnDishAim`, `coingun_arbiter::OnReliable` -- each
// bespoke, sharing no notion of authority with any other, while >=48 never let the sender reach a
// decision at all. So the defect A54 names is NOT "nothing owns the question": it is that authority
// is a per-lane folk practice with no shared owner, and nothing makes a new lane inherit any of it.
//
// WHAT MAKES THE CHECK HARD TO SKIP is not a classification word a handler can declare and ignore
// (`[[lesson-an-inc-of-ints-is-convention-wearing-a-ratchets-hat]]`) but the ARGUMENT TYPE: a
// client-intent handler takes an `IntentTarget` INSTEAD of a bare slot, and an actor comes back only
// from `Resolve`. THE HONEST CEILING, stated here so it is not overclaimed later: this gates the
// SLOT, not the actor. `[V]` `element::Registry::Get()` is a global singleton with 48
// `Registry::Get().Get(` and 111 `GetActor()` sites in the tree, so a handler holding a token can
// still resolve by hand. Skipping authority becomes a DELIBERATE act rather than an omission --
// strictly more than the tree had, strictly less than "cannot compile".
//
// AND IT IS A COVERAGE FIX, NOT A STRENGTH FIX. The reach anchor is the sender's own body as the
// HOST last saw it, and the sender writes that value: `[V]` `movement_ledger.cpp:255` stores
// `r.lastPos = pos` outside every trust branch, and per the user's 2026-08-25 "Just log" decision
// the ledger's TRUST verdict may never refuse. So an attacker pays ONE EXTRA POSE PACKET, not a
// closure -- the same honest label A50 already carries. What this buys is ten bespoke owners
// collapsing to one, and the lanes that asked nothing beginning to ask.
//
// THE ANCHOR DELIBERATELY DOES NOT MOVE IN THIS COMMIT. `movement_ledger.h:203` says the enforcing
// build should authorize off `TryGetAcceptedPosition` rather than the puppet transform (the puppet
// is written by the interpolator and SNAPS on a teleport, and it goes stale where the ledger's
// sample count visibly does not). That reasoning is sound and UNQUANTIFIED: `[V]` the entire log
// corpus across all four installs contains exactly ONE `wireVsActor` sample, reading 0 cm on a line
// that also reads `maxStep=0 cm maxImplied=0 cm/s` -- a STATIONARY peer, where the divergence is
// trivially zero. Moving a SHIPPED gate's anchor on that would be citing a header comment as a
// measurement. Instead `Resolve` LOGS both anchors at every real intent, which produces the number
// `movement_ledger.cpp:688` was built for at the site where the decision actually matters.
//
// GT-only: every path here touches engine object state.

#pragma once

#include <cstdint>

#include "coop/element/element.h"
#include "ue_wrap/engine/engine.h"

namespace coop::net { class Session; }

namespace coop::element {

// WHY the resolve did not produce a usable actor. This is an OUTCOME, not a bool, and that is the
// whole point of the type: `[V]` `trash_grab_intent.cpp:167-219` branches THREE ways with OPPOSITE
// remedies on what the canonical `LiveActorOfType` collapses into one `nullptr` -- absent row and
// stale-dead actor both broadcast the ghost-heal destroy, while a LIVE actor of the wrong class
// must NEVER be destroyed (`:194`) and is healed by re-asserting one authoritative PropSpawn, a
// branch that CONSUMES the actor at `:209-219`. A pointer-returning resolver cannot carry that
// distinction, which is why this type exists at all rather than an authority argument bolted onto
// `LiveActorOfType`.
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
// `reachUU` is the LANE'S OWN reach and belongs to the caller, not to this module: `[V]` the
// coin-gun's is `arm(1000.0)` from its ubergraph's own trace, while grab/collect/drop derive from
// `mainPlayer.armLength = 200`. A single shared constant here would be this module inventing a
// number the game already answers per verb.
class IntentTarget {
  public:
    // The sender is a CLIENT slot. Minting for slot 0 is a programming error (the host does not
    // send itself intents) and yields a token whose every Resolve answers `NoBody`.
    static IntentTarget ForClientIntent(coop::net::Session& session, uint8_t senderSlot,
                                        float reachUU);

    uint8_t Slot()    const { return slot_; }
    float   ReachUU() const { return reachUU_; }

    // Resolve + authorize in ONE call. Order is deliberate: identity first (so a caller that
    // branches on WrongType gets its actor even when the sender could not have reached it), then
    // reach. Fail-CLOSED on an unreadable body -- `[V]` both lanes that already ask this refuse
    // there (`coingun_arbiter.cpp:167-171`, `trash_grab_intent.cpp:161-164`), and the coin-gun's
    // header calls inventing a reach for a body we cannot see "the enumeration hole".
    IntentSubject Resolve(ElementId eid, ElementType type) const;

    // The REACH primitive, for a caller that already holds the artifact because it named it by
    // KEY rather than by eid. This is not a second API for one concept: `[V]` four wire payloads
    // (`PropDestroy`, `PropSpawn`, `PropRelease`, `PropStickState`, plus `CoinGunSell`) carry BOTH
    // `key` and `elementId` for ONE artifact, so "name by key" and "name by eid" are two spellings
    // of the same subject and `Resolve` is simply identity followed by this. Never returns
    // `NoRow`/`StaleDead`/`WrongType` -- the caller already answered identity.
    IntentSubject Authorize(void* actor) const;

  private:
    coop::net::Session* session_ = nullptr;
    uint8_t             slot_    = 0;
    float               reachUU_ = 0.f;
};

// UN-GATED, once per session start. Drives the REAL reach arithmetic through the branches a
// two-peer LAN smoke cannot reach on demand. Same reasoning as `movement_ledger`: a wrong verdict
// here does not crash, it merely reads wrong.
void RunSelftest();

}  // namespace coop::element
