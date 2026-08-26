// coop/element/intent_authority.cpp -- see the header for WHY this module exists and, above all,
// for the ceiling it claims (a COVERAGE fix, not a strength fix; the anchor deliberately does not
// move in this commit).

#include "coop/element/intent_authority.h"

#include <cmath>
#include <cstring>
#include <limits>

#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/movement_ledger.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

namespace coop::element {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// The pose-staleness budget, PROMOTED VERBATIM from `coingun_arbiter.cpp` (RULE 2 -- that copy is
// retired in the same commit). The host copy of a client body trails reality by the one-way
// latency + `RemotePlayer::kInterpWindowMs` (75 ms) + one send interval, and the game own reach
// traces start at the player CAMERA, at eye height, not at the actor root that `GetActorLocation`
// reports. 600 uu covers roughly half a second at a sprint plus that eye-height offset.
//
// Deliberately generous, and the reason is asymmetric cost: this bound exists to stop WORLD-WIDE
// enumeration, not to adjudicate centimetres, and a false refusal costs a real player a real
// action. Widening it does not re-open A50 -- the arbiter own destroy does that work.
constexpr float kPoseStalenessUU = 600.0f;

inline bool Finite3(const ue_wrap::FVector& v) {
    return std::isfinite(v.X) && std::isfinite(v.Y) && std::isfinite(v.Z);
}

inline float Dist3(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

namespace internal {

// The PURE half of the reach question, split out so the selftest at the bottom of this file can
// drive the real arithmetic rather than a copy of it. Returns the measured distance and what was
// allowed; the return value is the verdict.
//
// FAIL-CLOSED ON NON-FINITE INPUT, and stated rather than left implicit: the shipped predecessor
// had no explicit guard and relied on `dist <= allowed` being false for NaN, which is correct but
// invisible. A reader cannot tell an intentional fail-closed from an unconsidered one, and this
// lane has already shipped four generations of comment that overstated what was measured.
bool ReachVerdict(const ue_wrap::FVector& body, const ue_wrap::FVector& target,
                  float targetRadiusUU, float reachUU, float& outDist, float& outAllowed) {
    outDist = outAllowed = -1.f;
    if (!Finite3(body) || !Finite3(target) || !std::isfinite(targetRadiusUU) ||
        !std::isfinite(reachUU)) {
        return false;
    }
    outDist    = Dist3(target, body);
    outAllowed = reachUU + targetRadiusUU + kPoseStalenessUU;
    return outDist <= outAllowed;
}

// The target ORIGIN is not its surface, and the game traces stop at whatever they HIT, so a large
// prop is legitimately reachable from further away than reach-from-origin. Measure the real bounds
// instead of inventing a constant fudge for "big things".
//
// `GetActorBounds` returning true means THE DISPATCH SUCCEEDED, not that the box is meaningful:
// `[A]` UE4 `AActor::GetActorBounds` starts from an empty FBox and expands it per qualifying
// component, so an actor with no COLLIDING components (carried, physics off, inside a container)
// yields Origin=(0,0,0) Extent=(0,0,0) -- the WORLD ORIGIN, an ordinary position. A zero extent is
// therefore "no bounds", never "a point-sized prop at the origin".
bool TargetPointAndRadius(void* actor, ue_wrap::FVector& outOrigin, float& outRadiusUU) {
    outRadiusUU = 0.f;
    ue_wrap::FVector extent{};
    const bool haveBounds = E::GetActorBounds(actor, /*onlyColliding=*/true, outOrigin, extent) &&
                            !(extent.X == 0.f && extent.Y == 0.f && extent.Z == 0.f);
    if (haveBounds) {
        outRadiusUU = std::sqrt(extent.X * extent.X + extent.Y * extent.Y + extent.Z * extent.Z);
        return true;
    }
    // CHECKED READ ONLY. `E::GetActorLocation` returns a default FVector on failure and (0,0,0) is
    // the world origin, so a failed read would report the target as standing at the origin and
    // authorize anything else near it -- a fail-OPEN inside a function whose job is to fail closed.
    return E::TryGetActorLocation(actor, outOrigin);
}

void RunSelftest();

}  // namespace internal

namespace {

// THE DUAL-ANCHOR INSTRUMENT (see the header). We authorize off the PUPPET, which is what the
// shipped gate already did; we LOG what the ledger would have said. `movement_ledger.cpp:688`
// samples the same divergence at 0.1 Hz and `[V]` has produced exactly ONE sample in the whole
// corpus, on a stationary peer, so it has never answered the question it was built for. Sampling it
// HERE means every real intent contributes, at the site where the decision is actually made --
// which is the number that will justify or refute moving the anchor, instead of a header comment
// asserting it. It lives in `Authorize` because that is the only scope holding BOTH positions.
void LogAnchors(coop::net::Session* session, const IntentSubject& s, void* actor,
                const ue_wrap::FVector& body, const ue_wrap::FVector& target) {
    if (!session) return;
    ue_wrap::FVector ledgerPos{};
    if (!coop::movement_ledger::TryGetAcceptedPosition(*session, s.slot, ledgerPos) ||
        !Finite3(ledgerPos)) {
        UE_LOGI("intent_authority[slot %u]: actor=%p %s dist=%.0f (allowed %.0f) | ANCHOR the "
                "ledger has no accepted position for this slot",
                static_cast<unsigned>(s.slot), actor, OutcomeName(s.outcome), s.distUU, s.reachUU);
        return;
    }
    const float ledgerDist = Dist3(target, ledgerPos);
    UE_LOGI("intent_authority[slot %u]: actor=%p %s dist=%.0f (allowed %.0f) | ANCHOR "
            "puppet-vs-ledger=%.0f cm ledgerDist=%.0f (would %s) trusted=%d",
            static_cast<unsigned>(s.slot), actor, OutcomeName(s.outcome), s.distUU, s.reachUU,
            Dist3(body, ledgerPos), ledgerDist, ledgerDist <= s.reachUU ? "ALLOW" : "REFUSE",
            coop::movement_ledger::PositionTrusted(*session, s.slot) ? 1 : 0);
}

}  // namespace

const char* OutcomeName(IntentOutcome o) {
    switch (o) {
    case IntentOutcome::Ok:         return "ok";
    case IntentOutcome::NoRow:      return "no-row";
    case IntentOutcome::StaleDead:  return "stale-dead";
    case IntentOutcome::WrongType:  return "wrong-type";
    case IntentOutcome::NoBody:     return "no-body";
    case IntentOutcome::OutOfReach: return "out-of-reach";
    }
    return "?";
}

IntentTarget IntentTarget::ForClientIntent(coop::net::Session& session, uint8_t senderSlot,
                                           float reachUU) {
    IntentTarget t;
    t.session_ = &session;
    t.slot_    = senderSlot;
    t.reachUU_ = reachUU;
    return t;
}

IntentSubject IntentTarget::Resolve(ElementId eid, ElementType type) const {
    UE_ASSERT_GAME_THREAD("intent_authority::Resolve");

    IntentSubject s;
    s.slot    = slot_;
    s.reachUU = reachUU_;

    // --- IDENTITY FIRST -----------------------------------------------------------------------
    // Order matters and is not stylistic: a caller that branches on WrongType (the ghost-heal) must
    // get its actor even for a sender that could not have reached it, because that branch repairs a
    // cross-peer identity smear -- it does not grant an action.
    if (eid == 0) return s;                      // NoRow: the null eid names nothing
    Element* e = Registry::Get().Get(eid);
    if (!e) return s;                            // NoRow

    void* actor = e->GetActor();
    if (!actor || !R::IsLiveByIndex(actor, e->GetInternalIdx())) {
        s.outcome = IntentOutcome::StaleDead;
        return s;
    }
    if (e->GetType() != type) {
        s.outcome = IntentOutcome::WrongType;
        s.actor   = actor;                       // the caller heals with it; it must NEVER destroy
        return s;
    }

    // --- THEN REACH ---------------------------------------------------------------------------
    return Authorize(actor);
}

IntentSubject IntentTarget::Authorize(void* actor) const {
    IntentSubject s;
    s.slot    = slot_;
    s.reachUU = reachUU_;
    s.actor   = actor;   // set on every path below: OutOfReach and NoBody both name a real entity

    // Slot 0 is the HOST, which does not send itself intents. Minting a token for it is a
    // programming error rather than an attack, and it must not silently authorize: there is no
    // puppet for slot 0, so the resolve below would answer NoBody anyway -- this makes that
    // explicit instead of relying on a lookup happening to miss.
    //
    // THESE TWO CHECKS PRECEDE THE GAME-THREAD ASSERT ON PURPOSE, and the reason is a defect this
    // module shipped for exactly one smoke run: the assert used to sit at the top of the function,
    // and the selftest -- which runs from `OnSessionStart`, `[V]` NOT the game thread -- tripped it
    // twice per session start on every peer. The honest fix is not to stop testing the guard but to
    // notice that the assert was over-broad: neither branch below touches engine state, so neither
    // needs the game thread. Everything that DOES touch it is still asserted, one line down.
    if (!actor || slot_ == 0) { s.outcome = IntentOutcome::NoBody; return s; }

    UE_ASSERT_GAME_THREAD("intent_authority::Authorize");
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot_);
    void* puppet = (rp && rp->valid()) ? rp->GetActor() : nullptr;
    ue_wrap::FVector body{};
    if (!puppet || !E::TryGetActorLocation(puppet, body)) {
        // FAIL-CLOSED, and this is the shipped behaviour of both lanes that already ask:
        // `[V]` `coingun_arbiter.cpp:167-171` returns false and `[V]` `trash_grab_intent.cpp:161-164`
        // DENIES. The coin-gun header calls that "the principle-8 answer for this lane" and calls
        // inventing a reach for a body we cannot see "the enumeration hole".
        s.outcome = IntentOutcome::NoBody;
        return s;
    }

    ue_wrap::FVector target{};
    float radius = 0.f;
    if (!internal::TargetPointAndRadius(actor, target, radius)) {
        s.outcome = IntentOutcome::NoBody;       // we cannot say where it is, so we cannot vouch
        return s;
    }

    const bool ok = internal::ReachVerdict(body, target, radius, reachUU_, s.distUU, s.reachUU);
    s.outcome = ok ? IntentOutcome::Ok : IntentOutcome::OutOfReach;
    LogAnchors(session_, s, actor, body, target);
    return s;
}

namespace internal {

// UN-GATED, runs once per session start. Same reasoning as `movement_ledger`: a wrong verdict here
// does not crash, it merely reads wrong -- and the branches below are ones a two-peer LAN smoke
// cannot reach on demand. It drives the REAL `ReachVerdict`, not a copy of its arithmetic.
void RunSelftest() {
    int checks = 0, failed = 0;
    auto CHECK = [&](bool cond, const char* what) {
        ++checks;
        if (!cond) { ++failed; UE_LOGE("intent_authority selftest FAIL: %s", what); }
    };

    // 1. OutcomeName is TOTAL and distinct. A new enumerator added without a name would otherwise
    //    surface as "?" in a log line months later.
    {
        const IntentOutcome all[] = {IntentOutcome::Ok,        IntentOutcome::NoRow,
                                     IntentOutcome::StaleDead, IntentOutcome::WrongType,
                                     IntentOutcome::NoBody,    IntentOutcome::OutOfReach};
        const size_t n = sizeof(all) / sizeof(all[0]);
        for (size_t i = 0; i < n; ++i) {
            const char* a = OutcomeName(all[i]);
            CHECK(a && a[0] && a[0] != '?', "every outcome has a real name");
            for (size_t j = i + 1; j < n; ++j)
                CHECK(std::strcmp(a, OutcomeName(all[j])) != 0, "outcome names are distinct");
        }
    }

    // 2. The reach boundary, on the REAL arithmetic. allowed = reach + radius + staleness.
    {
        const ue_wrap::FVector origin{0.f, 0.f, 0.f};
        float d = 0.f, a = 0.f;

        // Exactly AT the boundary is allowed (<=, not <): a player standing at max reach is not a
        // cheater, and a strict < would make the boundary a coin flip against float rounding.
        const float allowed = 200.f + 0.f + kPoseStalenessUU;
        CHECK(ReachVerdict({allowed, 0.f, 0.f}, origin, 0.f, 200.f, d, a),
              "at the exact boundary -> ALLOW");
        CHECK(a == allowed, "allowed == reach + radius + staleness");
        CHECK(std::fabs(d - allowed) < 0.5f, "distance is the euclidean distance");

        CHECK(!ReachVerdict({allowed + 1.f, 0.f, 0.f}, origin, 0.f, 200.f, d, a),
              "one cm past the boundary -> REFUSE");

        // The prop own bounds widen the allowance -- that is the whole reason radius is a term.
        CHECK(ReachVerdict({allowed + 100.f, 0.f, 0.f}, origin, 150.f, 200.f, d, a),
              "a large prop is reachable from further away");

        // 3D, not 2D: a peer directly above must be measured in Z too.
        CHECK(!ReachVerdict({0.f, 0.f, allowed + 1.f}, origin, 0.f, 200.f, d, a),
              "the measurement is 3D");

        // THE ENUMERATION CASE this whole module exists for: the far side of the world.
        CHECK(!ReachVerdict({500000.f, 500000.f, 0.f}, origin, 0.f, 1000.f, d, a),
              "a peer across the map cannot reach a prop at the origin");
    }

    // 3. Non-finite input FAILS CLOSED, in every position.
    {
        const float inf = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        float d = 0.f, a = 0.f;
        CHECK(!ReachVerdict({nan, 0.f, 0.f}, {0.f, 0.f, 0.f}, 0.f, 200.f, d, a), "NaN body -> refuse");
        CHECK(!ReachVerdict({0.f, 0.f, 0.f}, {inf, 0.f, 0.f}, 0.f, 200.f, d, a), "inf target -> refuse");
        CHECK(!ReachVerdict({0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, nan, 200.f, d, a), "NaN radius -> refuse");
        CHECK(!ReachVerdict({0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 0.f, inf, d, a), "inf reach -> refuse");
        CHECK(d == -1.f && a == -1.f, "a refused verdict reports no measurement, not a stale one");
    }

    // 4. An INFINITE reach does not become an accept-everything door by arithmetic accident. (It is
    //    refused above because inf is non-finite -- this row pins that it is the GUARD doing it,
    //    not `inf <= inf` happening to be true.)
    {
        float d = 0.f, a = 0.f;
        CHECK(!ReachVerdict({1e9f, 0.f, 0.f}, {0.f, 0.f, 0.f}, 0.f,
                            std::numeric_limits<float>::infinity(), d, a),
              "an infinite reach is refused, not honoured");
    }

    // 5. A token for the HOST slot authorizes NOTHING, and it does so through the explicit guard
    //    rather than through a puppet lookup that happens to miss. This drives the PRODUCTION
    //    `Authorize`, not a copy: the actor is a non-null sentinel that never reaches an engine
    //    call because the slot check precedes it.
    {
        char sentinel = 0;
        IntentTarget hostTok;   // default: no session, slot 0
        const IntentSubject r = hostTok.Authorize(&sentinel);
        CHECK(r.outcome == IntentOutcome::NoBody, "a slot-0 token authorizes nothing");
        CHECK(!static_cast<bool>(r), "and its result is falsy");
        CHECK(r.actor == &sentinel, "while still naming the subject it refused");
        const IntentSubject rn = hostTok.Authorize(nullptr);
        CHECK(rn.outcome == IntentOutcome::NoBody, "a null subject authorizes nothing");
    }

    if (failed == 0) UE_LOGI("intent_authority selftest: ALL PASS (%d checks)", checks);
    else             UE_LOGE("intent_authority selftest: %d of %d checks FAILED", failed, checks);
}

}  // namespace internal

void RunSelftest() { internal::RunSelftest(); }

}  // namespace coop::element
