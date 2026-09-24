// coop/interactables/upgrade_sync.cpp -- see coop/interactables/upgrade_sync.h.

#include "coop/interactables/upgrade_sync.h"

#include "coop/interactables/device_occupancy.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/world/upgrades.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>

namespace coop::upgrade_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace UP = ue_wrap::upgrades;

// The two buttons, by their cooked delegate-binding names. They are the row's own UFunctions and
// nothing else in the game carries either name, so a name watch is exact. Watching the BUTTON and
// not the purchase is deliberate: the handler's whole body is the jump into the ubergraph, so a
// cancel here takes the debit, the level write, the sound and the repaint together.
const wchar_t* const kFnBuy =
    L"BndEvt__button_upgDownloadSpd_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__"
    L"DelegateSignature";
const wchar_t* const kFnSell =
    L"BndEvt__button_upgDown_K2Node_ComponentBoundEvent_1_OnButtonClickedEvent__"
    L"DelegateSignature";
constexpr int kTagUpgrade = 0x55504752;  // 'UPGR'

// The claim key of the shared widget the panel lives on. Holding it IS the sender's reach: the
// rows exist only inside that widget, and the occupancy lane already arbitrates who is at it.
const wchar_t* const kLaptopKey = L"laptop";

// One purchase is one press. The bound is not about cost -- the host re-checks every condition --
// but about a stalled client's backlog arriving as a burst of charges.
constexpr float  kPressBurst     = 3.0f;
constexpr float  kPressPerSecond = 2.0f;
constexpr size_t kMaxPending     = 6;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchBuy = false, g_watchSell = false;
bool g_saidLive = false;

// The last levels the host broadcast: the change poll's dedup.
int32_t g_published[UP::kLevelCount];
bool    g_havePublished = false;

// The client's latest unapplied mirror. Held until the store resolves, so a join-edge mirror that
// lands under the load screen is not lost.
coop::net::UpgradeLevelsPayload g_pendingApply{};
bool     g_havePendingApply = false;
uint32_t g_pendingGen = 0;  // the world the pending mirror was received in

uint64_t g_sent = 0, g_bought = 0, g_sold = 0, g_denied = 0, g_broadcast = 0, g_applied = 0;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

struct Bucket {
    float    tokens = kPressBurst;
    uint64_t lastMs = 0;
};
Bucket g_rate[coop::net::kMaxPeers];
std::deque<coop::net::UpgradeIntentPayload> g_pending[coop::net::kMaxPeers];

bool TakeToken(uint8_t slot) {
    Bucket& b = g_rate[slot];
    const uint64_t now = NowMs();
    if (b.lastMs != 0 && now > b.lastMs) {
        b.tokens += kPressPerSecond * static_cast<float>(now - b.lastMs) / 1000.0f;
        if (b.tokens > kPressBurst) b.tokens = kPressBurst;
    }
    b.lastMs = now;
    if (b.tokens < 1.0f) return false;
    b.tokens -= 1.0f;
    return true;
}

// ---- host side -------------------------------------------------------------------------------

// Read the struct and broadcast it if it moved. Also the acknowledgement of a purchase: the
// publisher is one function so a purchase and a cheat converge by the same route.
void Publish(coop::net::Session& s, const char* why) {
    if (s.role() != coop::net::Role::Host || !s.connected()) return;
    coop::net::UpgradeLevelsPayload p{};
    if (!UP::ReadLevels(p.level)) return;
    if (g_havePublished && std::memcmp(p.level, g_published, sizeof(g_published)) == 0) return;
    const bool first = !g_havePublished;  // the first publish moved nothing; say so
    std::memcpy(g_published, p.level, sizeof(g_published));
    g_havePublished = true;
    s.SendReliable(coop::net::ReliableKind::UpgradeLevels, &p, sizeof(p));
    ++g_broadcast;
    UE_LOGI("upgrade_sync: %s (%s) -- broadcast to all (#%llu)",
            first ? "the session's first levels" : "levels moved", why,
            static_cast<unsigned long long>(g_broadcast));
}

