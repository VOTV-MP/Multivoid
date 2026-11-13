// coop/world/power_upgrade.cpp -- see coop/world/power_upgrade.h.

#include "coop/world/power_upgrade.h"

#include "coop/element/intent_authority.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/props/remote_prop.h"  // the destroys this host applies
#include "coop/world/power_grid.h"   // kGeneratorReachUU

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/generator.h"
#include "ue_wrap/engine/engine.h"            // TryGetActorLocation
#include "ue_wrap/engine/engine_component.h"  // GetComponentLocation

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

namespace coop::power_upgrade {
namespace {

namespace EL  = coop::element;
namespace GEN = ue_wrap::generator;

// A spend waits for its install op; a player's inserts come seconds apart, so a few a slot cover any that race.
constexpr int kSpendsPerSlot = 4;

struct Spend {
    int32_t  index = -1;  // the generator it was spent at; -1 an empty place
    uint64_t ms = 0;
};
struct Spends {
    Spend   ring[kSpendsPerSlot];
    uint8_t next = 0;
};

std::atomic<coop::net::Session*> g_session{nullptr};
coop::roster_ledger::PerSlotState<Spends> g_spends;
uint64_t g_recorded = 0, g_refunded = 0, g_unplaced = 0;
bool g_listening = false;

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// A client's destroy this host applies: an upgrade its sender spent within reach of a generator is a spend there,
// judged as the install op is, from the host's puppet of the sender with the lane's own reach.
void OnDestroyHeard(int senderSlot, void* actor) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (senderSlot <= 0 || senderSlot >= coop::net::kMaxPeers || !GEN::IsUpgrade(actor)) return;
    const auto sender = static_cast<uint8_t>(senderSlot);
    std::vector<void*> gens;
    GEN::ReadGenerators(gens);
    const EL::IntentTarget target = EL::IntentTarget::ForClientIntent(*s, sender, coop::power_grid::kGeneratorReachUU);
    for (size_t i = 0; i < gens.size() && i < static_cast<size_t>(coop::net::kPowerGridGenerators); ++i) {
        if (!gens[i]) continue;
        const EL::IntentSubject reach = target.Authorize(gens[i]);
        if (!reach) continue;
        Spends& sp = g_spends[sender];
        sp.ring[sp.next] = {static_cast<int32_t>(i), NowMs()};
        sp.next = static_cast<uint8_t>((sp.next + 1) % kSpendsPerSlot);
        ++g_recorded;
        UE_LOGI("power_upgrade: slot %u spent an upgrade at generator %zu (%.0f uu, allowed %.0f)", sender, i,
                reach.distUU, reach.reachUU);
        return;
    }
    // An upgrade a client destroyed away from every generator: not a spend, whatever it was.
    if (++g_unplaced <= 3)
        UE_LOGI("power_upgrade: slot %u destroyed an upgrade out of every generator's reach -- no spend", sender);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_listening) return;
    g_listening = true;
    coop::remote_prop::AddDestroyListener(&OnDestroyHeard);
}

bool TakeSpend(uint8_t slot, int32_t index) {
    if (slot == 0 || slot >= coop::net::kMaxPeers) return false;
    Spends& sp = g_spends[slot];
    Spend* newest = nullptr;
    for (Spend& e : sp.ring)
        if (e.index == index && (!newest || e.ms > newest->ms)) newest = &e;
    if (!newest) return false;
    *newest = Spend{};
    return true;
}

void Refund(uint8_t slot, void* gen) {
    ue_wrap::FVector at{};
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    void* body = rp ? rp->GetActor() : nullptr;
    bool placed = body && ue_wrap::engine::TryGetActorLocation(body, at);
    if (!placed) {
        if (void* upgradeSlot = GEN::UpgradeSlot(gen)) {
            at = ue_wrap::engine::GetComponentLocation(upgradeSlot);
            at.Z += 80.f;
            placed = true;
        }
    }
    void* refund = placed ? GEN::SpawnUpgrade(at) : nullptr;
    if (refund) ++g_refunded;
    UE_LOGW("power_upgrade: slot %u's refused install refunded -- %s", slot,
            refund ? "an upgrade spawned where it stands" : "NOT spawned (the item is lost)");
}

void OnDisconnect() {
    if (g_recorded || g_refunded)
        UE_LOGI("power_upgrade: session end -- spends recorded %llu, refunds %llu",
                static_cast<unsigned long long>(g_recorded), static_cast<unsigned long long>(g_refunded));
    for (int slot = 0; slot < g_spends.size(); ++slot) g_spends[slot] = Spends{};
    g_recorded = g_refunded = g_unplaced = 0;
}

uint64_t RefundsSpawned() { return g_refunded; }

}  // namespace coop::power_upgrade
