// coop/dev/order_selftest.cpp -- see coop/dev/order_selftest.h.

#include "coop/dev/order_selftest.h"

#include "coop/config/config.h"
#include "coop/items/order_queue_sync.h"  // ClientCounts: what the mirror appended here
#include "coop/net/session.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/log.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/world/order_economy.h"

#include <string>
#include <vector>

namespace coop::dev::order_selftest {
namespace {

namespace OE = ue_wrap::order_economy;
namespace E  = ue_wrap::economy;

// See the header for why these three. `cup` is the load-bearing one: its `object` is the generic
// prop_C and its real identity is in `asProp`, so it only delivers correctly if the host copies the
// whole row.
const wchar_t* const kRows[] = {L"drive", L"cup", L"burger"};

bool g_clientFired = false;
bool g_clientDone  = false;
bool g_hostFired   = false;
int  g_readySeenTicks = 0;  // host: ticks since a client's world was first seen ready

bool Enabled() {
    static const bool s_on =
        coop::config::ResolveFlag(::coop::config_registry::rows::order_selftest);
    return s_on;
}

// The client's shop order, placed THROUGH THE GAME'S OWN SHOP -- generateStore, then addStoreCart on
// the slots whose stamped name matches, then makeAnOrder on the resulting cart. Deliberately NOT
// through `store_catalog`: a drill that resolved its rows through the catalog once BUILT it before the
// production path ever ran, which hid a defect for a whole session. An instrument that primes state
// the real path does not prime proves only itself.
void PlaceClientOrder() {
    int32_t before = 0;
    E::ReadPoints(&before);
    std::vector<std::wstring> rows;
    for (const wchar_t* n : kRows) rows.emplace_back(n);
    const int32_t total = OE::PlaceOrderFromShopUI(rows, /*etaSeconds*/ 150.f);
    if (total <= 0) {
        UE_LOGE("[order_selftest] ABORT: the game's own shop produced no cart for %ls/%ls/%ls -- "
                "either generateStore did not run or the chosen rows are stale against this build",
                kRows[0], kRows[1], kRows[2]);
        return;
    }
    // Button_order's LOCAL debit: the store price times -1 through lib_C::addPoints. The debit the
    // host's verdict has to correct, so a drill that skipped it would test the easy half only.
    E::AddPoints(-total);
    UE_LOGI("[order_selftest] client placed a real shop order (%ls, %ls, %ls) costing %d; local balance "
            "%d -> %d. EXPECT the host to log either 'committed ... and charged %d' or 'REFUSING order'",
            kRows[0], kRows[1], kRows[2], total, before, before - total, total);
}

void ClientTick() {
    // Its world is up and its join over: the order gate is installed at session start and reads the
    // order from makeAnOrder's own parameter, so nothing else has to settle first.
    if (!g_clientFired) {
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        g_clientFired = true;
        PlaceClientOrder();
        return;
    }
    if (g_clientDone) return;
    // The verdict on a world event's order -- the host's daily one, or one its save already queued and the
    // join snapshot carried: it reached this queue by class, every item built.
    const coop::order_queue_sync::Counts c = coop::order_queue_sync::ClientCounts();
    if (c.byClass == 0) return;
    g_clientDone = true;
    UE_LOGI("[order_selftest] client DONE: a world event's order reached this queue by class -- "
            "%llu order(s) appended, %llu item(s) by class (%llu with an asProp), %llu left out -- %s",
            static_cast<unsigned long long>(c.orders), static_cast<unsigned long long>(c.byClass),
            static_cast<unsigned long long>(c.withAsProp), static_cast<unsigned long long>(c.leftOut),
            c.leftOut == 0 ? "PASS" : "FAIL");
}

void HostTick(coop::net::Session& s) {
    if (g_hostFired) return;
    bool anyReady = false;
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && !anyReady; ++slot)
        anyReady = s.IsSlotWorldReady(slot);
    if (!anyReady) return;
    // The client's queue follows the host's only from the reset its world-ready snapshot carries, which
    // the host sends as it marks the slot ready; a tick later, the order made here comes after it.
    if (++g_readySeenTicks < 2) return;
    g_hostFired = true;
    const int32_t before = OE::OrderCount();
    const bool ok = OE::MakeDailyOrder();
    if (!ok) {
        UE_LOGW("[order_selftest] ABORT: the host's daily order was not queued (the drone, the tower or the "
                "laptop is not there, or the queue did not grow) -- the queue stays %d", OE::OrderCount());
        return;
    }
    UE_LOGI("[order_selftest] host made the day cycle's own daily order (makeAnOrder, automatic); the queue "
            "%d -> %d", before, OE::OrderCount());
}

}  // namespace

void Tick(coop::net::Session* s) {
    if (!Enabled() || !s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) HostTick(*s);
    else ClientTick();
}

void OnDisconnect() {
    g_clientFired = g_clientDone = g_hostFired = false;
    g_readySeenTicks = 0;
}

}  // namespace coop::dev::order_selftest
