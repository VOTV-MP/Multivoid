// coop/dev/order_selftest.cpp -- see coop/dev/order_selftest.h.

#include "coop/dev/order_selftest.h"

#include "coop/config/config.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/world/order_economy.h"

#include <chrono>
#include <string>
#include <vector>

namespace coop::dev::order_selftest {
namespace {

namespace OE = ue_wrap::order_economy;
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

    UE_LOGI("[order_selftest] settle elapsed -- FIRING now");
    g_fired = true;
    int32_t before = 0;
    E::ReadPoints(&before);

    // Place the order THROUGH THE GAME'S OWN SHOP -- generateStore, then addStoreCart on the slots
    // whose stamped name matches, then makeAnOrder on the resulting cart. Deliberately NOT through
    // `store_catalog`: the first version of this drill resolved its rows through the catalog and so
    // BUILT it before the production path ever ran, which hid a CRITICAL defect for a whole session
    // (`ReadOrder` never built the catalog, so on a real client every order silently failed to
    // forward). An instrument that primes state the real path does not prime proves only itself.
    std::vector<std::wstring> rows;
    for (const wchar_t* n : kRows) rows.emplace_back(n);
    const int32_t total = OE::PlaceOrderFromShopUI(rows, /*etaSeconds*/ 150.f);
    if (total <= 0) {
        UE_LOGE("[order_selftest] ABORT: the game's own shop produced no cart for %ls/%ls/%ls -- "
                "either generateStore did not run or the chosen rows are stale against this build",
                kRows[0], kRows[1], kRows[2]);
        return;
    }

    // Reproduce Button_order's LOCAL debit. `[V]` @6122 Multiply(storePrice,-1) -> @6168
    // lib_C::addPoints. This is the debit the host's verdict has to correct -- on a commit by the
    // change-polled broadcast, on a refusal by a direct send -- so a drill that skipped it would be
    // testing the easy half only.
    E::AddPoints(-total);

    UE_LOGI("[order_selftest] placed a real shop order (%ls, %ls, %ls) costing %d; local balance "
            "%d -> %d. NOTE: nothing on this path touched store_catalog, so the forward that "
            "follows starts from COLD. EXPECT the host to log either 'committed ... and charged %d' "
            "or 'REFUSING order'",
            kRows[0], kRows[1], kRows[2], total, before, before - total, total);
}

}  // namespace coop::dev::order_selftest