void Execute(coop::net::Session& s, const coop::net::UpgradeIntentPayload& p, uint8_t slot) {
    const int index = static_cast<int>(p.panelIndex);
    const bool buying = p.dir == 0;

    if (!UP::IsLevelRow(index)) {
        ++g_denied;
        UE_LOGW("upgrade_sync: DENY slot=%u -- index %d is not a level row",
                static_cast<unsigned>(slot), index);
        return;
    }
    // The sender must be the peer standing at the laptop. The rows have no identity of their own
    // to name, so the claim is the reach token, and the occupancy lane already owns who holds it.
    const uint8_t holder = coop::device_occupancy::HolderOf(kLaptopKey);
    if (holder != slot) {
        ++g_denied;
        UE_LOGW("upgrade_sync: DENY slot=%u -- the laptop is held by slot %u",
                static_cast<unsigned>(slot), static_cast<unsigned>(holder));
        return;
    }

    int32_t level = 0;
    if (!UP::ReadLevel(index, &level)) {
        ++g_denied;
        UE_LOGW("upgrade_sync: DENY slot=%u -- the upgrade store did not resolve here",
                static_cast<unsigned>(slot));
        return;
    }
    int32_t points = 0;
    if (!ue_wrap::economy::ReadPoints(&points)) {
        ++g_denied;
        UE_LOGW("upgrade_sync: DENY slot=%u -- the balance did not resolve here",
                static_cast<unsigned>(slot));
        return;
    }

    if (buying) {
        // The button's own two conditions, in its own order -- including the asymmetry it really
        // has: the balance is tested against the row's UNACCUMULATED price and then charged the
        // accumulated one, so from the second level up a purchase can take the group below zero.
        // The host asks the button's question rather than a fairer one, or a client at the panel is
        // refused a level the host could buy standing in the same place.
        const int32_t price = UP::PriceAtLevel(index, level);
        if (level >= UP::MaxLevel(index)) {
            ++g_denied;
            UE_LOGI("upgrade_sync: DENY slot=%u buy index=%d -- already at max (%d)",
                    static_cast<unsigned>(slot), index, UP::MaxLevel(index));
            return;
        }
        if (points < UP::BasePrice(index)) {
            ++g_denied;
            UE_LOGI("upgrade_sync: DENY slot=%u buy index=%d -- needs %d to press, the group has %d",
                    static_cast<unsigned>(slot), index, UP::BasePrice(index), points);
            return;
        }
        if (!ue_wrap::economy::AddPoints(-price)) {
            ++g_denied;
            UE_LOGW("upgrade_sync: slot=%u buy index=%d -- the charge did not dispatch, no level "
                    "written", static_cast<unsigned>(slot), index);
            return;
        }
        if (!UP::WriteLevel(index, level + 1)) {
            // Charged and not delivered: put it back rather than leave the group short. Written,
            // not added: the game's own credit path books a positive amount as EARNINGS in the
            // save's lifetime stats, and undoing our own charge is not income.
            ue_wrap::economy::WritePoints(points);
            ++g_denied;
            UE_LOGW("upgrade_sync: slot=%u buy index=%d -- the level write failed, charge refunded",
                    static_cast<unsigned>(slot), index);
            return;
        }
        ++g_bought;
        UE_LOGI("upgrade_sync: slot=%u BOUGHT index=%d level %d->%d for %d (balance %d->%d)",
                static_cast<unsigned>(slot), index, level, level + 1, price, points,
                points - price);
    } else {
        if (level <= 0) {
            ++g_denied;
            UE_LOGI("upgrade_sync: DENY slot=%u sell index=%d -- nothing to sell",
                    static_cast<unsigned>(slot), index);
            return;
        }
        const int32_t refund = UP::RefundAtLevel(index, level);
        if (!UP::WriteLevel(index, level - 1)) {
            ++g_denied;
            UE_LOGW("upgrade_sync: slot=%u sell index=%d -- the level write failed, nothing paid",
                    static_cast<unsigned>(slot), index);
            return;
        }
        // Pay AFTER the level is gone: a failed credit leaves the group a level down rather than
        // a level up and paid for it twice.
        if (!ue_wrap::economy::AddPoints(refund))
            UE_LOGW("upgrade_sync: slot=%u sell index=%d -- level taken but the %d refund did not "
                    "dispatch", static_cast<unsigned>(slot), index, refund);
        ++g_sold;
        UE_LOGI("upgrade_sync: slot=%u SOLD index=%d level %d->%d for %d back",
                static_cast<unsigned>(slot), index, level, level - 1, refund);
    }

    // The button's own second half: the panel repaint, then the gamemode's re-derive. Without the
    // second one the level is written and nothing in the world reads it until some later purchase
    // or a load runs the hook.
    UP::RefreshOpenRows();  // the host's own panel, if it is up
    UP::ApplyUpgradedHook();
    Publish(s, "a purchase");
}

