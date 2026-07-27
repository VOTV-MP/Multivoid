// coop/dev/roster_token_selftest.cpp -- see coop/dev/roster_token_selftest.h.

#include "coop/dev/roster_token_selftest.h"

#include "coop/config/config.h"
#include "coop/moderation/moderation.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"

#include "ue_wrap/core/log.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace coop::dev::roster_token_selftest {
namespace {

coop::net::Session* g_session = nullptr;

bool Enabled() {
    static const bool s =
        coop::config::ResolveFlag(::coop::config_registry::rows::roster_token_selftest);
    return s;
}

// What an open ban modal is holding. NOT a PerSlotState: this deliberately
// OUTLIVES the person it describes -- that is the entire hazard being tested, and
// a state that cleared itself on hand-over could not express it.
struct Capture {
    uint16_t playerNo = 0;
    uint32_t generation = 0;
};
std::array<Capture, coop::roster_ledger::kMaxSlots> g_captured{};

void RunSuccessorChecks(int slot, const Capture& stale,
                        const coop::roster_ledger::Row& incoming) {
    coop::net::Session* s = g_session;
    if (!s) return;

    char staleAddr[64] = {};
    const bool staleOk =
        s->GetPeerAddressWithToken(slot, stale.generation, staleAddr, sizeof(staleAddr));
    char liveAddr[64] = {};
    const bool liveOk = s->GetPeerAddressWithToken(slot, incoming.bornGeneration,
                                                   liveAddr, sizeof(liveAddr));

    UE_LOGI("roster_token_selftest: slot %d changed hands #%u -> #%u -- "
            "NEGATIVE(stale gen=%u)=%s POSITIVE(live gen=%u)=%s",
            slot, static_cast<unsigned>(stale.playerNo),
            static_cast<unsigned>(incoming.playerNo),
            stale.generation, staleOk ? "ACCEPTED" : "REJECTED",
            incoming.bornGeneration, liveOk ? "ACCEPTED" : "REJECTED");

    if (staleOk)
        UE_LOGW("roster_token_selftest: FAIL -- the stale token resolved the successor's "
                "address; a ban captured on #%u would be written against #%u",
                static_cast<unsigned>(stale.playerNo),
                static_cast<unsigned>(incoming.playerNo));
    else if (!liveOk)
        UE_LOGW("roster_token_selftest: INCONCLUSIVE -- the LIVE token was refused too, so "
                "the negative above proves nothing about staleness");
    else
        UE_LOGI("roster_token_selftest: PASS (net-layer generation guard: stale refused, "
                "live accepted)");

    // The real path, with the real stale token. It must ABORT before writing a ban
    // row; the reason string names the drill so a banlist entry, if one ever
    // appeared, would be attributable on sight.
    UE_LOGI("roster_token_selftest: firing the REAL BanPlayer with the stale token for #%u "
            "-- it must ABORT and ban nobody",
            static_cast<unsigned>(stale.playerNo));
    coop::moderation::BanPlayer(
        coop::moderation::TokenFor(slot, stale.playerNo, stale.generation),
        "roster_token_selftest (must ABORT)");
}

void OnSlotReplaced(int slot, const coop::roster_ledger::Row& outgoing,
                    const coop::roster_ledger::Row& incoming) {
    if (slot < 1 || slot >= coop::roster_ledger::kMaxSlots) return;
    if (!g_session || g_session->role() != coop::net::Role::Host) return;

    if (!incoming.occupied()) {
        // The target left and nobody has taken the seat yet. HOLD the capture --
        // an admin's modal stays open across exactly this window, and releasing it
        // here would quietly delete the hazard instead of testing it.
        if (g_captured[slot].playerNo != 0)
            UE_LOGI("roster_token_selftest: slot %d emptied (#%u left) -- holding the "
                    "captured token, as an open ban modal would",
                    slot, static_cast<unsigned>(g_captured[slot].playerNo));
        return;
    }

    const Capture stale = g_captured[slot];
    if (stale.playerNo != 0 && stale.playerNo != incoming.playerNo)
        RunSuccessorChecks(slot, stale, incoming);
    else if (stale.playerNo == 0)
        UE_LOGI("roster_token_selftest: captured a token for slot %d #%u gen=%u "
                "(as the ban modal does when it opens)",
                slot, static_cast<unsigned>(incoming.playerNo), incoming.bornGeneration);

    (void)outgoing;
    g_captured[slot] = Capture{incoming.playerNo, incoming.bornGeneration};
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!Enabled()) return;
    g_session = session;
    // subsystems::Install is a RETRY pump, not a one-shot -- it is re-entered
    // every tick so modules whose engine classes are not resolved yet get another
    // chance. Measured 2026-07-27: ~57 re-entries/second. Without this latch the
    // arming line printed 14,095 times in one 4-minute run and the subscribe ran
    // just as often (harmless only because the ledger dedupes by function
    // pointer). Every well-behaved neighbour in that list latches; this one did
    // not, and the smoke caught it.
    static std::atomic<bool> armed{false};
    if (armed.exchange(true, std::memory_order_acq_rel)) return;
    coop::roster_ledger::SubscribeSlotReplaced(&OnSlotReplaced);
    UE_LOGI("roster_token_selftest: armed -- will fire on the first slot hand-over");
}

}  // namespace coop::dev::roster_token_selftest
