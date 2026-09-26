// coop/creatures/kerfus_follow.cpp -- see coop/creatures/kerfus_follow.h.

#include "coop/creatures/kerfus_follow.h"

#include "coop/creatures/served_player.h"
#include "coop/net/session.h"

#include "ue_wrap/actors/kerfus.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>
#include <unordered_map>

namespace coop::kerfus_follow {
namespace {

namespace R  = ue_wrap::reflection;
namespace SP = coop::served_player;
namespace UK = ue_wrap::kerfus;

std::atomic<coop::net::Session*> g_session{nullptr};

// The Kerfuses a client pressed, by the actor, its slot index re-checked. `on` once the tick has seen
// the press turn it on: a record whose Kerfus is off afterwards -- pressed off, out of energy, a
// possess arrived -- goes, and with it the served-player record its reads are answered from.
struct Rec {
    int32_t idx;
    uint8_t slot;
    bool    on = false;
};
std::unordered_map<void*, Rec> g_activator;

bool OnHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Host;
}

void Drop(void* kerfus) {
    g_activator.erase(kerfus);
    SP::Unserve(kerfus);
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    for (auto it = g_activator.begin(); it != g_activator.end();) {
        void* k = it->first;
        bool on = false;
        if (!R::IsLiveByIndex(k, it->second.idx) || !UK::ReadActive(k, on)) {
            SP::Unserve(k);
            it = g_activator.erase(it);
            continue;
        }
        if (on && !it->second.on) {
            it->second.on = true;
            UE_LOGI("kerfus_follow: Kerfus %p is on by slot %u's press -- it follows that player", k,
                    static_cast<unsigned>(it->second.slot));
        } else if (!on) {
            if (it->second.on)
                UE_LOGI("kerfus_follow: Kerfus %p is off -- slot %u's press no longer leads it", k,
                        static_cast<unsigned>(it->second.slot));
            SP::Unserve(k);
            it = g_activator.erase(it);
            continue;
        }
        ++it;
    }
}

void OnToggle(void* kerfus, uint8_t slot) {
    if (!kerfus || !OnHost()) return;
    // A press on a Kerfus that is on turns it off: the tick sees that edge and drops its record.
    bool on = false;
    if (UK::ReadActive(kerfus, on) && on) return;
    if (slot == 0) {
        Drop(kerfus);  // the host's own press turns it on: it follows the host
        return;
    }
    // Its approach, stop and jump read GetPlayerCharacter, its path goal GetPlayerPawn.
    if (!SP::Serve(kerfus, slot, SP::kPlayerCharacter | SP::kPlayerPawn)) return;
    g_activator[kerfus] = Rec{R::InternalIndexOf(kerfus), slot};
}

void OnPeerLeft(uint8_t slot) {
    bool any = false;
    for (auto it = g_activator.begin(); it != g_activator.end();) {
        if (it->second.slot == slot) { it = g_activator.erase(it); any = true; }
        else ++it;
    }
    if (any)
        UE_LOGI("kerfus_follow: slot %u left -- its Kerfuses follow the host again", static_cast<unsigned>(slot));
    // served_player drops the leaver's own records.
}

void OnDisconnect() { g_activator.clear(); }

}  // namespace coop::kerfus_follow