// ---- the client's gate -----------------------------------------------------------------------

// The row's `index` and whether it is a MODULE row, both read off the pressed widget.
//
// The index alone does not identify a row: the panel really does carry two widgets numbered 27 --
// the ping-strength level and the 400-credit RC module -- and the module rows are what the level
// table has no entry for. Without the flag, a press on the module was cancelled as a level buy and
// re-sent as index 27, so the presser lost the module it paid for and the group bought a
// ping-strength level for a tenth of the price. `module` is the widget's own discriminator.
bool RowOf(void* widget, int* outIndex, bool* outModule) {
    static void*   sCls = nullptr;
    static int32_t sIdxOff = -1;
    static int32_t sModByte = -1;
    static uint8_t sModMask = 0;
    void* cls = R::ClassOf(widget);
    if (!cls) return false;
    if (cls != sCls) {
        sCls = cls;
        sIdxOff = R::FindPropertyOffset(cls, L"index");
        if (!R::FindBoolProperty(cls, L"module", sModByte, sModMask)) sModByte = -1;
    }
    if (sIdxOff < 0 || sModByte < 0) return false;  // an unreadable row is not pressed for us
    const uint8_t* base = reinterpret_cast<const uint8_t*>(widget);
    *outIndex = *reinterpret_cast<const int32_t*>(base + sIdxOff);
    *outModule = (*(base + sModByte) & sModMask) != 0;
    return true;
}

