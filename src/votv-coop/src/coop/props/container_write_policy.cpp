// coop/props/container_write_policy.cpp -- see coop/props/container_write_policy.h.

#include "coop/props/container_write_policy.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/element.h"
#include "coop/element/intent_authority.h"
#include "coop/net/connect_history.h"
#include "coop/net/session.h"
#include "coop/props/container_reach.h"
#include "ue_wrap/core/log.h"

namespace coop::props::container_write_policy {
namespace {

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

}  // namespace

// The compare-and-swap of MTA's sync time context (reference/mtasa-blue/Server/mods/deathmatch/logic/CElement.cpp:1281-1305:
// GenerateSyncTimeContext at every write, CanUpdateSync at the remote's). Two deliberate differences: the context here
// is a content hash read at the arbitration, not a counter bumped by each writer, so no writer needs a hook to be
// judged (A to B to A reads as current, harmless for contents); and base 0 is refused, where MTA's remote 0 means ignore.
Decision Judge(const Inputs& in) {
    // Nothing to compare a base with: refused, not guessed at.
    if (!in.liveReadable) return Decision::Unreadable;
    // An up-to-date author edited from what the container holds now; an author that never received
    // anything sends 0 and is refused rather than trusted.
    if (in.baseHash == 0 || in.baseHash != in.liveHash) return Decision::StaleBase;
    return Decision::Accept;
}

Decision SpendArrival(uint32_t eid, uint8_t authorSlot, uint64_t nowMs, coop::net::Session& session,
                      Source src, bool held) {
    // The budget is spent by ARRIVAL, not by acceptance: a refused slice still costs the host a
    // parse, this arbitration and a corrective re-publish. A replay out of bind_wait or the pen spends
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
                    "past the bound; refused for %llu ms. %s Total refused this session: %llu",
                    eid, static_cast<unsigned>(authorSlot), v.count,
                    static_cast<unsigned long long>(kRateWindowMs),
                    static_cast<unsigned long long>(v.retryMs),
                    held ? "A birth slice: HELD in the pen and replayed, since it is the thrower's only copy."
                         : "Write REFUSED, and nothing re-published: an author pushing faster must not make "
                           "the host spend more.",
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
    return Decision::Accept;
}

Decision Accept(uint32_t eid, uint64_t baseHash, LiveHashReader readLive, uint8_t authorSlot, uint64_t nowMs,
                coop::net::Session& session, Source src) {
    if (SpendArrival(eid, authorSlot, nowMs, session, src) == Decision::TooFast) return Decision::TooFast;

    // REACH: container_reach judges it; the refusal and its count are this policy's.
    const auto reach = container_reach::Judge(session, authorSlot, eid);
    if (reach.reach == container_reach::Reach::NoBody) return Decision::NoBodyYet;
    if (reach.reach == container_reach::Reach::Unreachable) {
        ++g_refused;
        UE_LOGW("container_contents: CONFLICT eid=%u slot %u -- the author cannot reach it nor any actor "
                "that opens it (%s, dist=%.0f allowed=%.0f). Write REFUSED; re-publishing host truth to the "
                "author. Total refused this session: %llu",
                eid, static_cast<unsigned>(authorSlot),
                coop::element::OutcomeName(reach.outcome), reach.distUU, reach.reachUU,
                static_cast<unsigned long long>(g_refused));
        return Decision::Unreachable;
    }

    // The container is read only now, past the rate and the reach: a slice refused earlier costs no read.
    Inputs in;
    in.baseHash     = baseHash;
    in.liveReadable = readLive(eid, in.liveHash);
    const uint64_t liveHash = in.liveHash;
    Decision d = Judge(in);
    // Dev red arm: a stale base that is not zero is accepted (an unreadable read is still refused), as when the host
    // judged against what it last published and never saw a write nothing published.
    static const bool judgeRed = coop::config::ResolveFlag(coop::config_registry::rows::container_judge_red);
    if (judgeRed && d == Decision::StaleBase && baseHash != 0) {
        UE_LOGW("container_contents: eid=%u slot %u -- container_judge_red: base %llu accepted against live "
                "contents %llu", eid, static_cast<unsigned>(authorSlot),
                static_cast<unsigned long long>(baseHash), static_cast<unsigned long long>(liveHash));
        d = Decision::Accept;
    }
    if (d == Decision::Accept) return d;

    ++g_refused;
    // The failed condition is named.
    UE_LOGW("container_contents: CONFLICT eid=%u slot %u -- %s (author base=%llu, host live=%llu). "
            "Write REFUSED; re-publishing host truth to the author. Total refused this session: %llu",
            eid, static_cast<unsigned>(authorSlot),
            d == Decision::Unreadable
                ? "the container's live contents do not read (UNREADABLE)"
                : "the author edited contents the container no longer holds (STALE BASE)",
            static_cast<unsigned long long>(baseHash),
            static_cast<unsigned long long>(liveHash),
            static_cast<unsigned long long>(g_refused));
    return d;
}

bool RunSelftest() {
    Reset();
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("container_write_policy selftest FAIL: %s", what);
    };
    constexpr uint64_t kH0 = 0x1111, kH1 = 0x2222, kH2 = 0x3333;
    auto judge = [](uint64_t base, uint64_t live, bool readable) {
        Inputs in;
        in.baseHash     = base;
        in.liveHash     = live;
        in.liveReadable = readable;
        return Judge(in);
    };

    // The refused base: nothing, or contents the container no longer holds.
    check(judge(0, kH0, true) == Decision::StaleBase, "a base of zero is refused");
    check(judge(kH1, kH0, true) == Decision::StaleBase, "a base the container does not hold is refused");

    // The accepted base: what the container holds now.
    check(judge(kH0, kH0, true) == Decision::Accept, "the base the container holds is accepted");

    // The sequence a third peer walks into. The host accepted client A's slice and relayed it, so
    // the container holds that content; B, which applied the relay, declares it and must be
    // accepted, while A's own older base must not be.
    check(judge(kH1, kH1, true) == Decision::Accept, "the peer holding what the relay wrote is accepted");
    check(judge(kH0, kH1, true) == Decision::StaleBase, "the peer still holding the older fan-out is refused");
    check(judge(kH1, kH2, true) == Decision::StaleBase,
          "and one write later that peer is behind in turn, whoever wrote it");

    // Contents that do not read name no base to judge against.
    check(judge(kH0, kH0, false) == Decision::Unreadable, "unreadable contents refuse a matching base");
    check(judge(kH0, 0, false) == Decision::Unreadable, "unreadable contents refuse any base");

    Reset();
    if (pass == total) {
        UE_LOGI("container_write_policy selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("container_write_policy selftest: %d/%d checks passed", pass, total);
    return false;
}

void Reset() {
    g_writes.Clear();
    g_refused = 0;
    container_reach::Reset();
}

}  // namespace coop::props::container_write_policy
