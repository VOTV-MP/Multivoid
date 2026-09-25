// coop/props/container_write_policy.cpp -- see coop/props/container_write_policy.h.

#include "coop/props/container_write_policy.h"

#include "coop/element/element.h"
#include "coop/element/intent_authority.h"
#include "coop/net/connect_history.h"
#include "coop/net/session.h"
#include "ue_wrap/actors/container_openers.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <map>
#include <set>
#include <utility>

namespace coop::props::container_write_policy {
namespace {

// Host: the content most recently published for an eid by any route, fan-out or a targeted
// connect seed. This is the compare-and-swap baseline ("what did I tell that peer the world looked
// like"); the lane's own g_sentHash answers a different question ("may I skip the next fan-out"),
// and a targeted send must not answer yes to that one.
std::map<uint32_t, uint64_t> g_publishedHash;

// This peer's own last verb edge per eid; the host uses it to detect a client write that raced a
// host-side change.
std::map<uint32_t, uint64_t> g_localChangeMs;

uint64_t g_refused = 0;

// One instance of the tree's per-source limit, the same class the accept edge and the lobby
// password use. MTA bounds a source at the JOIN edge alone (CConnectHistory, CGame.cpp:163) and
// needs no in-session twin, because there a client never authors a server-owned element's state --
// it sends pure-sync for the ped it owns and the server writes the rest. Our container verbs run
// below any seam that could refuse them, so the peer authors and the host arbitrates, and this is
// where that divergence gets its bound. Game thread, unlike its siblings on the net thread; each
// instance is lock-free and single-threaded on its own.
coop::net::connect_history::History g_writes{
    coop::net::connect_history::Policy{kRateMax, kRateWindowMs, kRateWindowMs},
    "container writes"};

// A container the game opens through another actor stands where no player does: the drone's inventory
// kilometres off, the ATV's under the world origin. Its author reaches it through the actor that opens
// it -- the game's own field on the opener, never the author's claim.
void* OpenerInReach(const coop::element::IntentTarget& tok, void* container) {
    struct Ctx {
        const coop::element::IntentTarget* tok;
        void* reached;
    } ctx{&tok, nullptr};
    ue_wrap::container_openers::ForEach(container, [](void* c, void* opener) {
        Ctx& x = *static_cast<Ctx*>(c);
        if (x.tok->Authorize(opener).outcome != coop::element::IntentOutcome::Ok) return true;
        x.reached = opener;
        return false;
    }, &ctx);
    return ctx.reached;
}

// Said once per container and author, the first time an author reaches it through an opener.
std::set<std::pair<uint32_t, uint8_t>> g_reachedThrough;

}  // namespace

Decision Judge(const Inputs& in) {
    // An up-to-date author edited from what the host last published; an author that never received
    // anything sends 0 and is refused rather than trusted.
    if (in.baseHash == 0 || in.baseHash != in.publishedHash) return Decision::StaleBase;
    // The author edited the published world; still refused if the host changed this container
    // inside the conflict window, a change in flight the author provably had not seen.
    if (in.lastLocalChangeMs != 0 && in.nowMs - in.lastLocalChangeMs <= kConflictWindowMs)
        return Decision::HostChangeInFlight;
    return Decision::Accept;
}

Decision JudgeAgainstState(uint32_t eid, uint64_t baseHash, uint64_t nowMs) {
    Inputs in;
    in.baseHash = baseHash;
    in.nowMs    = nowMs;
    if (auto it = g_publishedHash.find(eid); it != g_publishedHash.end()) in.publishedHash = it->second;
    if (auto it = g_localChangeMs.find(eid); it != g_localChangeMs.end()) in.lastLocalChangeMs = it->second;
    return Judge(in);
}

Decision Accept(uint32_t eid, uint64_t baseHash, uint8_t authorSlot, uint64_t nowMs,
                coop::net::Session& session, Source src) {
    // The budget is spent by ARRIVAL, not by acceptance: a refused slice still costs the host a
    // parse, this arbitration and a corrective re-publish. A replay out of the park spends
    // nothing -- it arrived once already, and a pen sweeping four times a second would otherwise
    // rate-block the author of the very slice it is holding. A seated peer's proved storage guid
    // is the source; an author the host has no proved identity for is not counted rather than
    // refused, the same way a full table here refuses nobody -- every other gate still applies.
    coop::net::connect_history::Key key;
    if (src == Source::Arrival &&
        coop::net::connect_history::KeyFromProvedGuid(session.ProvedGuidForSlot(authorSlot), key)) {
        const auto v = g_writes.Note(key, nowMs);
        if (v.refused) {
            ++g_refused;
            UE_LOGW("container_contents: CONFLICT eid=%u slot %u -- %d slices inside %llu ms is "
                    "past the bound; refused for %llu ms. Write REFUSED, and nothing re-published: an "
                    "author pushing faster must not make the host spend more. Total refused this "
                    "session: %llu",
                    eid, static_cast<unsigned>(authorSlot), v.count,
                    static_cast<unsigned long long>(kRateWindowMs),
                    static_cast<unsigned long long>(v.retryMs),
                    static_cast<unsigned long long>(g_refused));
            return Decision::TooFast;
        }
    } else if (src == Source::Arrival) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("container_contents: slot %u has no proved identity -- its slices are "
                    "arbitrated but not rate-bounded", static_cast<unsigned>(authorSlot));
        }
    }

    // REACH, and only where it is answerable. A container's contents are mutated through the
    // player's own look-at trace, so an author that is neither at the container nor at an actor that
    // opens it (the drone, its sack, the ATV) did not run the verb it is reporting. NoRow and StaleDead
    // are not refusals here: they mean the element has not arrived (or has gone), which is the park's
    // question, not this one.
    const auto tok = coop::element::IntentTarget::ForClientIntent(session, authorSlot, kReachUU);
    const auto sub = tok.Resolve(static_cast<coop::element::ElementId>(eid),
                                 coop::element::ElementType::Prop);
    if (sub.outcome == coop::element::IntentOutcome::NoBody) return Decision::NoBodyYet;
    // Far, or with no place to measure to: the author may still stand at an actor that opens it.
    const bool unmet = sub.outcome == coop::element::IntentOutcome::OutOfReach ||
                       sub.outcome == coop::element::IntentOutcome::NoTarget;
    void* const opener = unmet ? OpenerInReach(tok, sub.actor) : nullptr;
    if (opener && g_reachedThrough.insert({eid, authorSlot}).second)
        UE_LOGI("container_contents: eid=%u slot %u reached through the %ls that opens it (the container: "
                "%s, %.0f uu away)", eid, static_cast<unsigned>(authorSlot),
                ue_wrap::reflection::ClassNameOf(opener).c_str(), coop::element::OutcomeName(sub.outcome),
                sub.distUU);
    if (unmet && !opener) {
        ++g_refused;
        UE_LOGW("container_contents: CONFLICT eid=%u slot %u -- the author cannot reach it nor any actor "
                "that opens it (%s, dist=%.0f allowed=%.0f). Write REFUSED; re-publishing host truth to the "
                "author. Total refused this session: %llu",
                eid, static_cast<unsigned>(authorSlot),
                coop::element::OutcomeName(sub.outcome), sub.distUU, sub.reachUU,
                static_cast<unsigned long long>(g_refused));
        return Decision::Unreachable;
    }

    const Decision d = JudgeAgainstState(eid, baseHash, nowMs);
    if (d == Decision::Accept) return d;

    uint64_t published = 0;
    if (auto it = g_publishedHash.find(eid); it != g_publishedHash.end()) published = it->second;
    ++g_refused;
    // The failed condition is named: reported together, a never-published container once read as a
    // racing host change.
    UE_LOGW("container_contents: CONFLICT eid=%u slot %u -- %s (author base=%llu, host published=%llu). "
            "Write REFUSED; re-publishing host truth to the author. Total refused this session: %llu",
            eid, static_cast<unsigned>(authorSlot),
            d == Decision::HostChangeInFlight
                ? "a HOST-side change is in flight within the conflict window"
                : "the author edited a state the host has not published (STALE BASE)",
            static_cast<unsigned long long>(baseHash),
            static_cast<unsigned long long>(published),
            static_cast<unsigned long long>(g_refused));
    return d;
}

