// coop/props/container_write_policy.cpp -- see coop/props/container_write_policy.h.

#include "coop/props/container_write_policy.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/element.h"
#include "coop/element/intent_authority.h"
#include "coop/net/session.h"
#include "coop/props/container_reach.h"
#include "ue_wrap/core/log.h"

namespace coop::props::container_write_policy {
namespace {

uint64_t g_refused = 0;

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

Decision Accept(uint32_t eid, uint64_t baseHash, LiveHashReader readLive, uint8_t authorSlot, uint64_t nowMs,
                coop::net::Session& session, coop::props::container_birth::Source src) {
    if (container_birth::SpendArrival(eid, authorSlot, nowMs, session, src) == container_birth::Arrival::TooFast)
        return Decision::TooFast;

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
    g_refused = 0;
}

}  // namespace coop::props::container_write_policy
