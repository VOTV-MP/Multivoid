// coop/dev/order_selftest.cpp -- see coop/dev/order_selftest.h.

#include "coop/dev/order_selftest.h"

#include "coop/config/config.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/world/order_economy.h"
#include "ue_wrap/world/store_catalog.h"

#include <chrono>
#include <string>
#include <vector>

namespace coop::dev::order_selftest {
namespace {

namespace OE = ue_wrap::order_economy;
namespace SC = ue_wrap::store_catalog;
namespace E  = ue_wrap::economy;

// Deliberately later than order_sync's own watermark prime (which happens the first tick the
// client's saveSlot resolves). An order placed BEFORE the prime is swallowed as pre-existing local
// save state and never forwarded -- which would look exactly like a broken forward path.
//
// WALL CLOCK, not a tick count. The first cut used 1800 ticks copied from drone_probe and fired
// NEVER inside a 26-second connected window, because "a tick" is a frame here and the settle it
// buys therefore depends on the host's framerate. Seconds are what the intent actually is.
constexpr int kSettleMs = 9000;

// See the header for why these three. `cup` is the load-bearing one: its `object` is the generic
// prop_C and its real identity is in `asProp`, so it only delivers correctly if the host copies the
// whole row.
const wchar_t* const kRows[] = {L"drive", L"cup", L"burger"};

bool g_fired = false;
bool g_armed = false;
std::chrono::steady_clock::time_point g_armedAt{};
uint64_t g_waitLogs = 0;

bool Enabled() {
    static const bool s_on =
        coop::config::ResolveFlag(::coop::config_registry::rows::order_selftest);
    return s_on;
}

}  // namespace

void Tick(bool connected, bool isHost) {
    if (!Enabled() || g_fired || isHost || !connected) return;
    if (!g_armed) {
        // Say ONCE that the instrument is live. A selftest that prints nothing is indistinguishable
        // from a switched-off one, and that ambiguity has now cost two smoke runs on this feature
        // alone -- once when the ini key landed inside a comment, once when the settle never elapsed.
        g_armed = true;
        g_armedAt = std::chrono::steady_clock::now();
        UE_LOGI("[order_selftest] ARMED on this client -- will place a real shop order in %d ms",
                kSettleMs);
        return;
    }
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_armedAt).count() < kSettleMs) return;

    if (!SC::Ready()) {
        // Not fatal and not a verdict: the table may still be loading. Say so at a low rate rather
        // than latching, so a run that never fires is distinguishable from one that fired and failed.
        if ((g_waitLogs++ % 300) == 0)
            UE_LOGW("[order_selftest] waiting: this peer's store_catalog is not ready yet");
        return;
    }

    OE::OrderData od;
    int64_t total = 0;
    for (const wchar_t* name : kRows) {
        const SC::Row* row = SC::Find(name);
        if (!row) {
            UE_LOGE("[order_selftest] ABORT: row '%ls' is not in this peer's list_store -- the "
                    "selftest's chosen rows are stale against this game build", name);
            g_fired = true;  // do not spin; a stale row list is a code fix, not a retry
            return;
        }
        od.rowNames.emplace_back(name);
        total += row->price;
    }

    g_fired = true;
    int32_t before = 0;
    E::ReadPoints(&before);

    // Reproduce Button_order's LOCAL half: the client debits itself first. `[V]` @6122
    // Multiply(storePrice,-1) -> @6168 lib_C::addPoints. This is the debit the host's verdict has to
    // correct -- on a commit by the change-polled broadcast, on a refusal by a direct send -- so a
    // drill that skipped it would test the easy half only.
    E::AddPoints(-static_cast<int32_t>(total));

    // ...then the commit half. makeAnOrder appends to this peer's own saveSlot.orders, which is what
    // order_sync's watermark poll picks up and forwards. `automatic=false` matches what Button_order
    // passes (it gates the items_bought STAT, not any charge).
    const bool ok = OE::CommitOrder(od, /*etaSeconds*/ 150.f, /*automatic*/ false);

    UE_LOGI("[order_selftest] placed a %zu-item order (%ls, %ls, %ls) costing %lld; local balance "
            "%d -> %d; makeAnOrder dispatch=%d. EXPECT the host to log either 'committed ... and "
            "charged %lld' or 'REFUSING order'",
            od.rowNames.size(), kRows[0], kRows[1], kRows[2], static_cast<long long>(total),
            before, before - static_cast<int32_t>(total), ok ? 1 : 0,
            static_cast<long long>(total));
}

}  // namespace coop::dev::order_selftest