void NotePublished(uint32_t eid, uint64_t contentHash) { g_publishedHash[eid] = contentHash; }

void NoteLocalChange(uint32_t eid, uint64_t nowMs) { g_localChangeMs[eid] = nowMs; }

bool RunSelftest() {
    Reset();
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("container_write_policy selftest FAIL: %s", what);
    };
    constexpr uint32_t kEid = 4242;
    constexpr uint64_t kH0 = 0x1111, kH1 = 0x2222, kH2 = 0x3333;

    // The negatives first: nothing published yet, so no base can match.
    check(JudgeAgainstState(kEid, 0, 1000) == Decision::StaleBase, "a base of zero is refused");
    check(JudgeAgainstState(kEid, kH0, 1000) == Decision::StaleBase,
          "any base is refused for a container the host has not published");

    NotePublished(kEid, kH0);
    check(JudgeAgainstState(kEid, 0, 1000) == Decision::StaleBase,
          "a base of zero is refused even once published");
    check(JudgeAgainstState(kEid, kH1, 1000) == Decision::StaleBase,
          "a base that is not what was published is refused");
    check(JudgeAgainstState(kEid, kH0, 1000) == Decision::Accept,
          "the base the host published is accepted");

    // A host-side change inside the window refuses; outside it, the same base passes again.
    NoteLocalChange(kEid, 1000);
    check(JudgeAgainstState(kEid, kH0, 1000 + kConflictWindowMs) == Decision::HostChangeInFlight,
          "a host change inside the conflict window refuses");
    check(JudgeAgainstState(kEid, kH0, 1001 + kConflictWindowMs) == Decision::Accept,
          "the window ends and the same base is accepted");

    // The sequence a third peer walks into. The host accepted client A's slice and relayed it, so
    // the world it published is now that content; B, which applied the relay, declares it and must
    // be accepted, while A's own older base must not be.
    NotePublished(kEid, kH1);
    check(JudgeAgainstState(kEid, kH1, 100'000) == Decision::Accept,
          "the peer holding what the relay carried is accepted");
    check(JudgeAgainstState(kEid, kH0, 100'000) == Decision::StaleBase,
          "the peer still holding the older fan-out is refused");
    NotePublished(kEid, kH2);
    check(JudgeAgainstState(kEid, kH1, 100'000) == Decision::StaleBase,
          "and one publication later that peer is behind in turn");

    Reset();
    if (pass == total) {
        UE_LOGI("container_write_policy selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("container_write_policy selftest: %d/%d checks passed", pass, total);
    return false;
}

void Reset() {
    g_publishedHash.clear();
    g_localChangeMs.clear();
    g_writes.Clear();
    g_refused = 0;
    g_reachedThrough.clear();
}

}  // namespace coop::props::container_write_policy