sg::Verdict OnButton(const sg::Call& call, uint8_t dir) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client) return sg::Verdict::Run;
    if (!call.object) return sg::Verdict::Run;

    int index = -1;
    bool isModule = false;
    if (!RowOf(call.object, &index, &isModule)) return sg::Verdict::Run;
    // A module row buys a one-shot unlock in a store this lane does not carry. Cancelling it would
    // take the client's own unlock away and give it nothing, so it keeps running locally, exactly
    // as it did before this lane existed. The flag is tested BEFORE the index, because an index is
    // not unique across the two kinds of row.
    if (isModule || !UP::IsLevelRow(index)) return sg::Verdict::Run;

    coop::net::UpgradeIntentPayload p{};
    p.panelIndex = static_cast<uint8_t>(index);
    p.dir = dir;
    s->SendReliable(coop::net::ReliableKind::UpgradeIntent, &p, sizeof(p));
    ++g_sent;
    UE_LOGI("upgrade_sync: CLIENT asked to %s index=%d -- own body cancelled (#%llu)",
            dir == 0 ? "buy" : "sell", index, static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

sg::Verdict OnBuyPre(const sg::Call& call) { return OnButton(call, 0); }
sg::Verdict OnSellPre(const sg::Call& call) { return OnButton(call, 1); }

void ArmWatches() {
    if (!g_watchBuy)  g_watchBuy  = sg::WatchName(kFnBuy,  kTagUpgrade, &OnBuyPre,  nullptr);
    if (!g_watchSell) g_watchSell = sg::WatchName(kFnSell, kTagUpgrade, &OnSellPre, nullptr);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    ArmWatches();
}

void Tick(coop::net::Session& session) {
    sg::ResolvePendingNames();
    ArmWatches();
    if (!session.running()) return;

    if (!g_saidLive && sg::NameWatchLive(kFnBuy, kTagUpgrade)) {
        g_saidLive = true;
        UE_LOGI("upgrade_sync: the panel's buy and sell gates are live");
    }
    // And a gate that never goes live says so ONCE, because its silence is indistinguishable from
    // working: a client's purchase would run locally, debit that client and be stomped by the next
    // mirror, with nothing in either log to read.
    if (!g_saidLive && session.role() == coop::net::Role::Client) {
        static int s_ticksUnlive = 0;
        if (++s_ticksUnlive == 600)
            UE_LOGW("upgrade_sync: the panel's buy gate is still not live after ten seconds -- a "
                    "purchase made here will run locally and be overwritten");
    }

    if (session.role() == coop::net::Role::Client) {
        // A mirror belongs to the world it was sent for. A world change between its arrival and its
        // apply would write the previous world's numbers into this one, so it is dropped at the
        // seam instead; the host re-sends the levels at every world-ready.
        const uint32_t gen = ue_wrap::world_identity::Generation();
        if (g_havePendingApply && gen != g_pendingGen) {
            g_havePendingApply = false;
            UE_LOGI("upgrade_sync: a mirror from the previous world dropped at the world change");
        }
        if (g_havePendingApply) {
            if (UP::WriteLevels(g_pendingApply.level)) {
                g_havePendingApply = false;
                ++g_applied;
                UP::ApplyUpgradedHook();  // the numbers are nothing until the world re-derives from them
                const int rows = UP::RefreshOpenRows();
                UE_LOGI("upgrade_sync: applied host levels (#%llu, %d open row(s) repainted)",
                        static_cast<unsigned long long>(g_applied), rows);
            }
        }
        return;
    }

    Publish(session, "polled");
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        if (g_pending[slot].empty()) continue;
        if (!TakeToken(slot)) continue;
        const coop::net::UpgradeIntentPayload p = g_pending[slot].front();
        g_pending[slot].pop_front();
        Execute(session, p, slot);
    }
}

void SendCurrentToSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    coop::net::UpgradeLevelsPayload p{};
    if (!UP::ReadLevels(p.level)) return;
    s->SendReliableToSlot(slot, coop::net::ReliableKind::UpgradeLevels, &p, sizeof(p));
    UE_LOGI("upgrade_sync: sent the current levels directly to slot %d", slot);
}

void ApplyFromHost(const coop::net::UpgradeLevelsPayload& payload) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->role() == coop::net::Role::Host) return;  // authoritative
    g_pendingApply = payload;
    g_havePendingApply = true;
    g_pendingGen = ue_wrap::world_identity::Generation();  // the world it was sent for
}

void OnUpgradeIntent(coop::net::Session& session, const coop::net::UpgradeIntentPayload& payload,
                     uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    auto& q = g_pending[senderSlot];
    if (q.size() >= kMaxPending) {
        ++g_denied;
        return;
    }
    q.push_back(payload);
}

void OnPeerLeft(uint8_t slot) {
    if (slot >= coop::net::kMaxPeers) return;
    g_pending[slot].clear();
    g_rate[slot] = Bucket{};
}

void OnDisconnect() {
    if (g_sent || g_bought || g_sold || g_denied || g_broadcast || g_applied)
        UE_LOGI("upgrade_sync: session end -- asked=%llu bought=%llu sold=%llu denied=%llu "
                "broadcast=%llu applied=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_bought),
                static_cast<unsigned long long>(g_sold), static_cast<unsigned long long>(g_denied),
                static_cast<unsigned long long>(g_broadcast),
                static_cast<unsigned long long>(g_applied));
    for (uint8_t slot = 0; slot < coop::net::kMaxPeers; ++slot) {
        g_pending[slot].clear();
        g_rate[slot] = Bucket{};
    }
    g_havePublished = false;
    g_havePendingApply = false;
    g_sent = g_bought = g_sold = g_denied = g_broadcast = g_applied = 0;
}

}  // namespace coop::upgrade_sync
